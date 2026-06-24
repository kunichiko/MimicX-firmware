// ===================================================================================
// hid_function.h
// ===================================================================================
// HID 機能 (joystick, keyboard, mouse など) を抽象化するインターフェース。
//
// 各機能モジュールは functions/<name>/ 配下に置き、
// `extern const hid_function_t <name>_function;` を提供する。
//
// board_config.h で BOARD_FUNCTIONS マクロに有効な機能を列挙し、
// hid_dispatcher が MIDI イベントを各機能にディスパッチする。
// ===================================================================================
#ifndef _HID_FUNCTION_H
#define _HID_FUNCTION_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// ACK status コード (プロトコル仕様 6.1.3 と一致)
// ---------------------------------------------------------------------------
#define ACK_STATUS_OK             0x00
#define ACK_STATUS_UNKNOWN_CMD    0x01
#define ACK_STATUS_UNKNOWN_KEY    0x02
#define ACK_STATUS_INVALID_VALUE  0x03
#define ACK_STATUS_GENERIC_ERROR  0x7F

// ---------------------------------------------------------------------------
// HID Type 定数 (プロトコル仕様 6.4.2 と一致)
// ---------------------------------------------------------------------------
#define HID_TYPE_UNKNOWN   0x00
#define HID_TYPE_KEYBOARD  0x01
#define HID_TYPE_JOYSTICK  0x02
#define HID_TYPE_MOUSE     0x03
#define HID_TYPE_CUSTOM    0x10

// ---------------------------------------------------------------------------
// Target System 定数 (プロトコル仕様 6.4.3 と一致)
// ---------------------------------------------------------------------------
#define TARGET_GENERIC     0x00
#define TARGET_ATARI       0x01
#define TARGET_X68000      0x02
#define TARGET_PC98        0x03
#define TARGET_MSX         0x04
#define TARGET_FM_TOWNS    0x05
#define TARGET_PC88        0x06
#define TARGET_APPLE2      0x07
#define TARGET_C64         0x08
#define TARGET_AMIGA       0x09
#define TARGET_ZX          0x0A
#define TARGET_PC_AT       0x10
#define TARGET_PC_XT       0x11
#define TARGET_MD          0x40

// ---------------------------------------------------------------------------
// HID Function vtable
// ---------------------------------------------------------------------------
//
// 各 hook は呼ばれない場合があるので NULL を許容 (dispatcher が NULL チェックする)。
// channel が一致した機能のみ MIDI イベントが渡される。
// SysEx は全機能に broadcast される (key で機能側がフィルタ)。
//
// ---------------------------------------------------------------------------
typedef struct hid_function_s {
    // メタ情報
    const char* name;          // 表示名 (ASCII)
    uint8_t hid_type;          // HID_TYPE_*
    uint8_t target_system;     // TARGET_*
    uint8_t midi_channel;      // 0-15

    // ライフサイクル
    void (*init)(void);
    void (*release_all)(void);

    // MIDI イベント (channel が一致した機能のみ)
    void (*on_note_on)(uint8_t note, uint8_t velocity);
    void (*on_note_off)(uint8_t note);
    void (*on_cc)(uint8_t cc, uint8_t value);

    // SysEx SET_CONFIG (全機能に broadcast)。
    // 返り値: ACK_STATUS_*。key が自分の管轄でなければ ACK_STATUS_UNKNOWN_KEY を返す。
    uint8_t (*on_set_config)(uint8_t key, const uint8_t* val, int len);

    // メインループから定期呼び出し
    void (*poll)(void);

    // CAPABILITY_RESPONSE 用 TLV を buf に追記、書き込んだバイト数を返す
    int (*append_capabilities)(uint8_t* buf, int max_len);
} hid_function_t;

#endif
