/*
 * Atari ST RP2040 IKBD Emulator
 * Copyright (C) 2021 Roy Hopkins
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */
#ifndef STKEYS_H
#define STKEYS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum {
  ATARI_CTRL = 29,
  ATARI_LSHIFT = 42,
  ATARI_RSHIFT = 54,
  ATARI_ALT = 56,
};

// For reference: USB HID Usage Table page 0x07
// https://usb.org/sites/default/files/hut1_21.pdf

extern const unsigned char stkeys_lookup_hid_gb[128];
extern const unsigned char stkeys_lookup_hid_de[128];
extern const unsigned char stkeys_lookup_hid_fr[128];
extern const unsigned char stkeys_lookup_hid_it[128];
extern const unsigned char stkeys_lookup_hid_us[128];
extern const unsigned char stkeys_lookup_hid_es[128];
extern unsigned char key_states[128];

void stkeys_apply_keyboard_report_layout(const uint8_t* prev_keys,
                                         const uint8_t* cur_keys,
                                         size_t key_slots, uint8_t modifiers,
                                         const char* layout);
uint8_t stkeys_translate_hid(const char* layout, uint8_t hid_code,
                             bool* shift_active, bool* alt_active,
                             bool* ctrl_active);
#endif  // STKEYS_H
