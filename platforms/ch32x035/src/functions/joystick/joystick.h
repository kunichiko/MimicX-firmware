#ifndef _JOYSTICK_H
#define _JOYSTICK_H

#include <stdint.h>

// パッドモード
#define PAD_MODE_ATARI          0
#define PAD_MODE_MD6            1
#define PAD_MODE_LIBBLE_RABBLE  2  // XPD-1LR 互換 (左右十字キー + 中央 A/B)
#define PAD_MODE_MSX_MOUSE      3  // MSX マウスプロトコル (pin 8 STROBE で 4 ニブル送出)

// MIDI Note 番号 → ボタンインデックス
#define BTN_UP      0
#define BTN_DOWN    1
#define BTN_LEFT    2
#define BTN_RIGHT   3
#define BTN_A       4
#define BTN_B       5
#define BTN_C       6
#define BTN_START   7
#define BTN_X       8
#define BTN_Y       9
#define BTN_Z       10
#define BTN_MODE    11
// リブルラブル (XPD-1LR) 右側 D-pad。
// X68000 が COMMON (PA0/TH) を HIGH にしている期間にホストへ返す方向状態。
#define BTN_UP2     12
#define BTN_DOWN2   13
#define BTN_LEFT2   14
#define BTN_RIGHT2  15
// TOWNS パッド RUN/SELECT (protocol 0.9, ATARI モードのみ)。
// RUN = 左右同時アサート、SELECT = 上下同時アサート (通常方向との OR)
#define BTN_RUN     16
#define BTN_SELECT  17
#define BTN_COUNT   18

// GPIO ピン割り当て (ATARI / X68000 標準 D-SUB 9pin 配線済み)
// MimicX-hardware/atari-joystick の atari_joystick.ato と同期
// PA0: TH/COMMON (入力: コンソールからの SELECT 信号、D-SUB pin 8)
// PA7: D0 (Up,     D-SUB pin 1)
// PA5: D1 (Down,   D-SUB pin 2)
// PA3: D2 (Left,   D-SUB pin 3)
// PA2: D3 (Right,  D-SUB pin 4)
// PA6: D4 (TRIG-A, D-SUB pin 6 — MD: Button A / B)
// PA4: D5 (TRIG-B, D-SUB pin 7 — MD: Button B / C)
#define PIN_TH      0
#define PIN_D0      7
#define PIN_D1      5
#define PIN_D2      3
#define PIN_D3      2
#define PIN_D4      6
#define PIN_D5      4

// 初期化
void joystick_init(void);

// パッドモード設定
void joystick_set_mode(uint8_t mode);
uint8_t joystick_get_mode(void);

// ボタン状態設定 (MIDI Note 番号で指定)
// note: MIDI Note 番号, pressed: 1=押下, 0=解放
void joystick_set_button_by_note(uint8_t note, uint8_t pressed);

// 全ボタン解放
void joystick_release_all(void);

// ATARI モード: GPIO を直接更新 (メインループから呼ぶ)
void joystick_poll(void);

// hid_function 互換 vtable
#include "hid_function.h"
extern const hid_function_t joystick_function;

#endif
