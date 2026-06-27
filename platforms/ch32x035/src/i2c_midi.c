// ===================================================================================
// i2c_midi.c  (CH32X035 / MIDI-over-I2C スレーブ, protocol §2.5)
// ===================================================================================
// I2C1 をスレーブに設定し、受信した 1 トランザクション ([LEN][MIDI bytes]) を
// コールバックへ渡す。I2C ペリフェラル設定は ch32v003fun の i2c_slave サンプルに準拠。
//
// ⚠ ハード確認ポイント:
//   - I2C1 remap option 3 (SCL→PC19/pin24, SDA→PC18/pin25) を AFIO_PCFR1[4:2]=3 で選択。
//     CH32X035 データシートの remap 表と要照合 (本実装は pin 機能名 "SCL_3/SDA_3" から推定)。
//   - PC18/PC19 は SWD(DIO/DCK) と兼用。I2C 動作中は SWD 書き込み不可。
// ===================================================================================
#include "i2c_midi.h"
#include "ch32fun.h"
#include "funconfig.h"

// このトランスポートは 2 チップ構成専用。MIMICX_I2C 未定義のビルドでは空にする
// (PC18/PC19 = SWD ピンを I2C に使うため、有効化は I2C 版ビルドのみ)。
#ifdef MIMICX_I2C

#ifndef FUNCONF_SYSTEM_CORE_CLOCK
#define FUNCONF_SYSTEM_CORE_CLOCK 48000000
#endif

#define I2C_RX_BUF_SIZE 80

static volatile uint8_t  rx_buf[I2C_RX_BUF_SIZE];
static volatile uint8_t  rx_len;
static void (*s_on_frame)(const uint8_t* midi, int len);

// ---------------------------------------------------------------------------
// 初期化
// ---------------------------------------------------------------------------
void i2c_midi_init(uint8_t addr7, void (*on_frame)(const uint8_t* midi, int len)) {
    s_on_frame = on_frame;
    rx_len = 0;

    // --- GPIO: PC18(SDA)/PC19(SCL) を AF オープンドレインに (高位ピンは CFGXR) ---
    RCC->APB2PCENR |= RCC_IOPCEN | RCC_AFIOEN;
    // 各ピン 4bit: (MODE=50MHz | CNF=AF open-drain)。pin18→bit8-11, pin19→bit12-15。
    const uint32_t af_od = (GPIO_Speed_50MHz | GPIO_CNF_OUT_OD_AF);
    GPIOC->CFGXR &= ~((0xfu << ((18 - 16) * 4)) | (0xfu << ((19 - 16) * 4)));
    GPIOC->CFGXR |=  (af_od << ((18 - 16) * 4)) | (af_od << ((19 - 16) * 4));

    // --- I2C1 remap option 3 (SCL→PC19, SDA→PC18) ---
    AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_I2C1_REMAP) | (0x3u << 2);

    // --- I2C1 ペリフェラル (slave) ---
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;
    RCC->APB1PRSTR |=  RCC_APB1Periph_I2C1;
    RCC->APB1PRSTR &= ~RCC_APB1Periph_I2C1;

    I2C1->CTLR1 |= I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    // 入力クロック周波数 (MHz 値を FREQ[5:0] に)
    I2C1->CTLR2 |= (FUNCONF_SYSTEM_CORE_CLOCK / 1000000) & I2C_CTLR2_FREQ;
    // 割り込み有効 (バイト/イベント/エラー)
    I2C1->CTLR2 |= I2C_CTLR2_ITBUFEN | I2C_CTLR2_ITEVTEN | I2C_CTLR2_ITERREN;

    NVIC_EnableIRQ(I2C1_EV_IRQn);
    NVIC_SetPriority(I2C1_EV_IRQn, 2 << 4);
    NVIC_EnableIRQ(I2C1_ER_IRQn);
    NVIC_SetPriority(I2C1_ER_IRQn, 2 << 4);

    // バスクロック 400kHz (Fast mode, 33% duty)
    uint32_t clockrate = 400000;
    I2C1->CKCFGR = ((FUNCONF_SYSTEM_CORE_CLOCK / (3 * clockrate)) & I2C_CKCFGR_CCR) | I2C_CKCFGR_FS;

    I2C1->OADDR1 = ((uint16_t)addr7) << 1;
    I2C1->OADDR2 = 0;

    I2C1->CTLR1 |= I2C_CTLR1_PE;    // ペリフェラル有効
    I2C1->CTLR1 |= I2C_CTLR1_ACK;   // 受信バイトに ACK
}

void i2c_midi_enqueue(const uint8_t* midi, int len) {
    // Phase 2 で device→host キュー + INT に接続予定。Phase 1 では何もしない。
    (void)midi; (void)len;
}

// ---------------------------------------------------------------------------
// 割り込みハンドラ
// ---------------------------------------------------------------------------
void I2C1_EV_IRQHandler(void) __attribute__((interrupt));
void I2C1_EV_IRQHandler(void) {
    uint16_t star1 = I2C1->STAR1;
    (void)I2C1->STAR2;   // ADDR クリアには STAR1→STAR2 の読み出しが必要

    if (star1 & I2C_STAR1_ADDR) {
        // アドレス一致 (write/read 開始)。受信バッファをリセット。
        rx_len = 0;
    }

    if (star1 & I2C_STAR1_RXNE) {
        // マスターからの書き込みバイト
        uint8_t b = (uint8_t)I2C1->DATAR;
        if (rx_len < I2C_RX_BUF_SIZE) rx_buf[rx_len++] = b;
    }

    if (star1 & I2C_STAR1_TXE) {
        // マスターからの読み出し。Phase 1 では device→host 未実装 → 0 (LEN=0=データ無し)。
        I2C1->DATAR = 0x00;
    }

    if (star1 & I2C_STAR1_STOPF) {
        // STOP: write トランザクション完了。CTLR1 への書き込みで STOPF をクリア。
        I2C1->CTLR1 |= I2C_CTLR1_PE;
        // フレーム = [LEN][MIDI bytes]。先頭 LEN を除いた MIDI バイト列を渡す。
        if (rx_len >= 2 && s_on_frame) {
            uint8_t declared = rx_buf[0];
            int n = rx_len - 1;
            if (declared < n) n = declared;   // LEN を上限に
            s_on_frame((const uint8_t*)&rx_buf[1], n);
        }
        rx_len = 0;
    }
}

void I2C1_ER_IRQHandler(void) __attribute__((interrupt));
void I2C1_ER_IRQHandler(void) {
    uint16_t star1 = I2C1->STAR1;
    if (star1 & I2C_STAR1_BERR) I2C1->STAR1 &= ~I2C_STAR1_BERR;
    if (star1 & I2C_STAR1_ARLO) I2C1->STAR1 &= ~I2C_STAR1_ARLO;
    if (star1 & I2C_STAR1_AF)   I2C1->STAR1 &= ~I2C_STAR1_AF;
}

#endif // MIMICX_I2C
