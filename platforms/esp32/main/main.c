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
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_rom_sys.h"   // esp_rom_software_reset_system (§6.4.6)
// FORCE_DOWNLOAD_BOOT (§6.4.6)。C6 は LP_AON、それ以外は RTC_CNTL 配下。
#if defined(CONFIG_IDF_TARGET_ESP32C6)
#include "soc/lp_aon_reg.h"
#else
#include "soc/rtc_cntl_reg.h"
#endif

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "host/ble_store.h"

#include "ble_midi.h"
#include "i2c_bridge.h"
#include "ch32_swd.h"
#include "ch32_ota.h"
#include "board_config.h"
#include "mimicx_version.h"

#if defined(BOARD_HAS_USB_MIDI)
#include "usb_midi_bridge.h"   // S3 のみ: USB-OTG による USB-MIDI トランスポート
#endif

#if defined(BOARD_HAS_ANTENNA_SWITCH)
#include "driver/gpio.h"
#endif

// NimBLE のボンド永続化ストア (NVS)。ESP-IDF が提供 (ヘッダが include パスに無いため
// 前方宣言する。ESP-IDF の NimBLE 例と同じ書き方)。
void ble_store_config_init(void);

static const char *TAG = "mimicx";
#define DEVICE_NAME "MimicX"

// ---------------------------------------------------------------------------
// プロトコル定数 (§6, CH32 と一致させること)
// ---------------------------------------------------------------------------
#define BRIDGE_PROTO_MAJOR  0
#define BRIDGE_PROTO_MINOR  9   // ブリッジが実装する phone 側プロトコル (0.9)。FW 版数とは独立
// ESP ブリッジの FW 版数は共有の単一ソース (common/mimicx_version.h) を参照する。
// IDENTIFY 応答 (§6) と起動バナーでアプリへ申告する値。CH32 / タグと必ず一致する。
#define BRIDGE_FW_MAJOR     MIMICX_VERSION_MAJOR
#define BRIDGE_FW_MINOR     MIMICX_VERSION_MINOR
#define BRIDGE_FW_PATCH     MIMICX_VERSION_PATCH

#define SYSEX_MFR_ID            0x7D
#define SYSEX_SUB_ID            0x01
#define CMD_DISCONNECT          0x09
#define CMD_ACK                 0x06
#define CMD_BRIDGE_IDENTIFY_REQ 0x0A
#define CMD_BRIDGE_IDENTIFY_RSP 0x0B
#define CMD_BRIDGE_REBOOT_BL    0x0C
#define CMD_RESET               0x7F

#define ACK_OK                  0x00   // §6 ACK status
#define ACK_INVALID_VALUE       0x03

// BRIDGE_REBOOT_BOOTLOADER のマジック "BOT" (§6.4.6)。誤動作で書き込み不能な状態に
// 落ちるのを防ぐため、一致しない限り再起動しない。
#define REBOOT_BL_MAGIC0        0x42
#define REBOOT_BL_MAGIC1        0x4F
#define REBOOT_BL_MAGIC2        0x54

#define TRANSPORT_USB           0x00   // §6.4.5 transport: 0x00=USB, 0x01=BLE
#define TRANSPORT_BLE           0x01

static uint8_t g_addr_type;
static volatile bool g_ble_connected = false;   // GAP 接続状態 (切断通知の判定用)

static void start_advertising(void);

// ---------------------------------------------------------------------------
// ボード早期初期化: XIAO ESP32-C6 のアンテナ RF スイッチ設定 (§board_config.h)。
// オンボードセラミックアンテナを使うには GPIO3=Low (RF スイッチ給電) + GPIO14=Low
// (オンボード選択) を出力する。未設定だと BLE の電波が極端に弱くなるため、無線を
// 立ち上げる前 (app_main 冒頭) に行う。ESP32 DevKit では no-op。
// ---------------------------------------------------------------------------
static void board_early_init(void) {
#if defined(BOARD_HAS_ANTENNA_SWITCH)
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_ANT_RF_ENABLE_GPIO) | (1ULL << BOARD_ANT_SELECT_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(BOARD_ANT_RF_ENABLE_GPIO, 0);   // RF スイッチに給電
    gpio_set_level(BOARD_ANT_SELECT_GPIO, 0);      // オンボードアンテナを選択
    ESP_LOGI(TAG, "XIAO C6 antenna: onboard (RF_EN=GPIO%d SEL=GPIO%d)",
             BOARD_ANT_RF_ENABLE_GPIO, BOARD_ANT_SELECT_GPIO);
#endif
}

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
// BRIDGE_IDENTIFY_RESPONSE をブリッジ自身が組み立てて送信する (§6.4.5)。
// transport には要求が届いた経路 (TRANSPORT_BLE / TRANSPORT_USB) を申告し、
// send で同じ経路に返す。
// ---------------------------------------------------------------------------
static void send_bridge_identify_response(uint8_t transport,
                                          void (*send)(const uint8_t *msg, int n)) {
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
    rsp[i++] = transport;
    char ser[16];
    build_serial16(ser);
    memcpy(&rsp[i], ser, 16);
    i += 16;
    static const char name[] = "MimicX-ESP32";
    for (int j = 0; name[j] && i < (int)sizeof(rsp) - 1; j++) {
        rsp[i++] = (uint8_t)(name[j] & 0x7F);
    }
    rsp[i++] = 0xF7;
    send(rsp, i);
}

// ---------------------------------------------------------------------------
// 中継: phone(BLE) → CH32(I2C)
//   BRIDGE_IDENTIFY のみブリッジが自答。それ以外は全て CH32 へ素通し。
// ---------------------------------------------------------------------------
static bool is_bridge_identify_req(const uint8_t *m, int n) {
    return n >= 5 && m[0] == 0xF0 && m[1] == SYSEX_MFR_ID &&
           m[2] == SYSEX_SUB_ID && m[3] == CMD_BRIDGE_IDENTIFY_REQ;
}

// ---------------------------------------------------------------------------
// BRIDGE_REBOOT_BOOTLOADER (0x0C, §6.4.6)
//
// FORCE_DOWNLOAD_BOOT を立ててリセットすると、次回起動時に ROM のダウンロード
// モードで立ち上がる (= BOOT ボタンを押しながら電源投入したのと同じ状態)。
// これで基板上の小さな BOOT ボタンを押さずに esptool / esptool-js から書き込める。
//
// 注意: ダウンロードモードに入ると MIDI デバイスとしては消える。通常動作に戻すには
// 電源再投入かファーム書き込みが必要 (§6.4.6)。マジック必須にしているのはこのため。
// ---------------------------------------------------------------------------
static bool is_bridge_reboot_bl_req(const uint8_t *m, int n) {
    return n >= 5 && m[0] == 0xF0 && m[1] == SYSEX_MFR_ID &&
           m[2] == SYSEX_SUB_ID && m[3] == CMD_BRIDGE_REBOOT_BL;
}

static void enter_download_mode(void *arg) {
    (void)arg;
#if defined(CONFIG_IDF_TARGET_ESP32C6)
    REG_SET_BIT(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT);
#else
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
#endif
    // esp_restart() ではなく ROM の system reset を使う。esp_restart() は
    // ペリフェラル/RTC 周りのリセットを伴い、ROM がブートモードを読む前に
    // FORCE_DOWNLOAD_BOOT が消えてしまう (実機 S3 で確認: コマンドは届いて
    // 再起動もしたが通常モードで復帰した)。
    esp_rom_software_reset_system();
}

/// ACK を返してから約 300ms 後に再起動する。ACK を送出しきる時間を確保するため
/// 即時には落とさない (とくに BLE は次の接続イベントを待つ必要がある)。
static void handle_bridge_reboot_bl(const uint8_t *m, int n,
                                    void (*send)(const uint8_t *, int)) {
    const uint8_t req_id = (n >= 6) ? m[4] : 0x00;
    const bool ok = (n >= 9) && m[5] == REBOOT_BL_MAGIC0 &&
                    m[6] == REBOOT_BL_MAGIC1 && m[7] == REBOOT_BL_MAGIC2;

    uint8_t ack[8];
    int i = 0;
    ack[i++] = 0xF0;
    ack[i++] = SYSEX_MFR_ID;
    ack[i++] = SYSEX_SUB_ID;
    ack[i++] = CMD_ACK;
    ack[i++] = req_id;
    ack[i++] = ok ? ACK_OK : ACK_INVALID_VALUE;
    ack[i++] = CMD_BRIDGE_REBOOT_BL;
    ack[i++] = 0xF7;
    send(ack, i);

    if (!ok) {
        ESP_LOGW(TAG, "BRIDGE_REBOOT_BOOTLOADER: bad magic -> ignored");
        return;
    }
    ESP_LOGW(TAG, "BRIDGE_REBOOT_BOOTLOADER: entering download mode in 300ms");
    const esp_timer_create_args_t targs = {
        .callback = enter_download_mode,
        .name     = "reboot_bl",
    };
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK) {
        esp_timer_start_once(t, 300 * 1000);
    } else {
        enter_download_mode(NULL);   // タイマーが取れなければ即時
    }
}

static void on_ble_rx(const uint8_t *msg, int len) {
    if (is_bridge_identify_req(msg, len)) {
        ESP_LOGI(TAG, "BRIDGE_IDENTIFY_REQUEST (BLE) -> self-answer");
        send_bridge_identify_response(TRANSPORT_BLE, ble_midi_notify);
        return;
    }
    if (is_bridge_reboot_bl_req(msg, len)) {
        handle_bridge_reboot_bl(msg, len, ble_midi_notify);
        return;
    }
    i2c_bridge_write(msg, len);   // それ以外は CH32 へ中継
}

#if defined(BOARD_HAS_USB_MIDI)
// 中継: PC(USB-MIDI) → CH32(I2C)。BLE と対称。
static void on_usb_rx(const uint8_t *msg, int len) {
    if (is_bridge_identify_req(msg, len)) {
        ESP_LOGI(TAG, "BRIDGE_IDENTIFY_REQUEST (USB) -> self-answer");
        send_bridge_identify_response(TRANSPORT_USB, usb_midi_bridge_notify);
        return;
    }
    if (is_bridge_reboot_bl_req(msg, len)) {
        handle_bridge_reboot_bl(msg, len, usb_midi_bridge_notify);
        return;
    }
    i2c_bridge_write(msg, len);
}
#endif

// ---------------------------------------------------------------------------
// 中継: CH32(I2C+INT) → phone(BLE) / PC(USB)
//   device→host 通知は接続中の全トランスポートへ送る (CH32 の USB+I2C 並走と
//   同じ方針)。未接続側は各 notify 内で捨てられる。
// ---------------------------------------------------------------------------
static void on_i2c_rx(const uint8_t *midi, int len) {
    ble_midi_notify(midi, len);
#if defined(BOARD_HAS_USB_MIDI)
    usb_midi_bridge_notify(midi, len);
#endif
}

// 切断時、CH32 に DISCONNECT + RESET を送って HID を解放させる (押しっぱなし防止)。
// USB/BLE の両トランスポートがある構成では「最後のホストが居なくなったとき」だけ
// 解放する (もう一方がまだ接続中なら送らない)。
static void notify_ch32_disconnect(void) {
    static const uint8_t disc[] = { 0xF0, SYSEX_MFR_ID, SYSEX_SUB_ID, CMD_DISCONNECT, 0x00, 0xF7 };
    static const uint8_t rst[]  = { 0xF0, SYSEX_MFR_ID, SYSEX_SUB_ID, CMD_RESET,      0x00, 0xF7 };
    i2c_bridge_write(disc, sizeof(disc));
    i2c_bridge_write(rst, sizeof(rst));
}

static bool any_other_host_connected_than_ble(void) {
#if defined(BOARD_HAS_USB_MIDI)
    return usb_midi_bridge_mounted();
#else
    return false;
#endif
}

#if defined(BOARD_HAS_USB_MIDI)
// USB マウント状態の変化 (usb_midi_bridge の監視タスクから呼ばれる)。
static void on_usb_state(bool mounted) {
    if (!mounted && !g_ble_connected) {
        notify_ch32_disconnect();
    }
}
#endif

// ---------------------------------------------------------------------------
// 接続パラメータの実測ログ
//
// ble_gap_update_params() は「要求」でしかなく、セントラルが拒否しても
// エラーにはならない。とくに Apple 系ホストはアクセサリ設計ガイドラインの
// 条件 (Interval Min >= 15ms、Interval Max >= Interval Min + 15ms 等) を
// 満たさない要求を **丸ごと拒否** する挙動が知られており、その場合はホストが
// 選んだ既定値 (30ms 前後) のまま据え置かれる。
//
// macOS + BLE アダプタで HEART_BEAT の往復が平均 ~48ms かかっていた
// (USB 経由の同じ ESP32→I2C→CH32 経路は 1.6ms) ため、実際に合意された値を
// 出して確認する。conn_itvl は 1.25ms 単位、supervision_timeout は 10ms 単位。
// ---------------------------------------------------------------------------
static void log_conn_params(uint16_t conn_handle, const char *when) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        ESP_LOGW(TAG, "conn params (%s): ble_gap_conn_find failed", when);
        return;
    }
    // 1.25ms 単位 → ms を整数演算で (newlib nano だと %f が出ないため)
    unsigned itvl_x100 = (unsigned)desc.conn_itvl * 125u;
    ESP_LOGI(TAG,
             "conn params (%s): itvl=%u (%u.%02u ms) latency=%u timeout=%u (%u ms)",
             when, desc.conn_itvl, itvl_x100 / 100u, itvl_x100 % 100u,
             desc.conn_latency, desc.supervision_timeout,
             (unsigned)desc.supervision_timeout * 10u);
}

/// Apple 準拠の再要求を 1 接続につき 1 回だけ行うためのフラグ。
static bool s_conn_param_retried = false;

// ---------------------------------------------------------------------------
// GAP イベント
// ---------------------------------------------------------------------------
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "connected; conn_handle=%d", event->connect.conn_handle);
                g_ble_connected = true;
                s_conn_param_retried = false;
                ble_midi_set_conn(event->connect.conn_handle);
                // 要求前にホストが決めた値を出しておく (比較のため)
                log_conn_params(event->connect.conn_handle, "on connect");
                // レイテンシ低減のため connection interval を 7.5-15ms へ更新 (§2.3)。
                struct ble_gap_upd_params p = {
                    .itvl_min = 6,    // 6 * 1.25ms = 7.5ms
                    .itvl_max = 12,   // 12 * 1.25ms = 15ms
                    .latency  = 0,
                    .supervision_timeout = 400,  // 4s
                };
                int rc = ble_gap_update_params(event->connect.conn_handle, &p);
                ESP_LOGI(TAG, "update_params request (7.5-15ms) rc=%d", rc);
            } else {
                ESP_LOGW(TAG, "connect failed; status=%d", event->connect.status);
                start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnect; reason=%d", event->disconnect.reason);
            g_ble_connected = false;
            ble_midi_set_conn(BLE_HS_CONN_HANDLE_NONE);
            if (!any_other_host_connected_than_ble()) {
                notify_ch32_disconnect();   // 最後のホストが消えた → CH32 の HID を解放
            }
            start_advertising();
            return 0;

        case BLE_GAP_EVENT_CONN_UPDATE:
            // 接続パラメータが実際に変わったときに来る (要求が拒否された場合は
            // 来ないか、status != 0 で来る)。合意された値をそのまま出す。
            ESP_LOGI(TAG, "conn update; status=%d", event->conn_update.status);
            log_conn_params(event->conn_update.conn_handle, "after update");
            // 7.5-15ms の要求が通らなかった (= 15ms より粗いまま) 場合は、
            // Apple のガイドラインを満たす 15-30ms で 1 回だけ再要求して、
            // 「準拠した要求なら受け入れられるのか」を切り分ける。
            if (!s_conn_param_retried) {
                struct ble_gap_conn_desc d;
                if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0 &&
                    d.conn_itvl > 12) {
                    s_conn_param_retried = true;
                    struct ble_gap_upd_params q = {
                        .itvl_min = 12,   // 15ms   (Apple: Interval Min >= 15ms)
                        .itvl_max = 24,   // 30ms   (Apple: Max >= Min + 15ms)
                        .latency  = 0,
                        .supervision_timeout = 400,  // 4s (Apple: <= 6s)
                    };
                    int rc2 = ble_gap_update_params(event->conn_update.conn_handle, &q);
                    ESP_LOGI(TAG, "update_params retry (15-30ms) rc=%d", rc2);
                }
            }
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

        case BLE_GAP_EVENT_ENC_CHANGE:
            // ペアリング/暗号化の成立。status==0 で成功。
            ESP_LOGI(TAG, "encryption change; status=%d", event->enc_change.status);
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // 相手 (Windows) がボンドを削除して再ペアリングしてきたのに、ESP32 側に
            // 古いボンドが残っているケース。古いボンドを消して再ペアリングを許可する。
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        case BLE_GAP_EVENT_PASSKEY_ACTION:
            // Just Works では PIN 入力不要。何もしない。
            ESP_LOGI(TAG, "passkey action; action=%d", event->passkey.params.action);
            return 0;

        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// アドバタイズ: MIDI サービス UUID を広告し、名前は scan response に載せる
//   (128bit UUID(18B) + flags(3B) で adv が埋まるため名前は分離する)
// ---------------------------------------------------------------------------
// adv_start は接続確立失敗 (BLE_GAP_EVENT_CONNECT status!=0) の直後だと、失敗した
// 接続のスロットがまだ解放されておらず BLE_HS_ENOMEM で失敗することがある
// (MAX_CONNECTIONS=1)。そのまま諦めるとリセットまで誰からも見えなくなるので、
// 失敗時は 500ms 後にリトライする。
static void adv_retry_cb(void *arg) {
    (void)arg;
    start_advertising();
}

static void schedule_adv_retry(void) {
    static esp_timer_handle_t s_adv_retry_timer;
    if (s_adv_retry_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = adv_retry_cb,
            .name = "adv_retry",
        };
        if (esp_timer_create(&args, &s_adv_retry_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_adv_retry_timer);
    esp_timer_start_once(s_adv_retry_timer, 500 * 1000);   // 500ms
}

static void start_advertising(void) {
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    struct ble_gap_adv_params adv_params;
    int rc;

    // プライマリ広告に flags + 128bit サービス UUID + デバイス名 を載せる。
    // 名前を scan response だけに置くと、Windows (universal_ble のパッシブスキャン) が
    // 名前を拾えず result.name==null でスキップされ発見できない。31 バイトに収まる
    // (flags 3 + UUID128 18 + name "MimicX" 8 = 29) ので名前もプライマリへ入れる。
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&ble_midi_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;

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
    if (rc == BLE_HS_EALREADY) return;   // 既に広告中
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d (retrying in 500ms)", rc);
        schedule_adv_retry();
        return;
    }
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
    board_early_init();   // アンテナ等のボード固有初期化 (C6)。無線起動より前に行う。

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

    // BLE ペアリング (Just Works + bonding)。Windows は BLE-MIDI 利用にペアリングを
    // 要求するため必須。iOS/macOS/Android はペアリングせず接続するので影響なし。
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;   // 入出力なし → Just Works
    ble_hs_cfg.sm_bonding = 1;                           // ボンド (鍵を保存)
    ble_hs_cfg.sm_sc = 1;                                // LE Secure Connections
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();                             // ボンドを NVS に永続化

    // I2C ブリッジ (CH32 デバイスエンジンへの橋渡し) を起動。
    // BT コントローラの割り込み確保と競合しないよう NimBLE 初期化の後に行う。
    i2c_bridge_init(on_i2c_rx);

    // 起動時 CH32 ファーム自動更新: 内蔵イメージが新しければ SWD で書き換える。
    // (更新時は内部で I2C ブリッジを再 init して戻る)
    ch32_ota_check_and_update(on_i2c_rx);

    // BLE 受信 → 中継ハンドラを登録 (i2c_bridge_init 完了後)。
    ble_midi_set_rx_handler(on_ble_rx);

#if defined(BOARD_HAS_USB_MIDI)
    // USB-MIDI トランスポート (S3 のみ)。BLE と併走し、同じ CH32 へ中継する。
    usb_midi_bridge_init(on_usb_rx, on_usb_state);
#endif

    char ser[16];
    build_serial16(ser);
    ESP_LOGI(TAG, "MimicX-ESP32 bridge proto %d.%d fw %d.%d.%d serial=%.16s",
             BRIDGE_PROTO_MAJOR, BRIDGE_PROTO_MINOR,
             BRIDGE_FW_MAJOR, BRIDGE_FW_MINOR, BRIDGE_FW_PATCH, ser);

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "MimicX-ESP32 BLE-MIDI <-> I2C bridge started");
}
