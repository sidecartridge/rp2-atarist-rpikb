#include "nativeloop.h"

#include "constants.h"
#include "debug.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

extern void launch_config_cb(void);

static inline void select_native_keyboard_source(void) {
  gpio_put(KBD_ATARI_OUT_3V3_GPIO, 1);
  gpio_put(KBD_USB_OUT_3V3_GPIO, 0);
}

void enter_configuration_mode(void) {
  DPRINTF("Entering configuration mode...\n");
  DPRINTF("Launching configuration...\n");
  launch_config_cb();
  DPRINTF("You should not see this message... Halting.\n");
  while (1) {
    tight_loop_contents();
  }
}

void run_native_keyboard_mode(void (*reset_sequence_cb)(void)) {
  DPRINTF("Entering native keyboard mode (PARAM_MODE=0)\n");
  select_native_keyboard_source();
  int prev_config_state = gpio_get(KBD_CONFIG_IN_3V3_GPIO);
  int prev_reset_state = gpio_get(KBD_RESET_IN_3V3_GPIO);
  uint64_t clock_start_us = time_us_64();
  bool reset_checked = false;
  if (prev_config_state) {
    launch_config_cb();
  }
  while (true) {
    int reset_state = gpio_get(KBD_RESET_IN_3V3_GPIO);
    if (!reset_checked && reset_state != prev_reset_state) {
      DPRINTF("GPIO KBD_RESET_IN_3V3_GPIO changed: %d -> %d\n",
              prev_reset_state, reset_state);
      prev_reset_state = reset_state;
      uint64_t now_us = time_us_64();
      uint64_t elapsed_us = now_us - clock_start_us;
      DPRINTF("RESET change at %llu us since boot\n",
              (unsigned long long)elapsed_us);
      if (elapsed_us >= (ENTER_CONFIG_MODE_HOLD_TIME_SEC * SEC_TO_US) &&
          elapsed_us <= (MAX_RESET_HOLD_TIME_SEC * SEC_TO_US)) {
        DPRINTF("RESET change within config window. Entering configuration.\n");
        enter_configuration_mode();
      }
      reset_checked = true;
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
    if (reset_sequence_cb) {
      reset_sequence_cb();
    }
    tight_loop_contents();
  }
}
