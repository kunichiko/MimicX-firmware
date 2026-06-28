// ===================================================================================
// ch32_swd.h  (ESP32 / CH32 を RVSWD 2線デバッグで叩くための薄いグルー)
// ===================================================================================
// CNLohr の bitbang_rvswdio.h (MIT/BSD) を下敷きに、ESP32 classic の GPIO で
// WCH RISC-V 2線デバッグ (SWDIO/SWCLK) をビットバングする。CH32 ファームの
// OTA 更新 (protocol: ESP32 経由) の土台。
//
// 配線 (I2C と兼用の 2 本をそのまま使う):
//   ESP32 GPIO21 = SWDIO (CH32 PC18) / GPIO22 = SWCLK (CH32 PC19) / GND 共通
//
// 注意: CH32 側が SDI を有効化している (SW_CFG=0) ときだけ応答する。通常運用の
//   joystick-i2c は SDI を無効化しているため、事前に「SWD モードへ移行」させること。
// ===================================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// CH32 を RVSWD で初期化し、チップ種別を判定する (非破壊: コアは halt しない)。
//   chip_type_out: 判定した種別 (CH32X03x = 0x0d) を返す (NULL 可)
// 戻り値: CH32X03x を検出できたら true。
bool ch32_swd_probe(uint32_t *chip_type_out);

// CH32 のフラッシュを書き換える (halt → unlock → erase → program → verify → reboot)。
//   flash_addr: 通常 0x08000000 (X035 user flash 先頭)
//   img/len   : 書き込むイメージ。len は 4 の倍数でなくてもよい (末尾は 0xFF 詰め)。
// 戻り値: 全工程成功 (verify 一致) で true。CH32 は SDI 有効である必要がある。
// 注意: 失敗時 CH32 は中途半端な状態になり得る (BOOT-DFU で復旧可能)。
bool ch32_swd_flash(uint32_t flash_addr, const uint8_t *img, size_t len);
