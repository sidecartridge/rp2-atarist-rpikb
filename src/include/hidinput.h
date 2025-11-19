#ifndef HIDINPUT_H
#define HIDINPUT_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#include "bsp/board.h"
#include "debug.h"
#include "joystick.h"
#include "mouse.h"
#include "pico/stdlib.h"
#include "stkeys.h"
#include "tusb.h"

extern _Atomic int16_t pend_dx;
extern _Atomic int16_t pend_dy;

/**
 * Return the key states of the given Atari ST keyboard scancode. The scancode
 * must be in the range 0-127
 */
unsigned char st_keydown(const unsigned char code);

/**
 * Return the current state of the mouse buttons
 */
int st_mouse_buttons();

/**
 * Return a bitfield representing the joystick state
 */
unsigned char st_joystick();

/**
 * Return 1 if the mouse is enabled or 0 if they joystick is enabled
 */
int st_mouse_enabled();

void hidinput_update_mouse(int16_t dx, int16_t dy, bool left_down,
                           bool right_down);

// ---- HID interface info ring (for TinyUSB host HID interfaces) ----
// Stores recently seen HID interface infos along with the device address.
// Ring overwrites the oldest entry when full.

// Initialize/clear the ring buffer
void hidinput_if_ring_init(void);

// Push a new interface id (dev_addr + idx) into the ring. Returns false on
// parameter error.
bool hidinput_if_ring_push(uint8_t dev_addr, uint8_t idx);

// Pop the oldest interface id from the ring (returns false if empty)
bool hidinput_if_ring_pop(uint8_t* dev_addr, uint8_t* idx);

// Get current number of stored entries
int hidinput_if_ring_size(void);

// Peek an entry by index from oldest (0) to newest (size-1). Returns false if
// out of range.
bool hidinput_if_ring_peek(int index_from_oldest, uint8_t* dev_addr,
                           uint8_t* idx);

void hidinput_device_descriptor_complete_cb(tuh_xfer_t* xfer);

void tuh_descriptor_get_string_complete_cb(tuh_xfer_t* xfer);

#endif
