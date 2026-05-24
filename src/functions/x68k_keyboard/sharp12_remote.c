// ===================================================================================
// sharp12_remote.c
// ===================================================================================
// SHARP 12-bit リモコンコードを PB8 から送出する。ベースバンド (キャリアなし,
// 正論理 / アクティブ High) で、X68000 キーボード mini-DIN pin 6 → 本体背面
// TV CONTROL REMOTE 端子 経由で純正カラーディスプレイテレビ (CZ-607D /
// CZ-614D 等) が受信する。
//
// 参考 (CZ697D-IR-receiver) は IR 受光モジュール出力 (アクティブ Low) を扱う
// プロジェクトだが、キーボード → X68000 → TV のラインは正論理である (実機計測)。
// よって本実装は HIGH = "ON" (データパルス), LOW = "OFF" (ギャップ / アイドル) とする。
//
// アルゴリズムは CZ697D-IR-receiver のリピート送信コードから 38kHz PWM を
// 取り除いて極性を合わせた版。
// ===================================================================================
#include "sharp12_remote.h"
#include "ch32fun.h"
#include "funconfig.h"

// REMOTE = PB8 (push-pull, idle LOW, active HIGH = 正論理)
#define REMOTE_PORT GPIOB
#define REMOTE_PIN  8

static inline void remote_low(void)  { REMOTE_PORT->BCR  = (1 << REMOTE_PIN); }
static inline void remote_high(void) { REMOTE_PORT->BSHR = (1 << REMOTE_PIN); }

void sharp12_remote_init(void) {
    // PB8 は CFGHR (pin 8..15) の bit field
    REMOTE_PORT->CFGHR &= ~(0xfu << (4 * (REMOTE_PIN - 8)));
    REMOTE_PORT->CFGHR |=  ((uint32_t)(GPIO_Speed_2MHz | GPIO_CNF_OUT_PP))
                           << (4 * (REMOTE_PIN - 8));
    remote_low();  // idle (正論理なのでアイドルは LOW)
}

// 1 ビット送出: HIGH を on_us、LOW を off_us。on_us + off_us を返す。
static uint32_t send_pulse(uint32_t on_us, uint32_t off_us) {
    remote_high();
    Delay_Us(on_us);
    remote_low();
    Delay_Us(off_us);
    return on_us + off_us;
}

// 12 bit を LSB first で送出。
static uint32_t send_data12(uint16_t data) {
    uint32_t total = 0;
    for (int i = 0; i < 12; i++) {
        if (data & 1) {
            // データ 1: 320us HIGH + 1680us LOW
            total += send_pulse(320, 1680);
        } else {
            // データ 0: 320us HIGH + 680us LOW
            total += send_pulse(320, 680);
        }
        data >>= 1;
    }
    return total;
}

// データ部送出 + 1 パケット = 50ms になるようトレーラを伸ばす。
//   trailer = HIGH 320us + LOW 残り (50ms - データ部 - 320us)
static void send_one_frame(uint16_t data12) {
    uint32_t len = send_data12(data12);
    // トレーラー: HIGH 320us を打ってから LOW で残時間を埋める
    remote_high();
    Delay_Us(320);
    len += 320;
    int32_t remain = (int32_t)50000 - (int32_t)len;
    if (remain < 1000) remain = 1000;  // 異常時の最低マージン
    remote_low();
    Delay_Us((uint32_t)remain);
}

void sharp12_remote_emit(uint8_t code) {
    // SHARP12 のデータ部 12 bit のレイアウト (LSB first):
    //   [3 bit expansion = 000] [8 bit command] [1 bit padding = 0]
    // 反転側は全 12 bit をビット反転する。
    uint16_t frame   = (uint16_t)((uint16_t)code << 3) & 0x0FFF;
    uint16_t inv     = (uint16_t)((~frame) & 0x0FFF);

    send_one_frame(frame);
    send_one_frame(inv);
}
