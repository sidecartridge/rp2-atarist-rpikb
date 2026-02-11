#include "stkeys.h"

#include <stdbool.h>
#include <string.h>

#include "debug.h"
#include "tusb.h"

unsigned char key_states[128] = {0};

static uint8_t stkeys_translate_hid_table(const unsigned char* lookup,
                                          uint8_t hid_code) {
  if (!lookup || hid_code >= 128) {
    return 0;
  }
  return lookup[hid_code];
}

static uint8_t stkeys_translate_hid_plain(uint8_t hid_code, bool* shift_active,
                                          bool* alt_active, bool* ctrl_active,
                                          const unsigned char* lookup) {
  if (shift_active) {
    (void)*shift_active;
  }
  if (alt_active) {
    (void)*alt_active;
  }
  if (ctrl_active) {
    (void)*ctrl_active;
  }
  return stkeys_translate_hid_table(lookup, hid_code);
}

static bool stkeys_type_is_iso(const char* layout_type) {
  if (layout_type == NULL) return false;
  return strcmp(layout_type, "1") == 0 || strcmp(layout_type, "ISO") == 0 ||
         strcmp(layout_type, "iso") == 0;
}

static bool stkeys_type_is_ansi(const char* layout_type) {
  if (layout_type == NULL) return false;
  return strcmp(layout_type, "2") == 0 || strcmp(layout_type, "ANSI") == 0 ||
         strcmp(layout_type, "ansi") == 0;
}

static const unsigned char* stkeys_get_lookup(const char* layout,
                                              const char* layout_type) {
  const unsigned char* iso_lookup = NULL;
  const unsigned char* ansi_lookup = NULL;
  const bool want_iso = stkeys_type_is_iso(layout_type);
  const bool want_ansi = stkeys_type_is_ansi(layout_type);

  if (layout != NULL) {
    if (strcmp(layout, "ES") == 0 || strcmp(layout, "es") == 0) {
      iso_lookup = stkeys_lookup_hid_es_iso;
    } else if (strcmp(layout, "GB") == 0 || strcmp(layout, "gb") == 0 ||
               strcmp(layout, "UK") == 0 || strcmp(layout, "uk") == 0) {
      iso_lookup = stkeys_lookup_hid_gb_iso;
    } else if (strcmp(layout, "DE") == 0 || strcmp(layout, "de") == 0) {
      iso_lookup = stkeys_lookup_hid_de_iso;
    } else if (strcmp(layout, "FR") == 0 || strcmp(layout, "fr") == 0) {
      iso_lookup = stkeys_lookup_hid_fr_iso;
    } else if (strcmp(layout, "IT") == 0 || strcmp(layout, "it") == 0) {
      iso_lookup = stkeys_lookup_hid_it_iso;
    } else if (strcmp(layout, "US") == 0 || strcmp(layout, "us") == 0) {
      ansi_lookup = stkeys_lookup_hid_us_ansi;
    }
  }

  // Fallbacks for unknown layout.
  if (iso_lookup == NULL && ansi_lookup == NULL) {
    iso_lookup = stkeys_lookup_hid_gb_iso;
    ansi_lookup = stkeys_lookup_hid_us_ansi;
  }

  // Honor explicit type selection when that variant exists.
  if (want_iso && iso_lookup != NULL) return iso_lookup;
  if (want_ansi && ansi_lookup != NULL) return ansi_lookup;

  // Auto: language default. If only one variant exists, use it.
  if (ansi_lookup != NULL && iso_lookup == NULL) return ansi_lookup;
  if (iso_lookup != NULL && ansi_lookup == NULL) return iso_lookup;

  // Explicit type requested but not available: keep language default.
  if (layout != NULL &&
      (strcmp(layout, "US") == 0 || strcmp(layout, "us") == 0)) {
    return ansi_lookup;
  }
  return iso_lookup != NULL ? iso_lookup : ansi_lookup;
}

uint8_t stkeys_translate_hid(const char* layout, const char* layout_type,
                             uint8_t hid_code,
                             bool* shift_active, bool* alt_active,
                             bool* ctrl_active) {
  return stkeys_translate_hid_plain(
      hid_code, shift_active, alt_active, ctrl_active,
      stkeys_get_lookup(layout, layout_type));
}

void stkeys_apply_keyboard_report_layout(const uint8_t* prev_keys,
                                         const uint8_t* cur_keys,
                                         size_t key_slots, uint8_t modifiers,
                                         const char* layout,
                                         const char* layout_type) {
  if (!prev_keys || !cur_keys) {
    return;
  }

  size_t slots = key_slots;
  if (slots > 6) {
    slots = 6;
  }

  bool shift_active = (modifiers & (KEYBOARD_MODIFIER_LEFTSHIFT |
                                    KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
  bool alt_active =
      (modifiers & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) !=
      0;
  bool ctrl_active = (modifiers & (KEYBOARD_MODIFIER_LEFTCTRL |
                                   KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;

  for (size_t i = 0; i < slots; ++i) {
    uint8_t prev_code = prev_keys[i];

    if (!prev_code) {
      continue;
    }

    bool still_pressed = false;
    for (size_t j = 0; j < slots; ++j) {
      if (cur_keys[j] == prev_code) {
        still_pressed = true;
        break;
      }
    }

    if (!still_pressed) {
      uint8_t st = stkeys_translate_hid(layout, layout_type, prev_code,
                                        &shift_active, &alt_active,
                                        &ctrl_active);
      if (st) {
        key_states[st] = 0;
      }
    }
  }

  for (size_t i = 0; i < slots; ++i) {
    uint8_t cur_code = cur_keys[i];
    if (!cur_code) {
      continue;
    }

    uint8_t st = stkeys_translate_hid(layout, layout_type, cur_code,
                                      &shift_active, &alt_active,
                                      &ctrl_active);
    if (st) {
      key_states[st] = 1;
    }
  }

  if (shift_active) {
    (void)shift_active;
    key_states[ATARI_LSHIFT] =
        (modifiers & KEYBOARD_MODIFIER_LEFTSHIFT) ? 1 : 0;
    key_states[ATARI_RSHIFT] =
        (modifiers & KEYBOARD_MODIFIER_RIGHTSHIFT) ? 1 : 0;
  } else {
    key_states[ATARI_LSHIFT] = 0;
    key_states[ATARI_RSHIFT] = 0;
  }

  if (alt_active) {
    (void)alt_active;
    key_states[ATARI_ALT] =
        (modifiers & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT))
            ? 1
            : 0;
  } else {
    key_states[ATARI_ALT] = 0;
  }

  key_states[ATARI_CTRL] =
      (modifiers & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL))
          ? 1
          : 0;
  key_states[ATARI_ALT] =
      (modifiers & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT))
          ? 1
          : 0;
}

const unsigned char stkeys_lookup_hid_it_iso[128] = {
    0,   // 0x00 No key pressed
    0,   // 0x01 Keyboard Error Roll Over - used for all slots if too many keys
    0,   // 0x02 Not used
    0,   // 0x03 Not used
    30,  // 0x04 Keyboard a and A
    48,  // 0x05 Keyboard b and B
    46,  // 0x06 Keyboard c and C
    32,  // 0x07 Keyboard d and D
    18,  // 0x08 Keyboard e and E
    33,  // 0x09 Keyboard f and F
    34,  // 0x0a Keyboard g and G
    35,  // 0x0b Keyboard h and H
    23,  // 0x0c Keyboard i and I
    36,  // 0x0d Keyboard j and J
    37,  // 0x0e Keyboard k and K
    38,  // 0x0f Keyboard l and L
    50,  // 0x10 Keyboard m and M
    49,  // 0x11 Keyboard n and N
    24,  // 0x12 Keyboard o and O
    25,  // 0x13 Keyboard p and P
    16,  // 0x14 Keyboard q and Q
    19,  // 0x15 Keyboard r and R
    31,  // 0x16 Keyboard s and S
    20,  // 0x17 Keyboard t and T
    22,  // 0x18 Keyboard u and U
    47,  // 0x19 Keyboard v and V
    17,  // 0x1a Keyboard w and W
    45,  // 0x1b Keyboard x and X
    21,  // 0x1c Keyboard y and Y
    44,  // 0x1d Keyboard z and Z
    2,   // 0x1e Keyboard 1 and !
    3,   // 0x1f Keyboard 2 and "
    4,   // 0x20 Keyboard 3 and GBP
    5,   // 0x21 Keyboard 4 and $
    6,   // 0x22 Keyboard 5 and %
    7,   // 0x23 Keyboard 6 and &
    8,   // 0x24 Keyboard 7 and /
    9,   // 0x25 Keyboard 8 and (
    10,  // 0x26 Keyboard 9 and )
    11,  // 0x27 Keyboard 0 and =
    28,  // 0x28 Keyboard Return (ENTER)
    1,   // 0x29 Keyboard ESCAPE
    14,  // 0x2a Keyboard DELETE (Backspace)
    15,  // 0x2b Keyboard Tab
    57,  // 0x2c Keyboard Spacebar
    12,  // 0x2d Keyboard ' and ?
    13,  // 0x2e Keyboard i` and ^
    26,  // 0x2f Keyboard e` and e'
    27,  // 0x30 Keyboard + and *
    43,  // 0x31 Keyboard \ and |
    0,   // 0x32 Keyboard Non-US # and ~ (unused on IT ISO)
    39,  // 0x33 Keyboard o` and @
    40,  // 0x34 Keyboard a` and #
    0x60,  // 0x35 Keyboard < and >
    51,    // 0x36 Keyboard , and ;
    52,    // 0x37 Keyboard . and :
    53,    // 0x38 Keyboard - and _
    58,    // 0x39 Keyboard Caps Lock
    59,    // 0x3a Keyboard F1
    60,    // 0x3b Keyboard F2
    61,    // 0x3c Keyboard F3
    62,    // 0x3d Keyboard F4
    63,    // 0x3e Keyboard F5
    64,    // 0x3f Keyboard F6
    65,    // 0x40 Keyboard F7
    66,    // 0x41 Keyboard F8
    67,    // 0x42 Keyboard F9
    68,    // 0x43 Keyboard F10
    0x62,  // 0x44 Keyboard F11 (acts like HELP key)
    0x61,  // 0x45 Keyboard F12 (acts like UNDO key)
    0,     // 0x46 Keyboard Print Screen
    0,     // 0x47 Keyboard Scroll Lock
    0,     // 0x48 Keyboard Pause
    82,    // 0x49 Keyboard Insert
    71,    // 0x4a Keyboard Home
    98,    // 0x4b Keyboard Page Up
    0x53,  // 0x4c Keyboard Delete Forward (acts like Delete key)
    0,     // 0x4d Keyboard End
    97,    // 0x4e Keyboard Page Down
    77,    // 0x4f Keyboard Right Arrow
    75,    // 0x50 Keyboard Left Arrow
    80,    // 0x51 Keyboard Down Arrow
    72,    // 0x52 Keyboard Up Arrow
    0,     // 0x53 Keyboard Num Lock and Clear
    101,   // 0x54 Keypad /
    102,   // 0x55 Keypad *
    74,    // 0x56 Keypad -
    78,    // 0x57 Keypad +
    114,   // 0x58 Keypad ENTER
    109,   // 0x59 Keypad 1 and End
    110,   // 0x5a Keypad 2 and Down Arrow
    111,   // 0x5b Keypad 3 and PageDn
    106,   // 0x5c Keypad 4 and Left Arrow
    107,   // 0x5d Keypad 5
    108,   // 0x5e Keypad 6 and Right Arrow
    103,   // 0x5f Keypad 7 and Home
    104,   // 0x60 Keypad 8 and Up Arrow
    105,   // 0x61 Keypad 9 and Page Up
    112,   // 0x62 Keypad 0 and Insert
    113,   // 0x63 Keypad . and Delete
    0x29,  // 0x64 Keyboard u` and S
    0,     // 0x65 Keyboard Application
    0,     // 0x66 Keyboard Power
    0,     // 0x67 Keypad =
    0,     // 0x68 Keyboard F13
    0,     // 0x69 Keyboard F14
    0,     // 0x6a Keyboard F15
    0,     // 0x6b Keyboard F16
    0,     // 0x6c Keyboard F17
    0,     // 0x6d Keyboard F18
    0,     // 0x6e Keyboard F19
    0,     // 0x6f Keyboard F20
    0,     // 0x70 Keyboard F21
    0,     // 0x71 Keyboard F22
    0,     // 0x72 Keyboard F23
    0,     // 0x73 Keyboard F24
    0,     // 0x74
    0,     // 0x75
    0,     // 0x76
    0,     // 0x77
    0,     // 0x78
    0,     // 0x79
    0,     // 0x7a
    0,     // 0x7b
    0,     // 0x7c
    0,     // 0x7d
    0,     // 0x7e
    0,     // 0x7f

};

const unsigned char stkeys_lookup_hid_fr_iso[128] = {
    0,  // 0x00 No key pressed
    0,  // 0x01 Keyboard Error Roll Over - used for all slots if too many keys
    0,  // 0x02 Not used
    0,  // 0x03 Not used
    0x1e,  // 0x04 FR key Q
    48,    // 0x05 Keyboard b and B
    46,    // 0x06 Keyboard c and C
    32,    // 0x07 Keyboard d and D
    18,    // 0x08 Keyboard e and E
    33,    // 0x09 Keyboard f and F
    34,    // 0x0a Keyboard g and G
    35,    // 0x0b Keyboard h and H
    23,    // 0x0c Keyboard i and I
    36,    // 0x0d Keyboard j and J
    37,    // 0x0e Keyboard k and K
    38,    // 0x0f Keyboard l and L
    0x32,  // 0x10 FR key , and ?
    49,    // 0x11 Keyboard n and N
    24,    // 0x12 Keyboard o and O
    25,    // 0x13 Keyboard p and P
    0x10,  // 0x14 FR key A
    19,    // 0x15 Keyboard r and R
    31,    // 0x16 Keyboard s and S
    20,    // 0x17 Keyboard t and T
    22,    // 0x18 Keyboard u and U
    47,    // 0x19 Keyboard v and V
    0x11,  // 0x1a FR key Z
    45,    // 0x1b Keyboard x and X
    21,    // 0x1c Keyboard y and Y
    0x2c,  // 0x1d FR key W
    2,     // 0x1e FR key & and 1
    3,     // 0x1f FR key e and 2
    4,     // 0x20 FR key " and 3
    5,     // 0x21 FR key ' and 4
    6,     // 0x22 FR key ( and 5
    7,     // 0x23 FR key - and 6
    8,     // 0x24 FR key e and 7
    9,     // 0x25 FR key _ and 8
    10,    // 0x26 FR key c and 9
    11,    // 0x27 FR key a and 0
    28,    // 0x28 Keyboard Return (ENTER)
    1,     // 0x29 Keyboard ESCAPE
    14,    // 0x2a Keyboard DELETE (Backspace)
    15,    // 0x2b Keyboard Tab
    57,    // 0x2c Keyboard Spacebar
    0x0c,  // 0x2d FR key ) and o
    0x0d,  // 0x2e FR key = and +
    26,    // 0x2f FR key ^ and "
    27,    // 0x30 FR key $ and L
    0x2b,  // 0x31 FR key # and ~
    0x29,  // 0x32 FR ISO key (non-US)
    0x27,  // 0x33 FR key M
    0x28,  // 0x34 FR key u and %
    0x60,  // 0x35 FR key * and mu
    0x33,  // 0x36 FR key ; and .
    0x34,  // 0x37 FR key : and /
    0x35,  // 0x38 FR key ! and S
    58,    // 0x39 Keyboard Caps Lock
    59,    // 0x3a Keyboard F1
    60,    // 0x3b Keyboard F2
    61,    // 0x3c Keyboard F3
    62,    // 0x3d Keyboard F4
    63,    // 0x3e Keyboard F5
    64,    // 0x3f Keyboard F6
    65,    // 0x40 Keyboard F7
    66,    // 0x41 Keyboard F8
    67,    // 0x42 Keyboard F9
    68,    // 0x43 Keyboard F10
    0x62,  // 0x44 Keyboard F11 (acts like HELP key)
    0x61,  // 0x45 Keyboard F12 (acts like UNDO key)
    0,     // 0x46 Keyboard Print Screen
    0,     // 0x47 Keyboard Scroll Lock
    0,     // 0x48 Keyboard Pause
    82,    // 0x49 Keyboard Insert
    71,    // 0x4a Keyboard Home
    98,    // 0x4b Keyboard Page Up
    0x53,  // 0x4c Keyboard Delete Forward (acts like Delete key)
    0,     // 0x4d Keyboard End
    97,    // 0x4e Keyboard Page Down
    77,    // 0x4f Keyboard Right Arrow
    75,    // 0x50 Keyboard Left Arrow
    80,    // 0x51 Keyboard Down Arrow
    72,    // 0x52 Keyboard Up Arrow
    0,     // 0x53 Keyboard Num Lock and Clear
    101,   // 0x54 Keypad /
    102,   // 0x55 Keypad *
    74,    // 0x56 Keypad -
    78,    // 0x57 Keypad +
    114,   // 0x58 Keypad ENTER
    109,   // 0x59 Keypad 1 and End
    110,   // 0x5a Keypad 2 and Down Arrow
    111,   // 0x5b Keypad 3 and PageDn
    106,   // 0x5c Keypad 4 and Left Arrow
    107,   // 0x5d Keypad 5
    108,   // 0x5e Keypad 6 and Right Arrow
    103,   // 0x5f Keypad 7 and Home
    104,   // 0x60 Keypad 8 and Up Arrow
    105,   // 0x61 Keypad 9 and Page Up
    112,   // 0x62 Keypad 0 and Insert
    113,   // 0x63 Keypad . and Delete
    0x29,  // 0x64 FR key < and >
    0,     // 0x65 Keyboard Application
    0,     // 0x66 Keyboard Power
    0,     // 0x67 Keypad =
    0,     // 0x68 Keyboard F13
    0,     // 0x69 Keyboard F14
    0,     // 0x6a Keyboard F15
    0,     // 0x6b Keyboard F16
    0,     // 0x6c Keyboard F17
    0,     // 0x6d Keyboard F18
    0,     // 0x6e Keyboard F19
    0,     // 0x6f Keyboard F20
    0,     // 0x70 Keyboard F21
    0,     // 0x71 Keyboard F22
    0,     // 0x72 Keyboard F23
    0,     // 0x73 Keyboard F24
    0,     // 0x74
    0,     // 0x75
    0,     // 0x76
    0,     // 0x77
    0,     // 0x78
    0,     // 0x79
    0,     // 0x7a
    0,     // 0x7b
    0,     // 0x7c
    0,     // 0x7d
    0,     // 0x7e
    0,     // 0x7f

};

const unsigned char stkeys_lookup_hid_de_iso[128] = {
    0,   // 0x00  No key pressed
    0,   // 0x01  Keyboard Error Roll Over - used for all slots if too many keys
         // are pressed ("Phantom key")
    0,   // 0x02  Not used
    0,   // 0x03  Not used
    30,  // 0x04  Keyboard a and A
    48,  // 0x05  Keyboard b and B
    46,  // 0x06  Keyboard c and C
    32,  // 0x07  Keyboard d and D
    18,  // 0x08  Keyboard e and E
    33,  // 0x09  Keyboard f and F
    34,  // 0x0a  Keyboard g and G
    35,  // 0x0b  Keyboard h and H
    23,  // 0x0c  Keyboard i and I
    36,  // 0x0d  Keyboard j and J
    37,  // 0x0e  Keyboard k and K
    38,  // 0x0f  Keyboard l and L
    50,  // 0x10  Keyboard m and M
    49,  // 0x11  Keyboard n and N
    24,  // 0x12  Keyboard o and O
    25,  // 0x13  Keyboard p and P
    16,  // 0x14  Keyboard q and Q
    19,  // 0x15  Keyboard r and R
    31,  // 0x16  Keyboard s and S
    20,  // 0x17  Keyboard t and T
    22,  // 0x18  Keyboard u and U
    47,  // 0x19  Keyboard v and V
    17,  // 0x1a  Keyboard w and W
    45,  // 0x1b  Keyboard x and X
    0x15,  // 0x1c  Keyboard y and Y (DE physical key prints z/Z)
    0x2C,  // 0x1d  Keyboard z and Z (DE physical key prints y/Y)
    2,     // 0x1e  Keyboard 1 and !
    3,     // 0x1f  Keyboard 2 and "
    4,     // 0x20  Keyboard 3 and §
    5,     // 0x21  Keyboard 4 and $
    6,     // 0x22  Keyboard 5 and %
    7,     // 0x23  Keyboard 6 and &
    8,     // 0x24  Keyboard 7 and /
    9,     // 0x25  Keyboard 8 and (
    10,    // 0x26  Keyboard 9 and )
    11,    // 0x27  Keyboard 0 and =
    28,    // 0x28  Keyboard Return (ENTER)
    1,     // 0x29  Keyboard ESCAPE
    14,    // 0x2a  Keyboard DELETE (Backspace)
    15,    // 0x2b  Keyboard Tab
    57,    // 0x2c  Keyboard Spacebar
    12,    // 0x2d  Keyboard - and _
    13,    // 0x2e  Keyboard + and *
    26,    // 0x2f  Keyboard ß and ?
    27,    // 0x30  Keyboard ´ and `
    0x2b,  // 0x31  Keyboard ^ and °
    43,    // 0x32  Keyboard Non-US # and ~
    39,    // 0x33  Keyboard ü and Ü
    40,    // 0x34  Keyboard ä and Ä
    0x60,  // 0x35  Keyboard ö and Ö
    51,    // 0x36  Keyboard , and ;
    52,    // 0x37  Keyboard . and :
    53,    // 0x38  Keyboard # and '
    58,    // 0x39  Keyboard Caps Lock
    59,    // 0x3a  Keyboard F1
    60,    // 0x3b  Keyboard F2
    61,    // 0x3c  Keyboard F3
    62,    // 0x3d  Keyboard F4
    63,    // 0x3e  Keyboard F5
    64,    // 0x3f  Keyboard F6
    65,    // 0x40  Keyboard F7
    66,    // 0x41  Keyboard F8
    67,    // 0x42  Keyboard F9
    68,    // 0x43  Keyboard F10
    0x62,  // 0x44  Keyboard F11 (acts like HELP key)
    0x61,  // 0x45  Keyboard F12 (acts like UNDO key)
    0,     // 0x46  Keyboard Print Screen
    0,     // 0x47  Keyboard Scroll Lock
    0,     // 0x48  Keyboard Pause
    82,    // 0x49  Keyboard Insert
    71,    // 0x4a  Keyboard Home
    98,    // 0x4b  Keyboard Page Up
    0x53,  // 0x4c  Keyboard Delete Forward (acts like Delete key)
    0,     // 0x4d  Keyboard End
    97,    // 0x4e  Keyboard Page Down
    77,    // 0x4f  Keyboard Right Arrow
    75,    // 0x50  Keyboard Left Arrow
    80,    // 0x51  Keyboard Down Arrow
    72,    // 0x52  Keyboard Up Arrow
    0,     // 0x53  Keyboard Num Lock and Clear
    101,   // 0x54  Keypad /
    102,   // 0x55  Keypad *
    74,    // 0x56  Keypad -
    78,    // 0x57  Keypad +
    114,   // 0x58  Keypad ENTER
    109,   // 0x59  Keypad 1 and End
    110,   // 0x5a  Keypad 2 and Down Arrow
    111,   // 0x5b  Keypad 3 and PageDn
    106,   // 0x5c  Keypad 4 and Left Arrow
    107,   // 0x5d  Keypad 5
    108,   // 0x5e  Keypad 6 and Right Arrow
    103,   // 0x5f  Keypad 7 and Home
    104,   // 0x60  Keypad 8 and Up Arrow
    105,   // 0x61  Keypad 9 and Page Up
    112,   // 0x62  Keypad 0 and Insert
    113,   // 0x63  Keypad . and Delete
    0x29,  // 0x64  Keyboard Non-US < and > (AltGr: |)
    0,     // 0x65  Keyboard Application
    0,     // 0x66  Keyboard Power
    0,     // 0x67  Keypad =
    0,     // 0x68  Keyboard F13
    0,     // 0x69  Keyboard F14
    0,     // 0x6a  Keyboard F15
    0,     // 0x6b  Keyboard F16
    0,     // 0x6c  Keyboard F17
    0,     // 0x6d  Keyboard F18
    0,     // 0x6e  Keyboard F19
    0,     // 0x6f  Keyboard F20
    0,     // 0x70  Keyboard F21
    0,     // 0x71  Keyboard F22
    0,     // 0x72  Keyboard F23
    0,     // 0x73  Keyboard F24
    0,     // 0x74
    0,     // 0x75
    0,     // 0x76
    0,     // 0x77
    0,     // 0x78
    0,     // 0x79
    0,     // 0x7a
    0,     // 0x7b
    0,     // 0x7c
    0,     // 0x7d
    0,     // 0x7e
    0,     // 0x7f
};

const unsigned char stkeys_lookup_hid_us_ansi[128] = {
    0,   // 0x00  No key pressed
    0,   // 0x01  Keyboard Error Roll Over - used for all slots if too many keys
         // are pressed ("Phantom key")
    0,   // 0x02  Not used
    0,   // 0x03  Not used
    30,  // 0x04  Keyboard a and A
    48,  // 0x05  Keyboard b and B
    46,  // 0x06  Keyboard c and C
    32,  // 0x07  Keyboard d and D
    18,  // 0x08  Keyboard e and E
    33,  // 0x09  Keyboard f and F
    34,  // 0x0a  Keyboard g and G
    35,  // 0x0b  Keyboard h and H
    23,  // 0x0c  Keyboard i and I
    36,  // 0x0d  Keyboard j and J
    37,  // 0x0e  Keyboard k and K
    38,  // 0x0f  Keyboard l and L
    50,  // 0x10  Keyboard m and M
    49,  // 0x11  Keyboard n and N
    24,  // 0x12  Keyboard o and O
    25,  // 0x13  Keyboard p and P
    16,  // 0x14  Keyboard q and Q
    19,  // 0x15  Keyboard r and R
    31,  // 0x16  Keyboard s and S
    20,  // 0x17  Keyboard t and T
    22,  // 0x18  Keyboard u and U
    47,  // 0x19  Keyboard v and V
    17,  // 0x1a  Keyboard w and W
    45,  // 0x1b  Keyboard x and X
    21,  // 0x1c  Keyboard y and Y
    44,  // 0x1d  Keyboard z and Z
    2,   // 0x1e  Keyboard 1 and !
    3,   // 0x1f  Keyboard 2 and @
    4,   // 0x20  Keyboard 3 and #
    5,   // 0x21  Keyboard 4 and $
    6,   // 0x22  Keyboard 5 and %
    7,   // 0x23  Keyboard 6 and ^
    8,   // 0x24  Keyboard 7 and &
    9,   // 0x25  Keyboard 8 and *
    10,  // 0x26  Keyboard 9 and (
    11,  // 0x27  Keyboard 0 and )
    28,  // 0x28  Keyboard Return (ENTER)
    1,   // 0x29  Keyboard ESCAPE
    14,  // 0x2a  Keyboard DELETE (Backspace)
    15,  // 0x2b  Keyboard Tab
    57,  // 0x2c  Keyboard Spacebar
    12,  // 0x2d  Keyboard - and _
    13,  // 0x2e  Keyboard = and +
    26,  // 0x2f  Keyboard [ and {
    27,  // 0x30  Keyboard ] and }
    43,  // 0x31  Keyboard \ and |
    0,   // 0x32  Keyboard Non-US # and ~
    39,  // 0x33  Keyboard ; and :
    40,  // 0x34  Keyboard ' and "
    0,   // 0x35  Keyboard ` and ~
    51,  // 0x36  Keyboard , and <
    52,  // 0x37  Keyboard . and >
    53,  // 0x38  Keyboard / and ?
    58,  // 0x39  Keyboard Caps Lock
    59,  // 0x3a  Keyboard F1
    60,  // 0x3b  Keyboard F2
    61,  // 0x3c  Keyboard F3
    62,  // 0x3d  Keyboard F4
    63,  // 0x3e  Keyboard F5
    64,  // 0x3f  Keyboard F6
    65,  // 0x40  Keyboard F7
    66,  // 0x41  Keyboard F8
    67,  // 0x42  Keyboard F9
    68,  // 0x43  Keyboard F10
    0x62,  // 0x44  Keyboard F11 (acts like HELP key)
    0x61,  // 0x45  Keyboard F12 (acts like UNDO key)
    0,     // 0x46  Keyboard Print Screen
    0,     // 0x47  Keyboard Scroll Lock
    0,     // 0x48  Keyboard Pause
    82,    // 0x49  Keyboard Insert
    71,    // 0x4a  Keyboard Home
    98,    // 0x4b  Keyboard Page Up
    0x53,  // 0x4c  Keyboard Delete Forward (acts like Delete key)
    0,     // 0x4d  Keyboard End
    97,    // 0x4e  Keyboard Page Down
    77,    // 0x4f  Keyboard Right Arrow
    75,    // 0x50  Keyboard Left Arrow
    80,    // 0x51  Keyboard Down Arrow
    72,    // 0x52  Keyboard Up Arrow
    0,     // 0x53  Keyboard Num Lock and Clear
    101,   // 0x54  Keypad /
    102,   // 0x55  Keypad *
    74,    // 0x56  Keypad -
    78,    // 0x57  Keypad +
    114,   // 0x58  Keypad ENTER
    109,   // 0x59  Keypad 1 and End
    110,   // 0x5a  Keypad 2 and Down Arrow
    111,   // 0x5b  Keypad 3 and PageDn
    106,   // 0x5c  Keypad 4 and Left Arrow
    107,   // 0x5d  Keypad 5
    108,   // 0x5e  Keypad 6 and Right Arrow
    103,   // 0x5f  Keypad 7 and Home
    104,   // 0x60  Keypad 8 and Up Arrow
    105,   // 0x61  Keypad 9 and Page Up
    112,   // 0x62  Keypad 0 and Insert
    113,   // 0x63  Keypad . and Delete
    0x2b,  // 0x64  Keyboard Non-US \ and |
    0,     // 0x65  Keyboard Application
    0,     // 0x66  Keyboard Power
    0,     // 0x67  Keypad =
    0,     // 0x68  Keyboard F13
    0,     // 0x69  Keyboard F14
    0,     // 0x6a  Keyboard F15
    0,     // 0x6b  Keyboard F16
    0,     // 0x6c  Keyboard F17
    0,     // 0x6d  Keyboard F18
    0,     // 0x6e  Keyboard F19
    0,     // 0x6f  Keyboard F20
    0,     // 0x70  Keyboard F21
    0,     // 0x71  Keyboard F22
    0,     // 0x72  Keyboard F23
    0,     // 0x73  Keyboard F24
    0,     // 0x74
    0,     // 0x75
    0,     // 0x76
    0,     // 0x77
    0,     // 0x78
    0,     // 0x79
    0,     // 0x7a
    0,     // 0x7b
    0,     // 0x7c
    0,     // 0x7d
    0,     // 0x7e
    0,     // 0x7f
};

const unsigned char stkeys_lookup_hid_gb_iso[128] = {
    0,   // 0x00  No key pressed
    0,   // 0x01  Keyboard Error Roll Over - used for all slots if too many keys
         // are pressed ("Phantom key")
    0,   // 0x02  Not used
    0,   // 0x03  Not used
    30,  // 0x04  Keyboard a and A
    48,  // 0x05  Keyboard b and B
    46,  // 0x06  Keyboard c and C
    32,  // 0x07  Keyboard d and D
    18,  // 0x08  Keyboard e and E
    33,  // 0x09  Keyboard f and F
    34,  // 0x0a  Keyboard g and G
    35,  // 0x0b  Keyboard h and H
    23,  // 0x0c  Keyboard i and I
    36,  // 0x0d  Keyboard j and J
    37,  // 0x0e  Keyboard k and K
    38,  // 0x0f  Keyboard l and L
    50,  // 0x10  Keyboard m and M
    49,  // 0x11  Keyboard n and N
    24,  // 0x12  Keyboard o and O
    25,  // 0x13  Keyboard p and P
    16,  // 0x14  Keyboard q and Q
    19,  // 0x15  Keyboard r and R
    31,  // 0x16  Keyboard s and S
    20,  // 0x17  Keyboard t and T
    22,  // 0x18  Keyboard u and U
    47,  // 0x19  Keyboard v and V
    17,  // 0x1a  Keyboard w and W
    45,  // 0x1b  Keyboard x and X
    21,  // 0x1c  Keyboard y and Y
    44,  // 0x1d  Keyboard z and Z
    2,   // 0x1e  Keyboard 1 and !
    3,   // 0x1f  Keyboard 2 and @
    4,   // 0x20  Keyboard 3 and #
    5,   // 0x21  Keyboard 4 and $
    6,   // 0x22  Keyboard 5 and %
    7,   // 0x23  Keyboard 6 and ^
    8,   // 0x24  Keyboard 7 and &
    9,   // 0x25  Keyboard 8 and *
    10,  // 0x26  Keyboard 9 and (
    11,  // 0x27  Keyboard 0 and )
    28,  // 0x28  Keyboard Return (ENTER)
    1,   // 0x29  Keyboard ESCAPE
    14,  // 0x2a  Keyboard DELETE (Backspace)
    15,  // 0x2b  Keyboard Tab
    57,  // 0x2c  Keyboard Spacebar
    12,  // 0x2d  Keyboard - and _
    13,  // 0x2e  Keyboard = and +
    26,  // 0x2f  Keyboard [ and {
    27,  // 0x30  Keyboard ] and }
    0x60,  // 0x31  Keyboard \ and |
    41,    // 0x32  Keyboard Non-US # and ~
    39,    // 0x33  Keyboard ; and :
    40,    // 0x34  Keyboard ' and "
    43,    // 0x35  Keyboard ` and ~
    51,    // 0x36  Keyboard , and <
    52,    // 0x37  Keyboard . and >
    53,    // 0x38  Keyboard / and ?
    58,    // 0x39  Keyboard Caps Lock
    59,    // 0x3a  Keyboard F1
    60,    // 0x3b  Keyboard F2
    61,    // 0x3c  Keyboard F3
    62,    // 0x3d  Keyboard F4
    63,    // 0x3e  Keyboard F5
    64,    // 0x3f  Keyboard F6
    65,    // 0x40  Keyboard F7
    66,    // 0x41  Keyboard F8
    67,    // 0x42  Keyboard F9
    68,    // 0x43  Keyboard F10
    0x62,  // 0x44  Keyboard F11 (acts like HELP key)
    0x61,  // 0x45  Keyboard F12 (acts like UNDO key)
    0,     // 0x46  Keyboard Print Screen
    0,     // 0x47  Keyboard Scroll Lock
    0,     // 0x48  Keyboard Pause
    82,    // 0x49  Keyboard Insert
    71,    // 0x4a  Keyboard Home
    98,    // 0x4b  Keyboard Page Up
    0x53,  // 0x4c  Keyboard Delete Forward (acts like Delete key)
    0,     // 0x4d  Keyboard End
    97,    // 0x4e  Keyboard Page Down
    77,    // 0x4f  Keyboard Right Arrow
    75,    // 0x50  Keyboard Left Arrow
    80,    // 0x51  Keyboard Down Arrow
    72,    // 0x52  Keyboard Up Arrow
    0,     // 0x53  Keyboard Num Lock and Clear
    101,   // 0x54  Keypad /
    102,   // 0x55  Keypad *
    74,    // 0x56  Keypad -
    78,    // 0x57  Keypad +
    114,   // 0x58  Keypad ENTER
    109,   // 0x59  Keypad 1 and End
    110,   // 0x5a  Keypad 2 and Down Arrow
    111,   // 0x5b  Keypad 3 and PageDn
    106,   // 0x5c  Keypad 4 and Left Arrow
    107,   // 0x5d  Keypad 5
    108,   // 0x5e  Keypad 6 and Right Arrow
    103,   // 0x5f  Keypad 7 and Home
    104,   // 0x60  Keypad 8 and Up Arrow
    105,   // 0x61  Keypad 9 and Page Up
    112,   // 0x62  Keypad 0 and Insert
    113,   // 0x63  Keypad . and Delete
    0x29,  // 0x64  Keyboard Non-US \ and |
    0,     // 0x65  Keyboard Application
    0,     // 0x66  Keyboard Power
    0,     // 0x67  Keypad =
    0,     // 0x68  Keyboard F13
    0,     // 0x69  Keyboard F14
    0,     // 0x6a  Keyboard F15
    0,     // 0x6b  Keyboard F16
    0,     // 0x6c  Keyboard F17
    0,     // 0x6d  Keyboard F18
    0,     // 0x6e  Keyboard F19
    0,     // 0x6f  Keyboard F20
    0,     // 0x70  Keyboard F21
    0,     // 0x71  Keyboard F22
    0,     // 0x72  Keyboard F23
    0,     // 0x73  Keyboard F24
    0,     // 0x74
    0,     // 0x75
    0,     // 0x76
    0,     // 0x77
    0,     // 0x78
    0,     // 0x79
    0,     // 0x7a
    0,     // 0x7b
    0,     // 0x7c
    0,     // 0x7d
    0,     // 0x7e
    0,     // 0x7f
};

const unsigned char stkeys_lookup_hid_es_iso[128] = {
    0,   // 0x00  No key pressed
    0,   // 0x01  Keyboard Error Roll Over - used for all slots if too many keys
         // are pressed ("Phantom key")
    0,   // 0x02  Not used
    0,   // 0x03  Not used
    30,  // 0x04  Keyboard a and A
    48,  // 0x05  Keyboard b and B
    46,  // 0x06  Keyboard c and C
    32,  // 0x07  Keyboard d and D
    18,  // 0x08  Keyboard e and E
    33,  // 0x09  Keyboard f and F
    34,  // 0x0a  Keyboard g and G
    35,  // 0x0b  Keyboard h and H
    23,  // 0x0c  Keyboard i and I
    36,  // 0x0d  Keyboard j and J
    37,  // 0x0e  Keyboard k and K
    38,  // 0x0f  Keyboard l and L
    50,  // 0x10  Keyboard m and M
    49,  // 0x11  Keyboard n and N
    24,  // 0x12  Keyboard o and O
    25,  // 0x13  Keyboard p and P
    16,  // 0x14  Keyboard q and Q
    19,  // 0x15  Keyboard r and R
    31,  // 0x16  Keyboard s and S
    20,  // 0x17  Keyboard t and T
    22,  // 0x18  Keyboard u and U
    47,  // 0x19  Keyboard v and V
    17,  // 0x1a  Keyboard w and W
    45,  // 0x1b  Keyboard x and X
    21,  // 0x1c  Keyboard y and Y
    44,  // 0x1d  Keyboard z and Z
    2,   // 0x1e  Keyboard 1 and !
    3,   // 0x1f  Keyboard 2 and " @
    4,   // 0x20  Keyboard 3 and . #
    5,   // 0x21  Keyboard 4 and $
    6,   // 0x22  Keyboard 5 and %
    7,   // 0x23  Keyboard 6 and &
    8,   // 0x24  Keyboard 7 and /
    9,   // 0x25  Keyboard 8 and (
    10,  // 0x26  Keyboard 9 and )
    11,  // 0x27  Keyboard 0 and =
    28,  // 0x28  Keyboard Return (ENTER)
    1,   // 0x29  Keyboard ESCAPE
    14,  // 0x2a  Keyboard DELETE (Backspace)
    15,  // 0x2b  Keyboard Tab
    57,  // 0x2c  Keyboard Spacebar
    12,  // 0x2d  Keyboard - and _
    13,  // 0x2e  Keyboard + and *
    26,  // 0x2f  Keyboard ' and ?
    27,  // 0x30  Keyboard ¡ and ¿
    0x2b,  // 0x31  Keyboard º and ª
    41,    // 0x32  Keyboard Non-US # and ~ (unused on ES ISO)
    39,    // 0x33  Keyboard ` and ^
    40,    // 0x34  Keyboard ´ and ¨
    0x60,  // 0x35  Keyboard < and >
    51,    // 0x36  Keyboard , and ;
    52,    // 0x37  Keyboard . and :
    53,    // 0x38  Keyboard ç and Ç
    58,    // 0x39  Keyboard Caps Lock
    59,    // 0x3a  Keyboard F1
    60,    // 0x3b  Keyboard F2
    61,    // 0x3c  Keyboard F3
    62,    // 0x3d  Keyboard F4
    63,    // 0x3e  Keyboard F5
    64,    // 0x3f  Keyboard F6
    65,    // 0x40  Keyboard F7
    66,    // 0x41  Keyboard F8
    67,    // 0x42  Keyboard F9
    68,    // 0x43  Keyboard F10
    0x62,  // 0x44  Keyboard F11 (acts like HELP key)
    0x61,  // 0x45  Keyboard F12 (acts like UNDO key)
    0,     // 0x46  Keyboard Print Screen
    0,     // 0x47  Keyboard Scroll Lock
    0,     // 0x48  Keyboard Pause
    82,    // 0x49  Keyboard Insert
    71,    // 0x4a  Keyboard Home
    98,    // 0x4b  Keyboard Page Up
    0x53,  // 0x4c  Keyboard Delete Forward (acts like Delete key)
    0,     // 0x4d  Keyboard End
    97,    // 0x4e  Keyboard Page Down
    77,    // 0x4f  Keyboard Right Arrow
    75,    // 0x50  Keyboard Left Arrow
    80,    // 0x51  Keyboard Down Arrow
    72,    // 0x52  Keyboard Up Arrow
    0,     // 0x53  Keyboard Num Lock and Clear
    101,   // 0x54  Keypad /
    102,   // 0x55  Keypad *
    74,    // 0x56  Keypad -
    78,    // 0x57  Keypad +
    114,   // 0x58  Keypad ENTER
    109,   // 0x59  Keypad 1 and End
    110,   // 0x5a  Keypad 2 and Down Arrow
    111,   // 0x5b  Keypad 3 and PageDn
    106,   // 0x5c  Keypad 4 and Left Arrow
    107,   // 0x5d  Keypad 5
    108,   // 0x5e  Keypad 6 and Right Arrow
    103,   // 0x5f  Keypad 7 and Home
    104,   // 0x60  Keypad 8 and Up Arrow
    105,   // 0x61  Keypad 9 and Page Up
    112,   // 0x62  Keypad 0 and Insert
    113,   // 0x63  Keypad . and Delete
    0x29,  // 0x64  Keyboard º ª and '\'
    0,     // 0x65  Keyboard Application
    0,     // 0x66  Keyboard Power
    0,     // 0x67  Keypad =
    0,     // 0x68  Keyboard F13
    0,     // 0x69  Keyboard F14
    0,     // 0x6a  Keyboard F15
    0,     // 0x6b  Keyboard F16
    0,     // 0x6c  Keyboard F17
    0,     // 0x6d  Keyboard F18
    0,     // 0x6e  Keyboard F19
    0,     // 0x6f  Keyboard F20
    0,     // 0x70  Keyboard F21
    0,     // 0x71  Keyboard F22
    0,     // 0x72  Keyboard F23
    0,     // 0x73  Keyboard F24
    0,     // 0x74
    0,     // 0x75
    0,     // 0x76
    0,     // 0x77
    0,     // 0x78
    0,     // 0x79
    0,     // 0x7a
    0,     // 0x7b
    0,     // 0x7c
    0,     // 0x7d
    0,     // 0x7e
    0,     // 0x7f
};
