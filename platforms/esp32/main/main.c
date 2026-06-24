// ===================================================================================
// main.c  (ESP32 / BLE-MIDI スケルトン)
// ===================================================================================
// Project:  Mimic X (MimicX-firmware) — ESP32 無線アダプタ
// Author:   Kunihiko Ohnaka (@kunichiko)
// ===================================================================================
//
// ESP32-WROOM-32 を標準 BLE-MIDI ペリフェラルとして動作させ、iOS/macOS アプリから
// BLE 経由で IDENTIFY → HEART_BEAT まで完走できることを検証する最小ファーム。
// デバイス制御 (joystick / x68k / LED) は未実装。
//
// 構成:
//   main.c          : NVS / NimBLE host 起動、GAP アドバタイズ
//   ble_midi.c      : BLE-MIDI GATT サービス、パケット復元/分割
//   mimicx_proto.c  : SysEx 処理 (IDENTIFY / ACK)
//
// プロトコル仕様: MimicX-protocol §2.3 (v0.8.0)
// ===================================================================================
#include <string.h>

#include "nvs_flash.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_midi.h"
#include "mimicx_proto.h"

static const char *TAG = "mimicx";
#define DEVICE_NAME "MimicX"

static uint8_t g_addr_type;

static void start_advertising(void);

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

    mimicx_proto_init();

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "MimicX-ESP32 BLE-MIDI skeleton started");
}
