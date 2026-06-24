#ifndef _USB_MIDI_H
#define _USB_MIDI_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// USB-MIDI Event Packet (USB MIDI 1.0 spec, Section 4)
// ---------------------------------------------------------------------------
// Each USB-MIDI event is 4 bytes:
//   Byte 0: Cable Number (high nibble) | Code Index Number (low nibble)
//   Byte 1: MIDI status byte
//   Byte 2: MIDI data byte 1
//   Byte 3: MIDI data byte 2

// Code Index Numbers (CIN)
#define CIN_SYSEX_START       0x04  // SysEx starts or continues
#define CIN_SYSEX_END_1       0x05  // SysEx ends with 1 byte
#define CIN_SYSEX_END_2       0x06  // SysEx ends with 2 bytes
#define CIN_SYSEX_END_3       0x07  // SysEx ends with 3 bytes
#define CIN_NOTE_OFF          0x08
#define CIN_NOTE_ON           0x09
#define CIN_POLY_KEYPRESS     0x0A
#define CIN_CONTROL_CHANGE    0x0B
#define CIN_PROGRAM_CHANGE    0x0C
#define CIN_CHANNEL_PRESSURE  0x0D
#define CIN_PITCH_BEND        0x0E
#define CIN_SINGLE_BYTE       0x0F

// Cable number (we use cable 0)
#define MIDI_CABLE_0          0x00

// Ring buffer size (must be power of 2)
#define MIDI_RX_BUF_SIZE      64
#define MIDI_TX_BUF_SIZE      64

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Initialize USB-MIDI device
void usb_midi_init(void);

// Send a 4-byte USB-MIDI event packet to host
// Returns 0 on success, -1 if buffer full
int usb_midi_send_event(uint8_t cin, uint8_t midi0, uint8_t midi1, uint8_t midi2);

// Send a Note On message
static inline int usb_midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    return usb_midi_send_event(CIN_NOTE_ON, 0x90 | (channel & 0x0F), note & 0x7F, velocity & 0x7F);
}

// Send a Note Off message
static inline int usb_midi_note_off(uint8_t channel, uint8_t note, uint8_t velocity) {
    return usb_midi_send_event(CIN_NOTE_OFF, 0x80 | (channel & 0x0F), note & 0x7F, velocity & 0x7F);
}

// Send a Control Change message
static inline int usb_midi_control_change(uint8_t channel, uint8_t control, uint8_t value) {
    return usb_midi_send_event(CIN_CONTROL_CHANGE, 0xB0 | (channel & 0x0F), control & 0x7F, value & 0x7F);
}

// Send raw SysEx data (handles fragmentation into 4-byte USB-MIDI packets)
int usb_midi_send_sysex(const uint8_t* data, int len);

// Receive a 4-byte USB-MIDI event packet from host
// Returns number of bytes read (4 if event available, 0 if none)
int usb_midi_receive_event(uint8_t* cin, uint8_t* midi0, uint8_t* midi1, uint8_t* midi2);

// Must be called periodically from main loop to flush TX buffer
void usb_midi_poll(void);

#endif
