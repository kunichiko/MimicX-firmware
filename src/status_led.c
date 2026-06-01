// ===================================================================================
// status_led.c
// ===================================================================================
// PB0 に接続された WS2812B-2020 ドライバ。
//
// ch32v003fun extralibs/ws2812b_simple.h を利用する。SysTick (HCLK=48MHz, 1tick=20.8ns)
// を基準にビットを生成するので、自前で nop 数を合わせる必要がない。'0' bit の HIGH 区間は
// 短いためライブラリ内部で IRQ 抑止される (~100ns)。送信合計時間は 24bit × ~1µs ≒ 24µs。
//
// 前提:
//   funconfig.h で FUNCONF_SYSTICK_USE_HCLK = 1 を設定済 (SysTick = HCLK)。
//
// ハードウェア:
//   MCU PB0 → 470Ω → WS2812B-2020 DI、VDD33 共通。GRB 順 24bit。
// ===================================================================================
#include "status_led.h"
#include "ch32fun.h"
#include "funconfig.h"

#define WS2812BSIMPLE_IMPLEMENTATION
#include "ws2812b_simple.h"

#define LED_PIN  0  // PB0

void status_led_init(void) {
    // ws2812b_simple 側で必要なら CFGLR を設定するため、ここでは LOW 出力にしておくだけ。
    // (WS2812 への >50µs LOW = reset 状態に保つ)
    GPIOB->CFGLR &= ~(0xfu << (4 * LED_PIN));
    GPIOB->CFGLR |=  ((uint32_t)(GPIO_Speed_50MHz | GPIO_CNF_OUT_PP)) << (4 * LED_PIN);
    GPIOB->BCR = (1u << LED_PIN);
}

void status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    // WS2812B-2020 は GRB 順 MSB first。
    uint8_t buf[3] = { g, r, b };
    WS2812BSimpleSend(GPIOB, LED_PIN, buf, sizeof(buf));
    // 念のため latch 時間 (>50µs LOW) を確保。
    Delay_Us(60);
}
