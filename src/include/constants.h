#ifndef CONSTANTS_H
#define CONSTANTS_H

#include "hardware/vreg.h"

// GPIO for the IKBD interface
#define KBD_RESET_IN_3V3_GPIO 3
#define KBD_BD0SEL_3V3_GPIO 6

// GPIO for the select ATARI or USB keyboard mode
#define KBD_ATARI_OUT_3V3_GPIO 7
#define KBD_USB_OUT_3V3_GPIO 8
#define KBD_CONFIG_IN_3V3_GPIO 9

// GPIO assignments for serial connection to Atari ST
#define UART_TX 4
#define UART_RX 5
#define UART_DEVICE uart1

// Joystick 1
#define JOY1_UP 11
#define JOY1_DOWN 12
#define JOY1_LEFT 13
#define JOY1_RIGHT 14
#define JOY1_FIRE 15

// Joystick 0
#define JOY0_UP 16
#define JOY0_DOWN 17
#define JOY0_LEFT 18
#define JOY0_RIGHT 19
#define JOY0_FIRE 20

// Time macros
#define SEC_TO_MS 1000ULL
#define SEC_TO_US 1000000ULL
#define SEC_TO_NS 1000000000ULL

// Toggle IKBD source time in seconds
#define TOGGLE_IKBD_SOURCE_HOLD_TIME_SEC 3

// Enter into configuration mode time in seconds
#define ENTER_CONFIG_MODE_HOLD_TIME_SEC 10

// Ignore resets longer than this (in seconds)
#define MAX_RESET_HOLD_TIME_SEC 20

// Frequency constants.
#define SAMPLE_DIV_FREQ (1.f)         // Sample frequency division factor.
#define RP2040_CLOCK_FREQ_KHZ 225000  // Clock frequency in KHz (225MHz).
#define USB_CLOCK_FREQ_KHZ 48000      // Clock frequency in KHz (48MHz).

// Voltage constants.
#define RP2040_VOLTAGE VREG_VOLTAGE_1_20  // Voltage in 1.20 Volts.
#define VOLTAGE_VALUES                                                 \
  (const char *[]){"NOT VALID", "NOT VALID", "NOT VALID", "NOT VALID", \
                   "NOT VALID", "NOT VALID", "0.85v",     "0.90v",     \
                   "0.95v",     "1.00v",     "1.05v",     "1.10v",     \
                   "1.15v",     "1.20v",     "1.25v",     "1.30v",     \
                   "NOT VALID", "NOT VALID", "NOT VALID", "NOT VALID", \
                   "NOT VALID"}

#define CURRENT_APP_NAME_KEY "BOOSTER"

// Time macros
#define GET_CURRENT_TIME() \
  (((uint64_t)timer_hw->timerawh) << 32u | timer_hw->timerawl)
#define GET_CURRENT_TIME_INTERVAL_MS(start) \
  (uint32_t)((GET_CURRENT_TIME() - start) / \
             (((uint32_t)RP2040_CLOCK_FREQ_KHZ) / 1000))

// NOLINTBEGIN(readability-identifier-naming)
extern unsigned int __flash_binary_start;
extern unsigned int _booster_app_flash_start;
extern unsigned int _storage_flash_start;
extern unsigned int _config_flash_start;
extern unsigned int _global_lookup_flash_start;
extern unsigned int _global_config_flash_start;
// NOLINTEND(readability-identifier-naming)

#endif  // CONSTANTS_H
