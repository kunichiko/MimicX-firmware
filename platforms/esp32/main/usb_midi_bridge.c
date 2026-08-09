// ===================================================================================
// usb_midi_bridge.c  (ESP32-S3 / USB-MIDI デバイス トランスポート)
// ===================================================================================
// 実装の詳細は usb_midi_bridge.h を参照。esp_tinyusb (managed component) を使い、
// USB-MIDI クラス 1 インターフェースのデバイスとして enumerate する。
//
// 受信経路:
//   tud_midi_rx_cb (TinyUSB タスク文脈) → tud_midi_stream_read で素の MIDI バイト列
//   → feed_midi (SysEx 再結合 / running status。ble_midi.c の同名ロジックと同一契約)
//   → s_on_rx (main.c が CH32 への I2C 転送 / BRIDGE_IDENTIFY 自答を実装)
//
// 送信経路:
//   usb_midi_bridge_notify → tud_midi_stream_write (USB-MIDI パケット分割は TinyUSB 任せ)
//
// マウント状態は専用タスクで tud_ready() をポーリングしてエッジ検出する
// (tud_mount_cb/tud_umount_cb は esp_tinyusb 側と定義が衝突する可能性があるため使わない)。
// ===================================================================================
#include "board_config.h"

#if defined(BOARD_HAS_USB_MIDI)

#include "usb_midi_bridge.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tusb.h"

static const char *TAG = "usb_midi";

static void (*s_on_rx)(const uint8_t *msg, int len);
static void (*s_on_state)(bool mounted);

// ---------------------------------------------------------------------------
// USB ディスクリプタ
// ---------------------------------------------------------------------------
// VID/PID: Espressif の開発用 VID 0x303A を使用 (esp_tinyusb 既定と同じ流儀)。
// PID は MimicX ブリッジ用に 0x8000 台の任意値。製品化時は要検討。
#define USB_VID   0x303A
#define USB_PID   0x82A0
#define EPNUM_MIDI_OUT  0x01
#define EPNUM_MIDI_IN   0x81

enum { ITF_NUM_MIDI = 0, ITF_NUM_MIDI_STREAMING, ITF_NUM_TOTAL };

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

static const tusb_desc_device_t midi_device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,          // クラスはインターフェース側で定義
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t midi_config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 4, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
};

// iSerial: ESP32 base MAC から "mimicx-bridge-XXXXXXXXXXXX" を組み立てる。
// (デバイスの同一性は CH32 の IDENTIFY (Chip UID) が正。これはブリッジの識別用)
static char s_serial_str[32];

static const char *string_descriptor[] = {
    (const char[]){0x09, 0x04},   // 0: 言語 = English (0x0409)
    "Kunihiko Ohnaka",            // 1: Manufacturer
    "Mimic X (Bridge)",           // 2: Product
    s_serial_str,                 // 3: Serial
    "MimicX MIDI",                // 4: MIDI インターフェース名
};

// ---------------------------------------------------------------------------
// 受信: USB-MIDI ストリーム → 完成 MIDI メッセージ (SysEx / channel)
//   tud_midi_stream_read はイベントパケットを素の MIDI バイト列へ復元して返す。
//   その先の SysEx 再結合 / running status 処理は ble_midi.c の feed_midi と
//   同一契約 (完成メッセージ単位でハンドラへ)。
// ---------------------------------------------------------------------------
static uint8_t sx_buf[256];
static int     sx_len;
static bool    in_sysex;

static uint8_t ch_status = 0;
static uint8_t ch_data[2];
static int     ch_idx = 0;
static int     ch_len = 0;

static int channel_data_len(uint8_t status) {
    switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
        case 0xC0: case 0xD0: return 1;
        default: return 0;
    }
}

static void feed_midi(uint8_t b) {
    if (b == 0xF0) {
        in_sysex = true;
        sx_len = 0;
        sx_buf[sx_len++] = b;
        ch_status = 0;
        return;
    }
    if (b == 0xF7) {
        if (in_sysex && sx_len < (int)sizeof(sx_buf)) {
            sx_buf[sx_len++] = b;
            if (s_on_rx) s_on_rx(sx_buf, sx_len);
        }
        in_sysex = false;
        return;
    }
    if (b & 0x80) {
        in_sysex = false;
        if (b >= 0xF8) return;             // System Real-Time は無視
        int len = channel_data_len(b);
        if (len > 0) {
            ch_status = b;
            ch_idx = 0;
            ch_len = len;
        } else {
            ch_status = 0;
        }
        return;
    }
    if (in_sysex) {
        if (sx_len < (int)sizeof(sx_buf)) sx_buf[sx_len++] = b;
        return;
    }
    if (ch_status == 0) return;
    ch_data[ch_idx++] = b;
    if (ch_idx >= ch_len) {
        uint8_t msg[3];
        msg[0] = ch_status;
        msg[1] = ch_data[0];
        int mlen = 2;
        if (ch_len > 1) msg[2] = ch_data[1], mlen = 3;
        if (s_on_rx) s_on_rx(msg, mlen);
        ch_idx = 0;
    }
}

// TinyUSB 弱シンボル: MIDI OUT エンドポイントにデータが届いたら呼ばれる
// (TinyUSB デバイスタスク文脈)。i2c_bridge_write はミューテックスで短時間
// ブロックし得るが、USB タスクの一時停止は実害がないためここで直接処理する。
void tud_midi_rx_cb(uint8_t itf) {
    (void)itf;
    uint8_t buf[64];
    uint32_t n;
    while ((n = tud_midi_stream_read(buf, sizeof(buf))) > 0) {
        for (uint32_t i = 0; i < n; i++) feed_midi(buf[i]);
    }
}

// ---------------------------------------------------------------------------
// マウント状態監視 (エッジ検出 → main.c へ通知)
// ---------------------------------------------------------------------------
static void state_task(void *arg) {
    (void)arg;
    bool last = false;
    for (;;) {
        bool now = tud_ready();
        if (now != last) {
            ESP_LOGI(TAG, "USB %s", now ? "mounted" : "unmounted");
            if (s_on_state) s_on_state(now);
            last = now;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------------------------------------------------------------------------
// 公開 API
// ---------------------------------------------------------------------------
bool usb_midi_bridge_mounted(void) {
    return tud_ready();
}

void usb_midi_bridge_teardown(void) {
    // tinyusb_driver_uninstall は「USB イベントタスク停止 → TinyUSB スタック
    // 破棄 → ディスクリプタ資源解放 → **USB PHY 削除**」まで行う。PHY を削除
    // させるのが肝で、これによりホストには切断が伝わり、かつ ROM が自分で
    // PHY を構成し直せる (§6.4.6)。
    esp_err_t e = tinyusb_driver_uninstall();
    if (e != ESP_OK) ESP_LOGW(TAG, "tinyusb_driver_uninstall rc=%d", (int)e);
}

void usb_midi_bridge_notify(const uint8_t *msg, int n) {
    if (n <= 0 || !tud_ready()) return;
    // tud_midi_stream_write が USB-MIDI イベントパケットへの分割 (SysEx の
    // 3 バイト詰め等) を行う。FIFO 満杯時は書けた分だけ返るのでリトライする。
    int off = 0;
    int guard = 0;
    while (off < n && guard++ < 100) {
        uint32_t w = tud_midi_stream_write(0, msg + off, (uint32_t)(n - off));
        off += (int)w;
        if (w == 0) vTaskDelay(pdMS_TO_TICKS(1));   // FIFO 空き待ち
    }
    if (off < n) ESP_LOGW(TAG, "midi write dropped %d/%d bytes", n - off, n);
}

void usb_midi_bridge_init(void (*on_rx)(const uint8_t *msg, int len),
                          void (*on_state)(bool mounted)) {
    s_on_rx = on_rx;
    s_on_state = on_state;

    // iSerial を base MAC から組み立てる
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_serial_str, sizeof(s_serial_str), "mimicx-bridge-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const tinyusb_config_t cfg = {
        .device_descriptor        = &midi_device_descriptor,
        .string_descriptor        = string_descriptor,
        .string_descriptor_count  = sizeof(string_descriptor) / sizeof(string_descriptor[0]),
        .external_phy             = false,
        .configuration_descriptor = midi_config_descriptor,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));

    xTaskCreate(state_task, "usb_state", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "USB-MIDI device started (VID=%04X PID=%04X serial=%s)",
             USB_VID, USB_PID, s_serial_str);
}

#endif // BOARD_HAS_USB_MIDI
