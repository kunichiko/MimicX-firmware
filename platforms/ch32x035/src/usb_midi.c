// ===================================================================================
// USB-MIDI implementation using ch32v003fun fsusb driver (patched)
// ===================================================================================
// EP2 bidirectional (NoNamedCat 方式):
//   EP2 OUT (0x02): Host → Device (receive MIDI from smartphone)
//   EP2 IN  (0x82): Device → Host (send MIDI to smartphone)
//
// CH32X03x: TX+RX 同時有効 + AUTO_TOG
// DMA addr → RX (64B), DMA addr+64 → TX (64B)
// ===================================================================================

#include "usb_midi.h"

#include <string.h>

#include "ch32fun.h"
#include "fsusb.h"

// ---------------------------------------------------------------------------
// EP2 双方向 DMA バッファ (128 bytes: RX 0-63, TX 64-127)
// ---------------------------------------------------------------------------
uint8_t __attribute__((aligned(4))) ep2_bidi_buf[128];

// ---------------------------------------------------------------------------
// Ring Buffers
// ---------------------------------------------------------------------------

static volatile uint8_t rx_buf[MIDI_RX_BUF_SIZE * 4];
static volatile uint8_t rx_head;
static volatile uint8_t rx_tail;

static uint8_t tx_buf[MIDI_TX_BUF_SIZE * 4];
static uint8_t tx_head;
static uint8_t tx_tail;

// ---------------------------------------------------------------------------
// fsusb callbacks (called from USB ISR)
// ---------------------------------------------------------------------------

int HandleInRequest(struct _USBState* ctx, int endp, uint8_t* data, int len) {
    (void)ctx;
    (void)data;
    (void)len;
    return 0;
}

void HandleDataOut(struct _USBState* ctx, int endp, uint8_t* data, int len) {
    (void)ctx;
    if (endp == 0) {
        ctx->USBFS_SetupReqLen = 0;
        return;
    }
    if (endp == 2) {
        // EP2 OUT: 受信データは ep2_bidi_buf[0..63]
        data = ep2_bidi_buf;
        for (int i = 0; i + 3 < len; i += 4) {
            uint8_t next_head = (rx_head + 1) & (MIDI_RX_BUF_SIZE - 1);
            if (next_head != rx_tail) {
                int offset = rx_head * 4;
                rx_buf[offset + 0] = data[i + 0];
                rx_buf[offset + 1] = data[i + 1];
                rx_buf[offset + 2] = data[i + 2];
                rx_buf[offset + 3] = data[i + 3];
                rx_head = next_head;
            }
        }
    }
}

int HandleSetupCustom(struct _USBState* ctx, int setup_code) {
    (void)ctx;
    (void)setup_code;
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void usb_midi_init(void) {
    rx_head = 0;
    rx_tail = 0;
    tx_head = 0;
    tx_tail = 0;
    memset(ep2_bidi_buf, 0, sizeof(ep2_bidi_buf));
    // iSerialNumber descriptor (string3) を chip UID + variant tag から組み立てる。
    // USBFSSetup() より前 (= GET_DESCRIPTOR が来うる時点より前) に必ず実行する。
    usb_init_serial_descriptor();
    USBFSSetup();
    // DMA を 128 バイトバッファに上書き
    UEP_DMA(2) = (uintptr_t)ep2_bidi_buf;
}

int usb_midi_send_event(uint8_t cin, uint8_t midi0, uint8_t midi1, uint8_t midi2) {
    uint8_t next_head = (tx_head + 1) & (MIDI_TX_BUF_SIZE - 1);
    if (next_head == tx_tail) return -1;
    int offset = tx_head * 4;
    tx_buf[offset + 0] = (MIDI_CABLE_0 << 4) | (cin & 0x0F);
    tx_buf[offset + 1] = midi0;
    tx_buf[offset + 2] = midi1;
    tx_buf[offset + 3] = midi2;
    tx_head = next_head;
    return 0;
}

int usb_midi_send_sysex(const uint8_t* data, int len) {
    if (len < 2) return -1;
    int i = 0;
    while (i < len) {
        int remaining = len - i;
        uint8_t cin, b0, b1, b2;
        if (remaining >= 3) {
            cin = (remaining == 3 && data[i + 2] == 0xF7) ? CIN_SYSEX_END_3 : CIN_SYSEX_START;
            b0 = data[i]; b1 = data[i + 1]; b2 = data[i + 2];
            i += 3;
        } else if (remaining == 2) {
            cin = CIN_SYSEX_END_2;
            b0 = data[i]; b1 = data[i + 1]; b2 = 0;
            i += 2;
        } else {
            cin = CIN_SYSEX_END_1;
            b0 = data[i]; b1 = 0; b2 = 0;
            i += 1;
        }
        if (usb_midi_send_event(cin, b0, b1, b2) < 0) return -1;
    }
    return 0;
}

int usb_midi_receive_event(uint8_t* cin, uint8_t* midi0, uint8_t* midi1, uint8_t* midi2) {
    if (rx_head == rx_tail) return 0;
    int offset = rx_tail * 4;
    *cin = rx_buf[offset + 0] & 0x0F;
    *midi0 = rx_buf[offset + 1];
    *midi1 = rx_buf[offset + 2];
    *midi2 = rx_buf[offset + 3];
    rx_tail = (rx_tail + 1) & (MIDI_RX_BUF_SIZE - 1);
    return 4;
}

void usb_midi_poll(void) {
    // SUSPEND 状態 (= ホスト無通信 ≥3ms = ケーブル抜去 or ホスト suspend) への
    // 立ち上がりエッジで USB ペリフェラルを完全再初期化する。
    //
    // 背景: X68000 のジョイスティックポートから給電されている状態では USB
    // Vbus が落ちても MCU はリセットされない。そのため USB peripheral の
    // 内部ステート (ディスクリプタ要求の途中状態、EP FIFO の残骸など) が
    // 前回ホストとのセッションのまま残り、次に USB を挿し直すと新しいホスト
    // (Android) の enumerate で descriptor 取得が中途半端になって 100ms 後に
    // ホスト側からデバイスが drop される (serial_number=null 現象)。
    //
    // SUSPEND を検知した時点でホストは既に居ないので、ここで peripheral を
    // 作り直しておけば次回挿入時に fresh な状態で enumerate されて取りこぼし
    // が起きない。
    static uint8_t prev_suspended = 0;
    uint8_t suspended_now = (USBFS->MIS_ST & USBFS_UMS_SUSPEND) ? 1 : 0;
    if (suspended_now && !prev_suspended) {
        usb_midi_init();
        prev_suspended = 1;
        return;
    }
    prev_suspended = suspended_now;

    // bus reset 後に DMA が上書きされた場合は復旧
    if (UEP_DMA(2) != (uintptr_t)ep2_bidi_buf) {
        UEP_DMA(2) = (uintptr_t)ep2_bidi_buf;
    }

    // Flush TX buffer to EP2 IN
    if (tx_head == tx_tail) return;

    // T_RES が NAK でなければ前回のデータがまだ送信中
    if ((USBFS->UEP2_CTRL_H & USBFS_UEP_T_RES_MASK) != USBFS_UEP_T_RES_NAK) return;

    // TX バッファは ep2_bidi_buf + 64
    uint8_t* tx_ep_buf = ep2_bidi_buf + 64;
    int count = 0;
    while (tx_head != tx_tail && count + 4 <= 64) {
        int offset = tx_tail * 4;
        tx_ep_buf[count + 0] = tx_buf[offset + 0];
        tx_ep_buf[count + 1] = tx_buf[offset + 1];
        tx_ep_buf[count + 2] = tx_buf[offset + 2];
        tx_ep_buf[count + 3] = tx_buf[offset + 3];
        tx_tail = (tx_tail + 1) & (MIDI_TX_BUF_SIZE - 1);
        count += 4;
    }

    if (count > 0) {
        // USBFS_SendEndpoint のマクロが CH32X03x で正しく動作しないため、
        // 直接レジスタにアクセスする
        USBFS->UEP2_TX_LEN = count;
        USBFS->UEP2_CTRL_H = (USBFS->UEP2_CTRL_H & ~USBFS_UEP_T_RES_MASK) | USBFS_UEP_T_RES_ACK;
    }
}
