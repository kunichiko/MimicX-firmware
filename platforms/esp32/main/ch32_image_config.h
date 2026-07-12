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
//   ch32_img_start/end       : EMBED された bin のリンカシンボル
//
// バージョン (CH32_IMG_MAJ/MIN/PATCH) は全 variant 共通で、CMake が
// ch32x035/src/main.c の FW_VERSION_* から抽出して CH32_IMG_VER_* として注入する
// (esp32/main/CMakeLists.txt)。手動同期は不要 — 版数の単一ソースは CH32 の main.c。
// ===================================================================================
#pragma once

#include <stdint.h>

#if !defined(MIMICX_CH32_JOYSTICK) && !defined(MIMICX_CH32_KEYBOARD) && !defined(MIMICX_CH32_COMBINED)
#define MIMICX_CH32_JOYSTICK 1
#endif

#if defined(MIMICX_CH32_JOYSTICK)
  #define CH32_IMG_NAME  "mimic-x-joy"
  extern const uint8_t ch32_img_start[] asm("_binary_ch32_joy_i2c_bin_start");
  extern const uint8_t ch32_img_end[]   asm("_binary_ch32_joy_i2c_bin_end");
#elif defined(MIMICX_CH32_KEYBOARD)
  #define CH32_IMG_NAME  "mimic-x-x68k"
  extern const uint8_t ch32_img_start[] asm("_binary_ch32_kbd_i2c_bin_start");
  extern const uint8_t ch32_img_end[]   asm("_binary_ch32_kbd_i2c_bin_end");
#elif defined(MIMICX_CH32_COMBINED)
  #define CH32_IMG_NAME  "mimic-x-combo"
  extern const uint8_t ch32_img_start[] asm("_binary_ch32_combo_i2c_bin_start");
  extern const uint8_t ch32_img_end[]   asm("_binary_ch32_combo_i2c_bin_end");
#else
  #error "MIMICX_CH32_VARIANT が不正"
#endif

// バージョンは CMake が ch32x035/src/main.c の FW_VERSION_* から注入する。
// 全 variant で共通 (CH32 ソースは 1 つで variant はビルドフラグ違いのため)。
#if !defined(CH32_IMG_VER_MAJ) || !defined(CH32_IMG_VER_MIN) || !defined(CH32_IMG_VER_PATCH)
  #error "CH32_IMG_VER_* が未定義。esp32/main/CMakeLists.txt のバージョン注入を確認"
#endif
#define CH32_IMG_MAJ   CH32_IMG_VER_MAJ
#define CH32_IMG_MIN   CH32_IMG_VER_MIN
#define CH32_IMG_PATCH CH32_IMG_VER_PATCH
