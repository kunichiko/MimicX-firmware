// ===================================================================================
// mimicx_proto.c  (ESP32 / BLE-MIDI スケルトン)
// ===================================================================================
// CH32 ファーム (src/main.c) の SysEx 処理のうち、無線接続の疎通確認に必要な分だけを
// 移植した最小実装。デバイス制御 (joystick / x68k) は未実装で、Note/CC は無視する。
//
// 目的: iOS / macOS アプリが BLE 経由で IDENTIFY → HEART_BEAT まで完走できることの検証。
// ===================================================================================
#include "mimicx_proto.h"
#include "ble_midi.h"

#include <string.h>
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "mimicx_proto";

// ---------------------------------------------------------------------------
// プロトコル定数 (CH32 src/main.c と一致させること)
// ---------------------------------------------------------------------------
#define PROTOCOL_VERSION_MAJOR  0
#define PROTOCOL_VERSION_MINOR  8   // 0.8: トランスポート非依存化 + BLE-MIDI バインディング
#define FW_VERSION_MAJOR        0
#define FW_VERSION_MINOR        1
#define FW_VERSION_PATCH        0

#define SYSEX_MFR_ID       0x7D
#define SYSEX_SUB_ID       0x01
#define CMD_IDENTIFY_REQ   0x01
#define CMD_IDENTIFY_RSP   0x02
#define CMD_CAPABILITY_REQ 0x03
#define CMD_CAPABILITY_RSP 0x04
#define CMD_ACK            0x06
#define CMD_EMIT_REMOTE    0x07
#define CMD_HEART_BEAT     0x08
#define CMD_DISCONNECT     0x09
#define CMD_SET_CONFIG     0x10
#define CMD_SET_LED        0x20
#define CMD_SET_LED_BLINK  0x21
#define CMD_RESET          0x7F

// ACK status (プロトコル §6.1.3)
#define ACK_STATUS_OK           0x00
#define ACK_STATUS_UNKNOWN_CMD  0x01

// HID type / target (プロトコル §6.4.2 / §6.4.3、hid_function.h と一致)
#define HID_TYPE_JOYSTICK  0x02
#define TARGET_ATARI       0x01

// ---------------------------------------------------------------------------
// serial[16]: ESP32 base MAC (6 byte) を 8 byte に 0 埋めして 16 ASCII hex 化。
// CH32 は Chip UID(8 byte) を使うが、長さ・形式 (16 文字 uppercase hex) は共通。
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
// 応答ビルダ (CH32 src/main.c の send_* と同じバイト列)
// ---------------------------------------------------------------------------
static void send_identify_response(void) {
    uint8_t rsp[64];
    int i = 0;
    rsp[i++] = 0xF0;
    rsp[i++] = SYSEX_MFR_ID;
    rsp[i++] = SYSEX_SUB_ID;
    rsp[i++] = CMD_IDENTIFY_RSP;
    rsp[i++] = PROTOCOL_VERSION_MAJOR;
    rsp[i++] = PROTOCOL_VERSION_MINOR;
    rsp[i++] = FW_VERSION_MAJOR;
    rsp[i++] = FW_VERSION_MINOR;
    rsp[i++] = FW_VERSION_PATCH;
    // チャンネルマップ: <num_channels> <ch> <type> <target>
    // スケルトンは joystick(ATARI) を 1ch 申告し、アプリにジョイスティック UI を出させる。
    rsp[i++] = 1;
    rsp[i++] = 0;                 // MIDI channel 0
    rsp[i++] = HID_TYPE_JOYSTICK;
    rsp[i++] = TARGET_ATARI;
    // serial[16]
    char ser[16];
    build_serial16(ser);
    memcpy(&rsp[i], ser, 16);
    i += 16;
    // device name (ASCII)
    static const char name[] = "MimicX-ESP32";
    for (int j = 0; name[j] && i < (int)sizeof(rsp) - 1; j++) {
        rsp[i++] = (uint8_t)(name[j] & 0x7F);
    }
    rsp[i++] = 0xF7;
    ble_midi_send_sysex(rsp, i);
}

static void send_capability_response(uint8_t req_id, uint8_t status) {
    // スケルトンは TLV を持たない (空 capability)。
    uint8_t rsp[] = {
        0xF0, SYSEX_MFR_ID, SYSEX_SUB_ID, CMD_CAPABILITY_RSP,
        (uint8_t)(req_id & 0x7F), (uint8_t)(status & 0x7F),
        0xF7,
    };
    ble_midi_send_sysex(rsp, sizeof(rsp));
}

static void send_ack(uint8_t req_id, uint8_t status, uint8_t orig_cmd) {
    uint8_t rsp[] = {
        0xF0, SYSEX_MFR_ID, SYSEX_SUB_ID, CMD_ACK,
        (uint8_t)(req_id & 0x7F),
        (uint8_t)(status & 0x7F),
        (uint8_t)(orig_cmd & 0x7F),
        0xF7,
    };
    ble_midi_send_sysex(rsp, sizeof(rsp));
}

// ---------------------------------------------------------------------------
// SysEx ディスパッチ
// ---------------------------------------------------------------------------
void mimicx_proto_handle_sysex(const uint8_t *sx, int len) {
    if (len < 5) return;
    if (sx[0] != 0xF0 || sx[1] != SYSEX_MFR_ID || sx[2] != SYSEX_SUB_ID) return;

    uint8_t cmd = sx[3];
    // 0.5+ コマンドは sx[4] が req_id。IDENTIFY_REQ のみ req_id を持たない (sx[4]=0xF7)。
    uint8_t req_id = sx[4];

    switch (cmd) {
        case CMD_IDENTIFY_REQ:
            ESP_LOGI(TAG, "IDENTIFY_REQUEST");
            send_identify_response();
            break;
        case CMD_CAPABILITY_REQ:
            send_capability_response(req_id, ACK_STATUS_OK);
            break;
        case CMD_HEART_BEAT:
            send_ack(req_id, ACK_STATUS_OK, cmd);
            break;
        case CMD_DISCONNECT:
        case CMD_SET_CONFIG:
        case CMD_SET_LED:
        case CMD_SET_LED_BLINK:
        case CMD_RESET:
            // スケルトンでは no-op。OK を返してアプリのフローを進める。
            send_ack(req_id, ACK_STATUS_OK, cmd);
            break;
        case CMD_EMIT_REMOTE:
        default:
            send_ack(req_id, ACK_STATUS_UNKNOWN_CMD, cmd);
            break;
    }
}

void mimicx_proto_init(void) {
    char ser[16];
    build_serial16(ser);
    ESP_LOGI(TAG, "MimicX-ESP32 proto %d.%d fw %d.%d.%d serial=%.16s",
             PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR,
             FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH, ser);
}
