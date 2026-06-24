// ===================================================================================
// X68000 キーボード機能
// ===================================================================================
// USART1 (PC0=RX, PC1=TX) を 2400bps 8N1 で X68000 キーボード端子に接続。
// MIDI Note On/Off (Channel 1, デフォルト) でキー押下/解放を送信し、
// 本体側からの LED コマンドを受信して MIDI CC で通知する。
// ===================================================================================
#ifndef _X68K_KEYBOARD_H
#define _X68K_KEYBOARD_H

#include <stdint.h>
#include "hid_function.h"

extern const hid_function_t x68k_keyboard_function;

// EMIT_REMOTE (SysEx 0x07) のハンドラ。
// 指定された制御コードを SHARP12 (REMOTE 端子) で送出する。コード範囲 (0x01-0x1F)
// を満たさなければ何もせず 0 (失敗) を返す。約 100ms ブロックする。
// 戻り値: 1=送出成功, 0=コード不正
int x68k_keyboard_emit_remote(uint8_t code);

#endif
