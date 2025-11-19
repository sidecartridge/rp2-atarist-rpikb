#include <stdbool.h>

#include "6301.h"
#include "HD6301V1ST.h"
#include "btloop.h"
#include "debug.h"
#include "gconfig.h"
#include "hardware/clocks.h"
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

static void run_native_keyboard_mode(void) {
  DPRINTF("Entering native keyboard mode (PARAM_MODE=0)\n");
  select_native_keyboard_source();
  while (true) {
    handle_rx_from_st();
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

// static inline void handle_hid_found() {
//   if (hidinput_if_ring_size() == 0) {
//     return;
//   }
//   // Process HID device found in the ring buffer
//   uint8_t dev_addr;
//   uint8_t instance;
//   while (hidinput_if_ring_pop(&dev_addr, &instance)) {
//     tuh_itf_info_t itf_info;
//     bool info_ok = tuh_hid_itf_get_info(dev_addr, instance, &itf_info);
//     if (!info_ok) {
//       DPRINTF(
//           "HID ring entry but failed to fetch itf info: addr=%d
//           instance=%d\n", dev_addr, instance);
//       continue;
//     }
//     DPRINTF("HID device found: addr=%d(instance=%d), itf_num=%d\n", dev_addr,
//             instance, itf_info.desc.bInterfaceNumber);

//     DPRINTF("HID Interface Info: daddr=%d, itf_num=%d\r\n", itf_info.daddr,
//             itf_info.desc.bInterfaceNumber);
//     DPRINTF("HID Interface Class: %d\r\n", itf_info.desc.bInterfaceClass);
//     DPRINTF("HID Interface SubClass: %d\r\n",
//     itf_info.desc.bInterfaceSubClass); DPRINTF("HID Interface Protocol:
//     %d\r\n", itf_info.desc.bInterfaceProtocol);

//     // Interface protocol (hid_interface_protocol_enum_t)
//     const char* protocol_str[] = {"None", "Keyboard", "Mouse", "",
//     "Joystick"}; uint8_t const itf_protocol =
//     tuh_hid_interface_protocol(dev_addr, instance);

//     DPRINTF("HID Device Type: %s (%d)\r\n", protocol_str[itf_protocol],
//             itf_protocol);

//     static uint8_t dev_desc_buf[18] __attribute__((aligned(4)));
//     bool ok = false;
//     int retries = HID_DEV_DESC_MAX_RETRIES;
//     do {
//       ok = tuh_descriptor_get_device(dev_addr, dev_desc_buf,
//                                      sizeof(dev_desc_buf),
//                                      hidinput_device_descriptor_complete_cb,
//                                      0);
//       if (!ok) {
//         retries--;
//         if (retries > 0) {
//           DPRINTF(
//               "Descriptor request queue failed for device %u, retrying (%d "
//               "left)\n",
//               dev_addr, retries);
//           sleep_ms(HID_DEV_DESC_RETRY_DELAY_MS);
//         }
//       }
//     } while (!ok && retries > 0);
//     if (!ok) {
//       DPRINTF(
//           "Failed to queue device descriptor request for device %u after %d "
//           "retries\n",
//           dev_addr, HID_DEV_DESC_MAX_RETRIES);
//     }
//   }
// }

static void core1_entry() {
  // Setup the UART and HID instance.

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

  unsigned int flash_length =
      (unsigned int)&_config_flash_start - (unsigned int)&__flash_binary_start;
  unsigned int booster_flash_length = flash_length;
  unsigned int config_flash_length = (unsigned int)&_global_lookup_flash_start -
                                     (unsigned int)&_config_flash_start;
  unsigned int global_lookup_flash_length = FLASH_SECTOR_SIZE;
  unsigned int global_config_flash_length = FLASH_SECTOR_SIZE;

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

  // The second CPU core is dedicated to the HD6301 emulation.
  DPRINTF("Starting HD6301 core...\n");
  multicore_launch_core1(core1_entry);

  // Configure the input pins KBD RESET and BD0SEL
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
      run_native_keyboard_mode();
      break;
    case KEYBOARD_MODE_USB:
      select_rp_keyboard_source();
      main_usb_loop(prev_reset_state, prev_toggle_state, handle_rx_from_st);
      break;
    case KEYBOARD_MODE_BT:
      select_rp_keyboard_source();
      main_bt_bluepad32(prev_reset_state, prev_toggle_state, handle_rx_from_st);
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
