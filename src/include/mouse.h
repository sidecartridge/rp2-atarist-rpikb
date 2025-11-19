#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#include "pico/stdlib.h"

void mouse_tick(int64_t cpu_cycles, int* x_counter, int* y_counter);
void mouse_set_speed(int x, int y);
void mouse_update(void);
void mouse_init(void);

void mouse_set_sensitivity(int level);
int mouse_get_sensitivity(void);

#endif