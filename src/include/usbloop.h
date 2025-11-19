#ifndef USBLOOP_H
#define USBLOOP_H

#include "bsp/board_api.h"
#include "debug.h"
#include "gconfig.h"
#include "hidinput.h"
#include "joystick.h"
#include "mouse.h"
#include "settings.h"
#include "tusb.h"

// Serial task polling interval (microseconds)
#ifndef SERIAL_POLL_INTERVAL_US
#define SERIAL_POLL_INTERVAL_US 20000  // 20ms, typical for Atari ST
#endif

// Mouse line sampling interval (microseconds)
#ifndef MOUSE_LINE_POLL_INTERVAL_US
#define MOUSE_LINE_POLL_INTERVAL_US 750  // 0.75ms
#endif

// Original mouse line sampling interval (microseconds)
#ifndef ORIGINAL_MOUSE_LINE_POLL_INTERVAL_US
#define ORIGINAL_MOUSE_LINE_POLL_INTERVAL_US 2000  // 2ms
#endif

int main_usb_loop(int prev_reset_state, int prev_toggle_state,
                  void (*handle_rx)(void));

#endif  // USBLOOP_H
