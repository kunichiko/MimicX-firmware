// ===================================================================================
// i2c_bridge.h  (ESP32 / I2C マスター — CH32 デバイスエンジンへの橋渡し, protocol §2.5)
// ===================================================================================
// CH32 (I2C スレーブ, addr 0x33) に対し MimicX の MIDI バイト列を双方向に運ぶ。
//   - host→device: i2c_bridge_write() で [LEN][MIDI] を書き込む。
//   - device→host: CH32 の INT 線 (Low) を検出して read し、取得した MIDI を
//                  on_rx コールバックへ渡す ([LEN][MIDI])。
//
// 配線 (ESP32 側, 暫定・基板に合わせて i2c_bridge.c の #define を調整):
//   SDA=GPIO21 / SCL=GPIO22 (外付けプルアップ要) / INT=GPIO19 (入力, CH32 PB1 から)
// ===================================================================================
#pragma once

#include <stdint.h>

// I2C マスターと INT 受信タスクを起動する。
//   on_rx: device→host の 1 メッセージ受信ごとに呼ばれる。
void i2c_bridge_init(void (*on_rx)(const uint8_t* midi, int len));

// host→device メッセージ (Note/CC/SysEx) を CH32 へ送る。
void i2c_bridge_write(const uint8_t* midi, int len);
