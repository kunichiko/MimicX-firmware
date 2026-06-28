// ===================================================================================
// main.c  (ESP32 / BLE-MIDI ⇄ I2C ブリッジ)
// ===================================================================================
// Project:  Mimic X (MimicX-firmware) — ESP32 無線アダプタ (ブリッジ MCU)
// Author:   Kunihiko Ohnaka (@kunichiko)
// ===================================================================================
//
// ESP32-WROOM-32 を標準 BLE-MIDI ペリフェラルとして動作させ、phone から届く
// MIDI/SysEx をそのまま I2C 経由で CH32 (デバイスエンジン, I2C スレーブ 0x33) へ
// 中継する「ブリッジ」。タイミングが厳しい GPIO 制御 (joystick / x68k / MD6B) は
// 全て CH32 側に任せ、ESP32 はトランスポート変換に徹する (protocol §2.5)。
//
//   phone --BLE-MIDI--> ESP32 --I2C(0x33)--> CH32 --GPIO--> ターゲット機
//   phone <--BLE-MIDI-- ESP32 <--I2C+INT---- CH32  (IDENTIFY_RSP / ACK / TARGET_RX 等)
//
// ESP32 が自分で応答するのは BRIDGE_IDENTIFY (0x0A→0x0B, §6.4.5) のみ。
// それ以外 (IDENTIFY / HEART_BEAT / SET_CONFIG / Note / CC ...) は全て CH32 へ転送。
//
// 構成:
//   main.c          : NVS / NimBLE host 起動、GAP、BLE⇄I2C 中継、BRIDGE_IDENTIFY 自答
//   ble_midi.c      : BLE-MIDI GATT サービス、パケット復元/分割
//   i2c_bridge.c    : I2C マスター (CH32 への write / INT 駆動 read)
//
// プロトコル仕様: MimicX-protocol §2.3 / §2.5 / §6.4.5 (v0.8.0)
// ===================================================================================
#include <string.h>
#include <stdbool.h>

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_midi.h"
#include "i2c_bridge.h"
#include "ch32_swd.h"

static const char *TAG = "mimicx";
#define DEVICE_NAME "MimicX"

// ---------------------------------------------------------------------------
// プロトコル定数 (§6, CH32 と一致させること)
// ---------------------------------------------------------------------------
#define BRIDGE_PROTO_MAJOR  0
#define BRIDGE_PROTO_MINOR  8   // ブリッジが実装する phone 側プロトコル (0.8)
#define BRIDGE_FW_MAJOR     0
#define BRIDGE_FW_MINOR     1
#define BRIDGE_FW_PATCH     0

#define SYSEX_MFR_ID            0x7D
#define SYSEX_SUB_ID            0x01
#define CMD_DISCONNECT          0x09
#define CMD_BRIDGE_IDENTIFY_REQ 0x0A
#define CMD_BRIDGE_IDENTIFY_RSP 0x0B
#define CMD_RESET               0x7F

#define TRANSPORT_BLE           0x01   // §6.4.5 transport: 0x00=USB, 0x01=BLE

static uint8_t g_addr_type;

static void start_advertising(void);

// ---------------------------------------------------------------------------
// serial[16]: ESP32 base MAC (6 byte) を 8 byte に 0 埋めして 16 ASCII hex 化。
// これは「ブリッジの serial」(BLE 接続先の識別用)。デバイスの同一性は CH32 の
// IDENTIFY (Chip UID) を正とする (§6.4.5)。
// ---------------------------------------------------------------------------
static void build_serial16(char out[16]) {
    static const char hex[] = "0123456789ABCDEF";
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    uint8_t bytes[8] = { 0, 0, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5] };
    for (int i = 0; i < 8; i++) {
        out[i * 2]     = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0F];
    }
}

// ---------------------------------------------------------------------------
// BRIDGE_IDENTIFY_RESPONSE をブリッジ自身が組み立てて notify する (§6.4.5)。
// ---------------------------------------------------------------------------
static void send_bridge_identify_response(void) {
    uint8_t rsp[64];
    int i = 0;
    rsp[i++] = 0xF0;
    rsp[i++] = SYSEX_MFR_ID;
    rsp[i++] = SYSEX_SUB_ID;
    rsp[i++] = CMD_BRIDGE_IDENTIFY_RSP;
    rsp[i++] = BRIDGE_PROTO_MAJOR;
    rsp[i++] = BRIDGE_PROTO_MINOR;
    rsp[i++] = BRIDGE_FW_MAJOR;
    rsp[i++] = BRIDGE_FW_MINOR;
    rsp[i++] = BRIDGE_FW_PATCH;
    rsp[i++] = TRANSPORT_BLE;
    char ser[16];
    build_serial16(ser);
    memcpy(&rsp[i], ser, 16);
    i += 16;
    static const char name[] = "MimicX-ESP32";
    for (int j = 0; name[j] && i < (int)sizeof(rsp) - 1; j++) {
        rsp[i++] = (uint8_t)(name[j] & 0x7F);
    }
    rsp[i++] = 0xF7;
    ble_midi_notify(rsp, i);
}

// ---------------------------------------------------------------------------
// 中継: phone(BLE) → CH32(I2C)
//   BRIDGE_IDENTIFY のみブリッジが自答。それ以外は全て CH32 へ素通し。
// ---------------------------------------------------------------------------
static bool is_bridge_identify_req(const uint8_t *m, int n) {
    return n >= 5 && m[0] == 0xF0 && m[1] == SYSEX_MFR_ID &&
           m[2] == SYSEX_SUB_ID && m[3] == CMD_BRIDGE_IDENTIFY_REQ;
}

static void on_ble_rx(const uint8_t *msg, int len) {
    if (is_bridge_identify_req(msg, len)) {
        ESP_LOGI(TAG, "BRIDGE_IDENTIFY_REQUEST -> self-answer");
        send_bridge_identify_response();
        return;
    }
    i2c_bridge_write(msg, len);   // それ以外は CH32 へ中継
}

// ---------------------------------------------------------------------------
// 中継: CH32(I2C+INT) → phone(BLE)
// ---------------------------------------------------------------------------
static void on_i2c_rx(const uint8_t *midi, int len) {
    ble_midi_notify(midi, len);
}

// 切断時、CH32 に DISCONNECT + RESET を送って HID を解放させる (押しっぱなし防止)。
static void notify_ch32_disconnect(void) {
    static const uint8_t disc[] = { 0xF0, SYSEX_MFR_ID, SYSEX_SUB_ID, CMD_DISCONNECT, 0x00, 0xF7 };
    static const uint8_t rst[]  = { 0xF0, SYSEX_MFR_ID, SYSEX_SUB_ID, CMD_RESET,      0x00, 0xF7 };
    i2c_bridge_write(disc, sizeof(disc));
    i2c_bridge_write(rst, sizeof(rst));
}

// ---------------------------------------------------------------------------
// GAP イベント
// ---------------------------------------------------------------------------
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "connected; conn_handle=%d", event->connect.conn_handle);
                ble_midi_set_conn(event->connect.conn_handle);
                // レイテンシ低減のため connection interval を 7.5-15ms へ更新 (§2.3)。
                struct ble_gap_upd_params p = {
                    .itvl_min = 6,    // 6 * 1.25ms = 7.5ms
                    .itvl_max = 12,   // 12 * 1.25ms = 15ms
                    .latency  = 0,
                    .supervision_timeout = 400,  // 4s
                };
                ble_gap_update_params(event->connect.conn_handle, &p);
            } else {
                ESP_LOGW(TAG, "connect failed; status=%d", event->connect.status);
                start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnect; reason=%d", event->disconnect.reason);
            ble_midi_set_conn(BLE_HS_CONN_HANDLE_NONE);
            notify_ch32_disconnect();   // CH32 側の HID を解放
            start_advertising();
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "subscribe; cur_notify=%d", event->subscribe.cur_notify);
            return 0;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "mtu update; mtu=%d", event->mtu.value);
            return 0;

        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// アドバタイズ: MIDI サービス UUID を広告し、名前は scan response に載せる
//   (128bit UUID(18B) + flags(3B) で adv が埋まるため名前は分離する)
// ---------------------------------------------------------------------------
static void start_advertising(void) {
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    struct ble_gap_adv_params adv_params;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&ble_midi_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (uint8_t *)DEVICE_NAME;
    rsp_fields.name_len = strlen(DEVICE_NAME);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc); return; }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(g_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "adv_start rc=%d", rc); return; }
    ESP_LOGI(TAG, "advertising as \"%s\"", DEVICE_NAME);
}

// ---------------------------------------------------------------------------
// NimBLE host コールバック
// ---------------------------------------------------------------------------
static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr rc=%d", rc); return; }
    rc = ble_hs_id_infer_auto(0, &g_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return; }
    start_advertising();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "nimble host reset; reason=%d", reason);
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();              // ble_hs_shutdown まで戻らない
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// エントリポイント
// ---------------------------------------------------------------------------
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    // GATT / GAP サービス初期化
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_midi_register_gatt();
    if (rc != 0) { ESP_LOGE(TAG, "register_gatt rc=%d", rc); return; }

    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) { ESP_LOGE(TAG, "device_name_set rc=%d", rc); }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    // I2C ブリッジ (CH32 デバイスエンジンへの橋渡し) を起動。
    // BT コントローラの割り込み確保と競合しないよう NimBLE 初期化の後に行う。
    i2c_bridge_init(on_i2c_rx);

    // BLE 受信 → 中継ハンドラを登録 (i2c_bridge_init 完了後)。
    ble_midi_set_rx_handler(on_ble_rx);

    char ser[16];
    build_serial16(ser);
    ESP_LOGI(TAG, "MimicX-ESP32 bridge proto %d.%d fw %d.%d.%d serial=%.16s",
             BRIDGE_PROTO_MAJOR, BRIDGE_PROTO_MINOR,
             BRIDGE_FW_MAJOR, BRIDGE_FW_MINOR, BRIDGE_FW_PATCH, ser);

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "MimicX-ESP32 BLE-MIDI <-> I2C bridge started");
}
