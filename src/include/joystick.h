#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdint.h>

#include "constants.h"
#include "pico/stdlib.h"

// Quadrature channels (active-low)
#define MOUSE_X_A_PIN JOY0_DOWN
#define MOUSE_X_B_PIN JOY0_UP
#define MOUSE_Y_A_PIN JOY0_RIGHT
#define MOUSE_Y_B_PIN JOY0_LEFT

// Buttons (active-low). If you only have one button, set the other to an unused
// pin and ignore it.
#define MOUSE_BTN_L_PIN JOY0_FIRE
#if defined(JOY0_FIRE2)
#define MOUSE_BTN_R_PIN JOY0_FIRE2
#define MOUSE_BTN_R_PIN_ALT JOY1_FIRE
#else
#define MOUSE_BTN_R_PIN JOY1_FIRE
#endif

void joystick_init_usb(bool enabled, int8_t port);
void joystick_set_autoshoot(bool enabled, int speed);
void joystick_set_state(uint8_t fire_state_arg, uint8_t axis_state_arg);
void joystick_get_state(uint8_t* fire_state, uint8_t* axis_state);
void joystick_update(uint8_t port);
void joystick_read_edges(void);
void joystick_process_mouse_edges(void);
void joystick_init();

#endif
