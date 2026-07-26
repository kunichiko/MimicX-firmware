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
    // TOWNS パッド RUN/SELECT (protocol 0.9)
    case 21: return BTN_RUN;
    case 22: return BTN_SELECT;
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
// MSX マウス: pin 8 (STROBE) のエッジで 4 ニブル (X 上位 / X 下位 / Y 上位 / Y 下位)
// を順次 D0-D3 (pin 1-4) に出力する。pin 6 (D4) = 左ボタン, pin 7 (D5) = 右ボタン
// は静的に駆動 (常時アクティブ-Low)。3ms 以上 STROBE がトグルしなければアイドル
// と見なし、累積デルタを 0 クリアして次サイクルに備える。
// ---------------------------------------------------------------------------
//
// プロトコル (msx.org wiki + コミュニティ実装からの整理):
//   - byte 1 = X delta (8bit signed)、byte 2 = Y delta (8bit signed)
//   - 各 byte を 2 ニブルに分けて MSB ニブルから出力
//   - 順序: X 上位 → X 下位 → Y 上位 → Y 下位
//   - 配線は joystick と同じ active-low (bit=1 → pin LOW, bit=0 → pin Hi-Z)
//
// DMA モデル:
//   rising_lut[2]  = { X 上位, Y 上位 }   (TH rising edge で順次出力)
//   falling_lut[2] = { X 下位, Y 下位 }   (TH falling edge で順次出力)
// → 4 エッジで 1 サイクル完結。アイドル後は両 DMA ポインタが先頭に戻る。
// ---------------------------------------------------------------------------

// 累積デルタ (CC 受信時に更新、サイクル完了時に 0 クリア)。
// MSX 側がいま読んでいる現在値を直接保持する形にした。CC が来たら on_cc で
// 即時加算し LUT もすぐ書き換える。サイクル完了で 0 クリアすることで連続
// ポーリングでもドリフトしない。
static volatile int8_t  msx_dx_acc = 0;
static volatile int8_t  msx_dy_acc = 0;
// 1 MSX 読み取りサイクル = 4 byte (= 8 nibble = 4 H + 4 L) で 4 fallings を含む。
// 旧 Original protocol では mouse は byte 3/4 を要求されたら byte 1/2 を繰り返し
// 返す挙動が標準。MSX View は 8 nibble 読みでこの繰り返しを期待していると思われる
// ため、サイクル末 (4 falling 目) で初めて acc を 0 リセットして次サイクルへ。
// 2 falling 目で mid-cycle にリセットすると byte 3/4 = 0 になり、MSX View 側で
// byte 1/2 と byte 3/4 の不一致から異常動作 (右下ドリフト) する。
// falling のみを数える理由は、初期 strobe 状態 (HIGH/LOW) に依存しないため。
static volatile uint8_t msx_falling_count = 0;
// ボタン状態 (1 = 押下, 0 = リリース)。
static volatile uint8_t msx_btn_left  = 0;
static volatile uint8_t msx_btn_right = 0;

// LUT (BSHR format、上位 16bit = LOW にするピン / 下位 16bit = Hi-Z にするピン)。
// D4/D5 (ボタン) のビットは含めず、D0-D3 (ニブル) だけ書き換える。これによって
// DMA がニブルを更新してもボタンピンは静的状態を維持する。
static volatile uint32_t msx_lut_rising[2];
static volatile uint32_t msx_lut_falling[2];

// 4bit ニブルを D0-D3 (PA7/PA5/PA3/PA2) の BSHR 値にパックする。
// active-low: bit=1 → 該当ピンを LOW (BCR 側)、bit=0 → Hi-Z (BSR 側)。
static uint32_t msx_pack_nibble(uint8_t nibble) {
    static const uint8_t pins[4] = { PIN_D0, PIN_D1, PIN_D2, PIN_D3 };
    uint32_t set = 0, reset = 0;
    for (int b = 0; b < 4; b++) {
        uint32_t bit = 1u << pins[b];
        if (nibble & (1 << b)) reset |= bit;  // active (LOW)
        else                   set   |= bit;  // inactive (Hi-Z)
    }
    return set | (reset << 16);
}

// 累積デルタから LUT を再構築。CC 受信時 / アイドル 0 クリア時 / モード切替時に呼ぶ。
//
// 重要: MSX BIOS は読み取り生バイトを 2 の補数で 否定 (NEG) してデルタを返す。
//   PAD(13) = -byte_raw_signed
// 「マウス未接続」状態は D0-D3 が全 HIGH (pull-up) → BIOS reads 0xFF (= -1) →
// NEG = +1 と読まれてしまい、毎サイクル +1 ずつ右下にドリフトする。
//
// real mouse は「動きなし = byte 0x00」を返す = 全 D 端子 LOW である必要がある。
// 我々の pack_nibble は active-low (bit=1 → pin LOW) なので、BIOS が読む生バイト
// は encode 値の bitwise NOT になる。よって BIOS が見るバイト = ~V、NEG 後の
// PAD = V + 1 となる。PAD = acc にしたければ V = acc - 1 を encode する。
//
//   acc =  0 → V = -1 = 0xFF → 全 D LOW → BIOS reads 0x00 → NEG = 0 (no drift)
//   acc = +5 → V = +4 = 0x04 → BIOS reads 0xFB → NEG = +5
//   acc = -3 → V = -4 = 0xFC → BIOS reads 0x03 → NEG = -3
static void msx_rebuild_lut(void) {
    // (int) 経由で減算してオーバーフロー UB を避け、最後に 8bit に切り詰める。
    uint8_t x = (uint8_t)((int)msx_dx_acc - 1);
    uint8_t y = (uint8_t)((int)msx_dy_acc - 1);
    msx_lut_rising[0]  = msx_pack_nibble((x >> 4) & 0x0F);  // X 上位
    msx_lut_falling[0] = msx_pack_nibble(x & 0x0F);          // X 下位
    msx_lut_rising[1]  = msx_pack_nibble((y >> 4) & 0x0F);  // Y 上位
    msx_lut_falling[1] = msx_pack_nibble(y & 0x0F);          // Y 下位
}

// 左右ボタンを D4/D5 ピンに反映する (active-low、押下中は LOW)。
// MSX マウスモード時のみ呼び出す。
static void msx_update_button_pins(void) {
    if (msx_btn_left)  GPIOA->BCR  = (1 << PIN_D4);  // LOW
    else               GPIOA->BSHR = (1 << PIN_D4);  // Hi-Z
    if (msx_btn_right) GPIOA->BCR  = (1 << PIN_D5);
    else               GPIOA->BSHR = (1 << PIN_D5);
}

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
    uint8_t up    = btn_state[BTN_UP];
    uint8_t down  = btn_state[BTN_DOWN];
    uint8_t left  = btn_state[BTN_LEFT];
    uint8_t right = btn_state[BTN_RIGHT];

    // SOCD ガード (protocol 0.9 §4.2.1): 方向ノート単独での左右同時・上下同時は
    // 両方ニュートラルにする。ホスト側入力の一瞬の同時押しが TOWNS パッドの
    // RUN (左右同時) / SELECT (上下同時) として誤検出されるのを防ぐ。
    if (left && right) { left = 0; right = 0; }
    if (up && down)    { up = 0;   down = 0; }

    // TOWNS パッド RUN/SELECT: 意図的な同時アサートは専用ノート (21/22) で
    // 表現され、通常方向と OR で重畳する。
    if (btn_state[BTN_RUN])    { left = 1; right = 1; }
    if (btn_state[BTN_SELECT]) { up = 1;   down = 1; }

    set_pin(PIN_D0, up);
    set_pin(PIN_D1, down);
    set_pin(PIN_D2, left);
    set_pin(PIN_D3, right);
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
    // フラグの読み取りはクリア前に行う必要がある (どちらのエッジが起きたかの判定)。
    uint32_t intfr = TIM2->INTFR;
    TIM2->INTFR = ~(TIM_CC1IF | TIM_CC2IF);
    if (pad_mode == PAD_MODE_MSX_MOUSE) {
        // CC1 = falling edge (CC1P=1 で設定)。
        // MSX View が観測する 1 サイクル = 8 nibble (4 byte) = 4 falling。
        // mid-cycle (2 falling 目) で acc を 0 にすると byte 3/4 が 0 になり
        // MSX View 側で異常動作する (右下ドリフト) ため、4 falling 目 (= 完全
        // サイクル末) で初めて acc クリアする。DMA は 2 エントリ循環なので
        // 自然に byte 1/2 のニブルが byte 3/4 でも繰り返し返り、Original
        // protocol mouse の標準挙動 (byte 3/4 で byte 1/2 を繰り返す) と一致。
        if (intfr & TIM_CC1IF) {
            if (++msx_falling_count >= 4) {
                msx_falling_count = 0;
                msx_dx_acc = 0;
                msx_dy_acc = 0;
                msx_rebuild_lut();
                // DMA ポインタを先頭に戻す。3 エッジサイクル (初回の SET HIGH
                // が edge を生まないケース) で rising 側ポインタがずれることが
                // あるため、毎サイクル末で位置同期する。
                md6_dma_reset();
            }
        }
    }
    // TIM3 (watchdog) を再スタート
    TIM3->CNT = 0;
    TIM3->CTLR1 |= TIM_CEN;
}

void TIM3_IRQHandler(void) __attribute__((interrupt));
void TIM3_IRQHandler(void) {
    if (TIM3->INTFR & TIM_UIF) {
        TIM3->INTFR = ~TIM_UIF;
        if (pad_mode == PAD_MODE_MSX_MOUSE) {
            // 3ms アイドル: MSX 側がサイクル間に入ったとみなす。
            // 2-falling コミット (TIM2 CC ISR) が何らかの race / 取りこぼしで
            // acc を 0 にできなかった場合の保険として、ここでも acc を 0 にする。
            // 通常の MSX サイクル内 inter-strobe は最大 ~200us なのでアイドル
            // 3ms はサイクル中には発火しない。
            msx_falling_count = 0;
            msx_dx_acc = 0;
            msx_dy_acc = 0;
            msx_rebuild_lut();
        }
        // DMA ポインタを先頭 (X 上位 / X 下位) に戻す。
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
    } else if (mode == PAD_MODE_MSX_MOUSE) {
        // MSX マウス: 4 ニブル循環 (rising/falling 各 2 エントリ)
        th_dma_src_falling = msx_lut_falling;
        th_dma_src_rising  = msx_lut_rising;
        th_dma_count       = 2;
        msx_dx_acc = 0;
        msx_dy_acc = 0;
        msx_falling_count = 0;
        msx_btn_left = 0;
        msx_btn_right = 0;
        msx_rebuild_lut();
        msx_update_button_pins();
        md6_dma_setup();   // TIM2 + DMA + TIM3 watchdog を共有
        // MSX マウスのアイドルタイムアウトは 3ms (MD6 / LR の 1.8ms から上書き)
        TIM3->ATRLR = 3000 - 1;
        // 初期出力: TH の現在状態に応じて X 上位 (rising[0]) または X 下位 (falling[0])
        uint8_t th_high = (GPIOA->INDR >> PIN_TH) & 1;
        GPIOA->BSHR = th_high ? msx_lut_rising[0] : msx_lut_falling[0];
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

// MSX マウスモード時の MIDI ノート (joystick ボタン Note 1-18 と衝突しない値)
#define NOTE_MSX_MOUSE_LEFT   19
#define NOTE_MSX_MOUSE_RIGHT  20

// MSX マウスモード時の CC (x68k_mouse と同じく value=64 でデルタ 0、
// 0..127 が -64..+63 のオフセット表現)
#define CC_MSX_DX  0x30
#define CC_MSX_DY  0x31

static void joystick_fn_init(void) {
    joystick_init();
    // デフォルトは ATARI モード
    joystick_set_mode(PAD_MODE_ATARI);
}

static void joystick_fn_release_all(void) {
    joystick_release_all();
    if (pad_mode == PAD_MODE_MSX_MOUSE) {
        msx_btn_left = 0;
        msx_btn_right = 0;
        msx_dx_acc = 0;
        msx_dy_acc = 0;
        msx_rebuild_lut();
        msx_update_button_pins();
    }
}

static void joystick_fn_on_note_on(uint8_t note, uint8_t velocity) {
    (void)velocity;
    if (pad_mode == PAD_MODE_MSX_MOUSE) {
        // MSX マウスモードでは joystick ボタン Note は黙殺し、左右マウスボタンのみ受ける
        if (note == NOTE_MSX_MOUSE_LEFT)  { msx_btn_left  = 1; msx_update_button_pins(); }
        else if (note == NOTE_MSX_MOUSE_RIGHT) { msx_btn_right = 1; msx_update_button_pins(); }
        return;
    }
    joystick_set_button_by_note(note, 1);
}

static void joystick_fn_on_note_off(uint8_t note) {
    if (pad_mode == PAD_MODE_MSX_MOUSE) {
        if (note == NOTE_MSX_MOUSE_LEFT)  { msx_btn_left  = 0; msx_update_button_pins(); }
        else if (note == NOTE_MSX_MOUSE_RIGHT) { msx_btn_right = 0; msx_update_button_pins(); }
        return;
    }
    joystick_set_button_by_note(note, 0);
}

static void joystick_fn_on_cc(uint8_t cc, uint8_t value) {
    // MSX マウスモード時のみ DX/DY を受け付け、累積デルタへ加算する。
    // value はオフセット表現 (64 = 0、0 = -64、127 = +63)。
    // acc を即座に更新し LUT も書き換える。サイクル完了時 (TIM2 CC ISR で
    // 2 falling 検出) に 0 クリアされるため、次サイクル以降ドリフトしない。
    if (pad_mode != PAD_MODE_MSX_MOUSE) return;
    int16_t delta = (int16_t)value - 64;
    if (cc == CC_MSX_DX) {
        int16_t sum = (int16_t)msx_dx_acc + delta;
        if (sum > 127)  sum = 127;
        if (sum < -128) sum = -128;
        msx_dx_acc = (int8_t)sum;
    } else if (cc == CC_MSX_DY) {
        int16_t sum = (int16_t)msx_dy_acc + delta;
        if (sum > 127)  sum = 127;
        if (sum < -128) sum = -128;
        msx_dy_acc = (int8_t)sum;
    } else {
        return;
    }
    msx_rebuild_lut();
}

static uint8_t joystick_fn_on_set_config(uint8_t key, const uint8_t* val, int len) {
    if (key != CONFIG_PAD_MODE) return ACK_STATUS_UNKNOWN_KEY;
    if (len < 1) return ACK_STATUS_INVALID_VALUE;
    uint8_t mode = val[0];
    if (mode != PAD_MODE_ATARI &&
        mode != PAD_MODE_MD6 &&
        mode != PAD_MODE_LIBBLE_RABBLE &&
        mode != PAD_MODE_MSX_MOUSE) {
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
    .on_cc             = joystick_fn_on_cc,
    .on_set_config     = joystick_fn_on_set_config,
    .poll              = joystick_fn_poll,
    .append_capabilities = joystick_fn_append_capabilities,
};
