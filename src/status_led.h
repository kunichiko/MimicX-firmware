// ===================================================================================
// status_led.h
// ===================================================================================
// PB0 に接続された WS2812B-2020 (シリアル制御 RGB LED) を駆動する。
//
// ハードウェア (MimicX-hardware / atari-joystick, x68000-keyboard 共通):
//   MCU PB0 → 470Ω → WS2812B-2020 DI、VDD33 共通電源。
//
// プロトコル:
//   1 線シリアル, 800 kHz NRZ, GRB 順 24bit。bit-bang + 割込み禁止で送信する。
//   送信中の割込み停止は ~30µs。Latch (RES) は 50µs LOW で発火する。
// ===================================================================================
#ifndef _STATUS_LED_H
#define _STATUS_LED_H

#include <stdint.h>

// GPIO 設定 (PB0 を push-pull 50MHz 出力に) + LED 消灯 (LOW)。
void status_led_init(void);

// RGB 各 8bit (0-255) で点灯色を設定する。内部で GRB 順に並べ替えて WS2812 に送る。
void status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

#endif
