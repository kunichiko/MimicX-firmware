// ===================================================================================
// usb_midi_bridge.h  (ESP32-S3 / USB-MIDI デバイス トランスポート)
// ===================================================================================
// XIAO ESP32-S3 の USB-OTG を TinyUSB で USB-MIDI デバイス化し、有線接続の
// トランスポートを提供する。BLE-MIDI (ble_midi.c) と対称のインターフェース:
//   - ホスト→デバイス: USB-MIDI イベントパケットを素の MIDI バイト列へ復元し、
//     完成メッセージ (SysEx 1 件 / channel 1 件) 単位で登録ハンドラへ渡す
//   - デバイス→ホスト: MIDI メッセージを USB-MIDI パケットへ分割して IN 送信
//
// C3/C6 では利用不可 (USB Serial/JTAG 固定機能のみ)。BOARD_HAS_USB_MIDI が
// 定義されたターゲット (board_config.h) でのみビルド・リンクされる。
// ===================================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>

// TinyUSB を起動し USB-MIDI デバイスとして enumerate 可能にする。
//   on_rx    : 完成 MIDI メッセージの受信ハンドラ (main.c が CH32 転送を実装)
//   on_state : マウント状態変化 (mounted=true でホスト接続) の通知。NULL 可
void usb_midi_bridge_init(void (*on_rx)(const uint8_t *msg, int len),
                          void (*on_state)(bool mounted));

// device→host メッセージを USB-MIDI IN エンドポイントへ送る。未接続時は捨てる。
void usb_midi_bridge_notify(const uint8_t *msg, int n);

// USB ホストにマウントされているか (configured 状態)。
bool usb_midi_bridge_mounted(void);

// TinyUSB を停止し USB PHY を削除する (tinyusb_driver_uninstall)。
// ROM ダウンロードモードへ落ちる直前に呼ぶ (§6.4.6)。ホストには切断として見え、
// かつ PHY 設定を手放すので ROM が自分で構成し直せる。
// tud_disconnect() だけでは D+ プルアップを外すのみで PHY を手放さず、S3 では
// その設定が RTC ドメインに残ってリセット後に ROM がバス上に現れなくなる。
void usb_midi_bridge_teardown(void);
