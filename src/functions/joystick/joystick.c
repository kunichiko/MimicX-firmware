// ===================================================================================
// Joystick / Gamepad emulation for ATARI and Mega Drive 6-button
// ===================================================================================
// ATARI mode:  GPIO direct output (open-drain)
// MD 6B mode:  TH (PA0) edge interrupt driven, 8-step cycle with 1.8ms timeout
// ===================================================================================

// MD 6B の TH エッジ応答性を高めるため、このファイルだけ O3 で最適化
#pragma GCC optimize ("O3")

#include "joystick.h"
#include "ch32fun.h"
#include "funconfig.h"

// ---------------------------------------------------------------------------
// ボタン状態 (全モード共通、1=押下, 0=解放)
// ---------------------------------------------------------------------------
static volatile uint8_t btn_state[BTN_COUNT];
static volatile uint8_t pad_mode = PAD_MODE_ATARI;

// MD 6B: TH サイクルカウンタ (0-7)
static volatile uint8_t th_cycle;

// MIDI Note → ボタンインデックス変換テーブル
static int note_to_btn(uint8_t note) {
    switch (note) {
    case 1:  return BTN_UP;
    case 2:  return BTN_DOWN;
    case 3:  return BTN_LEFT;
    case 4:  return BTN_RIGHT;
    case 6:  return BTN_A;
    case 7:  return BTN_B;
    case 9:  return BTN_C;
    case 10: return BTN_START;
    case 11: return BTN_X;
    case 12: return BTN_Y;
    case 13: return BTN_Z;
    case 14: return BTN_MODE;
    // 右 D-pad (リブルラブル XPD-1LR の右レバー)
    case 15: return BTN_UP2;
    case 16: return BTN_DOWN2;
    case 17: return BTN_LEFT2;
    case 18: return BTN_RIGHT2;
    default: return -1;
    }
}

// ---------------------------------------------------------------------------
// GPIO 出力ヘルパー
// ---------------------------------------------------------------------------

// PA の指定ピンを Low (アクティブ) または Hi-Z (非アクティブ) にする
// active-low: pressed(1) → Low, released(0) → Hi-Z
static inline void set_pin(uint8_t pin, uint8_t active) {
    if (active) {
        GPIOA->BCR = (1 << pin);
    } else {
        GPIOA->BSHR = (1 << pin);
    }
}

// D0-D5 を一括設定 (割り込みハンドラ用、高速)
// 各ビットが 1 = Low (アクティブ), 0 = Hi-Z
static inline void set_d0_d5(uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4, uint8_t d5) {
    // BCR でセット (Low出力)、BSHR でクリア (Hi-Z)
    uint32_t bcr = 0;  // Low にするピン
    uint32_t bshr = 0; // High にするピン

    if (d0) bcr |= (1 << PIN_D0); else bshr |= (1 << PIN_D0);
    if (d1) bcr |= (1 << PIN_D1); else bshr |= (1 << PIN_D1);
    if (d2) bcr |= (1 << PIN_D2); else bshr |= (1 << PIN_D2);
    if (d3) bcr |= (1 << PIN_D3); else bshr |= (1 << PIN_D3);
    if (d4) bcr |= (1 << PIN_D4); else bshr |= (1 << PIN_D4);
    if (d5) bcr |= (1 << PIN_D5); else bshr |= (1 << PIN_D5);

    GPIOA->BCR = bcr;
    GPIOA->BSHR = bshr;
}

// ---------------------------------------------------------------------------
// MD 6B: 事前計算ルックアップテーブル
// ---------------------------------------------------------------------------
// インデックス 0..7 = step 1..8 (TH の各エッジに対応)
// step 0 = TH=LOW (Step 1)
// step 1 = TH=HIGH (Step 2)
// step 2 = TH=LOW (Step 3)
// step 3 = TH=HIGH (Step 4)
// step 4 = TH=LOW (Step 5, 6B 識別)
// step 5 = TH=HIGH (Step 6, 拡張ボタン)
// step 6 = TH=LOW (Step 7, 確認)
// step 7 = TH=HIGH (Step 8)
//
// 値は BSHR への単一 32bit 書き込み:
//   下位 16bit: BSx (Hi-Z にするピン) — オープンドレイン出力で High = Hi-Z
//   上位 16bit: BCx (Low にするピン)
static volatile uint32_t md6_lut[8];

// 両エッジ DMA 用に分割した LUT:
//   md6_lut_falling[0..3] = md6_lut[0,2,4,6] (TH=LOW のステップ)
//   md6_lut_rising[0..3]  = md6_lut[1,3,5,7] (TH=HIGH のステップ)
static volatile uint32_t md6_lut_falling[4];
static volatile uint32_t md6_lut_rising[4];

#define D_PINS_MASK ((1<<PIN_D0)|(1<<PIN_D1)|(1<<PIN_D2)|(1<<PIN_D3)|(1<<PIN_D4)|(1<<PIN_D5))

// ---------------------------------------------------------------------------
// リブルラブル XPD-1LR: 左右レバーを TH で時分割多重
// ---------------------------------------------------------------------------
// 純正 XPD-1LR は pin 8 (TH/COMMON) をそのまま左レバーの COMMON に、
// インバータ反転信号を右レバーの COMMON にしている。本ファームでは
// TH のエッジでバンクを切り替え、左右レバー状態を D0-D3 に乗せ替える。
//   TH = LOW (falling edge) → 左レバー (BTN_UP/DOWN/LEFT/RIGHT)
//   TH = HIGH (rising edge) → 右レバー (BTN_UP2/DOWN2/LEFT2/RIGHT2)
// A/B ボタン (D4/D5) は左右共通で常時出力。
static volatile uint32_t lr_lut_falling[1];
static volatile uint32_t lr_lut_rising[1];

// D0..D5 の active 配列から BSHR フォーマット (set | reset<<16) を組み立てる
static inline uint32_t pack_d_bshr(uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3,
                                   uint8_t d4, uint8_t d5) {
    static const uint8_t pins[6] = {PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5};
    const uint8_t vals[6] = {d0, d1, d2, d3, d4, d5};
    uint32_t set = 0, reset = 0;
    for (int p = 0; p < 6; p++) {
        uint32_t bit = 1u << pins[p];
        if (vals[p]) reset |= bit;   // active = Low (BCR 部)
        else         set   |= bit;   // inactive = Hi-Z (BSR 部)
    }
    return set | (reset << 16);
}

static void lr_rebuild_lut(void) {
    uint8_t a = btn_state[BTN_A];
    uint8_t b = btn_state[BTN_B];

    // TH=LOW: 左 D-pad
    lr_lut_falling[0] = pack_d_bshr(
        btn_state[BTN_UP], btn_state[BTN_DOWN],
        btn_state[BTN_LEFT], btn_state[BTN_RIGHT],
        a, b);
    // TH=HIGH: 右 D-pad
    lr_lut_rising[0] = pack_d_bshr(
        btn_state[BTN_UP2], btn_state[BTN_DOWN2],
        btn_state[BTN_LEFT2], btn_state[BTN_RIGHT2],
        a, b);
}

// btn_state を元に LUT を再計算する
// active=1 のビットを BCR (Low)、active=0 を BSHR (Hi-Z) に振り分ける
static void md6_rebuild_lut(void) {
    uint8_t up    = btn_state[BTN_UP];
    uint8_t down  = btn_state[BTN_DOWN];
    uint8_t left  = btn_state[BTN_LEFT];
    uint8_t right = btn_state[BTN_RIGHT];
    uint8_t a     = btn_state[BTN_A];
    uint8_t b     = btn_state[BTN_B];
    uint8_t c     = btn_state[BTN_C];
    uint8_t start = btn_state[BTN_START];
    uint8_t x     = btn_state[BTN_X];
    uint8_t y     = btn_state[BTN_Y];
    uint8_t z     = btn_state[BTN_Z];
    uint8_t mode  = btn_state[BTN_MODE];

    // d0..d5 各ピンの active 値を 8 ステップ分定義
    // step[i][0..5] = D0..D5 の active 値 (1=Low, 0=Hi-Z)
    static const uint8_t pins[6] = {PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5};

    // ステップごとの値テーブルを構築
    uint8_t step_vals[8][6] = {
        // Step 1 (cycle 0, LOW): Up, Down, 0, 0, A, Start  ※D2/D3 は固定 Low (=1)
        { up, down, 1, 1, a, start },
        // Step 2 (cycle 0, HIGH): Up, Down, Left, Right, B, C
        { up, down, left, right, b, c },
        // Step 3 (cycle 1, LOW)
        { up, down, 1, 1, a, start },
        // Step 4 (cycle 1, HIGH)
        { up, down, left, right, b, c },
        // Step 5 (cycle 2, LOW): D0-D3 全 Low (=1) = 6B 識別
        { 1, 1, 1, 1, a, start },
        // Step 6 (cycle 2, HIGH): Z, Y, X, Mode, 1(Hi-Z), 1(Hi-Z)
        { z, y, x, mode, 0, 0 },
        // Step 7 (cycle 3, LOW): D0-D3 全 Hi-Z (=0) = 確認
        { 0, 0, 0, 0, a, start },
        // Step 8 (cycle 3, HIGH): 通常状態に復帰
        { up, down, left, right, b, c },
    };

    // BSHR (= BSRR) フォーマット: 上位16bit = Low にするピン, 下位16bit = High (Hi-Z) にするピン
    for (int s = 0; s < 8; s++) {
        uint32_t set = 0, reset = 0;
        for (int p = 0; p < 6; p++) {
            uint32_t bit = 1u << pins[p];
            if (step_vals[s][p]) reset |= bit;  // active = Low
            else                 set   |= bit;  // inactive = Hi-Z (Pull-up で High)
        }
        md6_lut[s] = set | (reset << 16);
    }

    // 両エッジ DMA 用の分割 LUT を生成
    md6_lut_falling[0] = md6_lut[0];  // Step 1
    md6_lut_falling[1] = md6_lut[2];  // Step 3
    md6_lut_falling[2] = md6_lut[4];  // Step 5
    md6_lut_falling[3] = md6_lut[6];  // Step 7
    md6_lut_rising[0]  = md6_lut[1];  // Step 2
    md6_lut_rising[1]  = md6_lut[3];  // Step 4
    md6_lut_rising[2]  = md6_lut[5];  // Step 6
    md6_lut_rising[3]  = md6_lut[7];  // Step 8
}

// ---------------------------------------------------------------------------
// ATARI モード: GPIO 直接出力
// ---------------------------------------------------------------------------

static void atari_update_gpio(void) {
    set_pin(PIN_D0, btn_state[BTN_UP]);
    set_pin(PIN_D1, btn_state[BTN_DOWN]);
    set_pin(PIN_D2, btn_state[BTN_LEFT]);
    set_pin(PIN_D3, btn_state[BTN_RIGHT]);
    set_pin(PIN_D4, btn_state[BTN_A]);
    set_pin(PIN_D5, btn_state[BTN_B]);
}

// ---------------------------------------------------------------------------
// MD 6B: DMA + TIM2 input capture によるハードウェア駆動方式
// ---------------------------------------------------------------------------
// 構成:
//   PA0 (TH) → TIM2_CH1 input capture (両エッジ)
//   TIM2_CH1 capture event → DMA1_Channel5 trigger
//   DMA1_Channel5: md6_lut[0..7] → GPIOA->BSHR を循環転送 (32bit, M2P)
//
// CPU 介入なしで TH の各エッジで GPIOA->BSHR が更新されるため
// レイテンシは数サイクル (< 100ns) のはず。
//
// アイドル復帰時の DMA ポインタリセットは EXTI ISR で行う。
// ---------------------------------------------------------------------------

// DMA 設定の共通関数: チャネルに転送設定を入れて起動
static void dma_channel_setup(DMA_Channel_TypeDef* ch, void* src, int count) {
    ch->CFGR  = 0;
    ch->PADDR = (uint32_t)&GPIOA->BSHR;
    ch->MADDR = (uint32_t)src;
    ch->CNTR  = count;
    ch->CFGR =
        (1 << 4)  | // DIR (memory → peripheral)
        (1 << 5)  | // CIRC (circular)
        (0 << 6)  | // PINC = 0
        (1 << 7)  | // MINC = 1
        (2 << 8)  | // PSIZE = 32bit
        (2 << 10) | // MSIZE = 32bit
        (3 << 12) | // PL = very high
        (1 << 0);   // EN = 1
}

// TH エッジ駆動 DMA で使う現在のソース。
// MD6 は 4 ステップ循環、リブルラブルは 1 エントリ循環 (CNTR=1 で常に同じ位置)。
static volatile uint32_t* th_dma_src_falling = md6_lut_falling;
static volatile uint32_t* th_dma_src_rising  = md6_lut_rising;
static uint8_t th_dma_count = 4;

// DMA ポインタを先頭に戻す。MD6 は 8 ステップサイクルの先頭、リブルラブルは
// 唯一のエントリへ。circular DMA なので CNTR=1 のリブルラブルでは事実上 no-op。
static void md6_dma_reset(void) {
    DMA1_Channel5->CFGR &= ~1;
    DMA1_Channel5->CNTR  = th_dma_count;
    DMA1_Channel5->MADDR = (uint32_t)th_dma_src_falling;
    DMA1_Channel5->CFGR |= 1;

    DMA1_Channel7->CFGR &= ~1;
    DMA1_Channel7->CNTR  = th_dma_count;
    DMA1_Channel7->MADDR = (uint32_t)th_dma_src_rising;
    DMA1_Channel7->CFGR |= 1;
}

// TIM3: アイドル検出 watchdog (1.8ms)
//   TIM2 のキャプチャ ISR が CNT=0 にリセットして再スタート
//   TIM3 が overflow (= 1.8ms 経過) で割り込み → DMA ポインタを先頭に戻す
static void md6_watchdog_setup(void) {
    RCC->APB1PCENR |= RCC_TIM3EN;
    TIM3->CTLR1 = 0;
    TIM3->PSC   = 48 - 1;       // 48MHz / 48 = 1MHz (1us tick)
    TIM3->ATRLR = 1800 - 1;     // 1.8ms で overflow
    TIM3->CTLR1 |= TIM_OPM;     // One-Pulse Mode (overflow で停止)
    TIM3->INTFR = 0;
    TIM3->DMAINTENR = TIM_UIE;  // update interrupt 有効
    NVIC_EnableIRQ(TIM3_IRQn);
}

// CH32X035 では TIM2 のキャプチャ割り込みは TIM2_CC_IRQHandler (TIM2_CC_IRQn=51)
// TIM2_IRQHandler は Update 専用
void TIM2_CC_IRQHandler(void) __attribute__((interrupt));
void TIM2_CC_IRQHandler(void) {
    // CC1IF + CC2IF をクリア (DMA はハードウェアで処理済み)
    TIM2->INTFR = ~(TIM_CC1IF | TIM_CC2IF);
    // TIM3 (watchdog) を再スタート
    TIM3->CNT = 0;
    TIM3->CTLR1 |= TIM_CEN;
}

void TIM3_IRQHandler(void) __attribute__((interrupt));
void TIM3_IRQHandler(void) {
    if (TIM3->INTFR & TIM_UIF) {
        TIM3->INTFR = ~TIM_UIF;
        // 1.8ms アイドル → DMA ポインタを先頭に戻す
        md6_dma_reset();
    }
}

static void md6_dma_setup(void) {
    // RCC: TIM2 (APB1) と DMA1 (AHB) を有効化
    RCC->APB1PCENR |= RCC_TIM2EN;
    RCC->AHBPCENR  |= RCC_DMA1EN;

    // PA0 を TIM2_CH1 入力に設定
    AFIO->PCFR1 &= ~AFIO_PCFR1_TIM2_REMAP;  // No remap (TIM2_CH1 = PA0)
    GPIOA->CFGLR &= ~(0xf << (4 * PIN_TH));
    GPIOA->CFGLR |= (GPIO_Speed_In | GPIO_CNF_IN_FLOATING) << (4 * PIN_TH);

    // TIM2 設定
    TIM2->CTLR1 = 0;
    TIM2->PSC   = 0;
    TIM2->ATRLR = 0xFFFF;

    // CHCTLR1:
    //   CC1S = 01 (TI1 input) → CC1 が TI1 (PA0) からキャプチャ
    //   CC2S = 10 (TI1 input) → CC2 も TI1 (PA0) からキャプチャ
    TIM2->CHCTLR1 = (0x1 << 0) | (0x2 << 8);

    // CCER:
    //   CC1E + CC1P=1 → CC1 は falling edge でキャプチャ
    //   CC2E + CC2P=0 → CC2 は rising edge でキャプチャ
    TIM2->CCER = (1 << 0) | (1 << 1) | (1 << 4);  // CC1E | CC1P | CC2E

    // DMA on CC1 + CC2 events, plus interrupt to kick the watchdog
    TIM2->DMAINTENR = TIM_CC1DE | TIM_CC2DE | TIM_CC1IE | TIM_CC2IE;
    NVIC_EnableIRQ(TIM2_CC_IRQn);

    // アイドル検出 watchdog (TIM3) を初期化
    md6_watchdog_setup();

    // DMA1_Channel5: TIM2_CH1 (falling) → BSHR
    dma_channel_setup(DMA1_Channel5, (void*)th_dma_src_falling, th_dma_count);
    // DMA1_Channel7: TIM2_CH2 (rising) → BSHR
    dma_channel_setup(DMA1_Channel7, (void*)th_dma_src_rising, th_dma_count);

    // TIM2 開始
    TIM2->CTLR1 |= TIM_CEN;
}

static void md6_dma_disable(void) {
    DMA1_Channel5->CFGR &= ~1;
    DMA1_Channel7->CFGR &= ~1;
    TIM2->CTLR1 &= ~TIM_CEN;
    TIM2->DMAINTENR = 0;
    NVIC_DisableIRQ(TIM2_CC_IRQn);
    TIM3->CTLR1 &= ~TIM_CEN;
    TIM3->DMAINTENR = 0;
    NVIC_DisableIRQ(TIM3_IRQn);
}

// ---------------------------------------------------------------------------
// TH 割り込みハンドラ (EXTI line 0, PA0)
// ---------------------------------------------------------------------------

// EXTI 割り込み (PA0 falling edge) は使用しない (DMA がすべて処理する)
// 古いハンドラを無効化するため空実装
void EXTI7_0_IRQHandler(void) __attribute__((interrupt));
void EXTI7_0_IRQHandler(void) {
    EXTI->INTFR = (1 << PIN_TH);
}

// ---------------------------------------------------------------------------
// TIM2 タイムアウト割り込み (1.8ms でサイクルカウンタリセット)
// ---------------------------------------------------------------------------

// (TIM2 タイマー割り込みは ISR 内ポーリング方式では不要)

// ---------------------------------------------------------------------------
// 初期化
// ---------------------------------------------------------------------------

static void gpio_init_output_pins(void) {
    // D0-D5 をオープンドレイン出力 (3.3V/5V 共用、レトロ PC のプルアップ前提)
    const uint8_t out_pins[] = { PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5 };
    for (int i = 0; i < (int)(sizeof(out_pins) / sizeof(out_pins[0])); i++) {
        uint8_t p = out_pins[i];
        GPIOA->CFGLR &= ~(0xf << (4 * p));
        GPIOA->CFGLR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_OD) << (4 * p);
        GPIOA->BSHR = (1 << p);  // Hi-Z (非アクティブ)
    }
}

static void gpio_init_th_input(void) {
    // PA0 (TH): 入力、プルダウン
    GPIOA->CFGLR &= ~(0xf << (4 * PIN_TH));
    GPIOA->CFGLR |= (GPIO_Speed_In | GPIO_CNF_IN_PUPD) << (4 * PIN_TH);
    GPIOA->BCR = (1 << PIN_TH);  // Pull-Down
}

static void exti_init(void) {
    // PA0 を EXTI0 に接続 (PA = 0)
    AFIO->EXTICR1 = (AFIO->EXTICR1 & ~(0x0F << (4 * PIN_TH))) | (0x00 << (4 * PIN_TH));

    // 立ち下がりエッジのみで割り込み (最初の TH=LOW を捕捉)
    // 残りの 7 ステップは ISR 内のポーリングで処理する
    EXTI->INTENR |= (1 << PIN_TH);
    EXTI->RTENR &= ~(1 << PIN_TH);
    EXTI->FTENR |= (1 << PIN_TH);

    NVIC_EnableIRQ(EXTI7_0_IRQn);
}

void joystick_init(void) {
    for (int i = 0; i < BTN_COUNT; i++) btn_state[i] = 0;
    th_cycle = 0;

    gpio_init_output_pins();
    gpio_init_th_input();
}

void joystick_set_mode(uint8_t mode) {
    pad_mode = mode;
    joystick_release_all();
    th_cycle = 0;

    if (mode == PAD_MODE_MD6) {
        th_dma_src_falling = md6_lut_falling;
        th_dma_src_rising  = md6_lut_rising;
        th_dma_count       = 4;
        md6_rebuild_lut();
        md6_dma_setup();   // TIM2 input capture + DMA 起動
        // exti_init() は呼ばない (DMA だけで処理)
        // 初期出力: 現在の TH 状態に応じて step 0 または 1
        uint8_t th_high = (GPIOA->INDR >> PIN_TH) & 1;
        GPIOA->BSHR = md6_lut[th_high & 1];
    } else if (mode == PAD_MODE_LIBBLE_RABBLE) {
        th_dma_src_falling = lr_lut_falling;
        th_dma_src_rising  = lr_lut_rising;
        th_dma_count       = 1;
        lr_rebuild_lut();
        md6_dma_setup();   // 同じ TIM2 + DMA 機構を使う
        uint8_t th_high = (GPIOA->INDR >> PIN_TH) & 1;
        GPIOA->BSHR = th_high ? lr_lut_rising[0] : lr_lut_falling[0];
    } else {
        // DMA + TIM2 + EXTI 無効化
        md6_dma_disable();
        EXTI->INTENR &= ~(1 << PIN_TH);
        NVIC_DisableIRQ(EXTI7_0_IRQn);
        // PA0 を入力に戻す (元の COMMON 入力として)
        gpio_init_th_input();
        atari_update_gpio();
    }
}

uint8_t joystick_get_mode(void) {
    return pad_mode;
}

void joystick_set_button_by_note(uint8_t note, uint8_t pressed) {
    int btn_idx = note_to_btn(note);
    if (btn_idx < 0) return;
    btn_state[btn_idx] = pressed ? 1 : 0;

    if (pad_mode == PAD_MODE_ATARI) {
        atari_update_gpio();
    } else if (pad_mode == PAD_MODE_MD6) {
        // LUT 再構築 (ボタン状態が変わったため)
        md6_rebuild_lut();
        // 現在の TH 状態に応じて即座に出力 (cycle=0 として)
        uint8_t th_high = (GPIOA->INDR >> PIN_TH) & 1;
        GPIOA->BSHR = md6_lut[th_high & 1];
    } else if (pad_mode == PAD_MODE_LIBBLE_RABBLE) {
        lr_rebuild_lut();
        uint8_t th_high = (GPIOA->INDR >> PIN_TH) & 1;
        GPIOA->BSHR = th_high ? lr_lut_rising[0] : lr_lut_falling[0];
    }
}

void joystick_release_all(void) {
    for (int i = 0; i < BTN_COUNT; i++) btn_state[i] = 0;
    if (pad_mode == PAD_MODE_ATARI) {
        atari_update_gpio();
    }
}

void joystick_poll(void) {
    // ATARI モード: 特に何もしない (set_button_by_note で即座に更新済み)
    // MD6 モード: 割り込みハンドラが処理するので何もしない
}

// ---------------------------------------------------------------------------
// hid_function 互換 vtable
// ---------------------------------------------------------------------------

#define MIDI_CH_JOYSTICK 0
#define CONFIG_PAD_MODE  0x03

static void joystick_fn_init(void) {
    joystick_init();
    // デフォルトは ATARI モード
    joystick_set_mode(PAD_MODE_ATARI);
}

static void joystick_fn_release_all(void) {
    joystick_release_all();
}

static void joystick_fn_on_note_on(uint8_t note, uint8_t velocity) {
    (void)velocity;
    joystick_set_button_by_note(note, 1);
}

static void joystick_fn_on_note_off(uint8_t note) {
    joystick_set_button_by_note(note, 0);
}

static uint8_t joystick_fn_on_set_config(uint8_t key, const uint8_t* val, int len) {
    if (key != CONFIG_PAD_MODE) return ACK_STATUS_UNKNOWN_KEY;
    if (len < 1) return ACK_STATUS_INVALID_VALUE;
    uint8_t mode = val[0];
    if (mode != PAD_MODE_ATARI &&
        mode != PAD_MODE_MD6 &&
        mode != PAD_MODE_LIBBLE_RABBLE) {
        return ACK_STATUS_INVALID_VALUE;
    }
    joystick_set_mode(mode);
    return ACK_STATUS_OK;
}

static void joystick_fn_poll(void) {
    joystick_poll();
}

static int joystick_fn_append_capabilities(uint8_t* buf, int max_len) {
    int n = 0;
    // CAP_BUTTON_COUNT (0x01) = 12
    if (n + 3 > max_len) return n;
    buf[n++] = 0x01;
    buf[n++] = 1;
    buf[n++] = 12;
    return n;
}

const hid_function_t joystick_function = {
    .name              = "joystick",
    .hid_type          = HID_TYPE_JOYSTICK,
    .target_system     = TARGET_ATARI,
    .midi_channel      = MIDI_CH_JOYSTICK,
    .init              = joystick_fn_init,
    .release_all       = joystick_fn_release_all,
    .on_note_on        = joystick_fn_on_note_on,
    .on_note_off       = joystick_fn_on_note_off,
    .on_cc             = NULL,
    .on_set_config     = joystick_fn_on_set_config,
    .poll              = joystick_fn_poll,
    .append_capabilities = joystick_fn_append_capabilities,
};
