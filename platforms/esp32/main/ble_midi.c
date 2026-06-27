// ===================================================================================
// ble_midi.c  (ESP32 / BLE-MIDI スケルトン)
// ===================================================================================
// 標準 BLE-MIDI GATT サービスのトランスポート実装。詳細は ble_midi.h を参照。
// プロトコル §2.3 (BLE-MIDI バインディング) に従う。
// ===================================================================================
#include "ble_midi.h"
#include "mimicx_proto.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"

static const char *TAG = "ble_midi";

// ---------------------------------------------------------------------------
// UUID (NimBLE は 128bit UUID をリトルエンディアン byte 配列で受け取る)
//   service        : 03B80E5A-EDE8-4B33-A751-6CE34EC4C700
//   characteristic : 7772E5DB-3868-4112-A1A9-F2669D106BF3
// ---------------------------------------------------------------------------
const ble_uuid128_t ble_midi_svc_uuid = BLE_UUID128_INIT(
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
    0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);

static const ble_uuid128_t midi_chr_uuid = BLE_UUID128_INIT(
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1,
    0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

static uint16_t g_conn       = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_val_handle = 0;

// ---------------------------------------------------------------------------
// 受信: BLE-MIDI パケット → 素の MIDI バイト列 → SysEx 再結合
// ---------------------------------------------------------------------------
static uint8_t sx_buf[256];
static int     sx_len;
static bool    in_sysex;

// チャンネルボイスメッセージ用パーサ状態 (MIDI running status 対応)
static uint8_t ch_status = 0;   // 現在のチャンネル status (0 = 未確定)
static uint8_t ch_data[2];
static int     ch_idx = 0;
static int     ch_len = 0;      // 期待データバイト数

static int channel_data_len(uint8_t status) {
    switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
        case 0xC0: case 0xD0: return 1;
        default: return 0;
    }
}

static void feed_midi(uint8_t b) {
    if (b == 0xF0) {                       // SysEx 開始
        in_sysex = true;
        sx_len = 0;
        sx_buf[sx_len++] = b;
        ch_status = 0;                     // running status をクリア
        return;
    }
    if (b == 0xF7) {                       // SysEx 終了
        if (in_sysex && sx_len < (int)sizeof(sx_buf)) {
            sx_buf[sx_len++] = b;
            mimicx_proto_handle_sysex(sx_buf, sx_len);
        }
        in_sysex = false;
        return;
    }
    if (b & 0x80) {                        // ステータスバイト
        in_sysex = false;
        if (b >= 0xF8) return;             // System Real-Time は無視 (running status 維持)
        int len = channel_data_len(b);
        if (len > 0) {                     // チャンネルボイスメッセージ
            ch_status = b;
            ch_idx = 0;
            ch_len = len;
        } else {                           // System Common 等は未対応
            ch_status = 0;
        }
        return;
    }
    // データバイト
    if (in_sysex) {
        if (sx_len < (int)sizeof(sx_buf)) sx_buf[sx_len++] = b;
        return;
    }
    if (ch_status == 0) return;            // status 未確定のデータは捨てる
    ch_data[ch_idx++] = b;
    if (ch_idx >= ch_len) {
        mimicx_proto_handle_channel(ch_status, ch_data[0], ch_len > 1 ? ch_data[1] : 0);
        ch_idx = 0;                        // running status: 同 status の連続に対応
    }
}

void ble_midi_rx(const uint8_t *data, int len) {
    if (len < 1) return;
    // data[0] = packet header (timestampHigh) — 値は使わない。
    //
    // BLE-MIDI は MIDI バイト列の中に timestampLow (MSB=1) を挟む形式。各 status /
    // F0 / F7 の直前に timestampLow が入る。MSB=1 のバイトは timestampLow とみなして
    // 読み飛ばし、その直後のバイト (status/F0/F7) と、それ以降の MSB=0 データバイトは
    // すべて feed_midi に渡す。SysEx / チャンネルメッセージの区別は feed_midi が
    // 内部状態 (in_sysex / running status) で行う。
    int i = 1;
    while (i < len) {
        uint8_t b = data[i];
        if (b & 0x80) {
            // timestampLow → 読み飛ばし、直後のバイト (status/F0/F7) を渡す。
            i++;
            if (i < len) { feed_midi(data[i]); i++; }
        } else {
            // データバイト (SysEx ペイロード / note / velocity / CC 値)。
            feed_midi(b);
            i++;
        }
    }
}

// ---------------------------------------------------------------------------
// 送信: 完成 SysEx を BLE-MIDI パケットに分割して notify
//   1 パケット = [header] ([tsLow] F0 data... | data... | data [tsLow] F7)
//   SysEx の中身は全て 7bit (MSB=0) なので、tsLow は先頭 F0 と末尾 F7 の前だけ付く。
// ---------------------------------------------------------------------------
void ble_midi_send_sysex(const uint8_t *sx, int n) {
    if (g_conn == BLE_HS_CONN_HANDLE_NONE || g_val_handle == 0) return;

    int mtu = ble_att_mtu(g_conn);
    int payload = (mtu > 3) ? (mtu - 3) : 20;   // ATT notify ペイロード
    if (payload < 5)   payload = 5;
    if (payload > 240) payload = 240;

    uint16_t ts = (uint16_t)(esp_log_timestamp() & 0x1FFF);
    uint8_t hi = 0x80 | (uint8_t)(ts >> 7);
    uint8_t lo = 0x80 | (uint8_t)(ts & 0x7F);

    int idx = 0;
    while (idx < n) {
        uint8_t pkt[256];
        int p = 0;
        pkt[p++] = hi;   // packet header (timestampHigh)
        while (idx < n && p < payload) {
            uint8_t b = sx[idx];
            bool needs_ts = (idx == 0) || (b == 0xF7);   // F0(先頭) / F7(末尾) の前に tsLow
            if (needs_ts) {
                if (p + 2 > payload) break;              // tsLow + b の空きが要る
                pkt[p++] = lo;
                pkt[p++] = b;
                idx++;
            } else {
                pkt[p++] = b;
                idx++;
            }
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, p);
        if (om == NULL) {
            ESP_LOGW(TAG, "mbuf alloc failed");
            return;
        }
        int rc = ble_gatts_notify_custom(g_conn, g_val_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify rc=%d", rc);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// GATT 定義
// ---------------------------------------------------------------------------
static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            uint8_t buf[256];
            uint16_t outlen = 0;
            int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &outlen);
            if (rc == 0 && outlen > 0) {
                ble_midi_rx(buf, outlen);
            }
            return 0;
        }
        case BLE_GATT_ACCESS_OP_READ_CHR:
            // 空 (BLE-MIDI 仕様では read 時は空 packet を返す)
            return 0;
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &ble_midi_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid       = &midi_chr_uuid.u,
                .access_cb  = gatt_access_cb,
                .flags      = BLE_GATT_CHR_F_READ |
                              BLE_GATT_CHR_F_WRITE_NO_RSP |
                              BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &g_val_handle,
            },
            { 0 }
        },
    },
    { 0 }
};

int ble_midi_register_gatt(void) {
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) return rc;
    return ble_gatts_add_svcs(gatt_svcs);
}

void ble_midi_set_conn(uint16_t conn_handle) {
    g_conn = conn_handle;
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        in_sysex = false;
        sx_len = 0;
    }
}

uint16_t ble_midi_val_handle(void) {
    return g_val_handle;
}
