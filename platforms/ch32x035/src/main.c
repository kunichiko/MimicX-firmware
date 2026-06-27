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
#include "status_led.h"
#ifdef MIMICX_I2C
#include "i2c_midi.h"
#endif

// EMIT_REMOTE (0x07) は x68k_keyboard を搭載する variant でのみ有効。
// 該当 variant でない場合は CMD_EMIT_REMOTE は UNKNOWN_COMMAND を返す。
#if defined(BOARD_X68K_KEYBOARD) || defined(BOARD_COMBINED)
#include "functions/x68k_keyboard/x68k_keyboard.h"
#endif

// ---------------------------------------------------------------------------
// プロトコル定数 (MimicX-protocol v0.6.0)
// ---------------------------------------------------------------------------

#define PROTOCOL_VERSION_MAJOR  0
#define PROTOCOL_VERSION_MINOR  7  // 0.7: serial / SET_LED / SET_LED_BLINK / HEART_BEAT を追加
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  8
#define FW_VERSION_PATCH  1

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
#define CMD_EMIT_REMOTE   0x07  // ホスト→デバイス: REMOTE 端子から SHARP12 リモコンコード送出
#define CMD_HEART_BEAT    0x08  // ホスト→デバイス: 接続生存通知 (1秒間隔送信)。応答は ACK。
#define CMD_DISCONNECT    0x09  // ホスト→デバイス: 選択終了。即座に SCANNED に戻る。
#define CMD_SET_CONFIG    0x10
#define CMD_SET_LED       0x20  // ホスト→デバイス: PB0 LED の色 override (RGB=255,255,255 で reset)
#define CMD_SET_LED_BLINK 0x21  // ホスト→デバイス: PB0 LED の点滅速度 override (None/Slow/Mid/High)
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
    // Chip UID (64bit) を 16 文字 ASCII hex (uppercase) で埋め込む。
    // バイト順は ESIG 上のメモリ並び (= wchisp の表示と一致)。
    // 例: UID0=0x2CD8ABCD, UID1=0x95CCBD27 → "CDABD82C27BDCC95"
    {
        static const char hex[] = "0123456789ABCDEF";
        uint32_t uid[2] = { ESIG->UID0, ESIG->UID1 };
        for (int w = 0; w < 2; w++) {
            uint32_t v = uid[w];
            for (int b = 0; b < 4; b++) {  // little-endian: LSB が先頭バイト
                uint8_t byte = (v >> (b * 8)) & 0xFFu;
                rsp[i++] = hex[byte >> 4];
                rsp[i++] = hex[byte & 0xF];
            }
        }
    }
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
        // ブートストラップ用途で req_id を持たない (プロトコル 6.3)。
        // 状態を SCANNED に遷移し (黄→緑)、応答を返す。
        status_led_on_identify_request();
        send_identify_response();
        break;
    case CMD_HEART_BEAT: {
        // F0 7D 01 08 <req_id> F7
        // 状態を CONNECTED に遷移 (まだなら) し HB タイマーをリセット、ACK を返す。
        uint8_t req_id = (len >= 6) ? data[4] : 0;
        status_led_on_heart_beat();
        send_ack(req_id, ACK_STATUS_OK, cmd);
        break;
    }
    case CMD_DISCONNECT: {
        // F0 7D 01 09 <req_id> F7
        // アプリがデバイス選択を解除した宣言。override をクリアして SCANNED に戻る。
        uint8_t req_id = (len >= 6) ? data[4] : 0;
        status_led_on_disconnect_request();
        send_ack(req_id, ACK_STATUS_OK, cmd);
        break;
    }
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
    case CMD_SET_LED: {
        // F0 7D 01 20 <req_id> <R> <G> <B> F7  (R/G/B は 7bit, 0-127)
        // 7bit → 8bit スケール: (v<<1) | (v>>6)  (0→0, 127→255 で単調)
        // スケール後 RGB=(255,255,255) は「override reset」のセンチネル。
        if (len < 9) {
            send_ack((len >= 6) ? data[4] : 0, ACK_STATUS_INVALID_VALUE, cmd);
            break;
        }
        uint8_t req_id = data[4];
        uint8_t r7 = data[5] & 0x7F;
        uint8_t g7 = data[6] & 0x7F;
        uint8_t b7 = data[7] & 0x7F;
        uint8_t r = (uint8_t)((r7 << 1) | (r7 >> 6));
        uint8_t g = (uint8_t)((g7 << 1) | (g7 >> 6));
        uint8_t b = (uint8_t)((b7 << 1) | (b7 >> 6));
        // 255/255/255 は status_led_set_override_rgb 内部で reset 扱い
        status_led_set_override_rgb(r, g, b);
        send_ack(req_id, ACK_STATUS_OK, cmd);
        break;
    }
    case CMD_SET_LED_BLINK: {
        // F0 7D 01 21 <req_id> <speed> F7   (speed: 0=None / 1=Slow / 2=Mid / 3=High)
        if (len < 7) {
            send_ack((len >= 6) ? data[4] : 0, ACK_STATUS_INVALID_VALUE, cmd);
            break;
        }
        uint8_t req_id = data[4];
        uint8_t speed = data[5] & 0x7F;
        if (speed > LED_BLINK_HIGH) {
            send_ack(req_id, ACK_STATUS_INVALID_VALUE, cmd);
            break;
        }
        status_led_set_override_blink(speed);
        send_ack(req_id, ACK_STATUS_OK, cmd);
        break;
    }
    case CMD_EMIT_REMOTE: {
        // F0 7D 01 07 <req_id> <code> F7
        if (len < 7) {
            send_ack((len >= 6) ? data[4] : 0, ACK_STATUS_GENERIC_ERROR, cmd);
            break;
        }
        uint8_t req_id = data[4];
        uint8_t code = data[5];
#if defined(BOARD_X68K_KEYBOARD) || defined(BOARD_COMBINED)
        // 約 100ms ブロックする。USB-MIDI は OS バッファで吸収される想定。
        int ok = x68k_keyboard_emit_remote(code);
        send_ack(req_id, ok ? ACK_STATUS_OK : ACK_STATUS_INVALID_VALUE, cmd);
#else
        (void)code;
        // この variant はキーボード未搭載 = REMOTE 端子を持たない
        send_ack(req_id, ACK_STATUS_UNKNOWN_CMD, cmd);
#endif
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
            status_led_on_input_activity();
        }
        break;

    case CIN_NOTE_OFF:
        if (channel == MIDI_CH_STATUS) {
            debug_led_set(midi1, 0);
        } else {
            hid_dispatch_note_off(channel, midi1);
            status_led_on_input_activity();
        }
        break;

    case CIN_CONTROL_CHANGE:
        hid_dispatch_cc(channel, midi1, midi2);
        status_led_on_input_activity();
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// MIDI-over-I2C 受信 (protocol §2.5)
// ---------------------------------------------------------------------------
#ifdef MIMICX_I2C

// I2C ISR から受け取った MIDI バイトを溜める FIFO (ISR→メインループ)。
#define I2C_RX_FIFO 256
static volatile uint8_t  i2c_fifo[I2C_RX_FIFO];
static volatile uint16_t i2c_fifo_head, i2c_fifo_tail;

// raw-MIDI バイト列パーサ (running status 対応)。USB の CIN パケットと違い生バイトから
// SysEx (F0..F7) / Note / CC を復元し、既存の処理 (process_sysex / hid_dispatch_*) を呼ぶ。
static uint8_t rawmidi_sx[64];
static int     rawmidi_sx_len;
static uint8_t rawmidi_in_sysex;
static uint8_t rawmidi_status;   // 現在のチャンネル status (running status)
static uint8_t rawmidi_d[2];
static int     rawmidi_didx;
static int     rawmidi_dlen;

static int rawmidi_data_len(uint8_t status) {
    switch (status & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    case 0xC0: case 0xD0: return 1;
    default: return 0;
    }
}

static void rawmidi_feed(uint8_t b) {
    if (b == 0xF0) {                 // SysEx 開始
        rawmidi_in_sysex = 1;
        rawmidi_sx_len = 0;
        rawmidi_sx[rawmidi_sx_len++] = b;
        rawmidi_status = 0;
        return;
    }
    if (b == 0xF7) {                 // SysEx 終了
        if (rawmidi_in_sysex && rawmidi_sx_len < (int)sizeof(rawmidi_sx)) {
            rawmidi_sx[rawmidi_sx_len++] = b;
            process_sysex(rawmidi_sx, rawmidi_sx_len);
        }
        rawmidi_in_sysex = 0;
        return;
    }
    if (b & 0x80) {                  // ステータスバイト
        rawmidi_in_sysex = 0;
        if (b >= 0xF8) return;       // System Real-Time は無視
        int l = rawmidi_data_len(b);
        if (l > 0) { rawmidi_status = b; rawmidi_didx = 0; rawmidi_dlen = l; }
        else        rawmidi_status = 0;
        return;
    }
    // データバイト
    if (rawmidi_in_sysex) {
        if (rawmidi_sx_len < (int)sizeof(rawmidi_sx)) rawmidi_sx[rawmidi_sx_len++] = b;
        return;
    }
    if (!rawmidi_status) return;
    rawmidi_d[rawmidi_didx++] = b;
    if (rawmidi_didx >= rawmidi_dlen) {
        uint8_t ch = rawmidi_status & 0x0F;
        uint8_t d0 = rawmidi_d[0];
        uint8_t d1 = (rawmidi_dlen > 1) ? rawmidi_d[1] : 0;
        switch (rawmidi_status & 0xF0) {
        case 0x90:
            if (d1 > 0) hid_dispatch_note_on(ch, d0, d1);
            else        hid_dispatch_note_off(ch, d0);
            status_led_on_input_activity();
            break;
        case 0x80:
            hid_dispatch_note_off(ch, d0);
            status_led_on_input_activity();
            break;
        case 0xB0:
            hid_dispatch_cc(ch, d0, d1);
            status_led_on_input_activity();
            break;
        default: break;
        }
        rawmidi_didx = 0;            // running status: 同 status の連続に対応
    }
}

// I2C ISR コールバック: 受信 MIDI バイトを FIFO へ積む (処理はメインループで)。
static void i2c_on_frame(const uint8_t* midi, int len) {
    for (int i = 0; i < len; i++) {
        uint16_t nh = (uint16_t)((i2c_fifo_head + 1) % I2C_RX_FIFO);
        if (nh != i2c_fifo_tail) {
            i2c_fifo[i2c_fifo_head] = midi[i];
            i2c_fifo_head = nh;
        }
    }
}

// メインループから呼び、FIFO の MIDI バイトをパーサへ流す。
static void i2c_midi_pump(void) {
    while (i2c_fifo_tail != i2c_fifo_head) {
        uint8_t b = i2c_fifo[i2c_fifo_tail];
        i2c_fifo_tail = (uint16_t)((i2c_fifo_tail + 1) % I2C_RX_FIFO);
        rawmidi_feed(b);
    }
}
#endif // MIMICX_I2C

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
    status_led_init();   // 内部で WAITING (黄) を render する
    hid_dispatch_init();
    usb_midi_init();
#ifdef MIMICX_I2C
    // 2 チップ構成: I2C スレーブ (MIDI-over-I2C) を起動。SDA=PC18(pin25)/SCL=PC19(pin24)。
    i2c_midi_init(0x33, i2c_on_frame);
#endif

    uint8_t cin, midi0, midi1, midi2;

    while (1) {
        // USB-MIDI からコマンド受信・処理
        while (usb_midi_receive_event(&cin, &midi0, &midi1, &midi2) > 0) {
            process_midi_event(cin, midi0, midi1, midi2);
        }

#ifdef MIMICX_I2C
        // I2C (MIDI-over-I2C) で受信したコマンドを処理
        i2c_midi_pump();
#endif

        // 各 HID 機能の poll
        hid_dispatch_poll();

        // ステータス LED の点滅 toggle
        status_led_poll();

        // USB-MIDI TX バッファをフラッシュ
        usb_midi_poll();
    }
}
