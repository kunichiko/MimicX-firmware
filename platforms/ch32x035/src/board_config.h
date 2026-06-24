// ===================================================================================
// board_config.h
// ===================================================================================
// ビルド時にどのボード (どの HID 機能を有効化するか) を選ぶか定義する。
// platformio.ini の env 内で `-D BOARD_<NAME>` を指定する。
//
// BOARD_FUNCTIONS マクロは、有効な hid_function_t* の配列初期化子を定義する。
// 同時に複数機能を有効にすることも可能 (各機能を異なる MIDI チャンネルに割り当てる)。
//
// 新しいボード/機能の追加方法:
//   1. functions/<name>/ にモジュールを作成
//   2. extern 宣言を下のブロックに追加
//   3. BOARD_FUNCTIONS マクロを定義
//   4. platformio.ini に env を追加 (BOARD_<NAME> 定義 + src_filter 設定)
// ===================================================================================
#ifndef _BOARD_CONFIG_H
#define _BOARD_CONFIG_H

#include "hid_function.h"

// ---------------------------------------------------------------------------
// 各 variant の識別情報:
//   BOARD_NAME                 ASCII 識別子 (デバッグ出力等)
//   BOARD_USB_PRODUCT          USB iProduct (UTF-16 リテラル, ホスト側 MIDI デバイス名)
//   BOARD_USB_SERIAL_TAG       variant 識別タグ (ASCII リテラル, NUL 終端を除く文字数 = TAG_CHARS)
//   BOARD_USB_SERIAL_TAG_CHARS BOARD_USB_SERIAL_TAG の文字数 (NUL を含まない)
//   BOARD_FUNCTIONS            有効な hid_function_t* の配列初期化子
//
// USB iSerialNumber は usb_config.h 側で chip UID と TAG を組合せて
//   "mimicx-<UID16hex>-<tag>" の形式で起動時に組み立てる。VID:PID:iSerial で
//   ホスト OS が個体を識別するため、UID で個体・TAG で variant を両方反映する。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// BOARD_JOYSTICK: ATARI/MD6 ジョイスティック
// ---------------------------------------------------------------------------
#if defined(BOARD_JOYSTICK)
    extern const hid_function_t joystick_function;
    #define BOARD_NAME                 "mimic-x-joy"
    #define BOARD_USB_PRODUCT          u"Mimic X (Joystick)"
    #define BOARD_USB_SERIAL_TAG       "joy"
    #define BOARD_USB_SERIAL_TAG_CHARS 3
    #define BOARD_FUNCTIONS            { &joystick_function }

// ---------------------------------------------------------------------------
// BOARD_X68K_KEYBOARD: X68000 キーボード (本体側に接続するマウスポート付き)
// ---------------------------------------------------------------------------
#elif defined(BOARD_X68K_KEYBOARD)
    extern const hid_function_t x68k_keyboard_function;
    extern const hid_function_t x68k_mouse_function;
    #define BOARD_NAME                 "mimic-x-x68k"
    #define BOARD_USB_PRODUCT          u"Mimic X (X68000 Keyboard)"
    #define BOARD_USB_SERIAL_TAG       "x68k"
    #define BOARD_USB_SERIAL_TAG_CHARS 4
    #define BOARD_FUNCTIONS            { &x68k_keyboard_function, &x68k_mouse_function }

// ---------------------------------------------------------------------------
// BOARD_COMBINED: ジョイスティック + X68000 キーボード/マウス 同時搭載
// ---------------------------------------------------------------------------
// CH32X035G8U6 (QFN28) 1 個で全機能を載せる "全部入り" バリアント。
// GPIO / ペリフェラルは衝突しない:
//   joystick      : PA0 (TH = TIM2_CH1), PA2-PA7 (D0-D5)、TIM2 + TIM3 + DMA1_Ch5/Ch7
//   x68k_keyboard : PB10/PB11 (USART1 APB2)、PB12 (READY 入力)
//   x68k_mouse    : PB9 (USART4_TX APB1, REMAP=011)
// MIDI チャンネルは joystick=0 / x68k_keyboard=1 / x68k_mouse=2 で衝突しない。
// ---------------------------------------------------------------------------
#elif defined(BOARD_COMBINED)
    extern const hid_function_t joystick_function;
    extern const hid_function_t x68k_keyboard_function;
    extern const hid_function_t x68k_mouse_function;
    #define BOARD_NAME                 "mimic-x-combo"
    #define BOARD_USB_PRODUCT          u"Mimic X (Combined)"
    #define BOARD_USB_SERIAL_TAG       "combo"
    #define BOARD_USB_SERIAL_TAG_CHARS 5
    #define BOARD_FUNCTIONS            { &joystick_function, &x68k_keyboard_function, &x68k_mouse_function }

#else
    #error "No BOARD_* macro defined. Set one of BOARD_JOYSTICK, BOARD_X68K_KEYBOARD, BOARD_COMBINED via build_flags."
#endif

#endif
