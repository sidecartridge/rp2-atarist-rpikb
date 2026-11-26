#include "factory.h"

#include <stdio.h>

#include "constants.h"
#include "debug.h"
#include "gconfig.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "pico/flash.h"
#include "pico/time.h"
#include "settings.h"

volatile factory_live_t factory_live = {0};

#if defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_SOUFFLE_REV2

void launch_config_cb(void);

// The twelve joystick and mouse lines, GPIO 10-21 (JOY1 UP..FIRE2, then
// JOY0 UP..FIRE2). Bit n of the masks is GPIO (FACTORY_FIRST_GPIO + n).
#define FACTORY_FIRST_GPIO JOY1_UP
#define FACTORY_LINES 12
#define FACTORY_ALL_LOW ((1u << FACTORY_LINES) - 1u)
#define FACTORY_HOLD_MS 10000u

// The letters the operator types: HID usages 0x04 (a) to 0x08 (e), physical
// key positions on a US or UK keyboard.
#define FACTORY_HID_KEY_A 0x04u
#define FACTORY_LETTERS 5
#define FACTORY_ALL_LETTERS ((1u << FACTORY_LETTERS) - 1u)

// Mode LEDs: the USB LED blinks slowly while waiting and fast while all
// twelve lines are held; the BT LED lights once all five letters were typed.
// Plain GPIO, not the dimmed PWM of normal operation, so the jump into
// Booster switches both off as usual.
#define FACTORY_BLINK_WAITING_MS 500u
#define FACTORY_BLINK_HOLDING_MS 100u

// flash_safe_execute() parks core 1 (the HD6301, running from flash) before
// the write. Give it this long to answer.
#define FACTORY_FLASH_TIMEOUT_MS 1000u

static uint64_t hold_start_us = 0;

bool factory_init(void) {
  SettingsConfigEntry *entry =
      settings_find_entry(gconfig_getContext(), PARAM_FACTORY_MODE);
  if (entry == NULL || entry->value[0] != '1' || entry->value[1] != '\0') {
    return false;
  }

  // The joystick code leaves JOY1 FIRE2 (GPIO 15) unconfigured, and an
  // unconfigured pad always reads low. Set up all twelve the same way.
  for (uint gpio = FACTORY_FIRST_GPIO;
       gpio < FACTORY_FIRST_GPIO + FACTORY_LINES; ++gpio) {
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio);
  }

  gpio_init(KBD_USB_OUT_3V3_GPIO);
  gpio_set_dir(KBD_USB_OUT_3V3_GPIO, GPIO_OUT);
  gpio_put(KBD_USB_OUT_3V3_GPIO, 0);
  gpio_init(KBD_ATARI_OUT_3V3_GPIO);
  gpio_set_dir(KBD_ATARI_OUT_3V3_GPIO, GPIO_OUT);
  gpio_put(KBD_ATARI_OUT_3V3_GPIO, 0);

  factory_live.state = FACTORY_STATE_WAITING;
  DPRINTF("Factory test started\n");
  return true;
}

bool factory_active(void) {
  return factory_live.state == FACTORY_STATE_WAITING ||
         factory_live.state == FACTORY_STATE_HOLDING;
}

void factory_poll(void) {
  if (!factory_active()) {
    return;
  }
  uint64_t now_us = time_us_64();
  uint32_t low =
      (~gpio_get_all() >> FACTORY_FIRST_GPIO) & FACTORY_ALL_LOW;
  factory_live.low_mask = low;
  factory_live.seen_mask |= low;

  if (low == FACTORY_ALL_LOW) {
    if (hold_start_us == 0) {
      hold_start_us = now_us;
      factory_live.state = FACTORY_STATE_HOLDING;
    }
    uint32_t held_ms = (uint32_t)((now_us - hold_start_us) / 1000u);
    factory_live.held_ms = held_ms;
    if (held_ms > factory_live.max_held_ms) {
      factory_live.max_held_ms = held_ms;
    }
  } else if (hold_start_us != 0) {
    hold_start_us = 0;
    factory_live.held_ms = 0;
    factory_live.state = FACTORY_STATE_WAITING;
  }

  uint32_t half_period_ms = factory_live.state == FACTORY_STATE_HOLDING
                                ? FACTORY_BLINK_HOLDING_MS
                                : FACTORY_BLINK_WAITING_MS;
  gpio_put(KBD_USB_OUT_3V3_GPIO,
           ((now_us / 1000u) / half_period_ms) % 2u == 0u);
  gpio_put(KBD_ATARI_OUT_3V3_GPIO,
           factory_live.keys_mask == FACTORY_ALL_LETTERS);

  if (factory_live.held_ms >= FACTORY_HOLD_MS &&
      factory_live.keys_mask == FACTORY_ALL_LETTERS) {
    factory_finish(false);
  }
}

void factory_note_keystroke(uint8_t hid_code) {
  if (factory_active()) {
    factory_live.keystrokes++;
    if (hid_code >= FACTORY_HID_KEY_A &&
        hid_code < FACTORY_HID_KEY_A + FACTORY_LETTERS) {
      factory_live.keys_mask |= 1u << (hid_code - FACTORY_HID_KEY_A);
    }
  }
}

void factory_note_keyboard(uint16_t vid, uint16_t pid) {
  if (factory_active()) {
    factory_live.vid = vid;
    factory_live.pid = pid;
  }
}

// Runs with core 1 parked and interrupts off: no printing in here.
static void factory_write_settings(void *param) {
  SettingsContext *ctx = (SettingsContext *)param;
  flash_range_erase(ctx->flashSettingsOffset, ctx->flashSettingsSize);
  flash_range_program(ctx->flashSettingsOffset,
                      (const uint8_t *)ctx->configData.entries,
                      ctx->flashSettingsSize);
}

void factory_finish(bool config_pressed) {
  if (factory_live.state == FACTORY_STATE_OFF) {
    launch_config_cb();
    return;
  }
  factory_live.state = FACTORY_STATE_SAVING;

  bool pass = !config_pressed && factory_live.max_held_ms >= FACTORY_HOLD_MS &&
              factory_live.keys_mask == FACTORY_ALL_LETTERS;
  char result[SETTINGS_MAX_VALUE_LENGTH];
  snprintf(result, sizeof(result),
           "v=1;pass=%d;kbd=%lu;seen=%03lx;held=%lu;vid=%04x:%04x;keys=%02lx",
           pass ? 1 : 0, (unsigned long)factory_live.keystrokes,
           (unsigned long)factory_live.seen_mask,
           (unsigned long)factory_live.max_held_ms, factory_live.vid,
           factory_live.pid, (unsigned long)factory_live.keys_mask);
  DPRINTF("Factory test %s: %s\n", pass ? "passed" : "failed", result);

  SettingsContext *ctx = gconfig_getContext();
  int err = settings_put_string(ctx, PARAM_FACTORY_RESULT, result);
  if (err == 0 && ctx->configData.count * sizeof(SettingsConfigEntry) <=
                      ctx->flashSettingsSize) {
    err = flash_safe_execute(factory_write_settings, ctx,
                             FACTORY_FLASH_TIMEOUT_MS);
  } else if (err == 0) {
    err = -1;
  }
  if (err != 0) {
    DPRINTF("Factory result not saved (error %d); entering Booster anyway\n",
            err);
  }
  launch_config_cb();
}

#else  // Croissant: no USB mode loop and no factory test.

bool factory_init(void) { return false; }
bool factory_active(void) { return false; }
void factory_poll(void) {}
void factory_note_keystroke(uint8_t hid_code) { (void)hid_code; }
void factory_note_keyboard(uint16_t vid, uint16_t pid) {
  (void)vid;
  (void)pid;
}
void factory_finish(bool config_pressed) { (void)config_pressed; }

#endif
