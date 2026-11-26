/**
 * File: factory.h
 * Description: Factory test run by the USB mode loop on Souffle when the
 * FACTORY_MODE setting is 1. It counts the keystrokes passed to the ST,
 * notes which of the keys a, b, c, d and e were typed, records the USB
 * keyboard's VID:PID and watches the twelve joystick lines (GPIO 10-21). It
 * passes once all twelve have read low for 10 seconds in a row and all five
 * letters were typed; CONFIG ends it as a failure. Either way it writes
 * FACTORY_RESULT once and enters Booster the way the CONFIG button does.
 * With FACTORY_MODE 0, or on Croissant, every call is a no-op.
 */

#ifndef FACTORY_H
#define FACTORY_H

#include <stdbool.h>
#include <stdint.h>

// Live test state, kept in RAM only so the SWD tools can read it by symbol.
// Flash is written once, on the way out.
typedef struct {
  uint32_t state;        // FACTORY_STATE_*
  uint32_t seen_mask;    // bit n set: GPIO (10 + n) has read low
  uint32_t low_mask;     // the lines low at the last sample
  uint32_t held_ms;      // current all-low hold
  uint32_t max_held_ms;  // longest all-low hold
  uint32_t keystrokes;   // new key presses passed to the ST
  uint32_t keys_mask;    // bit n set: letter ('a' + n) typed, n = 0..4
  uint16_t vid;          // USB keyboard vendor ID, 0 if none mounted
  uint16_t pid;          // USB keyboard product ID
} factory_live_t;

enum {
  FACTORY_STATE_OFF = 0,
  FACTORY_STATE_WAITING = 1,
  FACTORY_STATE_HOLDING = 2,
  FACTORY_STATE_SAVING = 3,
};

extern volatile factory_live_t factory_live;

// Reads FACTORY_MODE; when it is 1, sets up the twelve lines as inputs with
// pull-ups and the mode LEDs, and starts the test. Returns true when it runs.
bool factory_init(void);

bool factory_active(void);

// Samples the lines and updates the LEDs. Call from the loop's 20 ms tick,
// in thread context. Finishes the test (and does not return) on a pass.
void factory_poll(void);

// A new key press, as a USB HID usage code, was translated to an ST key.
void factory_note_keystroke(uint8_t hid_code);

// A USB boot keyboard was mounted.
void factory_note_keyboard(uint16_t vid, uint16_t pid);

// Writes FACTORY_RESULT (pass=0 when CONFIG ended the test), saves the
// settings once and enters Booster. Does not return.
void factory_finish(bool config_pressed);

#endif  // FACTORY_H
