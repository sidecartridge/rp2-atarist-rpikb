#include "btloop.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"
#include "gconfig.h"
#include "pico/async_context.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "stkeys.h"
#include "uni.h"
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

  DPRINTF("raw report (len=%u):", (unsigned)len);
  for (uint16_t i = 0; i < len; ++i) {
    DPRINTFRAW(" %02x", report[i]);
  }
  DPRINTFRAW("\n");

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
  DPRINTF("Normalized modifiers: 0x%02x\n", fixed_modifiers);

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
      DPRINTF("Shortcut: ALT+BACKSPACE -> ALT+INSERT\n");
    }
  }

  const bool shift_active =
      (fixed_modifiers & (MOD_LEFT_SHIFT | MOD_RIGHT_SHIFT)) != 0;

  DPRINTF("Shift active: %s\n", shift_active ? "yes" : "no");

  stkeys_apply_keyboard_report(last_keyboard_keys, normalized_keys, 6,
                               fixed_modifiers, stkeys_lookup_hid_gb);

  uint8_t changed_modifiers = fixed_modifiers ^ last_modifiers;
  if (changed_modifiers != 0) {
    for (size_t i = 0;
         i < (sizeof(k_modifier_entries) / sizeof(k_modifier_entries[0]));
         ++i) {
      const modifier_entry_t *entry = &k_modifier_entries[i];
      if ((changed_modifiers & entry->mask) != 0 &&
          (fixed_modifiers & entry->mask) != 0) {
        DPRINTF("Key pressed: 0x%02x (%s)\n", entry->usage, entry->name);
      }
    }
  }

  for (size_t i = 0; i < key_slots; ++i) {
    uint8_t keycode = normalized_keys[i];
    if (keycode == 0) {
      continue;
    }
    if (key_array_contains(last_keyboard_keys, keycode)) {
      continue;
    }

    DPRINTF("New key detected: 0x%02x + shift: %s\n", keycode,
            shift_active ? "yes" : "no");
    char printable = hid_usage_to_printable(keycode, shift_active);
    if (printable) {
      log_printable_key(printable);
    } else {
      const char *name = hid_usage_to_name(keycode);
      if (name) {
        DPRINTF("Key pressed: 0x%02x (%s)\n", keycode, name);
      } else {
        DPRINTF("Key pressed: usage=0x%02x (shift=%u)\n", keycode,
                shift_active ? 1u : 0u);
      }
    }
  }

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

  // Start scanning and autoconnect to supported controllers.
  uni_bt_start_scanning_and_autoconnect_unsafe();
  // uni_bt_allow_incoming_connections(true);

  // uni_bt_service_set_enabled(true);

  // uni_bt_allowlist_set_enabled(true);

  // uni_bt_allowlist_add_addr((bd_addr_t){0x00, 0x11, 0x67, 0xF7, 0x0F,
  //                                       0x03});  // Logitech Mouse example
  // uni_bt_allowlist_add_addr(
  //     (bd_addr_t){0x60, 0xC5, 0x47, 0x1E, 0x01, 0x74});  // Magic Keyboard

  // uni_bt_allowlist_add_addr(
  //     (bd_addr_t){0x76, 0x08, 0x31, 0x01, 0x49, 0x9B});  // ESYNYC

  // uni_bt_allowlist_add_addr(
  //     (bd_addr_t){0xEB, 0x60, 0x31, 0xFB, 0x69, 0xE2});  // Microsoft mouse

  uni_bt_allowlist_list();

  uni_bt_list_keys_unsafe();

  uni_property_dump_all();
}

static uni_error_t btloop_on_device_discovered(bd_addr_t addr, const char *name,
                                               uint16_t cod, uint8_t rssi) {
  // You can filter discovered devices here. Return any value different from
  // UNI_ERROR_SUCCESS;
  // @param addr: the Bluetooth address
  // @param name: could be NULL, could be zero-length, or might contain the
  // name.
  // @param cod: Class of Device. See "uni_bt_defines.h" for possible values.
  // @param rssi: Received Signal Strength Indicator (RSSI) measured in dBms.
  // The higher (255) the better.

  DPRINTF("btloop_on_device_discovered: %s, name='%s', cod=0x%06x, rssi=%d\n",
          bd_addr_to_str(addr), name ? name : "<null>", (unsigned)cod,
          (int8_t)rssi);
  return UNI_ERROR_SUCCESS;
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
  uni_bt_list_keys_safe();
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

static void btloop_on_controller_data(uni_hid_device_t *d,
                                      uni_controller_t *ctl) {
  static uint8_t leds = 0;
  static uint8_t enabled = true;
  static uni_controller_t prev = {0};
  uni_gamepad_t *gp;

  // Used to prevent spamming the log, but should be removed in production.
  //    if (memcmp(&prev, ctl, sizeof(*ctl)) == 0) {
  //        return;
  //    }
  prev = *ctl;
  // Print device Id before dumping gamepad.
  DPRINTF("(%p) id=%d ", d, uni_hid_device_get_idx_for_instance(d));
  uni_controller_dump(ctl);
  DPRINTFRAW("\n");

  switch (ctl->klass) {
    case UNI_CONTROLLER_CLASS_GAMEPAD:
      gp = &ctl->gamepad;

      // Debugging
      // Axis ry: control rumble
      if ((gp->buttons & BUTTON_A) &&
          d->report_parser.play_dual_rumble != NULL) {
        d->report_parser.play_dual_rumble(
            d, 0 /* delayed start ms */, 250 /* duration ms */,
            128 /* weak magnitude */, 0 /* strong magnitude */);
      }

      if ((gp->buttons & BUTTON_B) &&
          d->report_parser.play_dual_rumble != NULL) {
        d->report_parser.play_dual_rumble(
            d, 0 /* delayed start ms */, 250 /* duration ms */,
            0 /* weak magnitude */, 128 /* strong magnitude */);
      }
      // Buttons: Control LEDs On/Off
      if ((gp->buttons & BUTTON_X) &&
          d->report_parser.set_player_leds != NULL) {
        d->report_parser.set_player_leds(d, leds++ & 0x0f);
      }
      // Axis: control RGB color
      if ((gp->buttons & BUTTON_Y) &&
          d->report_parser.set_lightbar_color != NULL) {
        uint8_t r = (gp->axis_x * 256) / 512;
        uint8_t g = (gp->axis_y * 256) / 512;
        uint8_t b = (gp->axis_rx * 256) / 512;
        d->report_parser.set_lightbar_color(d, r, g, b);
      }

      // Toggle Bluetooth connections
      if ((gp->buttons & BUTTON_SHOULDER_L) && enabled) {
        DPRINTF("*** Disabling Bluetooth connections\n");
        uni_bt_stop_scanning_safe();
        enabled = false;
      }
      if ((gp->buttons & BUTTON_SHOULDER_R) && !enabled) {
        DPRINTF("*** Enabling Bluetooth connections\n");
        uni_bt_start_scanning_and_autoconnect_safe();
        enabled = true;
      }
      break;
    case UNI_CONTROLLER_CLASS_BALANCE_BOARD:
      // Do something
      uni_balance_board_dump(&ctl->balance_board);
      break;
    case UNI_CONTROLLER_CLASS_MOUSE: {
      const uni_mouse_t *ms = &ctl->mouse;
      bool left = (ms->buttons & UNI_MOUSE_BUTTON_LEFT) != 0;
      bool right = (ms->buttons & UNI_MOUSE_BUTTON_RIGHT) != 0;
      int16_t dx = (int16_t)ms->delta_x;
      int16_t dy = (int16_t)ms->delta_y;
      hidinput_update_mouse(dx, dy, left, right);
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
      DPRINTF("Unsupported controller class: %d\n", ctl->klass);
      break;
  }
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

int main_bt_bluepad32(int prev_reset_state, int prev_toggle_state,
                      void (*handle_rx)(void),
                      void (*reset_sequence_cb)(void)) {
  (void)prev_reset_state;
  (void)prev_toggle_state;

  // Mouse initialization
  mouse_init();

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

  // Drive Bluepad32 / BTstack similarly to other BT loops.
  while (true) {
    btloop_tick();

    if (handle_rx) {
      handle_rx();
    }

    if (reset_sequence_cb) {
      reset_sequence_cb();
    }

    if (!btstack_paused) {
      async_context_poll(cyw43_arch_async_context());
    }
    tight_loop_contents();
  }

  return 0;
}
