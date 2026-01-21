#include "btloop.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"
#include "btstack_util.h"
#include "gconfig.h"
#include "joystick.h"
#include "pico/async_context.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "stkeys.h"
#include "uni.h"

void launch_config_cb(void);
#ifdef __cplusplus
extern "C" {
#endif
void hidinput_update_mouse(int16_t dx, int16_t dy, bool left_down,
                           bool right_down);
#ifdef __cplusplus
}
#endif
#include "mouse.h"

static uint8_t last_keyboard_keys[6];

static bool btstack_paused = false;
static absolute_time_t s_mouse_last_sample;

typedef struct {
  const char *param;
  bd_addr_t addr;
  bool valid;
} bt_allow_entry_t;

static bt_allow_entry_t bt_allow_entries[] = {
    {PARAM_BT_KEYBOARD, {0}, false},
    {PARAM_BT_MOUSE, {0}, false},
    {PARAM_BT_GAMEPAD, {0}, false},
};

static void bt_allow_entry_set(size_t index, const bd_addr_t addr) {
  if (index >= (sizeof(bt_allow_entries) / sizeof(bt_allow_entries[0]))) {
    return;
  }
  bt_allow_entries[index].valid = true;
  memcpy(bt_allow_entries[index].addr, addr, sizeof(bd_addr_t));
}

static const char *bt_get_layout(void) {
  SettingsConfigEntry *entry =
      settings_find_entry(gconfig_getContext(), PARAM_BT_KB_LAYOUT);
  if (entry != NULL && entry->value != NULL) {
    return entry->value;
  }
  return "us";
}

#ifndef MOUSE_LINE_POLL_INTERVAL_US
#define MOUSE_LINE_POLL_INTERVAL_US 1000
#endif

static bool key_array_contains(const uint8_t *array, uint8_t key) {
  for (size_t i = 0; i < 6; ++i) {
    if (array[i] == key) {
      return true;
    }
  }
  return false;
}

typedef struct {
  uint8_t usage;
  const char *name;
} usage_name_entry_t;

static const usage_name_entry_t k_non_printable_names[] = {
    {0x28, "ENTER"},       {0x29, "ESC"},          {0x2A, "BACKSPACE"},
    {0x2B, "TAB"},         {0x2C, "SPACE"},        {0x2D, "MINUS"},
    {0x2E, "EQUAL"},       {0x2F, "LEFT BRACKET"}, {0x30, "RIGHT BRACKET"},
    {0x31, "BACKSLASH"},   {0x33, "SEMICOLON"},    {0x34, "APOSTROPHE"},
    {0x35, "GRAVE"},       {0x36, "COMMA"},        {0x37, "PERIOD"},
    {0x38, "SLASH"},       {0x39, "CAPS LOCK"},    {0x3A, "F1"},
    {0x3B, "F2"},          {0x3C, "F3"},           {0x3D, "F4"},
    {0x3E, "F5"},          {0x3F, "F6"},           {0x40, "F7"},
    {0x41, "F8"},          {0x42, "F9"},           {0x43, "F10"},
    {0x44, "F11"},         {0x45, "F12"},          {0x46, "PRINT SCREEN"},
    {0x47, "SCROLL LOCK"}, {0x48, "PAUSE"},        {0x49, "INSERT"},
    {0x4A, "HOME"},        {0x4B, "PAGE UP"},      {0x4C, "DELETE"},
    {0x4D, "END"},         {0x4E, "PAGE DOWN"},    {0x4F, "RIGHT ARROW"},
    {0x50, "LEFT ARROW"},  {0x51, "DOWN ARROW"},   {0x52, "UP ARROW"},
    {0x53, "NUM LOCK"},    {0x54, "KEYPAD /"},     {0x55, "KEYPAD *"},
    {0x56, "KEYPAD -"},    {0x57, "KEYPAD +"},     {0x58, "KEYPAD ENTER"},
    {0x59, "KEYPAD 1"},    {0x5A, "KEYPAD 2"},     {0x5B, "KEYPAD 3"},
    {0x5C, "KEYPAD 4"},    {0x5D, "KEYPAD 5"},     {0x5E, "KEYPAD 6"},
    {0x5F, "KEYPAD 7"},    {0x60, "KEYPAD 8"},     {0x61, "KEYPAD 9"},
    {0x62, "KEYPAD 0"},    {0x63, "KEYPAD ."},     {0x64, "NON-US BACKSLASH"},
    {0x65, "APPLICATION"}, {0x66, "POWER"},        {0x67, "KEYPAD ="},
    {0x68, "F13"},         {0x69, "F14"},          {0x6A, "F15"},
    {0x6B, "F16"},         {0x6C, "F17"},          {0x6D, "F18"},
    {0x6E, "F19"},         {0x6F, "F20"},          {0x70, "F21"},
    {0x71, "F22"},         {0x72, "F23"},          {0x73, "F24"},
    {0xE0, "LEFT CTRL"},   {0xE1, "LEFT SHIFT"},   {0xE2, "LEFT ALT"},
    {0xE3, "LEFT GUI"},    {0xE4, "RIGHT CTRL"},   {0xE5, "RIGHT SHIFT"},
    {0xE6, "RIGHT ALT"},   {0xE7, "RIGHT GUI"},
};

enum {
  MOD_LEFT_CTRL = 0x01,
  MOD_LEFT_SHIFT = 0x02,
  MOD_LEFT_ALT = 0x04,
  MOD_LEFT_GUI = 0x08,
  MOD_RIGHT_CTRL = 0x10,
  MOD_RIGHT_SHIFT = 0x20,
  MOD_RIGHT_ALT = 0x40,
  MOD_RIGHT_GUI = 0x80,
};

static const char *hid_usage_to_name(uint8_t usage) {
  for (size_t i = 0;
       i < (sizeof(k_non_printable_names) / sizeof(k_non_printable_names[0]));
       ++i) {
    if (k_non_printable_names[i].usage == usage) {
      return k_non_printable_names[i].name;
    }
  }
  return NULL;
}

typedef struct {
  uint8_t mask;
  uint8_t usage;
  const char *name;
} modifier_entry_t;

static const modifier_entry_t k_modifier_entries[] = {
    {MOD_LEFT_CTRL, 0xE0, "LEFT CTRL"},
    {MOD_LEFT_SHIFT, 0xE1, "LEFT SHIFT"},
    {MOD_LEFT_ALT, 0xE2, "LEFT ALT"},
    {MOD_LEFT_GUI, 0xE3, "LEFT GUI"},
    {MOD_RIGHT_CTRL, 0xE4, "RIGHT CTRL"},
    {MOD_RIGHT_SHIFT, 0xE5, "RIGHT SHIFT"},
    {MOD_RIGHT_ALT, 0xE6, "RIGHT ALT"},
    {MOD_RIGHT_GUI, 0xE7, "RIGHT GUI"},
};

static char hid_usage_to_printable(uint8_t usage, bool shift_active) {
  if (usage >= 0x04 && usage <= 0x1D) {
    char base = shift_active ? 'A' : 'a';
    return (char)(base + (usage - 0x04));
  }

  switch (usage) {
    case 0x1E:
      return shift_active ? '!' : '1';
    case 0x1F:
      return shift_active ? '@' : '2';
    case 0x20:
      return shift_active ? '#' : '3';
    case 0x21:
      return shift_active ? '$' : '4';
    case 0x22:
      return shift_active ? '%' : '5';
    case 0x23:
      return shift_active ? '^' : '6';
    case 0x24:
      return shift_active ? '&' : '7';
    case 0x25:
      return shift_active ? '*' : '8';
    case 0x26:
      return shift_active ? '(' : '9';
    case 0x27:
      return shift_active ? ')' : '0';
    case 0x28:
      return '\n';
    case 0x2A:
      return '\b';
    case 0x2B:
      return '\t';
    case 0x2C:
      return ' ';
    case 0x2D:
      return shift_active ? '_' : '-';
    case 0x2E:
      return shift_active ? '+' : '=';
    case 0x2F:
      return shift_active ? '{' : '[';
    case 0x30:
      return shift_active ? '}' : ']';
    case 0x31:
      return shift_active ? '|' : '\\';
    case 0x33:
      return shift_active ? ':' : ';';
    case 0x34:
      return shift_active ? '"' : '\'';
    case 0x35:
      return shift_active ? '~' : '`';
    case 0x36:
      return shift_active ? '<' : ',';
    case 0x37:
      return shift_active ? '>' : '.';
    case 0x38:
      return shift_active ? '?' : '/';
    default:
      break;
  }

  return 0;
}

static void log_printable_key(char c) {
  switch (c) {
    case '\n':
      DPRINTF("Key pressed: <ENTER>\n");
      break;
    case '\t':
      DPRINTF("Key pressed: <TAB>\n");
      break;
    case '\b':
      DPRINTF("Key pressed: <BACKSPACE>\n");
      break;
    case ' ':
      DPRINTF("Key pressed: <SPACE>\n");
      break;
    default:
      if (c >= 32 && c <= 126) {
        DPRINTF("Key pressed: %c\n", c);
      } else {
        DPRINTF("Key pressed: 0x%02x\n", (unsigned char)c);
      }
      break;
  }
}

static void handle_keyboard_report(const uint8_t *report, uint16_t len) {
  if (report == NULL || len == 0) {
    return;
  }

  if (len < 8) {
    return;
  }

  const uint8_t raw_modifiers = report[0];
  const uint8_t *keycodes = &report[2];
  const size_t key_slots = len - 2 < 6 ? len - 2 : 6;
  static uint8_t last_modifiers = 0;

  // DPRINTF("raw report (len=%u):", (unsigned)len);
  // for (uint16_t i = 0; i < len; ++i) {
  //   DPRINTFRAW(" %02x", report[i]);
  // }
  // DPRINTFRAW("\n");

  uint8_t normalized_keys[6] = {0};
  if (key_slots > 0) {
    memcpy(normalized_keys, keycodes, key_slots);
  }

  uint8_t fixed_modifiers = 0;
  if (key_slots > 0) {
    uint8_t code0 = normalized_keys[0];
    if (code0 != 0) {
      if (code0 & MOD_LEFT_CTRL) fixed_modifiers |= MOD_LEFT_CTRL;
      if (code0 & MOD_LEFT_SHIFT) fixed_modifiers |= MOD_LEFT_SHIFT;
      if (code0 & MOD_LEFT_ALT) fixed_modifiers |= MOD_LEFT_ALT;
      if (code0 & MOD_LEFT_GUI) fixed_modifiers |= MOD_LEFT_GUI;
      if (code0 & MOD_RIGHT_CTRL) fixed_modifiers |= MOD_RIGHT_CTRL;
      if (code0 & MOD_RIGHT_SHIFT) fixed_modifiers |= MOD_RIGHT_SHIFT;
      if (code0 & MOD_RIGHT_ALT) fixed_modifiers |= MOD_RIGHT_ALT;
      if (code0 & MOD_RIGHT_GUI) fixed_modifiers |= MOD_RIGHT_GUI;
      normalized_keys[0] = 0;
    }
  }
  DPRINTF("Normalized modifiers: 0x%02x, ", fixed_modifiers);

  // Show the normalized keys as a row
  DPRINTFRAW("Normalized keys:");
  for (size_t i = 0; i < key_slots; ++i) {
    DPRINTFRAW(" %02x", normalized_keys[i]);
  }
  DPRINTFRAW("\n");

  bool alt_pressed = (fixed_modifiers & (MOD_LEFT_ALT | MOD_RIGHT_ALT)) != 0;

  if (alt_pressed) {
    bool replaced = false;
    for (size_t i = 0; i < key_slots; ++i) {
      if (normalized_keys[i] == 0x2A) {  // Backspace
        normalized_keys[i] = 0x49;       // Insert
        replaced = true;
      }
    }
    if (replaced) {
      fixed_modifiers &= ~(MOD_LEFT_CTRL | MOD_RIGHT_CTRL);
      DPRINTF("Shortcut: ALT+BACKSPACE -> INSERT\n");
    }
    bool clrhome_replaced = false;
    for (size_t i = 0; i < key_slots; ++i) {
      if (normalized_keys[i] == 0x4C) {  // Delete Forward
        normalized_keys[i] = 0x4A;       // Home (CLR/HOME on Atari ST)
        clrhome_replaced = true;
      }
    }
    if (clrhome_replaced) {
      fixed_modifiers &= ~(MOD_LEFT_ALT | MOD_RIGHT_ALT);
      DPRINTF("Shortcut: ALT+DELETE -> CLR/HOME\n");
    }
  }

  // const bool shift_active =
  //     (fixed_modifiers & (MOD_LEFT_SHIFT | MOD_RIGHT_SHIFT)) != 0;

  // DPRINTF("Shift active: %s\n", shift_active ? "yes" : "no");

  stkeys_apply_keyboard_report_layout(last_keyboard_keys, normalized_keys, 6,
                                      fixed_modifiers, bt_get_layout());

  // uint8_t changed_modifiers = fixed_modifiers ^ last_modifiers;
  // if (changed_modifiers != 0) {
  //   for (size_t i = 0;
  //        i < (sizeof(k_modifier_entries) / sizeof(k_modifier_entries[0]));
  //        ++i) {
  //     const modifier_entry_t *entry = &k_modifier_entries[i];
  //     if ((changed_modifiers & entry->mask) != 0 &&
  //         (fixed_modifiers & entry->mask) != 0) {
  //       DPRINTF("Key pressed: 0x%02x (%s)\n", entry->usage, entry->name);
  //     }
  //   }
  // }

  // for (size_t i = 0; i < key_slots; ++i) {
  //   uint8_t keycode = normalized_keys[i];
  //   if (keycode == 0) {
  //     continue;
  //   }
  //   if (key_array_contains(last_keyboard_keys, keycode)) {
  //     continue;
  //   }

  //   DPRINTF("New key detected: 0x%02x + shift: %s\n", keycode,
  //           shift_active ? "yes" : "no");
  //   char printable = hid_usage_to_printable(keycode, shift_active);
  //   if (printable) {
  //     log_printable_key(printable);
  //   } else {
  //     const char *name = hid_usage_to_name(keycode);
  //     if (name) {
  //       DPRINTF("Key pressed: 0x%02x (%s)\n", keycode, name);
  //     } else {
  //       DPRINTF("Key pressed: usage=0x%02x (shift=%u)\n", keycode,
  //               shift_active ? 1u : 0u);
  //     }
  //   }
  // }

  memset(last_keyboard_keys, 0, sizeof(last_keyboard_keys));
  memcpy(last_keyboard_keys, normalized_keys, sizeof(last_keyboard_keys));
  last_modifiers = fixed_modifiers;
}

void btloop_tick(void) {
  absolute_time_t now = get_absolute_time();

  if (absolute_time_diff_us(s_mouse_last_sample, now) >=
      MOUSE_LINE_POLL_INTERVAL_US) {
    s_mouse_last_sample = now;
    mouse_update();
  }
}

static void load_bt_allowlist_entries(void) {
  for (size_t i = 0;
       i < (sizeof(bt_allow_entries) / sizeof(bt_allow_entries[0])); ++i) {
    bt_allow_entries[i].valid = false;
    SettingsConfigEntry *entry =
        settings_find_entry(gconfig_getContext(), bt_allow_entries[i].param);
    if (entry == NULL || entry->value == NULL || entry->value[0] == '\0') {
      DPRINTF("No BD_ADDR configured for %s\n", bt_allow_entries[i].param);
      continue;
    }
    if (sscanf_bd_addr(entry->value, bt_allow_entries[i].addr) == 1) {
      bt_allow_entries[i].valid = true;
      DPRINTF("Loaded BD_ADDR for %s: %s\n", bt_allow_entries[i].param,
              bd_addr_to_str(bt_allow_entries[i].addr));
    } else {
      DPRINTF("Invalid BD_ADDR for %s: '%s'\n", bt_allow_entries[i].param,
              entry->value);
    }
  }
}

//
// Platform Overrides
//
static void btloop_init(int argc, const char **argv) {
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  DPRINTF("btloop_init: init()\n");
}

static void btloop_on_init_complete(void) {
  DPRINTF("btloop: on_init_complete()\n");

  // Safe to call "unsafe" functions since they are called from BT thread

  // for (size_t i = 0;
  //      i < (sizeof(bt_allow_entries) / sizeof(bt_allow_entries[0])); ++i) {
  //   if (bt_allow_entries[i].valid) {
  //     uni_bt_allowlist_add_addr(bt_allow_entries[i].addr);
  //   }
  // }

  // Stay idle and let already paired devices initiate the connection.
  DPRINTF(
      "BT scanning/autoconnect disabled; waiting for paired devices to connect "
      "back\n");

  uni_bt_start_scanning_and_autoconnect_safe();

  // uni_bt_service_set_enabled(true);

  // uni_bt_allowlist_add_addr((bd_addr_t){0x00, 0x11, 0x67, 0xF7, 0x0F,
  //                                       0x03});  // Logitech Mouse example
  // uni_bt_allowlist_add_addr(
  //     (bd_addr_t){0x60, 0xC5, 0x47, 0x1E, 0x01, 0x74});  // Magic Keyboard

  // uni_bt_allowlist_add_addr(
  //     (bd_addr_t){0x76, 0x08, 0x31, 0x01, 0x49, 0x9B});  // ESYNYC

  // uni_bt_allowlist_add_addr(
  //     (bd_addr_t){0xEB, 0x60, 0x31, 0xFB, 0x69, 0xE2});  // Microsoft mouse

  // uni_bt_del_keys_unsafe();
  // uni_bt_le_delete_bonded_keys();
  // uni_bt_allowlist_remove_all();

  uni_bt_allowlist_list();

  uni_bt_list_keys_unsafe();

  uni_property_dump_all();
}

static uni_error_t btloop_on_device_discovered(bd_addr_t addr, const char *name,
                                               uint16_t cod, uint8_t rssi) {
  const uint16_t allowed_minor =
      UNI_BT_COD_MINOR_KEYBOARD_AND_MICE | UNI_BT_COD_MINOR_KEYBOARD |
      UNI_BT_COD_MINOR_MICE | UNI_BT_COD_MINOR_GAMEPAD |
      UNI_BT_COD_MINOR_JOYSTICK;

  // You can filter discovered devices here. Return any value different from
  // UNI_ERROR_SUCCESS;
  // @param addr: the Bluetooth address
  // @param name: could be NULL, could be zero-length, or might contain the
  // name.
  // @param cod: Class of Device. See "uni_bt_defines.h" for possible values.
  // @param rssi: Received Signal Strength Indicator (RSSI) measured in dBms.
  // The higher (255) the better.

  DPRINTF("%s, name='%s', cod=0x%06x, rssi=%d\n", bd_addr_to_str(addr),
          name ? name : "<null>", (unsigned)cod, (int8_t)rssi);

  bool is_peripheral =
      (cod & UNI_BT_COD_MAJOR_MASK) == UNI_BT_COD_MAJOR_PERIPHERAL;
  bool is_keyboard_mouse = (cod & UNI_BT_COD_MINOR_MASK & allowed_minor) != 0;
  bool is_keyboard =
      (cod & UNI_BT_COD_MINOR_MASK & UNI_BT_COD_MINOR_KEYBOARD) != 0;
  bool is_mouse = (cod & UNI_BT_COD_MINOR_MASK & UNI_BT_COD_MINOR_MICE) != 0;
  bool is_gamepad = (cod & UNI_BT_COD_MINOR_MASK &
                     (UNI_BT_COD_MINOR_GAMEPAD | UNI_BT_COD_MINOR_JOYSTICK)) !=
                    0;
  DPRINTF(
      "is_peripheral=%s, is_keyboard=%s, is_mouse=%s, is_gamepad=%s\n",
      is_peripheral ? "yes" : "no",
      (cod & UNI_BT_COD_MINOR_MASK & (UNI_BT_COD_MINOR_KEYBOARD)) ? "yes"
                                                                  : "no",
      (cod & UNI_BT_COD_MINOR_MASK & (UNI_BT_COD_MINOR_MICE)) ? "yes" : "no",
      (cod & UNI_BT_COD_MINOR_MASK &
       (UNI_BT_COD_MINOR_GAMEPAD | UNI_BT_COD_MINOR_JOYSTICK))
          ? "yes"
          : "no");

  if (is_peripheral && is_keyboard_mouse) {
    int matched_index = -1;
    for (size_t i = 0;
         i < (sizeof(bt_allow_entries) / sizeof(bt_allow_entries[0])); ++i) {
      if (bt_allow_entries[i].valid &&
          memcmp(bt_allow_entries[i].addr, addr, sizeof(bd_addr_t)) == 0) {
        matched_index = (int)i;
        break;
      }
    }

    if (matched_index >= 0) {
      return UNI_ERROR_SUCCESS;
    }

    int assign_index = -1;
    if (is_keyboard && !bt_allow_entries[0].valid) {
      assign_index = 0;
    } else if (is_mouse && !bt_allow_entries[1].valid) {
      assign_index = 1;
    } else if (is_gamepad && !bt_allow_entries[2].valid) {
      assign_index = 2;
    }

    if (assign_index >= 0) {
      bt_allow_entry_set((size_t)assign_index, addr);
      DPRINTF("Assigned %s to %s\n", bd_addr_to_str(addr),
              bt_allow_entries[assign_index].param);
      return UNI_ERROR_SUCCESS;
    }

    DPRINTF("Ignoring device %s: no available allow entry slot\n",
            bd_addr_to_str(addr));
    return UNI_ERROR_IGNORE_DEVICE;
  }

  DPRINTF("Ignoring device %s: unsupported COD 0x%06x\n", bd_addr_to_str(addr),
          (unsigned)cod);
  return UNI_ERROR_IGNORE_DEVICE;
}

static void btloop_on_device_connected(uni_hid_device_t *d) {
  DPRINTF("btloop: device connected: %p\n", d);
  uni_bt_list_keys_safe();
}

static void btloop_on_device_disconnected(uni_hid_device_t *d) {
  DPRINTF("btloop: device disconnected: %p\n", d);
  uni_bt_list_keys_safe();
}

static uni_error_t btloop_on_device_ready(uni_hid_device_t *d) {
  DPRINTF("btloop: device ready: %p\n", d);
  // You can reject the connection by returning an error.
  // uni_bt_list_keys_safe();

  // bd_addr_t addr;
  // uni_bt_conn_get_address(&d->conn, addr);

  // bool is_rpa = (addr[5] & 0xC0) == 0xC0;
  // DPRINTF("addr=%s. IS RANDOM? %s\n", bd_addr_to_str(addr),
  //         is_rpa ? "yes" : "no");

  // bd_addr_t entry_address;

  // for (int i = 0; i < le_device_db_max_count(); i++) {
  //   int entry_address_type = BD_ADDR_TYPE_UNKNOWN;
  //   sm_key_t irk;  // 16-byte IRK buffer

  //   le_device_db_info(i, &entry_address_type, entry_address, irk);

  //   // skip unused entries
  //   if (entry_address_type == BD_ADDR_TYPE_UNKNOWN) continue;

  //   DPRINTF("%s - type %u", bd_addr_to_str(entry_address),
  //   entry_address_type);

  //   DPRINTFRAW(" - IRK: ");
  //   for (int j = 0; j < 16; j++) DPRINTFRAW("%02x ", irk[j]);
  //   DPRINTFRAW("\n");
  // }

  return UNI_ERROR_SUCCESS;
}

//
// Helpers
//
static void trigger_event_on_gamepad(uni_hid_device_t *d) {
  if (d->report_parser.play_dual_rumble != NULL) {
    d->report_parser.play_dual_rumble(
        d, 0 /* delayed start ms */, 50 /* duration ms */,
        128 /* weak magnitude */, 40 /* strong magnitude */);
  }

  if (d->report_parser.set_player_leds != NULL) {
    static uint8_t led = 0;
    led += 1;
    led &= 0xf;
    d->report_parser.set_player_leds(d, led);
  }

  if (d->report_parser.set_lightbar_color != NULL) {
    static uint8_t red = 0x10;
    static uint8_t green = 0x20;
    static uint8_t blue = 0x40;

    red += 0x10;
    green -= 0x20;
    blue += 0x40;
    d->report_parser.set_lightbar_color(d, red, green, blue);
  }
}

static void btloop_on_oob_event(uni_platform_oob_event_t event, void *data) {
  switch (event) {
    case UNI_PLATFORM_OOB_GAMEPAD_SYSTEM_BUTTON:
      // Optional: do something when "system" button gets pressed.
      trigger_event_on_gamepad((uni_hid_device_t *)data);
      break;

    case UNI_PLATFORM_OOB_BLUETOOTH_ENABLED:
      // When the "bt scanning" is on / off. Could be triggered by different
      // events Useful to notify the user
      DPRINTF("btloop_on_oob_event: Bluetooth enabled: %d\n", (bool)(data));
      break;

    default:
      DPRINTF("btloop_on_oob_event: unsupported event: 0x%04x\n", event);
  }
}

static const uni_property_t *btloop_get_property(uni_property_idx_t idx) {
  ARG_UNUSED(idx);
  DPRINTF("btloop_get_property: idx=%d\n", idx);
  return NULL;
}

enum {
  AXIS_IDLE_MIN = -65,
  AXIS_IDLE_MAX = 64,
};

static int32_t normalize_axis(int32_t value) {
  if (value >= AXIS_IDLE_MIN && value <= AXIS_IDLE_MAX) {
    return 0;
  }
  return value;
}

static const char *axis_dir(int32_t value, const char *neg, const char *pos) {
  if (value < 0) {
    return neg;
  }
  if (value > 0) {
    return pos;
  }
  return "center";
}

static void btloop_on_controller_data(uni_hid_device_t *d,
                                      uni_controller_t *ctl) {
  static uint8_t enabled = true;
  static uni_controller_t prev = {0};
  bool no_dis = false;
  uni_gamepad_t *gp;
  uni_controller_t old = prev;
  bd_addr_t addr = {0};
  uni_bt_conn_get_address(&d->conn, addr);

  // Used to prevent spamming the log, but should be removed in production.
  //    if (memcmp(&prev, ctl, sizeof(*ctl)) == 0) {
  //        return;
  //    }
  // Print device Id before dumping gamepad.
  // DPRINTF("(%p) id=%d addr=%s klass=%d ", d,
  //         uni_hid_device_get_idx_for_instance(d), bd_addr_to_str(addr),
  //         ctl->klass);
  // uni_controller_dump(ctl);
  // DPRINTFRAW("\n");

  uni_controller_class_t klass = ctl->klass;
  if (d->cod & UNI_BT_COD_MINOR_MASK & UNI_BT_COD_MINOR_MICE) {
    if (klass != UNI_CONTROLLER_CLASS_MOUSE) {
      DPRINTF("Overriding controller class to MOUSE based on COD\n");
      klass = UNI_CONTROLLER_CLASS_MOUSE;
      no_dis = true;
    }
  }

  switch (klass) {
    case UNI_CONTROLLER_CLASS_GAMEPAD:
      gp = &ctl->gamepad;

      const uni_gamepad_t *old_gp = &old.gamepad;

      uint8_t axis_state = 0;
      uint8_t fire_state = 0;
      const int32_t axes[] = {
          normalize_axis(gp->axis_x), normalize_axis(gp->axis_y),
          normalize_axis(gp->axis_rx), normalize_axis(gp->axis_ry)};

      // Map d-pad to joystick directions.
      if (gp->dpad & DPAD_UP) axis_state |= 0x01;
      if (gp->dpad & DPAD_DOWN) axis_state |= 0x02;
      if (gp->dpad & DPAD_LEFT) axis_state |= 0x04;
      if (gp->dpad & DPAD_RIGHT) axis_state |= 0x08;

      // Map left stick.
      if (axes[1] < 0) axis_state |= 0x01;
      if (axes[1] > 0) axis_state |= 0x02;
      if (axes[0] < 0) axis_state |= 0x04;
      if (axes[0] > 0) axis_state |= 0x08;

      // Map right stick.
      if (axes[3] < 0) axis_state |= 0x01;
      if (axes[3] > 0) axis_state |= 0x02;
      if (axes[2] < 0) axis_state |= 0x04;
      if (axes[2] > 0) axis_state |= 0x08;

      // Any button (including shoulders/triggers) → fire.
      uint16_t shoulder_buttons = BUTTON_SHOULDER_L | BUTTON_SHOULDER_R |
                                  BUTTON_TRIGGER_L | BUTTON_TRIGGER_R;
      if (gp->buttons || gp->misc_buttons || (gp->buttons & shoulder_buttons)) {
        fire_state |= 0x02;  // Joystick 0 fire
      }

      joystick_set_state(fire_state, axis_state);

      // Report button changes.
      typedef struct {
        uint16_t mask;
        const char *name;
      } button_name_t;

      const button_name_t button_names[] = {
          {BUTTON_A, "A"},           {BUTTON_B, "B"},
          {BUTTON_X, "X"},           {BUTTON_Y, "Y"},
          {BUTTON_SHOULDER_L, "L1"}, {BUTTON_SHOULDER_R, "R1"},
          {BUTTON_TRIGGER_L, "L2"},  {BUTTON_TRIGGER_R, "R2"},
          {BUTTON_THUMB_L, "L3"},    {BUTTON_THUMB_R, "R3"},
      };

      const button_name_t misc_button_names[] = {
          {MISC_BUTTON_SYSTEM, "SYSTEM"},
          {MISC_BUTTON_SELECT, "SELECT"},
          {MISC_BUTTON_START, "START"},
          {MISC_BUTTON_CAPTURE, "CAPTURE"},
      };

      const button_name_t dpad_names[] = {
          {DPAD_UP, "DPAD_UP"},
          {DPAD_DOWN, "DPAD_DOWN"},
          {DPAD_LEFT, "DPAD_LEFT"},
          {DPAD_RIGHT, "DPAD_RIGHT"},
      };

      uint16_t changed_buttons = gp->buttons ^ old_gp->buttons;
      for (size_t i = 0; i < sizeof(button_names) / sizeof(button_names[0]);
           ++i) {
        if (changed_buttons & button_names[i].mask) {
          bool pressed = (gp->buttons & button_names[i].mask) != 0;
          DPRINTF("Button %s %s\n", button_names[i].name,
                  pressed ? "pressed" : "released");
        }
      }

      uint8_t changed_misc = gp->misc_buttons ^ old_gp->misc_buttons;
      for (size_t i = 0;
           i < sizeof(misc_button_names) / sizeof(misc_button_names[0]); ++i) {
        if (changed_misc & misc_button_names[i].mask) {
          bool pressed = (gp->misc_buttons & misc_button_names[i].mask) != 0;
          DPRINTF("Button %s %s\n", misc_button_names[i].name,
                  pressed ? "pressed" : "released");
        }
      }

      uint8_t changed_dpad = gp->dpad ^ old_gp->dpad;
      for (size_t i = 0; i < sizeof(dpad_names) / sizeof(dpad_names[0]); ++i) {
        if (changed_dpad & dpad_names[i].mask) {
          bool pressed = (gp->dpad & dpad_names[i].mask) != 0;
          DPRINTF("Button %s %s\n", dpad_names[i].name,
                  pressed ? "pressed" : "released");
        }
      }

      // Report axis directions when they move.
      int32_t axis_x = normalize_axis(gp->axis_x);
      int32_t axis_y = normalize_axis(gp->axis_y);
      int32_t axis_rx = normalize_axis(gp->axis_rx);
      int32_t axis_ry = normalize_axis(gp->axis_ry);
      int32_t old_axis_x = normalize_axis(old_gp->axis_x);
      int32_t old_axis_y = normalize_axis(old_gp->axis_y);
      int32_t old_axis_rx = normalize_axis(old_gp->axis_rx);
      int32_t old_axis_ry = normalize_axis(old_gp->axis_ry);

      if (axis_x != old_axis_x) {
        DPRINTF("Axis LX: %s (%ld)\n", axis_dir(axis_x, "left", "right"),
                (long)axis_x);
      }
      if (axis_y != old_axis_y) {
        DPRINTF("Axis LY: %s (%ld)\n", axis_dir(axis_y, "up", "down"),
                (long)axis_y);
      }
      if (axis_rx != old_axis_rx) {
        DPRINTF("Axis RX: %s (%ld)\n", axis_dir(axis_rx, "left", "right"),
                (long)axis_rx);
      }
      if (axis_ry != old_axis_ry) {
        DPRINTF("Axis RY: %s (%ld)\n", axis_dir(axis_ry, "up", "down"),
                (long)axis_ry);
      }
      break;
    case UNI_CONTROLLER_CLASS_MOUSE: {
      if (no_dis) {
        const uni_mouse_t *ms = &ctl->mouse;
        DPRINTF(
            "Mouse raw: dx=%ld, dy=%ld, buttons=0x%04x, misc=0x%02x, "
            "wheel=0x%02x\n",
            (long)ms->delta_x, (long)ms->delta_y, ms->buttons, ms->misc_buttons,
            (uint8_t)ms->scroll_wheel);
      } else {
        const uni_mouse_t *ms = &ctl->mouse;
        bool left = (ms->buttons & UNI_MOUSE_BUTTON_LEFT) != 0;
        bool right = (ms->buttons & UNI_MOUSE_BUTTON_RIGHT) != 0;
        int16_t dx = (int16_t)ms->delta_x;
        int16_t dy = (int16_t)ms->delta_y;
        DPRINTF("Mouse move: dx=%d, dy=%d, left=%d, right=%d\n", (int)dx,
                (int)dy, (int)left, (int)right);
        hidinput_update_mouse(dx, dy, left, right);
      }
      break;
    }
    case UNI_CONTROLLER_CLASS_KEYBOARD: {
      uni_keyboard_dump(&ctl->keyboard);
      DPRINTFRAW("\n");
      const uni_keyboard_t *kb = &ctl->keyboard;
      uint8_t report[10] = {0};
      report[2] = kb->modifiers;  // raw modifiers byte
      // report[3] is reserved (0)
      for (size_t i = 0; i < 6 && i < UNI_KEYBOARD_PRESSED_KEYS_MAX; ++i) {
        report[4 + i] = kb->pressed_keys[i];
      }
      DPRINTF("Show keyboard report: ");
      printf_hexdump(report, sizeof(report));
      handle_keyboard_report(report, sizeof(report));
      break;
    }
    default:
      DPRINTF("Unsupported controller class: %d\n", klass);
      break;
  }

  prev = *ctl;
}

struct uni_platform *get_my_bluepad32_code(void) {
  static struct uni_platform plat = {
      .name = "Croissant",
      .init = btloop_init,
      .on_init_complete = btloop_on_init_complete,
      .on_device_discovered = btloop_on_device_discovered,
      .on_device_connected = btloop_on_device_connected,
      .on_device_disconnected = btloop_on_device_disconnected,
      .on_device_ready = btloop_on_device_ready,
      .on_oob_event = btloop_on_oob_event,
      .on_controller_data = btloop_on_controller_data,
      .get_property = btloop_get_property,
  };

  return &plat;
}

int main_bt_bluepad32(int prev_reset_state, int prev_config_state,
                      void (*handle_rx)(void),
                      void (*reset_sequence_cb)(void)) {
  (void)prev_reset_state;

  // Load any persisted BT addresses so the allowlist can be populated later.
  load_bt_allowlist_entries();

  // Mouse initialization
  mouse_init();
  joystick_init();
  // Drive joystick emulation from Bluetooth gamepads on port 1.
  joystick_init_usb(true, 1);

  int mouse_speed = 5;
  SettingsConfigEntry *entry =
      settings_find_entry(gconfig_getContext(), PARAM_MOUSE_SPEED);
  if (entry != NULL) {
    DPRINTF("Mouse speed setting: %s\n", entry->value);
    mouse_speed = atoi(entry->value);
  } else {
    DPRINTF("Setting not found. Defaulting to %d\n", mouse_speed);
  }

  // Mouse sensitivity initialization
  mouse_set_sensitivity(mouse_speed);

  // initialize CYW43 driver architecture (will enable BT if/because
  // CYW43_ENABLE_BLUETOOTH == 1)
  if (cyw43_arch_init()) {
    DPRINTF("failed to initialise cyw43_arch\n");
    return -1;
  }
  // Must be called before uni_init()
  uni_platform_set_custom(get_my_bluepad32_code());

  // Initialize BP32
  uni_init(0, NULL);

  // Ensure BTstack is marked as running for this loop.
  btstack_paused = false;

  if (prev_config_state) {
    launch_config_cb();
  }

  // Drive Bluepad32 / BTstack similarly to other BT loops.
  while (true) {
    btloop_tick();

    if (handle_rx) {
      handle_rx();
    }

    if (reset_sequence_cb) {
      reset_sequence_cb();
    }

    int config_state = gpio_get(KBD_CONFIG_IN_3V3_GPIO);
    if (config_state != prev_config_state) {
      DPRINTF("GPIO KBD_CONFIG_IN_3V3_GPIO changed: %d -> %d\n",
              prev_config_state, config_state);
      prev_config_state = config_state;
      if (config_state) {
        launch_config_cb();
      }
    }

    if (!btstack_paused) {
      async_context_poll(cyw43_arch_async_context());
    }
    tight_loop_contents();
  }

  return 0;
}
