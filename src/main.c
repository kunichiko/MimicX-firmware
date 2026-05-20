// ===================================================================================
// Project:  Mimic X (MimicX-firmware)
// Author:   Kunihiko Ohnaka (@kunichiko)
// Year:     2026
// URL:      https://github.com/kunichiko/MimicX-firmware
// ===================================================================================
//
// USB-MIDI 経由でスマートフォンから制御コマンドを受信し、
// レトロ PC の HID デバイス（キーボード・ジョイスティック・マウス等）を模倣する。
//
// 本ファイルはコア処理のみ。各 HID 機能は functions/<name>/ 配下のモジュールが
// hid_function_t を export し、board_config.h で有効化したものが
// hid_dispatcher 経由で呼び出される。
//
// プロトコル仕様: https://github.com/kunichiko/MimicX-protocol
// ===================================================================================

#include <stdio.h>
#include <string.h>

#include "ch32fun.h"
#include "funconfig.h"
#include "usb_midi.h"
#include "hid_dispatcher.h"
#include "board_config.h"

// ---------------------------------------------------------------------------
// プロトコル定数 (MimicX-protocol v0.5.0)
// ---------------------------------------------------------------------------

#define PROTOCOL_VERSION_MAJOR  0
#define PROTOCOL_VERSION_MINOR  5
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  6
#define FW_VERSION_PATCH  0

// MIDI チャンネル (デバイス→ホスト 通知用)
#define MIDI_CH_STATUS    15

// SysEx コマンド
#define SYSEX_MFR_ID      0x7D
#define SYSEX_SUB_ID      0x01
#define CMD_IDENTIFY_REQ  0x01
#define CMD_IDENTIFY_RSP  0x02
#define CMD_CAPABILITY_REQ 0x03
#define CMD_CAPABILITY_RSP 0x04
#define CMD_ACK           0x06
#define CMD_SET_CONFIG    0x10
#define CMD_RESET         0x7F

// ---------------------------------------------------------------------------
// デバッグ LED (PB3, PB4)
// ---------------------------------------------------------------------------

#define DEBUG_LED0_PIN  3
#define DEBUG_LED1_PIN  4

static void gpio_init_debug_leds(void) {
    GPIOB->CFGLR &= ~(0xf << (4 * DEBUG_LED0_PIN));
    GPIOB->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP) << (4 * DEBUG_LED0_PIN);
    GPIOB->BSHR = (1 << DEBUG_LED0_PIN);
    GPIOB->CFGLR &= ~(0xf << (4 * DEBUG_LED1_PIN));
    GPIOB->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP) << (4 * DEBUG_LED1_PIN);
    GPIOB->BSHR = (1 << DEBUG_LED1_PIN);
}

static void debug_led_set(uint8_t led, uint8_t on) {
    uint8_t pin = (led == 0) ? DEBUG_LED0_PIN : DEBUG_LED1_PIN;
    if (led > 1) return;
    if (on) GPIOB->BCR = (1 << pin);
    else    GPIOB->BSHR = (1 << pin);
}

// ---------------------------------------------------------------------------
// SysEx 処理
// ---------------------------------------------------------------------------

static uint8_t sysex_buf[64];
static uint8_t sysex_len;
static uint8_t sysex_receiving;

static void sysex_reset(void) { sysex_len = 0; sysex_receiving = 0; }

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
    // チャンネルマップ: <num_channels> <ch_0> <type_0> <target_0> ...
    i += hid_dispatch_build_channel_map(rsp + i, sizeof(rsp) - i - 1);
    // ボード名 (BOARD_NAME)
    static const char board_name[] = BOARD_NAME;
    for (int j = 0; board_name[j] && i < (int)sizeof(rsp) - 1; j++) {
        rsp[i++] = board_name[j] & 0x7F;
    }
    rsp[i++] = 0xF7;
    usb_midi_send_sysex(rsp, i);
}

static void send_capability_response(uint8_t req_id, uint8_t status) {
    uint8_t rsp[64];
    int i = 0;
    rsp[i++] = 0xF0;
    rsp[i++] = SYSEX_MFR_ID;
    rsp[i++] = SYSEX_SUB_ID;
    rsp[i++] = CMD_CAPABILITY_RSP;
    rsp[i++] = req_id & 0x7F;
    rsp[i++] = status & 0x7F;
    i += hid_dispatch_build_capabilities(rsp + i, sizeof(rsp) - i - 1);
    rsp[i++] = 0xF7;
    usb_midi_send_sysex(rsp, i);
}

// 汎用 ACK (専用レスポンスを持たないコマンド用)
//   F0 7D 01 06 <req_id> <status> <orig_cmd> F7
static void send_ack(uint8_t req_id, uint8_t status, uint8_t orig_cmd) {
    uint8_t rsp[] = {
        0xF0, SYSEX_MFR_ID, SYSEX_SUB_ID, CMD_ACK,
        (uint8_t)(req_id & 0x7F),
        (uint8_t)(status & 0x7F),
        (uint8_t)(orig_cmd & 0x7F),
        0xF7,
    };
    usb_midi_send_sysex(rsp, sizeof(rsp));
}

static void process_sysex(const uint8_t* data, int len) {
    if (len < 5) return;
    if (data[0] != 0xF0 || data[len - 1] != 0xF7) return;
    if (data[1] != SYSEX_MFR_ID || data[2] != SYSEX_SUB_ID) return;

    uint8_t cmd = data[3];
    switch (cmd) {
    case CMD_IDENTIFY_REQ:
        // ブートストラップ用途で req_id を持たない (プロトコル 6.3)
        send_identify_response();
        break;
    case CMD_CAPABILITY_REQ: {
        // F0 7D 01 03 <req_id> F7
        uint8_t req_id = (len >= 6) ? data[4] : 0;
        send_capability_response(req_id, ACK_STATUS_OK);
        break;
    }
    case CMD_SET_CONFIG: {
        // F0 7D 01 10 <req_id> <key> <value...> F7
        if (len < 7) {
            // 最低でも req_id + key の 2 byte が必要
            send_ack((len >= 6) ? data[4] : 0, ACK_STATUS_GENERIC_ERROR, cmd);
            break;
        }
        uint8_t req_id = data[4];
        uint8_t key = data[5];
        const uint8_t* val = data + 6;
        int val_len = len - 7;  // F0..key+F7 を除く
        uint8_t status = hid_dispatch_set_config(key, val, val_len);
        send_ack(req_id, status, cmd);
        break;
    }
    case CMD_RESET: {
        // F0 7D 01 7F <req_id> F7
        uint8_t req_id = (len >= 6) ? data[4] : 0;
        hid_dispatch_release_all();
        send_ack(req_id, ACK_STATUS_OK, cmd);
        break;
    }
    default: {
        // 未知のコマンド: req_id が含まれているなら ACK で返す
        uint8_t req_id = (len >= 6) ? data[4] : 0;
        send_ack(req_id, ACK_STATUS_UNKNOWN_CMD, cmd);
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// USB-MIDI メッセージ処理
// ---------------------------------------------------------------------------

static void process_midi_event(uint8_t cin, uint8_t midi0, uint8_t midi1, uint8_t midi2) {
    uint8_t channel = midi0 & 0x0F;

    // SysEx の組み立て
    switch (cin) {
    case CIN_SYSEX_START:
        if (!sysex_receiving) { sysex_reset(); sysex_receiving = 1; }
        if (sysex_len + 3 <= sizeof(sysex_buf)) {
            sysex_buf[sysex_len++] = midi0;
            sysex_buf[sysex_len++] = midi1;
            sysex_buf[sysex_len++] = midi2;
        }
        return;
    case CIN_SYSEX_END_1:
        if (sysex_receiving && sysex_len + 1 <= sizeof(sysex_buf)) {
            sysex_buf[sysex_len++] = midi0;
            process_sysex(sysex_buf, sysex_len);
        }
        sysex_reset();
        return;
    case CIN_SYSEX_END_2:
        if (sysex_receiving && sysex_len + 2 <= sizeof(sysex_buf)) {
            sysex_buf[sysex_len++] = midi0;
            sysex_buf[sysex_len++] = midi1;
            process_sysex(sysex_buf, sysex_len);
        }
        sysex_reset();
        return;
    case CIN_SYSEX_END_3:
        if (sysex_receiving && sysex_len + 3 <= sizeof(sysex_buf)) {
            sysex_buf[sysex_len++] = midi0;
            sysex_buf[sysex_len++] = midi1;
            sysex_buf[sysex_len++] = midi2;
            process_sysex(sysex_buf, sysex_len);
        }
        sysex_reset();
        return;
    }

    // 通常の MIDI メッセージ処理 → dispatcher に委譲
    switch (cin) {
    case CIN_NOTE_ON:
        if (channel == MIDI_CH_STATUS) {
            // デバッグ用: ステータスチャンネルの Note → LED
            debug_led_set(midi1, 1);
        } else {
            hid_dispatch_note_on(channel, midi1, midi2);
        }
        break;

    case CIN_NOTE_OFF:
        if (channel == MIDI_CH_STATUS) {
            debug_led_set(midi1, 0);
        } else {
            hid_dispatch_note_off(channel, midi1);
        }
        break;

    case CIN_CONTROL_CHANGE:
        hid_dispatch_cc(channel, midi1, midi2);
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// メインループ
// ---------------------------------------------------------------------------

int main() {
    SystemInit();
    RCC->CTLR |= RCC_HSION;

    // ポートクロック有効化
    RCC->APB2PCENR |= RCC_IOPAEN | RCC_IOPBEN | RCC_IOPCEN | RCC_AFIOEN;

    // 初期化
    gpio_init_debug_leds();
    hid_dispatch_init();
    usb_midi_init();

    uint8_t cin, midi0, midi1, midi2;

    while (1) {
        // USB-MIDI からコマンド受信・処理
        while (usb_midi_receive_event(&cin, &midi0, &midi1, &midi2) > 0) {
            process_midi_event(cin, midi0, midi1, midi2);
        }

        // 各 HID 機能の poll
        hid_dispatch_poll();

        // USB-MIDI TX バッファをフラッシュ
        usb_midi_poll();
    }
}
