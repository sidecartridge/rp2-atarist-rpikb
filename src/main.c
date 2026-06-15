#include <assert.h>
#include <stdbool.h>

#include "6301.h"
#include "HD6301V1ST.h"
#if COMPUTER_TARGET_BT
#include "btloop.h"
#endif
#include "constants.h"
#include "debug.h"
#include "gconfig.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#if defined(PICO_RP2040) && PICO_RP2040
#include "hardware/regs/m0plus.h"
#elif defined(PICO_RP2350) && PICO_RP2350
#include "hardware/regs/m33.h"
#endif
#include "hardware/sync.h"
#include "hardware/timer.h"
#if defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_CROISSANT_REV2
#include "nativeloop.h"
#endif
#include "mode_shutdown.h"
#include "pico/btstack_flash_bank.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "serialp.h"
#include "settings.h"
#if COMPUTER_TARGET_USB
#include "usbloop.h"
#endif

// ~1.28 ms per byte at 7812.5 baud (10 bits)
#define IKBD_BYTE_US 800
#define IKBD_RESET_SEQ_FIRST_BYTE 0x80
#define IKBD_RESET_SEQ_SECOND_BYTE 0x01
#define IKBD_CMD_SET_TOD 0x1b
#define IKBD_TOD_YEAR 0x90
#define IKBD_TOD_MONTH 0x01
#define IKBD_TOD_DAY 0x01
#define IKBD_TOD_HOUR 0x00
#define IKBD_TOD_MINUTE 0x00
#define IKBD_TOD_SECOND 0x00
#define IKBD_ROMBASE 256
#define IKBD_CYCLES_PER_LOOP 1000

#define KEYBOARD_MODE_NATIVE 0
#define KEYBOARD_MODE_USB 1
#define KEYBOARD_MODE_BT 2
#define KEYBOARD_MODE_CONFIG 255

// Timestamp in microseconds since boot when we first saw the reset sequence.
static uint64_t ikbd_first_reset_sequence_us = 0;
static bool ikbd_waiting_for_reset_sequence = false;
static bool ikbd_reset_sequence_recorded = false;

#ifndef USBDRIVE_APP_OFFSET
#define USBDRIVE_APP_OFFSET \
  ((unsigned int)&_booster_app_flash_start - (unsigned int)XIP_BASE)
#endif

// Tracks which mode loop is currently active so jump_to_booster_app() can
// dispatch the right teardown. Each mode loop calls mode_shutdown_set_active()
// once its peripherals are up; the dispatcher then routes to the matching
// teardown (USB, BT, native no-op) at jump time. Set under a single 8-bit
// write so cross-core visibility is straightforward; reads happen from
// Core 0 only (Core 1 never calls jump_to_booster_app).
static volatile mode_shutdown_kind_t s_active_mode = MODE_SHUTDOWN_NONE;

void mode_shutdown_set_active(mode_shutdown_kind_t kind) {
  s_active_mode = kind;
}

void mode_shutdown_for_jump(void) {
  switch (s_active_mode) {
#if COMPUTER_TARGET_USB
    case MODE_SHUTDOWN_USB:
      usbloop_shutdown_for_jump();
      break;
#endif
#if COMPUTER_TARGET_BT
    case MODE_SHUTDOWN_BT:
      btloop_shutdown_for_jump();
      break;
#endif
    case MODE_SHUTDOWN_NATIVE:
    case MODE_SHUTDOWN_NONE:
    default:
      // Nothing to tear down (native mode has no IRQ-active peripherals;
      // NONE covers the boot-time direct jump to booster).
      break;
  }
}

// Sanity-check the booster app's vector table before swapping VTOR to it.
// Catches the case where the booster region is empty or corrupted (e.g., the
// IKBD UF2 was flashed without first flashing the booster). Returns false if
// the initial SP or reset vector looks invalid; the caller refuses to jump
// in that case, leaving the device in a known idle state for diagnosis.
static bool booster_vector_looks_sane(void) {
  const uint32_t *vt;
#if defined(PICO_RP2040) && PICO_RP2040
  vt = (const uint32_t *)((unsigned int)&_booster_app_flash_start + 256);
#elif defined(PICO_RP2350) && PICO_RP2350
  vt = (const uint32_t *)&_booster_app_flash_start;
#else
  return false;
#endif
  uint32_t sp = vt[0];
  uint32_t reset = vt[1];

  // Initial Main Stack Pointer must be inside SRAM. Window covers both
  // RP2040 (264 KB total) and RP2350 (520 KB total) layouts.
  if (sp < 0x20000000u || sp >= 0x21000000u) {
    return false;
  }
  // Reset vector must have the Thumb bit (bit 0) set and point into the
  // booster's flash region (BOOSTER_APP_FLASH is 768 KB per linker scripts).
  if ((reset & 1u) == 0u) {
    return false;
  }
  const uint32_t booster_start = (unsigned int)&_booster_app_flash_start;
  const uint32_t reset_addr = reset & ~1u;
  if (reset_addr < booster_start || reset_addr >= booster_start + 0xC0000u) {
    return false;
  }
  return true;
}

// Hand control to the booster app flashed at _booster_app_flash_start by
// rewriting Core 0's VTOR and branching to the booster's reset vector.
//
// Caller contract: MUST NOT be invoked while Core 0 is mid-flight in a
// flash_safe_execute() call. multicore_reset_core1() halts Core 1 before it
// can acknowledge the lockout handshake; any pending flash op on Core 0 will
// then deadlock waiting for an acknowledgement that can never arrive. Today
// no caller violates this — settings_save / gconfig_init's defaults save /
// BTstack TLV writes all happen at boot, before any mode loop is running and
// therefore before any config-press can fire jump_to_booster_app. If a
// future change adds a runtime flash write from a mode loop, it MUST drain
// any in-flight flash work before letting config-press dispatch here.
static inline void jump_to_booster_app() {
  // Refuse to jump if the booster vector table is missing or corrupted.
  // Done before any peripheral teardown so the device stays in a sane state
  // (LEDs still lit, UART alive) for the user to diagnose.
  if (!booster_vector_looks_sane()) {
    DPRINTF(
        "Booster vector at 0x%X looks invalid (SP/reset out of range). "
        "Refusing to jump. Power-cycle to recover.\n",
        (unsigned int)&_booster_app_flash_start);
    while (1) {
      tight_loop_contents();
    }
  }

  // Disable the LEDs before leaving
  gpio_put(KBD_ATARI_OUT_3V3_GPIO, 0);
  gpio_put(KBD_USB_OUT_3V3_GPIO, 0);

  // Disable ST UART path to avoid pin/peripheral conflicts after jump.
  serialp_close();

  // Disabling core 1 before leaving
  DPRINTF("Stopping the core 1...\n");

  // Mask all maskable interrupts BEFORE the mode-specific teardown. This is
  // a hard requirement for the BT path: cyw43_arch_deinit() nulls the
  // BTstack run loop mid-teardown, and any CYW43 SPI/SDIO IRQ that fires
  // before the run loop pointer is gone would assert in
  // btstack_run_loop_poll_data_sources_from_irq. With IRQs masked, no such
  // dispatch can happen. The booster app sets up its own interrupt state
  // after the jump; we discard the prior PRIMASK.
  (void)save_and_disable_interrupts();

  // Mode-specific teardown for whichever loop is currently active (USB drops
  // TinyUSB host + timers; BT deinits CYW43; native is a no-op). Safe to
  // call even at boot before any mode loop started — the dispatcher's NONE
  // case is also a no-op.
  mode_shutdown_for_jump();

  // Jumping to the FLASH entry of the booster app
  multicore_reset_core1();
  // Give Core 1 a settle window before we change Core 0's VTOR. The SDK's
  // multicore_reset_core1() already handshakes via the FIFO, but on some
  // boards / clock conditions a few extra microseconds before the asm block
  // below avoid edge cases where Core 1 is mid-bootrom-fetch when we begin
  // rewriting state. 100 us is overkill at 225 MHz; cost is negligible.
  busy_wait_us(100);
  // Jump to booster code (RP2040-only sequence).
#if defined(PICO_RP2040) && PICO_RP2040
  __asm__ __volatile__(
      "mov r0, %[start]\n"
      "ldr r1, =%[vtable]\n"
      "str r0, [r1]\n"
      "ldmia r0, {r0, r1}\n"
      "msr msp, r0\n"
      "bx r1\n"
      :
      : [start] "r"((unsigned int)&_booster_app_flash_start + 256),
        [vtable] "X"(PPB_BASE + M0PLUS_VTOR_OFFSET)
      : "r0", "r1", "memory", "cc");
  DPRINTF("You should never reach this point\n");
#elif defined(PICO_RP2350) && PICO_RP2350
  __asm__ __volatile__(
      "mov r0, %[start]\n"
      "ldr r1, =%[vtable]\n"
      "str r0, [r1]\n"
      "ldmia r0, {r0, r1}\n"
      "msr msp, r0\n"
      "bx r1\n"
      :
      : [start] "r"((unsigned int)&_booster_app_flash_start),
        [vtable] "X"(PPB_BASE + M33_VTOR_OFFSET)
      : "r0", "r1", "memory", "cc");
  DPRINTF("You should never reach this point\n");
#else
  DPRINTF("Booster jump is only supported on RP2040/RP2350 builds\n");
  return;
#endif
}

void launch_config_cb(void) {
  DPRINTF("launch_config_cb called\n");
  jump_to_booster_app();
}

static void enter_configuration_mode_cb(void) {
#if defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_CROISSANT_REV2
  enter_configuration_mode();
#else
  jump_to_booster_app();
#endif
}

static uint64_t get_first_reset_sequence_cb(void) {
  if (!ikbd_reset_sequence_recorded) {
    return 0;
  }
  return ikbd_first_reset_sequence_us;
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
#if defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_CROISSANT_REV2
      if (!ikbd_reset_sequence_recorded) {
        if (!ikbd_waiting_for_reset_sequence) {
          ikbd_waiting_for_reset_sequence = (data == IKBD_RESET_SEQ_FIRST_BYTE);
        } else {
          if (data == IKBD_RESET_SEQ_SECOND_BYTE) {
            ikbd_first_reset_sequence_us =
                to_us_since_boot(get_absolute_time());
            ikbd_reset_sequence_recorded = true;
            ikbd_waiting_for_reset_sequence = false;
            DPRINTF("First RESET sequence seen at %llu us since boot\n",
                    (unsigned long long)ikbd_first_reset_sequence_us);
          } else {
            ikbd_waiting_for_reset_sequence =
                (data == IKBD_RESET_SEQ_FIRST_BYTE);
          }
        }
      }
#endif
      DPRINTF("ST -> 6301 %02X\n", data);
      // sleep_us(IKBD_BYTE_US);  // Small delay to avoid overwhelming the 6301
      hd6301_receive_byte(data);
    }
  }
}

static inline void select_rp_keyboard_source(void) {
  gpio_put(KBD_ATARI_OUT_3V3_GPIO, 0);
  gpio_put(KBD_USB_OUT_3V3_GPIO, 1);
}

static inline void select_no_source(void) {
  gpio_put(KBD_ATARI_OUT_3V3_GPIO, 0);
  gpio_put(KBD_USB_OUT_3V3_GPIO, 0);
}

// Per-mode indicator helpers. Semantics differ by board:
//
// - Croissant: KBD_ATARI / KBD_USB drive an analog mux that selects whether
//   the native Atari keyboard or the RP-emulated IKBD feeds the IKBD line.
//   Both USB and BT modes need the "RP source" position, so both indicator
//   helpers route to select_rp_keyboard_source().
//
// - Souffle: there is no native-Atari source path. The same two GPIOs drive
//   the BT-labeled and USB-labeled mode-indicator LEDs:
//       KBD_ATARI_OUT_3V3_GPIO (GPIO 7) -> BT-labeled LED
//       KBD_USB_OUT_3V3_GPIO   (GPIO 8) -> USB-labeled LED
//   So we light exactly one at a time based on which RP mode is running.
#if defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_SOUFFLE_REV2
// Active mode-indicator LED brightness as a PWM duty. The LED is held lit for
// the entire session, so at the lowered core operating point it is a large
// share of board current. ~20% keeps it clearly visible while cutting most of
// that current. PWM frequency (sys_clk / (wrap+1) ~= 50 kHz) is far above any
// visible flicker. Tune MODE_LED_PWM_LEVEL to taste.
#define MODE_LED_PWM_WRAP 1000
#define MODE_LED_PWM_LEVEL 200

// Drive a mode-LED pin off (plain GPIO low).
static inline void mode_led_off(uint pin) {
  gpio_set_function(pin, GPIO_FUNC_SIO);
  gpio_set_dir(pin, GPIO_OUT);
  gpio_put(pin, 0);
}

// Drive a mode-LED pin dimmed via PWM at MODE_LED_PWM_LEVEL duty.
static inline void mode_led_dim(uint pin) {
  gpio_set_function(pin, GPIO_FUNC_PWM);
  uint slice = pwm_gpio_to_slice_num(pin);
  uint chan = pwm_gpio_to_channel(pin);
  pwm_set_wrap(slice, MODE_LED_PWM_WRAP);
  pwm_set_chan_level(slice, chan, MODE_LED_PWM_LEVEL);
  pwm_set_enabled(slice, true);
}

static inline void indicate_usb_mode(void) {
  mode_led_off(KBD_ATARI_OUT_3V3_GPIO);  // BT-labeled LED off
  mode_led_dim(KBD_USB_OUT_3V3_GPIO);    // USB-labeled LED dimmed
}
static inline void indicate_bt_mode(void) {
  mode_led_off(KBD_USB_OUT_3V3_GPIO);    // USB-labeled LED off
  mode_led_dim(KBD_ATARI_OUT_3V3_GPIO);  // BT-labeled LED dimmed
}
#else
#define indicate_usb_mode() select_rp_keyboard_source()
#define indicate_bt_mode() select_rp_keyboard_source()
#endif

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
    enter_configuration_mode_cb();
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

// Fired on Core 1 by the pacing alarm. Intentionally empty: its only job is to
// raise an IRQ on this core so the pacing loop's __wfe() wakes at the deadline.
static void core1_pacing_alarm_cb(uint alarm_num) { (void)alarm_num; }

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
  memcpy(pram + IKBD_ROMBASE, rom_HD6301V1ST_img, rom_HD6301V1ST_img_len);
  DPRINTF("Loaded HD6301 ROM\n");

  // Reset the HD6301
  DPRINTF("Resetting HD6301...\n");
  hd6301_reset(1);

  // Seed IKBD time-of-day so the emulated clock starts ticking.
  rx_buffer_put(IKBD_CMD_SET_TOD);  // IKBD command to set TOD
  rx_buffer_put(IKBD_TOD_YEAR);
  rx_buffer_put(IKBD_TOD_MONTH);
  rx_buffer_put(IKBD_TOD_DAY);
  rx_buffer_put(IKBD_TOD_HOUR);
  rx_buffer_put(IKBD_TOD_MINUTE);
  rx_buffer_put(IKBD_TOD_SECOND);
  DPRINTF("Seeded IKBD time-of-day clock\n");

  // Dedicated hardware alarm so the pacing loop can idle in __wfe() between run
  // windows instead of busy-spinning. The callback is installed from Core 1, so
  // the alarm IRQ fires on this core and wakes __wfe() at the deadline. Cuts
  // Core 1 idle duty (and thus static power) without changing 6301 throughput.
  int pacing_alarm = hardware_alarm_claim_unused(true);
  hardware_alarm_set_callback(pacing_alarm, core1_pacing_alarm_cb);

  // Main loop in the HD6301 core
  DPRINTF("Entering HD6301 core loop...\n");
  while (true) {
    static uint64_t last_run_us = 0;
    uint64_t now_us = time_us_64();
    if (last_run_us == 0) {
      last_run_us = now_us;
    }
    uint64_t delta_us = now_us - last_run_us;
    if (delta_us >= IKBD_CYCLES_PER_LOOP) {
      last_run_us = now_us;
      hd6301_run_clocks(IKBD_CYCLES_PER_LOOP);
      hd6301_tx_empty(1);
    } else {
      // Sleep until the next pacing deadline. set_target returns true if that
      // deadline is already in the past (nothing armed); only then skip the
      // wait and re-evaluate. The alarm IRQ — or any other event, e.g. the
      // multicore-lockout FIFO IRQ during a flash write — wakes __wfe().
      absolute_time_t due =
          from_us_since_boot(last_run_us + IKBD_CYCLES_PER_LOOP);
      if (!hardware_alarm_set_target(pacing_alarm, due)) {
        __wfe();
      }
    }
  }
}

int main() {
  // Set the clock frequency.
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
  DPRINTF("PICO_FLASH_SAFE_EXECUTE_PICO_SUPPORT_MULTICORE_LOCKOUT: %i\n",
          PICO_FLASH_SAFE_EXECUTE_PICO_SUPPORT_MULTICORE_LOCKOUT);
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
  unsigned int bt_tlv_flash_length = (unsigned int)PICO_FLASH_BANK_TOTAL_SIZE;
  unsigned int bt_tlv_flash_start =
      (unsigned int)(XIP_BASE + PICO_FLASH_BANK_STORAGE_OFFSET);

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

  // Check first
  int keyboard_mode = get_keyboard_mode_from_settings();
  if (keyboard_mode == KEYBOARD_MODE_CONFIG) {
    DPRINTF("Starting in configuration mode directly...\n");
    jump_to_booster_app();
    while (1) {
      tight_loop_contents();
    }
    DPRINTF("You should never reach this point\n");
  }

  // Core 1 runs the HD6301 emulation. BTStack/Bluepad32 flash persistence only
  // worked reliably once Core 1 was enabled with
  // PICO_FLASH_ASSUME_CORE1_SAFE=1; when Core 1 was disabled for development
  // the flash writes silently failed. Keep Core 1 active when relying on
  // BTStack TLV storage.
  DPRINTF("Starting HD6301 core...\n");
  multicore_launch_core1(core1_entry);

#if defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_CROISSANT_REV2
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
#endif

  // Configure the input pin KBD_CONFIG_IN_3V3_GPIO
  gpio_init(KBD_CONFIG_IN_3V3_GPIO);
  gpio_set_dir(KBD_CONFIG_IN_3V3_GPIO, GPIO_IN);
  gpio_set_pulls(KBD_CONFIG_IN_3V3_GPIO, false,
                 true);  // Pull down (false, true)
  gpio_pull_down(KBD_CONFIG_IN_3V3_GPIO);

  // Capture initial states to detect edges/changes later
  int prev_reset_state = 0;
#if defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_CROISSANT_REV2
  prev_reset_state = gpio_get(KBD_RESET_IN_3V3_GPIO);
#endif
  int prev_config_state = gpio_get(KBD_CONFIG_IN_3V3_GPIO);

  void (*reset_sequence_cb_ptr)(void) = NULL;
#if defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_CROISSANT_REV2
  reset_sequence_cb_ptr = handle_reset_sequence_cb;
#endif

  switch (keyboard_mode) {
    case KEYBOARD_MODE_NATIVE:
#if COMPUTER_TARGET_NATIVE
#if defined(BOARD_TARGET) && BOARD_TARGET == BOARD_TARGET_CROISSANT_REV2
      run_native_keyboard_mode(handle_reset_sequence_cb);
#else
      DPRINTF("Native mode unsupported on BOARD_TARGET=%d\n", BOARD_TARGET);
      select_no_source();
      run_configuration_mode();
#endif
#else
      DPRINTF("Native mode disabled by COMPUTER_TARGET_NATIVE\n");
      select_no_source();
      run_configuration_mode();
#endif
      break;
    case KEYBOARD_MODE_BT:
#if COMPUTER_TARGET_BT
      indicate_bt_mode();
      main_bt_bluepad32(prev_reset_state, prev_config_state, handle_rx_from_st,
                        reset_sequence_cb_ptr);
#else
      DPRINTF("BT mode disabled by COMPUTER_TARGET_BT\n");
      select_no_source();
      run_configuration_mode();
#endif
      break;
    case KEYBOARD_MODE_USB:
#if COMPUTER_TARGET_USB
      indicate_usb_mode();
      main_usb_loop(prev_reset_state, prev_config_state, handle_rx_from_st,
                    reset_sequence_cb_ptr);
      break;
#else
      DPRINTF("USB mode disabled by COMPUTER_TARGET_USB\n");
#endif
    default:
      select_no_source();
      run_configuration_mode();
      break;
  }

  while (1) {
    tight_loop_contents();
  }
}
