// ===================================================================================
// hid_dispatcher.c
// ===================================================================================
#include "hid_dispatcher.h"
#include "board_config.h"

static const hid_function_t* const fns[] = BOARD_FUNCTIONS;
#define NUM_FUNCTIONS ((int)(sizeof(fns) / sizeof(fns[0])))

// ---------------------------------------------------------------------------
// ライフサイクル
// ---------------------------------------------------------------------------

void hid_dispatch_init(void) {
    for (int i = 0; i < NUM_FUNCTIONS; i++) {
        if (fns[i]->init) fns[i]->init();
    }
}

void hid_dispatch_release_all(void) {
    for (int i = 0; i < NUM_FUNCTIONS; i++) {
        if (fns[i]->release_all) fns[i]->release_all();
    }
}

void hid_dispatch_poll(void) {
    for (int i = 0; i < NUM_FUNCTIONS; i++) {
        if (fns[i]->poll) fns[i]->poll();
    }
}

// ---------------------------------------------------------------------------
// MIDI イベント
// ---------------------------------------------------------------------------

void hid_dispatch_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    for (int i = 0; i < NUM_FUNCTIONS; i++) {
        if (fns[i]->midi_channel == channel && fns[i]->on_note_on) {
            fns[i]->on_note_on(note, velocity);
        }
    }
}

void hid_dispatch_note_off(uint8_t channel, uint8_t note) {
    for (int i = 0; i < NUM_FUNCTIONS; i++) {
        if (fns[i]->midi_channel == channel && fns[i]->on_note_off) {
            fns[i]->on_note_off(note);
        }
    }
}

void hid_dispatch_cc(uint8_t channel, uint8_t cc, uint8_t value) {
    for (int i = 0; i < NUM_FUNCTIONS; i++) {
        if (fns[i]->midi_channel == channel && fns[i]->on_cc) {
            fns[i]->on_cc(cc, value);
        }
    }
}

uint8_t hid_dispatch_set_config(uint8_t key, const uint8_t* val, int len) {
    // 集約規則:
    //   INVALID_VALUE > OK > UNKNOWN_KEY
    // どれか 1 つでも INVALID_VALUE → INVALID_VALUE (値が key に対して不正)
    // それ以外でどれか 1 つでも OK → OK
    // 全部 UNKNOWN_KEY → UNKNOWN_KEY
    uint8_t result = ACK_STATUS_UNKNOWN_KEY;
    for (int i = 0; i < NUM_FUNCTIONS; i++) {
        if (!fns[i]->on_set_config) continue;
        uint8_t s = fns[i]->on_set_config(key, val, len);
        if (s == ACK_STATUS_INVALID_VALUE) {
            result = ACK_STATUS_INVALID_VALUE;
        } else if (s == ACK_STATUS_OK && result != ACK_STATUS_INVALID_VALUE) {
            result = ACK_STATUS_OK;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// IDENTIFY / CAPABILITY 応答補助
// ---------------------------------------------------------------------------

int hid_dispatch_build_channel_map(uint8_t* buf, int max_len) {
    int n = 0;
    if (n + 1 > max_len) return n;
    buf[n++] = (uint8_t)NUM_FUNCTIONS;
    for (int i = 0; i < NUM_FUNCTIONS; i++) {
        if (n + 3 > max_len) break;
        buf[n++] = fns[i]->midi_channel & 0x7F;
        buf[n++] = fns[i]->hid_type & 0x7F;
        buf[n++] = fns[i]->target_system & 0x7F;
    }
    return n;
}

int hid_dispatch_build_device_name(uint8_t* buf, int max_len) {
    int n = 0;
    for (int i = 0; i < NUM_FUNCTIONS; i++) {
        const char* name = fns[i]->name;
        if (!name) continue;
        if (i > 0 && n < max_len) buf[n++] = ' ';
        for (int j = 0; name[j] && n < max_len; j++) {
            buf[n++] = (uint8_t)(name[j] & 0x7F);
        }
    }
    return n;
}

int hid_dispatch_build_capabilities(uint8_t* buf, int max_len) {
    int n = 0;
    for (int i = 0; i < NUM_FUNCTIONS; i++) {
        if (fns[i]->append_capabilities) {
            n += fns[i]->append_capabilities(buf + n, max_len - n);
        }
    }
    return n;
}
