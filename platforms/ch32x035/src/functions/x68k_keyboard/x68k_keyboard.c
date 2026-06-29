// ===================================================================================
// X68000 キーボード機能
// ===================================================================================
// プロトコル:
//   2400bps 8N1, TTL 5V
//   キーボード→本体: 1 byte/key (bit7=1:break, 0:make / bit6-0:scancode)
//   本体→キーボード: LED 制御 (bit7=1) / リピート設定 (0x60-0x7F) など
//
// ピン (CH32X035 G8U6 / QFN28):
//   PB10: USART1_TX  (キーコード送信、USART1 デフォルト = PB10/PB11)
//   PB11: USART1_RX  (本体→キーボードのコマンド受信)
//   PB12: READY      (本体→キーボード, 1=ホスト準備完了, 0=送信抑止)
// USART1 はリマップ不要 (CH32X035 デフォルトで PB10/PB11)
// ===================================================================================
#include "x68k_keyboard.h"
#include "ch32fun.h"
#include "funconfig.h"
#include "../../mimicx_tx.h"
#include "../x68k_mouse/x68k_mouse.h"
#include "sharp12_remote.h"

// ---------------------------------------------------------------------------
// 設定
// ---------------------------------------------------------------------------

#define MIDI_CH_KEYBOARD  1   // ホスト→デバイス: キー押下/解放
                              // デバイス→ホスト: ターゲット機受信バイト (SysEx TARGET_RX)

// SysEx TARGET_RX (0x05): ターゲット機から受信した生バイトをホストへ転送
#define SYSEX_CMD_TARGET_RX  0x05

// ---------------------------------------------------------------------------
// X68000 から受信した REMOTE 関連の状態フラグ
// ---------------------------------------------------------------------------
// X68000 本体は SRAM 設定や CPU 状態に応じてこれらを送ってくる。受信前 (未通知時)
// の初期値:
//   - key_en   : 1 (送信可) 起動直後はキー送信できないと初期化中の通信が止まるため
//   - ctrl_en  : 0 (極性未検証だが従来挙動を維持)
//   - opt2_en  : 0 (極性未検証)
//   - mode_x1  : 0 (極性未検証)
static volatile uint8_t key_en       = 1;  // 0b010010*X の X (1=送信可, 0=送信不可)
static volatile uint8_t ctrl_en_bit  = 0;  // 0b010110*X の X
static volatile uint8_t opt2_en_bit  = 0;  // 0b010111*X の X (本実装では未使用、app 側で参照)
static volatile uint8_t mode_x1_bit  = 0;  // 0b010100*X の X (本実装では未使用)

// ---------------------------------------------------------------------------
// 送信キュー (READY=Low の間にキーが来ても取りこぼさないため)
// ---------------------------------------------------------------------------

#define TX_QUEUE_SIZE 32  // 2 のべき乗にしておくと head/tail のラップが安い
static volatile uint8_t  tx_queue[TX_QUEUE_SIZE];
static volatile uint16_t tx_head = 0;  // 次に書き込む位置
static volatile uint16_t tx_tail = 0;  // 次に読み出す位置

static inline int tx_queue_empty(void) { return tx_head == tx_tail; }

static inline void tx_queue_push(uint8_t byte) {
    uint16_t next = (tx_head + 1) % TX_QUEUE_SIZE;
    if (next == tx_tail) return;  // フル: 古いほうを優先して新規を捨てる
    tx_queue[tx_head] = byte;
    tx_head = next;
}

// READY 信号 (PB12): 1=ホスト準備完了, 0=送信抑止
static inline int host_ready(void) {
    return (GPIOB->INDR & (1 << 12)) != 0;
}

// ---------------------------------------------------------------------------
// UART 駆動
// ---------------------------------------------------------------------------

static void uart_init(void) {
    // USART1 は APB2 ペリフェラル
    RCC->APB2PCENR |= RCC_USART1EN;

    // USART1 はデフォルトで PB10(TX) / PB11(RX) なのでリマップ不要

    // PB10 (TX): Push-Pull AF 出力 — CFGHR bit field for pin 10
    GPIOB->CFGHR &= ~(0xf << (4 * (10 - 8)));
    GPIOB->CFGHR |= (GPIO_Speed_50MHz | GPIO_CNF_OUT_PP_AF) << (4 * (10 - 8));

    // PB11 (RX): フローティング入力 — CFGHR bit field for pin 11
    // PB12 (READY): フローティング入力 — CFGHR bit field for pin 12
    GPIOB->CFGHR &= ~((0xf << (4 * (11 - 8))) | (0xf << (4 * (12 - 8))));
    GPIOB->CFGHR |=  ((GPIO_Speed_In | GPIO_CNF_IN_FLOATING) << (4 * (11 - 8))) |
                     ((GPIO_Speed_In | GPIO_CNF_IN_FLOATING) << (4 * (12 - 8)));

    // 2400bps 8N1
    USART1->BRR = F_CPU / 2400;
    USART1->CTLR1 = USART_CTLR1_TE | USART_CTLR1_RE;
    USART1->CTLR1 |= USART_CTLR1_UE;
}

// TX レジスタが空いていて READY=High なら 1 byte 送り出す
static int uart_try_send(uint8_t byte) {
    if (!host_ready()) return 0;
    if (!(USART1->STATR & USART_STATR_TXE)) return 0;
    USART1->DATAR = byte;
    return 1;
}

// 受信データがあれば返す。なければ -1
static int uart_receive(void) {
    if (USART1->STATR & USART_STATR_RXNE) {
        return (uint8_t)USART1->DATAR;
    }
    return -1;
}

// キューにある間、可能な限り送り出す
static void drain_tx_queue(void) {
    uint8_t byte;
    while (!tx_queue_empty()) {
        // peek: 送信できるか試して、成功した時だけキューから取り除く
        byte = tx_queue[tx_tail];
        if (!uart_try_send(byte)) return;
        tx_tail = (tx_tail + 1) % TX_QUEUE_SIZE;
    }
}

// ---------------------------------------------------------------------------
// ホスト→キーボード コマンド処理
// ---------------------------------------------------------------------------

// 受信バイトを SysEx TARGET_RX (0x05) で生のままホストへ転送する。
// アプリ側でターゲット機固有のコマンド (LED 制御 / キーリピート設定 /
// LED 輝度など) を解釈する。
//
// SysEx レイアウト:
//   F0 7D 01 05 <midi_channel> <byte_hi4> <byte_lo4> F7
static void forward_target_rx(uint8_t byte) {
    uint8_t sysex[8] = {
        0xF0, 0x7D, 0x01, SYSEX_CMD_TARGET_RX,
        MIDI_CH_KEYBOARD,
        (uint8_t)((byte >> 4) & 0x0F),
        (uint8_t)(byte & 0x0F),
        0xF7,
    };
    // USB と (2 チップ構成なら) I2C の両方へ。usb_midi_send_sysex 直送りだと
    // I2C に乗らず BLE 経由でホスト (LED フィードバック等) に届かない。
    mimicx_tx(sysex, sizeof(sysex));
}

// ---------------------------------------------------------------------------
// hid_function インターフェース
// ---------------------------------------------------------------------------

static void x68k_kb_init(void) {
    uart_init();
    sharp12_remote_init();
}

int x68k_keyboard_emit_remote(uint8_t code) {
    if (code < 0x01 || code > 0x1F) return 0;
    sharp12_remote_emit(code);
    return 1;
}

static void x68k_kb_release_all(void) {
    // 全キーリリースは MIDI 側のステートに任せる (デバイス側はステートレス)
}

static void x68k_kb_on_note_on(uint8_t note, uint8_t velocity) {
    (void)velocity;
    // KEY_EN=0 のときは X68000 がキー受付不能状態。送信せずに破棄する
    // (キューに溜めると後で押下/解放が時系列ズレして表示されるため)。
    // TV Control (REMOTE 端子) の発射は別経路なので影響を受けない。
    if (!key_en) return;
    // bit7=0 = make, bit6-0 = スキャンコード
    tx_queue_push(note & 0x7F);
}

static void x68k_kb_on_note_off(uint8_t note) {
    if (!key_en) return;
    // bit7=1 = break
    tx_queue_push(0x80 | (note & 0x7F));
}

static void x68k_kb_poll(void) {
    // RX: ターゲット機から届いた生バイトを処理
    int byte = uart_receive();
    if (byte >= 0) {
        const uint8_t b = (uint8_t)byte;
        // 0b01000xxM (0x40-0x47): MSCTRL コマンド → 内蔵マウスサブシステムへ
        // (アプリにも転送する必要はないので TARGET_RX には流さない)
        if ((b & 0xF8) == 0x40) {
            x68k_mouse_handle_msctrl(b);
        } else {
            // 状態フラグ系の傍受 (app 側でも解釈するが、firmware も使うので保持)
            //   0b010010*X: KEY_EN (キー送信許可: 1=可, 0=不可)
            //   0b010100*X: X68k/X1 モード選択
            //   0b010110*X: CTRL EN (本体発ディスプレイ制御の有効/無効)
            //   0b010111*X: OPT2 EN (OPT.2 キーによる発射の許可/禁止 — app 用)
            if ((b & 0xFC) == 0x48) {
                key_en = b & 0x01;
                // KEY_EN=0 に遷移したら未送信のキューを破棄 (時系列ズレ防止)。
                // 純正キーボードの挙動: 本体 CPU が非応答に入る直前に KEY_EN=0
                // を送るパターン。送り損ねたキーは諦める。
                if (!key_en) {
                    tx_tail = tx_head;
                }
            } else if ((b & 0xFC) == 0x50) {
                mode_x1_bit = b & 0x01;
            } else if ((b & 0xFC) == 0x58) {
                ctrl_en_bit = b & 0x01;
            } else if ((b & 0xFC) == 0x5C) {
                opt2_en_bit = b & 0x01;
            } else if ((b & 0xC0) == 0x00 && b != 0x00) {
                // 0b00xxxxxx (0x01-0x3F): 本体発の専用ディスプレイ制御コマンド。
                // CTRL EN が有効 (= 1 と仮定) のときだけ REMOTE 端子から発射する。
                // app 側にもパススルー (snackbar 用) するために TARGET_RX も併送する。
                if (ctrl_en_bit && b >= 0x01 && b <= 0x1F) {
                    sharp12_remote_emit(b);
                }
            }
            // 解釈の有無にかかわらず、app 側で表示・状態管理できるよう生バイトを転送する
            forward_target_rx(b);
        }
    }
    // TX: READY=High かつ TXE=1 の間、キューから送り出す
    drain_tx_queue();
}

static int x68k_kb_append_capabilities(uint8_t* buf, int max_len) {
    int n = 0;
    // CAP_LED_COUNT (0x03) = 7
    if (n + 3 > max_len) return n;
    buf[n++] = 0x03;
    buf[n++] = 1;
    buf[n++] = 7;
    // CAP_KEYCODE_RANGE (0x10): 0x01-0x73
    if (n + 4 > max_len) return n;
    buf[n++] = 0x10;
    buf[n++] = 2;
    buf[n++] = 0x01;
    buf[n++] = 0x73;
    // CAP_BIDI (0x20): 双方向通信対応
    if (n + 3 > max_len) return n;
    buf[n++] = 0x20;
    buf[n++] = 1;
    buf[n++] = 1;
    return n;
}

const hid_function_t x68k_keyboard_function = {
    .name              = "x68k-keyboard",
    .hid_type          = HID_TYPE_KEYBOARD,
    .target_system     = TARGET_X68000,
    .midi_channel      = MIDI_CH_KEYBOARD,
    .init              = x68k_kb_init,
    .release_all       = x68k_kb_release_all,
    .on_note_on        = x68k_kb_on_note_on,
    .on_note_off       = x68k_kb_on_note_off,
    .on_cc             = NULL,
    .on_set_config     = NULL,
    .poll              = x68k_kb_poll,
    .append_capabilities = x68k_kb_append_capabilities,
};
