// ===================================================================================
// i2c_midi.h  (CH32X035 / MIDI-over-I2C スレーブ, protocol §2.5)
// ===================================================================================
// 2 チップ構成で CH32 をデバイスエンジン(I2C スレーブ)として動かすための I2C 受信層。
// ホスト MCU (ESP32 等, I2C マスター) が MimicX の MIDI/SysEx バイト列を書き込み、
// CH32 は既存の MIDI パーサ (process_sysex / hid_dispatch_*) でそのまま処理する。
//
// ピン (CH32X035G8U6, I2C1 remap option 3):
//   SCL = PC19 (package pin 24, DCK と兼用) / SDA = PC18 (package pin 25, DIO と兼用)
// ===================================================================================
#ifndef _I2C_MIDI_H
#define _I2C_MIDI_H

#include <stdint.h>

// I2C スレーブを初期化する。
//   addr7    : 7bit スレーブアドレス (既定 0x33)
//   on_frame : 1 write トランザクション ([LEN][MIDI bytes×LEN]) 受信ごとに、
//              MIDI バイト列 (先頭の LEN を除いた len バイト) を渡すコールバック。
void i2c_midi_init(uint8_t addr7, void (*on_frame)(const uint8_t* midi, int len));

// device→host メッセージ (IDENTIFY_RSP / ACK / TARGET_RX 等) を読み出しキューに積み、
// INT 線をアサートする。ホストが I2C read で取り出す ([LEN][MIDI bytes])。
void i2c_midi_enqueue(const uint8_t* midi, int len);

// ファーム更新用: I2C を停止し SDI(2線デバッグ)を再有効化して PC18/PC19 を解放し、
// 待機ループに入る (戻らない)。以降はホスト MCU (ESP32) が SWD でフラッシュを書き換える。
// 復帰は SWD reboot か電源再投入 (通常の I2C スレーブで起動し直す)。
void i2c_midi_enter_swd(void) __attribute__((noreturn));

#endif
