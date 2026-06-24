// ===================================================================================
// hid_dispatcher.h
// ===================================================================================
// MIDI イベントを board_config.h で有効化された各 hid_function に振り分ける。
// IDENTIFY/CAPABILITY 応答用のメタ情報構築も行う。
// ===================================================================================
#ifndef _HID_DISPATCHER_H
#define _HID_DISPATCHER_H

#include <stdint.h>

// 全機能に対して init() を呼ぶ
void hid_dispatch_init(void);

// 全機能に対して release_all() を呼ぶ
void hid_dispatch_release_all(void);

// メインループから定期呼び出し (各機能の poll を呼ぶ)
void hid_dispatch_poll(void);

// MIDI イベントの dispatch (channel が一致した機能だけ呼ばれる)
void hid_dispatch_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void hid_dispatch_note_off(uint8_t channel, uint8_t note);
void hid_dispatch_cc(uint8_t channel, uint8_t cc, uint8_t value);

// SysEx SET_CONFIG (全機能に broadcast)。
// 返り値: 集約された ACK_STATUS_*。
// - いずれかの機能が OK を返したら OK
// - いずれかの機能が INVALID_VALUE を返したら INVALID_VALUE 優先
// - どの機能も key を認識しなかったら UNKNOWN_KEY
uint8_t hid_dispatch_set_config(uint8_t key, const uint8_t* val, int len);

// IDENTIFY_RESPONSE 用: チャンネルマップを buf に追記
//   フォーマット: <num_channels> <ch_0> <type_0> <target_0> <ch_1> ... <ASCII name terminator なし>
//   戻り値: buf に書いた合計バイト数
int hid_dispatch_build_channel_map(uint8_t* buf, int max_len);

// IDENTIFY_RESPONSE 用: 全機能名を連結した文字列 (空白区切り) を buf に書く
int hid_dispatch_build_device_name(uint8_t* buf, int max_len);

// CAPABILITY_RESPONSE 用: 全機能の capability を TLV で連結
int hid_dispatch_build_capabilities(uint8_t* buf, int max_len);

#endif
