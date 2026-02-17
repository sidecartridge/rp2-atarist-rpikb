#include "usbloop.h"

#include "pico/time.h"

// Provided by main.c
void launch_config_cb(void);

static volatile bool original_mouse_timer_enabled = false;
static volatile bool joystick_poll_timer_enabled = false;
static volatile bool joystick_poll_usb = false;
static volatile uint8_t joystick_poll_usb_port = 1;
static volatile bool joystick_poll_mouse_original = false;

static bool __not_in_flash_func(original_mouse_timer_cb)(repeating_timer_t* rt) {
  (void)rt;
  if (!original_mouse_timer_enabled) return true;

  joystick_process_mouse_edges();
  mouse_update_native();
  return true;
}

static bool __not_in_flash_func(joystick_poll_timer_cb)(repeating_timer_t* rt) {
  (void)rt;
  if (!joystick_poll_timer_enabled) return true;

  // Handle here the Joystick inputs to avoid overwhelming delays.
  if (joystick_poll_usb) {
    if (joystick_poll_usb_port == 1) {
      joystick_update(3);  // Joystick 1
    } else {
      joystick_update(4);  // Joystick 0
    }
  } else {
    joystick_update(0);  // Joystick 0
    joystick_update(1);  // Joystick 1
  }

  if (!joystick_poll_mouse_original) mouse_update_hid();
  return true;
}

int main_usb_loop(int prev_reset_state, int prev_config_state,
                  void (*handle_rx)(void), void (*reset_sequence_cb)(void)) {
#if !(defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_CROISSANT_REV2)
  (void)prev_reset_state;
#endif

  // Initialize the board (USB, HID, etc)
  DPRINTF("Initializing board...\n");
  hidinput_if_ring_init();
  board_init();
  DPRINTF("Initialising USB...\n");

  tusb_rhport_init_t host_init = {.role = TUSB_ROLE_HOST,
                                  .speed = TUSB_SPEED_AUTO};
  tusb_init(0, &host_init);
  if (board_init_after_tusb) {
    board_init_after_tusb();
  }
  DPRINTF("USB initialization complete.\n");

  // Mouse initialization
  mouse_init();

  int mouse_speed = 5;
  SettingsConfigEntry* entry =
      settings_find_entry(gconfig_getContext(), PARAM_MOUSE_SPEED);
  if (entry != NULL) {
    DPRINTF("Mouse speed setting: %s\n", entry->value);
    mouse_speed = atoi(entry->value);
  } else {
    DPRINTF("Setting not found. Defaulting to %d\n", mouse_speed);
  }

  // Mouse sensitivity initialization
  mouse_set_sensitivity_hid(mouse_speed);
  mouse_set_sensitivity_native(mouse_speed);
  joystick_init();

  // Check if we must emulate original Atari ST mouse on joystick port
  bool mouse_original = false;
  entry = settings_find_entry(gconfig_getContext(), PARAM_MOUSE_ORIGINAL);

  if (entry != NULL) {
    DPRINTF("Mouse original setting: %s\n", entry->value);
    mouse_original = entry->value[0] == 't' || entry->value[0] == 'T' ||
                     entry->value[0] == '1' || entry->value[0] == 'y' ||
                     entry->value[0] == 'Y';
  } else {
    DPRINTF("Setting not found. Defaulting to %s\n",
            mouse_original ? "true" : "false");
  };
  // mouse_original = true;

  mouse_set_active_path(mouse_original ? MOUSE_PATH_NATIVE : MOUSE_PATH_HID);

  // Emulate Joystick over USB or use original Joysticks
  int joystick_usb_port = 1;
  entry = settings_find_entry(gconfig_getContext(), PARAM_JOYSTICK_USB_PORT);
  if (entry != NULL) {
    DPRINTF("Joystick USB port setting: %s\n", entry->value);
    joystick_usb_port = atoi(entry->value);
  } else {
    DPRINTF("Setting not found. Defaulting to %d\n", joystick_usb_port);
  }
  if (joystick_usb_port > 1) {
    DPRINTF("Invalid joystick USB port %d, defaulting to 1\n",
            joystick_usb_port);
    joystick_usb_port = 1;
  }
  bool joystick_usb = false;
  entry = settings_find_entry(gconfig_getContext(), PARAM_JOYSTICK_USB);
  if (entry != NULL) {
    DPRINTF("Joystick USB setting: %s\n", entry->value);
    joystick_usb = entry->value[0] == 't' || entry->value[0] == 'T' ||
                   entry->value[0] == '1' || entry->value[0] == 'y' ||
                   entry->value[0] == 'Y';
  } else {
    DPRINTF("Setting not found. Defaulting to %s\n",
            joystick_usb ? "true" : "false");
  };

  if (joystick_usb && joystick_usb_port == 0 && mouse_original) {
    DPRINTF(
        "Cannot use USB joystick on port 0 with original mouse emulation. "
        "Disabling USB joystick.\n");
    joystick_usb = false;
  }
  DPRINTF("Joystick type: %s\n", joystick_usb ? "USB" : "Original");
  if (joystick_usb) {
    DPRINTF("Using USB Joystick on port %d\n", joystick_usb_port);
  }
  joystick_init_usb(joystick_usb, joystick_usb_port);

  int joystick_autoshoot = 0;
  entry =
      settings_find_entry(gconfig_getContext(), PARAM_JOYSTICK_USB_AUTOSHOOT);
  if (entry != NULL) {
    DPRINTF("Joystick autoshoot setting: %s\n", entry->value);
    joystick_autoshoot = atoi(entry->value);
  } else {
    DPRINTF("Joystick autoshoot setting not found. Defaulting to %d\n",
            joystick_autoshoot);
  }
  // In USB mode this applies to both USB-fed and GPIO-native joystick paths.
  joystick_set_autoshoot(joystick_usb, joystick_autoshoot);

  // If configuration pin is already asserted, jump to configuration
  // immediately.
  if (prev_config_state) {
    launch_config_cb();
  }

  // Main loop
  DPRINTF("Entering main loop...\n");
  absolute_time_t serial_ten_ms = get_absolute_time();

  repeating_timer_t original_mouse_timer;
  bool original_mouse_timer_started = false;
  original_mouse_timer_enabled = mouse_original;
  if (mouse_original) {
    if (!add_repeating_timer_us(
            -((int64_t)ORIGINAL_MOUSE_LINE_POLL_INTERVAL_US),
            original_mouse_timer_cb, NULL, &original_mouse_timer)) {
      DPRINTF("Failed to start original mouse timer\n");
    } else {
      original_mouse_timer_started = true;
      DPRINTF("Original mouse timer started: %d us\n",
              ORIGINAL_MOUSE_LINE_POLL_INTERVAL_US);
    }
  }

  repeating_timer_t joystick_poll_timer;
  bool joystick_poll_timer_started = false;
  joystick_poll_timer_enabled = true;
  joystick_poll_usb = joystick_usb;
  joystick_poll_usb_port = (uint8_t)joystick_usb_port;
  joystick_poll_mouse_original = mouse_original;
  if (!add_repeating_timer_us(-((int64_t)MOUSE_LINE_POLL_INTERVAL_US),
                              joystick_poll_timer_cb, NULL,
                              &joystick_poll_timer)) {
    DPRINTF("Failed to start joystick poll timer\n");
  } else {
    joystick_poll_timer_started = true;
    DPRINTF("Joystick poll timer started: %d us\n",
            MOUSE_LINE_POLL_INTERVAL_US);
  }

  while (true) {
    absolute_time_t tm = get_absolute_time();

    // Service TinyUSB every loop so short HID button transitions are not
    // missed.
    tuh_task();

    if (mouse_original) joystick_read_edges();

    if (handle_rx) handle_rx();

    if (absolute_time_diff_us(serial_ten_ms, tm) >= SERIAL_POLL_INTERVAL_US) {
      serial_ten_ms = tm;

      // Poll inputs and report changes
#if defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_CROISSANT_REV2
      int reset_state = gpio_get(KBD_RESET_IN_3V3_GPIO);
      if (reset_state != prev_reset_state) {
        DPRINTF("GPIO KBD_RESET_IN_3V3_GPIO changed: %d -> %d\n",
                prev_reset_state, reset_state);
        prev_reset_state = reset_state;
      }
#endif

      int config_state = gpio_get(KBD_CONFIG_IN_3V3_GPIO);
      if (config_state != prev_config_state) {
        DPRINTF("GPIO KBD_CONFIG_IN_3V3_GPIO changed: %d -> %d\n",
                prev_config_state, config_state);
        prev_config_state = config_state;
        if (config_state) {
          original_mouse_timer_enabled = false;
          joystick_poll_timer_enabled = false;
          if (original_mouse_timer_started) {
            cancel_repeating_timer(&original_mouse_timer);
            original_mouse_timer_started = false;
          }
          if (joystick_poll_timer_started) {
            cancel_repeating_timer(&joystick_poll_timer);
            joystick_poll_timer_started = false;
          }
          launch_config_cb();
        }
      }
    }

#if defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_CROISSANT_REV2
    if (reset_sequence_cb) {
      reset_sequence_cb();
    }
#endif
  }
  return -1;
}
