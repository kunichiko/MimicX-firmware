// ===================================================================================
// i2c_midi.c  (CH32X035 / MIDI-over-I2C スレーブ, protocol §2.5)
// ===================================================================================
// I2C1 をスレーブに設定し、MimicX の MIDI/SysEx バイト列を双方向に運ぶ。
//   - 受信 (host→device): write トランザクション [LEN][MIDI bytes] をコールバックへ。
//   - 送信 (device→host): IDENTIFY_RSP/ACK/TARGET_RX 等をキューに積み、read トランザクション
//     で [LEN][MIDI bytes] を返す。データがある間 INT 線を Low にしてホストに知らせる。
//
// I2C ペリフェラル設定は ch32v003fun の i2c_slave サンプルに準拠。
//
// ハード設定 (CH32X035 RM 8.3.2.1 AFIO_PCFR1 で確認済み):
//   - I2C1 remap option 3 (I2C1_RM[4:2]=011): SCL→PC19/pin24, SDA→PC18/pin25。
//   - PC18/PC19 は既定で SDI(2線式デバッグ)が占有 → SW_CFG[26:24]=100 で SDI を無効化して解放。
//     これ以降 PC18/PC19 経由の SWD 書き込みは不可 (書き込みは BOOT-DFU/wchisp で行う)。
//   - INT 線は PB1 (暫定)。基板に合わせて I2C_INT_PIN を変更可。
// ===================================================================================
#include "i2c_midi.h"
#include "ch32fun.h"
#include "funconfig.h"

// このトランスポートは 2 チップ構成専用。MIMICX_I2C 未定義のビルドでは空にする。
#ifdef MIMICX_I2C

#ifndef FUNCONF_SYSTEM_CORE_CLOCK
#define FUNCONF_SYSTEM_CORE_CLOCK 48000000
#endif

// INT 線 (device→host データ有りで Low, active-low / open-drain)。基板に合わせて変更可。
#define I2C_INT_PORT  GPIOB
#define I2C_INT_PIN   1

#define I2C_RX_BUF_SIZE   80
#define I2C_TX_RING_SIZE  512
#define I2C_TX_FRAME_MAX  66   // [LEN] + 最大 63B + 余裕

// --- 受信 (host→device) ---
static volatile uint8_t  rx_buf[I2C_RX_BUF_SIZE];
static volatile uint8_t  rx_len;
static volatile uint8_t  is_write;     // 現トランザクションが write か
static void (*s_on_frame)(const uint8_t* midi, int len);

// --- 送信 (device→host): 可変長レコード [LEN][payload] のリングバッファ ---
static volatile uint8_t  tx_ring[I2C_TX_RING_SIZE];
static volatile uint16_t tx_head;      // producer (メインループ) が進める
static volatile uint16_t tx_tail;      // consumer (ISR) が進める
// 現在 read 中フレーム ([LEN][payload])
static volatile uint8_t  txf[I2C_TX_FRAME_MAX];
static volatile int      txf_len;
static volatile int      txf_pos;

// ---------------------------------------------------------------------------
// INT 線
// ---------------------------------------------------------------------------
static inline void int_assert(void)   { I2C_INT_PORT->BCR  = (1 << I2C_INT_PIN); } // Low
static inline void int_deassert(void) { I2C_INT_PORT->BSHR = (1 << I2C_INT_PIN); } // Hi-Z(High)

// ---------------------------------------------------------------------------
// 送信キュー
// ---------------------------------------------------------------------------
static inline uint16_t ring_free(void) {
    return (uint16_t)((tx_tail - tx_head - 1 + I2C_TX_RING_SIZE) % I2C_TX_RING_SIZE);
}

void i2c_midi_enqueue(const uint8_t* midi, int len) {
    if (len <= 0 || len > 63) return;
    if (ring_free() < (uint16_t)(len + 1)) return;   // 満杯ならドロップ
    uint16_t h = tx_head;
    tx_ring[h] = (uint8_t)len; h = (uint16_t)((h + 1) % I2C_TX_RING_SIZE);
    for (int i = 0; i < len; i++) {
        tx_ring[h] = midi[i]; h = (uint16_t)((h + 1) % I2C_TX_RING_SIZE);
    }
    tx_head = h;            // 全バイト書き込み後に head を更新 (SPSC)
    int_assert();
}

// read トランザクション開始時にリングから 1 フレームを取り出して txf に展開。
// 空なら LEN=0 のフレームを返す。
static void tx_load_frame(void) {
    txf_pos = 0;
    if (tx_tail == tx_head) {       // 空
        txf[0] = 0;
        txf_len = 1;
        return;
    }
    uint16_t t = tx_tail;
    uint8_t len = tx_ring[t]; t = (uint16_t)((t + 1) % I2C_TX_RING_SIZE);
    if (len > I2C_TX_FRAME_MAX - 1) len = I2C_TX_FRAME_MAX - 1;
    txf[0] = len;
    for (int i = 0; i < len; i++) {
        txf[1 + i] = tx_ring[t]; t = (uint16_t)((t + 1) % I2C_TX_RING_SIZE);
    }
    txf_len = len + 1;
    tx_tail = t;
    if (tx_tail == tx_head) int_deassert();   // 空になったら INT 解除
}

// ---------------------------------------------------------------------------
// 初期化
// ---------------------------------------------------------------------------
void i2c_midi_init(uint8_t addr7, void (*on_frame)(const uint8_t* midi, int len)) {
    s_on_frame = on_frame;
    rx_len = 0;
    is_write = 0;
    tx_head = tx_tail = 0;
    txf_len = txf_pos = 0;

    RCC->APB2PCENR |= RCC_IOPBEN | RCC_IOPCEN | RCC_AFIOEN;

    // INT 線 (PB1): オープンドレイン出力、初期 Hi-Z(High) = 非アサート
    I2C_INT_PORT->CFGLR &= ~(0xfu << (4 * I2C_INT_PIN));
    I2C_INT_PORT->CFGLR |=  ((GPIO_Speed_50MHz | GPIO_CNF_OUT_OD) << (4 * I2C_INT_PIN));
    int_deassert();

    // PC18/PC19 は既定で SDI(2線式デバッグ)が占有しているため、まず SDI を無効化してから
    // I2C1 を remap option 3 へ切り替える (どちらも AFIO_PCFR1、RM 8.3.2.1 で確認済み)。
    //   SW_CFG[26:24]=100 : SWD(SDI) off → PC18/PC19 を AF として使用可
    //   I2C1_RM[4:2]=011  : SCL/PC19, SDA/PC18
    // ⚠ これ以降 PC18/PC19 では SWD 書き込み不可。書き込みは BOOT-DFU (wchisp) で行う。
    {
        uint32_t pcfr1 = AFIO->PCFR1;
        pcfr1 &= ~(AFIO_PCFR1_I2C1_REMAP | (0x7u << 24));  // I2C1_RM[4:2], SW_CFG[26:24] をクリア
        pcfr1 |=  (0x3u << 2) | (0x4u << 24);              // I2C1_RM=011, SW_CFG=100 (SDI off)
        AFIO->PCFR1 = pcfr1;
    }

    // PC18(SDA)/PC19(SCL): AF オープンドレイン (高位ピンは CFGXR)
    const uint32_t af_od = (GPIO_Speed_50MHz | GPIO_CNF_OUT_OD_AF);
    GPIOC->CFGXR &= ~((0xfu << ((18 - 16) * 4)) | (0xfu << ((19 - 16) * 4)));
    GPIOC->CFGXR |=  (af_od << ((18 - 16) * 4)) | (af_od << ((19 - 16) * 4));

    // I2C1 ペリフェラル (slave)
    RCC->APB1PCENR |= RCC_APB1Periph_I2C1;
    RCC->APB1PRSTR |=  RCC_APB1Periph_I2C1;
    RCC->APB1PRSTR &= ~RCC_APB1Periph_I2C1;

    I2C1->CTLR1 |= I2C_CTLR1_SWRST;
    I2C1->CTLR1 &= ~I2C_CTLR1_SWRST;

    I2C1->CTLR2 |= (FUNCONF_SYSTEM_CORE_CLOCK / 1000000) & I2C_CTLR2_FREQ;
    I2C1->CTLR2 |= I2C_CTLR2_ITBUFEN | I2C_CTLR2_ITEVTEN | I2C_CTLR2_ITERREN;

    NVIC_EnableIRQ(I2C1_EV_IRQn);
    NVIC_SetPriority(I2C1_EV_IRQn, 2 << 4);
    NVIC_EnableIRQ(I2C1_ER_IRQn);
    NVIC_SetPriority(I2C1_ER_IRQn, 2 << 4);

    uint32_t clockrate = 400000;   // 400kHz Fast mode 33% duty
    I2C1->CKCFGR = ((FUNCONF_SYSTEM_CORE_CLOCK / (3 * clockrate)) & I2C_CKCFGR_CCR) | I2C_CKCFGR_FS;

    I2C1->OADDR1 = ((uint16_t)addr7) << 1;
    I2C1->OADDR2 = 0;

    I2C1->CTLR1 |= I2C_CTLR1_PE;
    I2C1->CTLR1 |= I2C_CTLR1_ACK;
}

// ---------------------------------------------------------------------------
// 割り込みハンドラ
// ---------------------------------------------------------------------------
void I2C1_EV_IRQHandler(void) __attribute__((interrupt));
void I2C1_EV_IRQHandler(void) {
    uint16_t star1 = I2C1->STAR1;
    uint16_t star2 = I2C1->STAR2;   // STAR1→STAR2 の読み出しで ADDR をクリア + 方向ラッチ

    if (star1 & I2C_STAR1_ADDR) {
        if (star2 & I2C_STAR2_TRA) {
            // マスターが読み出し (slave 送信) → 1 フレームを用意
            is_write = 0;
            tx_load_frame();
        } else {
            // マスターが書き込み (slave 受信)
            is_write = 1;
            rx_len = 0;
        }
    }

    if (star1 & I2C_STAR1_RXNE) {
        uint8_t b = (uint8_t)I2C1->DATAR;
        if (is_write && rx_len < I2C_RX_BUF_SIZE) rx_buf[rx_len++] = b;
    }

    if (star1 & I2C_STAR1_TXE) {
        // device→host: txf を順に送出。尽きたら 0。
        if (txf_pos < txf_len) I2C1->DATAR = txf[txf_pos++];
        else                   I2C1->DATAR = 0x00;
    }

    if (star1 & I2C_STAR1_STOPF) {
        I2C1->CTLR1 |= I2C_CTLR1_PE;   // STOPF クリア
        if (is_write && rx_len >= 2 && s_on_frame) {
            uint8_t declared = rx_buf[0];
            int n = rx_len - 1;
            if (declared < n) n = declared;
            s_on_frame((const uint8_t*)&rx_buf[1], n);
        }
        rx_len = 0;
        is_write = 0;
    }
}

void I2C1_ER_IRQHandler(void) __attribute__((interrupt));
void I2C1_ER_IRQHandler(void) {
    uint16_t star1 = I2C1->STAR1;
    if (star1 & I2C_STAR1_BERR) I2C1->STAR1 &= ~I2C_STAR1_BERR;
    if (star1 & I2C_STAR1_ARLO) I2C1->STAR1 &= ~I2C_STAR1_ARLO;
    if (star1 & I2C_STAR1_AF)   I2C1->STAR1 &= ~I2C_STAR1_AF;  // slave 送信終了 (NACK) でも発生
}

#endif // MIMICX_I2C
