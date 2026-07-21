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

// CONFIG_IDF_TARGET_* は sdkconfig.h で定義される。include 順に依存しないよう
// (先に ESP ヘッダを include していないソースでも正しく分岐するよう) 明示的に含める。
#include "sdkconfig.h"

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

#elif defined(CONFIG_IDF_TARGET_ESP32S3)
// --- Seeed Studio XIAO ESP32-S3 ---
//   XIAO C6 とパッド互換 (D4=SDA, D5=SCL, D3=INT)。GPIO 番号のみ異なる:
//   D0=GPIO1, D1=GPIO2, D2=GPIO3, D3=GPIO4, D4=GPIO5, D5=GPIO6。
#define BOARD_I2C_SDA_GPIO   5    // D4
#define BOARD_I2C_SCL_GPIO   6    // D5
#define BOARD_I2C_INT_GPIO   4    // D3  (CH32 PB1 INT ← 入力)
#define BOARD_SWDIO_GPIO     5    // = SDA (CH32 PC18)
#define BOARD_SWCLK_GPIO     6    // = SCL (CH32 PC19)

// XIAO ESP32-S3 のアンテナはオンボード直結 (C6 のような RF スイッチ無し) のため
// BOARD_HAS_ANTENNA_SWITCH は定義しない。

// S3 は USB-OTG を持つため、XIAO の USB-C を USB-MIDI デバイスにできる
// (usb_midi_bridge.c)。BLE-MIDI と併走し、どちらのトランスポートからも
// CH32 (I2C) へ中継する。C3/C6 は USB Serial/JTAG のみ (固定機能) なので不可。
#define BOARD_HAS_USB_MIDI   1

// wireless-combo 基板の CH32 電源/BOOT 制御 (D0/D1)。復旧シーケンスは未実装
// (MimicX-hardware/wireless-combo/README.md 参照)。実装時にこの define を使う。
// #define BOARD_CH32_PWR_GPIO  1    // D0: High=CH32 電源OFF (P-FET ゲート)
// #define BOARD_CH32_BOOT_GPIO 2    // D1: POR 時 High=ブートローダ突入 (PC17)

#elif defined(CONFIG_IDF_TARGET_ESP32C3)
// --- Seeed Studio XIAO ESP32-C3 ---
//   D4=GPIO6(SDA), D5=GPIO7(SCL), D3=GPIO5(空き) を使用。
//   アンテナは U.FL 外付けのみ (オンボード無し / RF スイッチ無し) なので初期化不要。
#define BOARD_I2C_SDA_GPIO   6    // D4
#define BOARD_I2C_SCL_GPIO   7    // D5
#define BOARD_I2C_INT_GPIO   5    // D3  (CH32 PB1 INT ← 入力)
#define BOARD_SWDIO_GPIO     6    // = SDA (CH32 PC18)
#define BOARD_SWCLK_GPIO     7    // = SCL (CH32 PC19)

#else
// --- ESP32-WROOM-32 DevKit (既定) ---
#define BOARD_I2C_SDA_GPIO   21
#define BOARD_I2C_SCL_GPIO   22
#define BOARD_I2C_INT_GPIO   19   // CH32 PB1(INT) ← 入力
#define BOARD_SWDIO_GPIO     21   // = SDA (CH32 PC18)
#define BOARD_SWCLK_GPIO     22   // = SCL (CH32 PC19)
#endif
