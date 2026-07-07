// ===================================================================================
// board_config.h  (ESP32 系ボード別のピン/機能割り当て)
// ===================================================================================
// ESP32-WROOM-32 DevKit (Xtensa) と Seeed XIAO ESP32-C6 (RISC-V) で GPIO 配置が
// 異なるため、ターゲット (CONFIG_IDF_TARGET_*) でピンを切り替える。ソースは共通
// (platforms/esp32/main/*)、差分はこのヘッダと sdkconfig / ビルド系のみ。
//
//   ESP32 DevKit  : platforms/esp32     (idf.py set-target esp32)
//   XIAO ESP32-C6 : platforms/esp32c6   (idf.py set-target esp32c6)
//
// SWDIO/SWCLK は I2C の SDA/SCL と同一線 (OTA 時のみ SWD、稼働中は I2C)。CH32 側で
// 2 線を SWD/I2C にモード切替する前提 (protocol §2.5 / CH32 ファーム参照)。
// ===================================================================================
#pragma once

#if defined(CONFIG_IDF_TARGET_ESP32C6)
// --- Seeed Studio XIAO ESP32-C6 ---
//   D4=GPIO22(SDA), D5=GPIO23(SCL), D3=GPIO21(空き) を使用。
#define BOARD_I2C_SDA_GPIO   22   // D4
#define BOARD_I2C_SCL_GPIO   23   // D5
#define BOARD_I2C_INT_GPIO   21   // D3  (CH32 PB1 INT ← 入力)
#define BOARD_SWDIO_GPIO     22   // = SDA (CH32 PC18)
#define BOARD_SWCLK_GPIO     23   // = SCL (CH32 PC19)

// XIAO ESP32-C6 は FM8625H RF スイッチ経由でアンテナを選択する。オンボード
// セラミックアンテナを使うには起動時に GPIO3=Low (RF スイッチ給電) + GPIO14=Low
// (オンボード選択) を出力する必要がある。未設定だと BLE の電波が極端に弱くなる。
#define BOARD_HAS_ANTENNA_SWITCH 1
#define BOARD_ANT_RF_ENABLE_GPIO 3    // Low で RF スイッチに給電
#define BOARD_ANT_SELECT_GPIO    14   // Low=オンボード / High=外部(U.FL)

#else
// --- ESP32-WROOM-32 DevKit (既定) ---
#define BOARD_I2C_SDA_GPIO   21
#define BOARD_I2C_SCL_GPIO   22
#define BOARD_I2C_INT_GPIO   19   // CH32 PB1(INT) ← 入力
#define BOARD_SWDIO_GPIO     21   // = SDA (CH32 PC18)
#define BOARD_SWCLK_GPIO     22   // = SCL (CH32 PC19)
#endif
