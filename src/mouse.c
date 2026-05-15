#include "mouse.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "debug.h"
#include "pico/time.h"

#define MOUSE_MASK 0x33333333u
#define MOUSE_GAIN_LEVELS 10

typedef struct {
  double max_speed;
  int min_speed_us;
  int deadzone_speed;
  int stop_if_period_us;
  int idle_timeout_us;
  float gain[MOUSE_GAIN_LEVELS];
} mouse_profile_t;

typedef struct {
  absolute_time_t last_input_us;
  volatile int x_period_us;  // signed: sign = direction
  volatile int y_period_us;  // signed: sign = direction
  absolute_time_t last_x_us;
  absolute_time_t last_y_us;
  uint32_t x_reg;
  uint32_t y_reg;
  int mouse_sensitivity;  // 0..9
} mouse_runtime_t;

static const mouse_profile_t s_mouse_profiles[MOUSE_PATH_COUNT] = {
    [MOUSE_PATH_HID] =
        {
            .max_speed = 150000.0,
            .min_speed_us = 650,
            .deadzone_speed = 1,
            .stop_if_period_us = 100000,
            .idle_timeout_us = 80000,
            .gain = {1.0f, 1.3f, 1.6f, 1.9f, 2.2f, 2.5f, 3.0f, 3.2f, 3.6f,
                     4.0f},
        },
    [MOUSE_PATH_NATIVE] =
        {
            .max_speed = 120000.0,
            // 2 ms floor keeps emitted rotation rate below ~500 Hz so the
            // 6301 ROM's Port-4 quadrature decoder (sampling at ~1 kHz)
            // does not alias fast motion into reverse direction.
            .min_speed_us = 2000,
            .deadzone_speed = 0,
            .stop_if_period_us = 120000,
            .idle_timeout_us = 120000,
            .gain = {1.0f, 1.2f, 1.4f, 1.7f, 2.0f, 2.3f, 2.6f, 2.9f, 3.2f,
                     3.5f},
        },
};

static mouse_runtime_t s_mouse_runtime[MOUSE_PATH_COUNT];
static volatile mouse_path_t s_active_path = MOUSE_PATH_HID;

static inline bool mouse_path_valid(mouse_path_t path) {
  return (path == MOUSE_PATH_HID || path == MOUSE_PATH_NATIVE);
}

static inline int clamp_sensitivity(int level) {
  if (level < 0) return 0;
  if (level > 9) return 9;
  return level;
}

static inline uint32_t rotl32(uint32_t v, unsigned s) {
  s &= 31;
  return (v << s) | (v >> (32 - s));
}

static inline uint32_t rotr32(uint32_t v, unsigned s) {
  s &= 31;
  return (v >> s) | (v << (32 - s));
}

static inline int apply_gain(const mouse_profile_t* profile, int v, int level) {
  return (int)(v * profile->gain[level]);
}

static inline int with_deadzone(const mouse_profile_t* profile, int v) {
  if (v >= -profile->deadzone_speed && v <= profile->deadzone_speed) return 0;
  return v;
}

static inline void map_speed_to_period_axis(const mouse_profile_t* profile,
                                            int speed,
                                            volatile int* period_us) {
  if (speed == 0) {
    *period_us = 0;
    return;
  }

  double mag = profile->max_speed / fabs((double)speed);
  int p = (int)mag;
  if (p < profile->min_speed_us) p = profile->min_speed_us;
  if (p < 1) p = 1;

  *period_us = (speed > 0) ? +p : -p;
}

static void mouse_init_runtime(mouse_path_t path) {
  mouse_runtime_t* rt = &s_mouse_runtime[path];
  absolute_time_t now = get_absolute_time();

  rt->x_reg = MOUSE_MASK;
  rt->y_reg = MOUSE_MASK;
  rt->x_reg = rotl32(rt->x_reg, (unsigned)(rand() & 15));
  rt->y_reg = rotl32(rt->y_reg, (unsigned)(rand() & 15));

  rt->x_period_us = 0;
  rt->y_period_us = 0;
  rt->last_x_us = now;
  rt->last_y_us = now;
  rt->last_input_us = now;
  rt->mouse_sensitivity = 9;
}

static void mouse_set_sensitivity_for_path(mouse_path_t path, int level) {
  if (!mouse_path_valid(path)) return;
  s_mouse_runtime[path].mouse_sensitivity = clamp_sensitivity(level);
}

static int mouse_get_sensitivity_for_path(mouse_path_t path) {
  if (!mouse_path_valid(path)) return 0;
  return s_mouse_runtime[path].mouse_sensitivity;
}

static void mouse_set_speed_for_path(mouse_path_t path, int x_in, int y_in) {
  if (!mouse_path_valid(path)) return;

  const mouse_profile_t* profile = &s_mouse_profiles[path];
  mouse_runtime_t* rt = &s_mouse_runtime[path];

  int x = apply_gain(profile, x_in, rt->mouse_sensitivity);
  int y = apply_gain(profile, y_in, rt->mouse_sensitivity);

  x = with_deadzone(profile, x);
  y = with_deadzone(profile, y);

  map_speed_to_period_axis(profile, x, &rt->x_period_us);
  map_speed_to_period_axis(profile, y, &rt->y_period_us);

  if (abs(rt->x_period_us) > profile->stop_if_period_us) rt->x_period_us = 0;
  if (abs(rt->y_period_us) > profile->stop_if_period_us) rt->y_period_us = 0;

  rt->last_input_us = get_absolute_time();
}

static void mouse_update_for_path(mouse_path_t path) {
  if (!mouse_path_valid(path)) return;

  const mouse_profile_t* profile = &s_mouse_profiles[path];
  mouse_runtime_t* rt = &s_mouse_runtime[path];
  absolute_time_t now = get_absolute_time();

  // If no fresh input in a while, force stop (prevents residual drift).
  if (absolute_time_diff_us(rt->last_input_us, now) >
      profile->idle_timeout_us) {
    rt->x_period_us = 0;
    rt->y_period_us = 0;
  }

  if (rt->x_period_us != 0) {
    int step = (rt->x_period_us > 0) ? rt->x_period_us : -rt->x_period_us;
    absolute_time_t due = delayed_by_us(rt->last_x_us, step);
    if (time_reached(due)) {
      rt->last_x_us = now;
      rt->x_reg =
          (rt->x_period_us > 0) ? rotr32(rt->x_reg, 1) : rotl32(rt->x_reg, 1);
    }
  } else {
    rt->last_x_us = now;
  }

  if (rt->y_period_us != 0) {
    int step = (rt->y_period_us > 0) ? rt->y_period_us : -rt->y_period_us;
    absolute_time_t due = delayed_by_us(rt->last_y_us, step);
    if (time_reached(due)) {
      rt->last_y_us = now;
      rt->y_reg =
          (rt->y_period_us > 0) ? rotr32(rt->y_reg, 1) : rotl32(rt->y_reg, 1);
    }
  } else {
    rt->last_y_us = now;
  }
}

void mouse_init(void) {
  mouse_init_runtime(MOUSE_PATH_HID);
  mouse_init_runtime(MOUSE_PATH_NATIVE);
}

void mouse_init_path(mouse_path_t path) {
  if (!mouse_path_valid(path)) return;
  mouse_init_runtime(path);
}

void mouse_set_active_path(mouse_path_t path) {
  if (!mouse_path_valid(path)) return;
  s_active_path = path;
}

mouse_path_t mouse_get_active_path(void) { return s_active_path; }

void mouse_set_sensitivity(int level) {
  mouse_set_sensitivity_for_path(s_active_path, level);
}

int mouse_get_sensitivity(void) {
  return mouse_get_sensitivity_for_path(s_active_path);
}

void mouse_set_sensitivity_hid(int level) {
  mouse_set_sensitivity_for_path(MOUSE_PATH_HID, level);
}

void mouse_set_sensitivity_native(int level) {
  mouse_set_sensitivity_for_path(MOUSE_PATH_NATIVE, level);
}

int mouse_get_sensitivity_hid(void) {
  return mouse_get_sensitivity_for_path(MOUSE_PATH_HID);
}

int mouse_get_sensitivity_native(void) {
  return mouse_get_sensitivity_for_path(MOUSE_PATH_NATIVE);
}

void mouse_set_speed(int x, int y) {
  mouse_set_speed_for_path(s_active_path, x, y);
}

void mouse_set_speed_hid(int x, int y) {
  mouse_set_speed_for_path(MOUSE_PATH_HID, x, y);
}

void mouse_set_speed_native(int x, int y) {
  mouse_set_speed_for_path(MOUSE_PATH_NATIVE, x, y);
}

void mouse_update(void) { mouse_update_for_path(s_active_path); }

void mouse_update_hid(void) { mouse_update_for_path(MOUSE_PATH_HID); }

void mouse_update_native(void) { mouse_update_for_path(MOUSE_PATH_NATIVE); }

void mouse_tick(int64_t /*cpu_cycles*/, int* x_counter, int* y_counter) {
  mouse_path_t active_path = s_active_path;
  if (!mouse_path_valid(active_path)) active_path = MOUSE_PATH_HID;
  mouse_runtime_t* rt = &s_mouse_runtime[active_path];
  *x_counter = (int)rt->x_reg;
  *y_counter = (int)rt->y_reg;
}
