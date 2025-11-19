#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdint.h>

#include "pico/stdlib.h"

// Quadrature channels (active-low)
#define MOUSE_X_A_PIN JOY0_DOWN
#define MOUSE_X_B_PIN JOY0_UP
#define MOUSE_Y_A_PIN JOY0_RIGHT
#define MOUSE_Y_B_PIN JOY0_LEFT

// Buttons (active-low). If you only have one button, set the other to an unused
// pin and ignore it.
#define MOUSE_BTN_L_PIN JOY0_FIRE
#define MOUSE_BTN_R_PIN JOY1_FIRE

void joystick_init_usb(bool enabled, int8_t port);
void joystick_set_state(uint8_t fire_state_arg, uint8_t axis_state_arg);
void joystick_get_state(uint8_t* fire_state, uint8_t* axis_state);
void joystick_update(uint8_t port);
void joystick_init();

#endif