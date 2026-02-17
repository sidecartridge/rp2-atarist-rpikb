#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#include "pico/stdlib.h"

typedef enum {
  MOUSE_PATH_HID = 0,
  MOUSE_PATH_NATIVE = 1,
  MOUSE_PATH_COUNT
} mouse_path_t;

void mouse_tick(int64_t cpu_cycles, int* x_counter, int* y_counter);
void mouse_set_speed(int x, int y);
void mouse_update(void);
void mouse_init(void);
void mouse_init_path(mouse_path_t path);

void mouse_set_active_path(mouse_path_t path);
mouse_path_t mouse_get_active_path(void);

void mouse_set_sensitivity(int level);
int mouse_get_sensitivity(void);
void mouse_set_sensitivity_hid(int level);
void mouse_set_sensitivity_native(int level);
int mouse_get_sensitivity_hid(void);
int mouse_get_sensitivity_native(void);

void mouse_set_speed_hid(int x, int y);
void mouse_set_speed_native(int x, int y);
void mouse_update_hid(void);
void mouse_update_native(void);

#endif
