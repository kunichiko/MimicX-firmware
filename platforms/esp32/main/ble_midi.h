// ===================================================================================
// ble_midi.h  (ESP32 / BLE-MIDI スケルトン)
// ===================================================================================
// 標準 BLE-MIDI GATT サービス (Apple MIDI-BLE 仕様) のトランスポート層。
//   service        : 03B80E5A-EDE8-4B33-A751-6CE34EC4C700
//   characteristic : 7772E5DB-3868-4112-A1A9-F2669D106BF3 (Write w/o Rsp / Notify)
//
// 役割:
//   - characteristic write を受け取り、BLE-MIDI パケットを素の MIDI バイト列へ復元、
//     SysEx を跨パケットで再結合して mimicx_proto に渡す (プロトコル §2.3)
//   - 応答 SysEx を BLE-MIDI パケットへ分割して notify 送信
// ===================================================================================
#pragma once

#include <stdint.h>
#include "host/ble_uuid.h"

// アドバタイズで広告する MIDI サービス UUID (iOS/macOS central はこれで絞り込む)。
extern const ble_uuid128_t ble_midi_svc_uuid;

// GATT サービス/特性を登録する (ble_svc_gatt_init() のあと、host 起動前に呼ぶ)。
// 戻り値: 0=成功、それ以外は NimBLE エラーコード。
int  ble_midi_register_gatt(void);

// GAP イベントから接続状態を通知する。
void ble_midi_set_conn(uint16_t conn_handle);   // 接続時。切断時は BLE_HS_CONN_HANDLE_NONE
uint16_t ble_midi_val_handle(void);             // notify 用の attribute handle

// characteristic write の生ペイロード (BLE-MIDI パケット) を投入する。
void ble_midi_rx(const uint8_t *data, int len);

// 完成した SysEx (0xF0..0xF7) を BLE-MIDI パケットに分割して notify 送信する。
void ble_midi_send_sysex(const uint8_t *sx, int n);
