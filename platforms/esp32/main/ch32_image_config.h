// ===================================================================================
// ch32_image_config.h  (ESP32 が内包する CH32 イメージの選択)
// ===================================================================================
// ESP32 ファームは joystick / keyboard / combined の「いずれか 1 つだけ」の CH32
// イメージを内包する。これにより ESP32 ファーム自体が 3 機種に分かれる。
//
// ビルド時に MIMICX_CH32_JOYSTICK / _KEYBOARD / _COMBINED のいずれかを定義する
// (CMakeLists の MIMICX_CH32_VARIANT で選択)。未定義なら joystick を既定とする。
//
// 各 variant につき:
//   CH32_IMG_NAME            : IDENTIFY の BOARD_NAME と一致させる文字列
//   CH32_IMG_MAJ/MIN/PATCH   : 内蔵 bin の FW_VERSION (★bin 再生成時に必ず合わせる)
//   CH32_IMG_SYM_START/END   : EMBED された bin のリンカシンボル
// ===================================================================================
#pragma once

#include <stdint.h>

#if !defined(MIMICX_CH32_JOYSTICK) && !defined(MIMICX_CH32_KEYBOARD) && !defined(MIMICX_CH32_COMBINED)
#define MIMICX_CH32_JOYSTICK 1
#endif

#if defined(MIMICX_CH32_JOYSTICK)
  #define CH32_IMG_NAME  "mimic-x-joy"
  #define CH32_IMG_MAJ   0
  #define CH32_IMG_MIN   8
  #define CH32_IMG_PATCH 2
  extern const uint8_t ch32_img_start[] asm("_binary_ch32_joy_i2c_bin_start");
  extern const uint8_t ch32_img_end[]   asm("_binary_ch32_joy_i2c_bin_end");
#elif defined(MIMICX_CH32_KEYBOARD)
  #define CH32_IMG_NAME  "mimic-x-x68k"
  #define CH32_IMG_MAJ   0
  #define CH32_IMG_MIN   8
  #define CH32_IMG_PATCH 2
  extern const uint8_t ch32_img_start[] asm("_binary_ch32_kbd_i2c_bin_start");
  extern const uint8_t ch32_img_end[]   asm("_binary_ch32_kbd_i2c_bin_end");
#elif defined(MIMICX_CH32_COMBINED)
  #define CH32_IMG_NAME  "mimic-x-combo"
  #define CH32_IMG_MAJ   0
  #define CH32_IMG_MIN   8
  #define CH32_IMG_PATCH 2
  extern const uint8_t ch32_img_start[] asm("_binary_ch32_combo_i2c_bin_start");
  extern const uint8_t ch32_img_end[]   asm("_binary_ch32_combo_i2c_bin_end");
#else
  #error "MIMICX_CH32_VARIANT が不正"
#endif
