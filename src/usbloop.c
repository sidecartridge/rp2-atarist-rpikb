#include "usbloop.h"

// Provided by main.c
void launch_config_cb(void);

int main_usb_loop(int prev_reset_state, int prev_config_state,
                  void (*handle_rx)(void),
                  void (*reset_sequence_cb)(void)) {
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
  mouse_set_sensitivity(mouse_speed);
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

  joystick_usb = true;  // --- FORCE USB JOYSTICK ---

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

  // If configuration pin is already asserted, jump to configuration immediately.
  if (prev_config_state) {
    launch_config_cb();
  }

  // Main loop
  DPRINTF("Entering main loop...\n");
  absolute_time_t serial_ten_ms = get_absolute_time();
  absolute_time_t mouse_sampling_ms = get_absolute_time();
  absolute_time_t original_mouse_sampling_ms = get_absolute_time();

  while (true) {
    absolute_time_t tm = get_absolute_time();

    if (handle_rx) {
      handle_rx();
    }

    if (absolute_time_diff_us(original_mouse_sampling_ms, tm) >=
        ORIGINAL_MOUSE_LINE_POLL_INTERVAL_US) {
      original_mouse_sampling_ms = tm;
      // Handle here the Original Mouse inputs to avoid overwhelming delays
      if (mouse_original) {
        // Emulate original Atari ST mouse on joystick 0 port
        joystick_update(2);  // Mouse on GPIOs
        mouse_update();
      }
    }

    if (absolute_time_diff_us(mouse_sampling_ms, tm) >=
        MOUSE_LINE_POLL_INTERVAL_US) {
      mouse_sampling_ms = tm;
      // Handle here the Joystick inputs to avoid overwhelming delays
      if (joystick_usb) {
        if (joystick_usb_port == 1) {
          joystick_update(3);  // Joystick 1
        } else {
          joystick_update(4);  // Joystick 0
        }
      } else {
        joystick_update(0);  // Joystick 0
        joystick_update(1);  // Joystick 1
      }
      if (!mouse_original) mouse_update();
    }

    if (absolute_time_diff_us(serial_ten_ms, tm) >= SERIAL_POLL_INTERVAL_US) {
      serial_ten_ms = tm;

      // Poll inputs and report changes
      int reset_state = gpio_get(KBD_RESET_IN_3V3_GPIO);
      if (reset_state != prev_reset_state) {
        DPRINTF("GPIO KBD_RESET_IN_3V3_GPIO changed: %d -> %d\n",
                prev_reset_state, reset_state);
        prev_reset_state = reset_state;
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
      tuh_task();
      // handle_hid_found();
    }

    if (reset_sequence_cb) {
      reset_sequence_cb();
    }
  }
  return -1;
}
