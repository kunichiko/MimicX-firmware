// ===================================================================================
// mimicx_proto.h  (ESP32 / BLE-MIDI スケルトン)
// ===================================================================================
// MimicX SysEx プロトコルの最小実装。トランスポート (ble_midi) から再結合済みの
// SysEx を受け取り、IDENTIFY / HEART_BEAT / 各 ACK を返す。
//
// ワイヤフォーマットは CH32 ファーム (src/main.c) とバイト互換。
// プロトコル仕様: MimicX-protocol §6 (v0.8.0)
// ===================================================================================
#pragma once

#include <stdint.h>

void mimicx_proto_init(void);

// 完全に再結合された 1 件の SysEx (先頭 0xF0 〜 末尾 0xF7 を含む) を処理する。
void mimicx_proto_handle_sysex(const uint8_t *sx, int len);
