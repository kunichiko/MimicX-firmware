// ===================================================================================
// status_led.h
// ===================================================================================
// PB0 に接続された WS2812B-2020 (シリアル制御 RGB LED) を駆動する。
//
// ハードウェア (MimicX-hardware / atari-joystick, x68000-keyboard 共通):
//   MCU PB0 → 470Ω → WS2812B-2020 DI、VDD33 共通電源。
//
// LED は「アプリ接続状態」をユーザーに視覚的に伝えるためのインジケータ。
// アダプタの内部状態 (status_led_state_t) で色が決まり、その上にホストアプリからの
// override (色 + 点滅) を被せられる構造。
//
// 状態遷移 (status_led_state_t):
//     WAITING   ── IDENTIFY_REQ 受信 ──→ SCANNED
//     SCANNED   ── HEART_BEAT 受信  ──→ CONNECTED
//     CONNECTED ── 3 秒 HB 無し    ──→ WAITING (override も自動 reset)
//     ※ どの状態でも IDENTIFY_REQ で SCANNED に遷移する (rescan = 接続リセット)
//
// 状態色:
//     WAITING   = 黄色 (R=64, G=32, B=0)
//     SCANNED   = 緑   (R=0,  G=64, B=0)
//     CONNECTED = 青   (R=0,  G=0,  B=64)
//
// アクティビティ点滅:
//     CONNECTED 状態でホストから Note/CC を受信すると 4Hz (High) で青点滅。
//     500ms 入力が無ければ青 solid に戻る。Override 中は無効。
//
// Override:
//     status_led_set_override_rgb / set_override_blink で色 + 点滅を上書きする。
//     RGB(255,255,255) を渡すと reset 扱いとなり、状態色に戻り点滅もクリアされる。
//
// HEART_BEAT 監視:
//     status_led_heartbeat() を呼ぶたびに「最後の HB 時刻」を更新する。
//     poll で 3 秒経過を検出したら状態を WAITING に戻し override も自動 reset。
//
// 前提:
//     funconfig.h で FUNCONF_SYSTICK_USE_HCLK = 1 (SysTick = HCLK 48MHz)
// ===================================================================================
#ifndef _STATUS_LED_H
#define _STATUS_LED_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// 点滅速度 (プロトコル CMD_SET_LED_BLINK の speed 値と一致)
// ---------------------------------------------------------------------------
#define LED_BLINK_NONE  0   // 点滅しない (色を保持して常時点灯)
#define LED_BLINK_SLOW  1   // 1 Hz  (500ms ON / 500ms OFF)
#define LED_BLINK_MID   2   // 2 Hz  (250ms ON / 250ms OFF)
#define LED_BLINK_HIGH  3   // 4 Hz  (125ms ON / 125ms OFF)

// ---------------------------------------------------------------------------
// アダプタ接続状態
// ---------------------------------------------------------------------------
typedef enum {
    STATUS_LED_STATE_WAITING   = 0,  // アプリスキャン待ち (黄)
    STATUS_LED_STATE_SCANNED   = 1,  // IDENTIFY 済 (緑)
    STATUS_LED_STATE_CONNECTED = 2,  // HB 受信中 (青 / 操作中は青点滅)
} status_led_state_t;

// GPIO 設定 (PB0 を push-pull 50MHz 出力に) + LED 消灯 (LOW)。
void status_led_init(void);

// メインループから定期呼び出し。HB タイムアウト・点滅 toggle・状態反映を行う。
// 非ブロッキング (LED 更新時のみ ~30µs ブロック)。
void status_led_poll(void);

// ---------------------------------------------------------------------------
// 状態通知 (main.c から呼ぶ)
// ---------------------------------------------------------------------------

// 任意のタイミングで状態を強制設定する (通常は使わない)。
void status_led_set_state(status_led_state_t state);

// IDENTIFY_REQUEST 受信時に呼ぶ。状態を SCANNED に遷移する。
void status_led_on_identify_request(void);

// HEART_BEAT 受信時に呼ぶ。状態を CONNECTED に遷移し、HB タイマーをリセットする。
void status_led_on_heart_beat(void);

// DISCONNECT 受信時に呼ぶ。アプリがデバイス選択を解除した宣言として、
// override をクリアして即座に SCANNED 状態 (緑) に戻す。
void status_led_on_disconnect_request(void);

// CONNECTED 状態で Note/CC を受信したら呼ぶ。500ms 青点滅させる。
// 他の状態では無視される。Override 中も無視される。
void status_led_on_input_activity(void);

// ---------------------------------------------------------------------------
// Override (ホストアプリ → SET_LED / SET_LED_BLINK)
// ---------------------------------------------------------------------------

// 色 override を設定する。8bit RGB。
// RGB が全て 0xFF の場合は reset として扱い、override をクリアする。
void status_led_set_override_rgb(uint8_t r, uint8_t g, uint8_t b);

// 点滅速度 override を設定する。override 色がない状態でも値だけは保持し、
// 次回の色 override 時に適用される。
void status_led_set_override_blink(uint8_t speed);

// Override を明示的にクリアする (HB タイムアウト時に内部からも呼ばれる)。
void status_led_reset_override(void);

#endif
