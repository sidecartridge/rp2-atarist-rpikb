#include <assert.h>
#include <stdbool.h>

#include "6301.h"
#include "HD6301V1ST.h"
#include "btloop.h"
#include "debug.h"
#include "gconfig.h"
#include "pico/btstack_flash_bank.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "serialp.h"
#include "settings.h"
#include "usbloop.h"

#define ROMBASE 256
#define CYCLES_PER_LOOP 1000

// Retries for queuing USB device descriptor requests
#ifndef HID_DEV_DESC_MAX_RETRIES
#define HID_DEV_DESC_MAX_RETRIES 3
#endif

// Delay between retries (ms)
#ifndef HID_DEV_DESC_RETRY_DELAY_MS
#define HID_DEV_DESC_RETRY_DELAY_MS 1000
#endif

// ~1.28 ms per byte at 7812.5 baud (10 bits)
#define IKBD_BYTE_US 800
#define RESET_SEQ_FIRST_BYTE 0x80
#define RESET_SEQ_SECOND_BYTE 0x01

#define KEYBOARD_MODE_NATIVE 0
#define KEYBOARD_MODE_USB 1
#define KEYBOARD_MODE_BT 2
#define KEYBOARD_MODE_CONFIG 255

// Timestamp in microseconds since boot when we first saw the reset sequence.
static uint64_t first_reset_sequence_us = 0;
static bool waiting_for_reset_sequence = false;
static bool reset_sequence_recorded = false;

static void launch_config_cb(void) {
  DPRINTF("launch_config_cb called\n");
  // Enable both leds to indicate configuration mode
  gpio_put(KBD_ATARI_OUT_3V3_GPIO, 1);
  gpio_put(KBD_USB_OUT_3V3_GPIO, 1);
  DPRINTF("Stopping the core 1...\n");
  multicore_reset_core1();
}

static uint64_t get_first_reset_sequence_cb(void) {
  if (!reset_sequence_recorded) {
    return 0;
  }
  return first_reset_sequence_us;
}

// static absolute_time_t next_rx_time = {0};

/**
 * Read a byte from the physical serial port and pass
 * it to the HD6301
 */
static inline void handle_rx_from_st() {
  // First ensure the 6301 SCI can accept data
  if (!hd6301_sci_busy() && (rx_available() > 0)) {
    // Drain all currently available bytes into the 6301
    unsigned char data;
    while (rx_buffer_get(&data)) {
      if (!reset_sequence_recorded) {
        if (!waiting_for_reset_sequence) {
          waiting_for_reset_sequence = (data == RESET_SEQ_FIRST_BYTE);
        } else {
          if (data == RESET_SEQ_SECOND_BYTE) {
            first_reset_sequence_us = to_us_since_boot(get_absolute_time());
            reset_sequence_recorded = true;
            waiting_for_reset_sequence = false;
            DPRINTF("First RESET sequence seen at %llu us since boot\n",
                    (unsigned long long)first_reset_sequence_us);
          } else {
            waiting_for_reset_sequence = (data == RESET_SEQ_FIRST_BYTE);
          }
        }
      }
      // DPRINTF("ST -> 6301 %02X\n", data);
      sleep_us(IKBD_BYTE_US);  // Small delay to avoid overwhelming the 6301
      hd6301_receive_byte(data);
    }
  }
}

static inline void select_native_keyboard_source(void) {
  gpio_put(KBD_ATARI_OUT_3V3_GPIO, 1);
  gpio_put(KBD_USB_OUT_3V3_GPIO, 0);
}

static inline void select_rp_keyboard_source(void) {
  gpio_put(KBD_ATARI_OUT_3V3_GPIO, 0);
  gpio_put(KBD_USB_OUT_3V3_GPIO, 1);
}

static inline void select_no_source(void) {
  gpio_put(KBD_ATARI_OUT_3V3_GPIO, 0);
  gpio_put(KBD_USB_OUT_3V3_GPIO, 0);
}

void toogle_ikbd_source_cb(void) {
  int atari_state = gpio_get(KBD_ATARI_OUT_3V3_GPIO);
  int usb_state = gpio_get(KBD_USB_OUT_3V3_GPIO);

  gpio_put(KBD_ATARI_OUT_3V3_GPIO, !atari_state);
  gpio_put(KBD_USB_OUT_3V3_GPIO, !usb_state);

  // Stop core 1 if switching to native keyboard mode
  if (!atari_state) {
    multicore_reset_core1();
  }
}

static void handle_reset_sequence_cb(void) {
  uint64_t reset_sequence = get_first_reset_sequence_cb();
  if (reset_sequence == 0) {
    return;
  }

  if (reset_sequence > (MAX_RESET_HOLD_TIME_SEC * SEC_TO_US)) {
    // Ignore
    return;
  }

  // DPRINTF("Reset sequence detected: %llu\n",
  //         (unsigned long long)reset_sequence);

  if (reset_sequence >= (ENTER_CONFIG_MODE_HOLD_TIME_SEC * SEC_TO_US)) {
    DPRINTF("Entering configuration mode...\n");
    DPRINTF("Stopping the CYW43 chipset...\n");
    cyw43_arch_deinit();
    DPRINTF("CYW43 stopped.\n");
    DPRINTF("Launching configuration...\n");
    launch_config_cb();
    DPRINTF("You should not see this message... Halting.\n");
    while (1) {
      tight_loop_contents();
    }
  } else if (reset_sequence >= (TOGGLE_IKBD_SOURCE_HOLD_TIME_SEC * SEC_TO_US)) {
    DPRINTF("Toggling IKBD source...\n");
    toogle_ikbd_source_cb();
    DPRINTF("Stopping the CYW43 chipset...\n");
    cyw43_arch_deinit();
    DPRINTF("CYW43 stopped. Halting.\n");
    DPRINTF(
        "The device is now in bypass mode. Restart to re-enable "
        "Bluetooth.\n");
    while (1) {
      tight_loop_contents();
    }
  }
}

static void run_native_keyboard_mode(void (*reset_sequence_cb)(void)) {
  DPRINTF("Entering native keyboard mode (PARAM_MODE=0)\n");
  select_native_keyboard_source();
  while (true) {
    handle_rx_from_st();
    if (reset_sequence_cb) {
      reset_sequence_cb();
    }
    tight_loop_contents();
  }
}

static void run_configuration_mode(void) {
  DPRINTF("Entering configuration mode (PARAM_MODE default/other)\n");
  while (true) {
    handle_rx_from_st();
    tight_loop_contents();
  }
}

static int get_keyboard_mode_from_settings(void) {
  SettingsConfigEntry* entry =
      settings_find_entry(gconfig_getContext(), PARAM_MODE);
  if (!entry || entry->value[0] == '\0') {
    DPRINTF("PARAM_MODE missing. Falling back to configuration mode.\n");
    return KEYBOARD_MODE_CONFIG;
  }

  char* endptr = NULL;
  long parsed = strtol(entry->value, &endptr, 10);
  if (endptr == entry->value) {
    DPRINTF("Invalid PARAM_MODE value '%s'. Starting configuration mode.\n",
            entry->value);
    return KEYBOARD_MODE_CONFIG;
  }

  DPRINTF("Configured keyboard mode: %ld\n", parsed);
  return (int)parsed;
}

static void core1_entry() {
  flash_safe_execute_core_init();

  // Initialise the HD6301
  DPRINTF("HD6301 core started\n");
  DPRINTF("Initialising HD6301...\n");

  BYTE* pram = hd6301_init();
  if (!pram) {
    DPRINTF("Failed to initialise HD6301\n");
    exit(-1);
  }
  memcpy(pram + ROMBASE, rom_HD6301V1ST_img, rom_HD6301V1ST_img_len);
  DPRINTF("Loaded HD6301 ROM\n");

  // Reset the HD6301
  DPRINTF("Resetting HD6301...\n");
  hd6301_reset(1);

  // Main loop in the HD6301 core
  DPRINTF("Entering HD6301 core loop...\n");
  while (true) {
    hd6301_tx_empty(1);
    hd6301_run_clocks(CYCLES_PER_LOOP);
  }
}

int main() {
  // Set the clock frequency. 20% overclocking
  set_sys_clock_khz(RP2040_CLOCK_FREQ_KHZ, true);

  // Set the voltage
  vreg_set_voltage(RP2040_VOLTAGE);

  // Configure the output pins
  gpio_init(KBD_ATARI_OUT_3V3_GPIO);
  gpio_set_dir(KBD_ATARI_OUT_3V3_GPIO, GPIO_OUT);
  gpio_put(KBD_ATARI_OUT_3V3_GPIO, 0);

  gpio_init(KBD_USB_OUT_3V3_GPIO);
  gpio_set_dir(KBD_USB_OUT_3V3_GPIO, GPIO_OUT);
  gpio_put(KBD_USB_OUT_3V3_GPIO, 0);

#if defined(_DEBUG) && (_DEBUG != 0)
  // Initialize chosen serial port
  stdio_init_all();
  setvbuf(stdout, NULL, _IOFBF, 256);

  // Only startup information to display
  DPRINTF("SidecarTridge IKBD Emulator. %s (%s). %s mode.\n\n", RELEASE_VERSION,
          RELEASE_DATE, _DEBUG ? "DEBUG" : "RELEASE");

  // Show information about the frequency and voltage
  int current_clock_frequency_khz = RP2040_CLOCK_FREQ_KHZ;
  const char* current_voltage = VOLTAGE_VALUES[RP2040_VOLTAGE];
  DPRINTF("Clock frequency: %i KHz\n", current_clock_frequency_khz);
  DPRINTF("Voltage: %s\n", current_voltage);
  DPRINTF("PICO_FLASH_SIZE_BYTES: %i\n", PICO_FLASH_SIZE_BYTES);
  DPRINTF("PICO_FLASH_BANK_STORAGE_OFFSET: 0x%X\n",
          (unsigned int)PICO_FLASH_BANK_STORAGE_OFFSET);
  DPRINTF("PICO_FLASH_BANK_TOTAL_SIZE: %u bytes\n",
          (unsigned int)PICO_FLASH_BANK_TOTAL_SIZE);

  unsigned int flash_length =
      (unsigned int)&_config_flash_start - (unsigned int)&__flash_binary_start;
  unsigned int booster_flash_length = flash_length;
  unsigned int config_flash_length = (unsigned int)&_global_lookup_flash_start -
                                     (unsigned int)&_config_flash_start;
  unsigned int global_lookup_flash_length = FLASH_SECTOR_SIZE;
  unsigned int global_config_flash_length = FLASH_SECTOR_SIZE;
  unsigned int bt_tlv_flash_length = 2 * FLASH_SECTOR_SIZE;
  unsigned int bt_tlv_flash_start =
      XIP_BASE + PICO_FLASH_SIZE_BYTES - bt_tlv_flash_length;

  assert(PICO_FLASH_BANK_STORAGE_OFFSET == (bt_tlv_flash_start - XIP_BASE));
  assert(PICO_FLASH_BANK_TOTAL_SIZE == bt_tlv_flash_length);

  DPRINTF("Flash start: 0x%X, length: %u bytes\n",
          (unsigned int)&__flash_binary_start, flash_length);
  DPRINTF("Booster Flash start: 0x%X, length: %u bytes\n",
          (unsigned int)&__flash_binary_start, booster_flash_length);
  DPRINTF("Config Flash start: 0x%X, length: %u bytes\n",
          (unsigned int)&_config_flash_start, config_flash_length);
  DPRINTF("Global Lookup Flash start: 0x%X, length: %u bytes\n",
          (unsigned int)&_global_lookup_flash_start,
          global_lookup_flash_length);
  DPRINTF("Global Config Flash start: 0x%X, length: %u bytes\n",
          (unsigned int)&_global_config_flash_start,
          global_config_flash_length);
  DPRINTF("BT TLV Flash start: 0x%X, length: %u bytes\n", bt_tlv_flash_start,
          bt_tlv_flash_length);

#endif

  // Initialize serial port before the hd6301 core starts
  DPRINTF("Initialising serial port...\n");
  serialp_open();

  // Load the global configuration parameters
  int err = gconfig_init("IKBD");
  DPRINTF("gconfig_init returned: %i\n", err);
  if (err != GCONFIG_SUCCESS) {
    if (err == GCONFIG_MISMATCHED_APP) {
      DPRINTF(
          "Current app does not match the stored configuration. "
          "Reinitializing settings.\n");
      err = settings_erase(gconfig_getContext());
      DPRINTF("settings_erase returned: %i\n", err);
      if (err != 0) {
        DPRINTF("Error erasing the global configuration manager: %i. STOP!\n",
                err);
        while (1);
      }
      DPRINTF("Forcing reset of the board...\n");
      watchdog_reboot(0, 0, 0);
    } else {
      settings_print(gconfig_getContext(), NULL);
      // Let's create the default configuration
      err = settings_save(gconfig_getContext(), true);
      if (err != 0) {
        DPRINTF("Error initializing the global configuration manager: %i\n",
                err);
        return err;
      }
    }
  }

  // Core 1 runs the HD6301 emulation. BTStack/Bluepad32 flash persistence only
  // worked reliably once Core 1 was enabled with
  // PICO_FLASH_ASSUME_CORE1_SAFE=1; when Core 1 was disabled for development
  // the flash writes silently failed. Keep Core 1 active when relying on
  // BTStack TLV storage.
  DPRINTF("Starting HD6301 core...\n");
  multicore_launch_core1(core1_entry);

  // Configure the input pins KBD RESET and BD0SEL0000
  gpio_init(KBD_RESET_IN_3V3_GPIO);
  gpio_set_dir(KBD_RESET_IN_3V3_GPIO, GPIO_IN);
  gpio_set_pulls(KBD_RESET_IN_3V3_GPIO, false,
                 true);  // Pull down (false, true)
  gpio_pull_down(KBD_RESET_IN_3V3_GPIO);

  gpio_init(KBD_BD0SEL_3V3_GPIO);
  gpio_set_dir(KBD_BD0SEL_3V3_GPIO, GPIO_IN);
  gpio_disable_pulls(
      KBD_BD0SEL_3V3_GPIO);  // Ignore the signal. We don't use it.

  // Configure the input pin KBD_TOOGLE_IN_3V3_GPIO
  gpio_init(KBD_TOOGLE_IN_3V3_GPIO);
  gpio_set_dir(KBD_TOOGLE_IN_3V3_GPIO, GPIO_IN);
  gpio_set_pulls(KBD_TOOGLE_IN_3V3_GPIO, false,
                 true);  // Pull down (false, true)
  gpio_pull_down(KBD_TOOGLE_IN_3V3_GPIO);

  // Capture initial states to detect edges/changes later
  int prev_reset_state = gpio_get(KBD_RESET_IN_3V3_GPIO);
  int prev_toggle_state = gpio_get(KBD_TOOGLE_IN_3V3_GPIO);

  int keyboard_mode = get_keyboard_mode_from_settings();
  keyboard_mode = 2;
  switch (keyboard_mode) {
    case KEYBOARD_MODE_NATIVE:
      run_native_keyboard_mode(handle_reset_sequence_cb);
      break;
    case KEYBOARD_MODE_USB:
      select_rp_keyboard_source();
      main_usb_loop(prev_reset_state, prev_toggle_state, handle_rx_from_st,
                    handle_reset_sequence_cb);
      break;
    case KEYBOARD_MODE_BT:
      select_rp_keyboard_source();
      main_bt_bluepad32(prev_reset_state, prev_toggle_state, handle_rx_from_st,
                        handle_reset_sequence_cb);
      break;
    default:
      select_no_source();
      run_configuration_mode();
      break;
  }

  while (1) {
    tight_loop_contents();
  }
}
