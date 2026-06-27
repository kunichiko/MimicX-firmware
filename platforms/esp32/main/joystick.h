// ===================================================================================
// joystick.h  (ESP32 / ATARI ジョイスティック出力)
// ===================================================================================
// ATARI / D-SUB 9pin 互換のデジタルジョイスティックを GPIO で駆動する (active-low)。
// MIDI Note (channel 0) からボタン状態を受け取る。
// ===================================================================================
#pragma once

#include <stdint.h>

void joystick_init(void);

// MIDI Note 番号 (1=UP 2=DOWN 3=LEFT 4=RIGHT 6=A 7=B) でボタンを押下/解放する。
void joystick_note_on(uint8_t note);
void joystick_note_off(uint8_t note);

// 全ボタンを解放する (RESET / 切断時)。
void joystick_release_all(void);
