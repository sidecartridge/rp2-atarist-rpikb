#include "mouse.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "debug.h"
#include "pico/time.h"

// --- Constants (from the working version) ---
#define MOUSE_MASK 0x33333333u
// scaler: larger |speed| -> shorter period.
// Tuned so that small but real movements (after gain) still produce
// visible motion without being clamped out.
#define MAX_SPEED 150000.0
#define MIN_SPEED 650  // µs: fastest allowed edge interval

// --- Tuning ---
// After gain, small deltas within [-DEADZONE_SPEED, +DEADZONE_SPEED]
// are treated as zero to avoid jitter at low speed.
#define DEADZONE_SPEED 1
// Allow slow movement down to ~100 ms between edges before treating as stop.
#define STOP_IF_PERIOD_US 100000  // if |period| > 100 ms, treat as stop
// If no HID input for a short time, force stop to prevent residual drift.
#define IDLE_TIMEOUT_US 80000  // if no HID for 80 ms, stop

static absolute_time_t last_input_us;

// --- Internal state ---
static volatile int x_period_us = 0;  // signed: sign = direction
static volatile int y_period_us = 0;

static absolute_time_t last_x_us;
static absolute_time_t last_y_us;

static uint32_t x_reg;
static uint32_t y_reg;

static int mouse_sensitivity = 9;  // 0..9

void mouse_set_sensitivity(int level) {
  if (level < 0)
    mouse_sensitivity = 0;
  else if (level > 9)
    mouse_sensitivity = 9;
  else
    mouse_sensitivity = level;
}

int mouse_get_sensitivity() { return mouse_sensitivity; }

// --- Helpers: 32-bit rotates ---
static inline uint32_t rotl32(uint32_t v, unsigned s) {
  s &= 31;
  return (v << s) | (v >> (32 - s));
}
static inline uint32_t rotr32(uint32_t v, unsigned s) {
  s &= 31;
  return (v >> s) | (v << (32 - s));
}

// --- Period mapping (same logic as set_speed_internal) ---
static inline void map_speed_to_period(int speed, volatile int* period_us) {
  if (speed == 0) {
    *period_us = 0;
    return;
  }

  double mag = MAX_SPEED / fabs((double)speed);  // magnitude only
  int p = (int)mag;
  if (p < MIN_SPEED) p = MIN_SPEED;  // clamp fastest rate

  *period_us = (speed > 0) ? +p : -p;  // sign comes from speed
}

// --- Public API ---
void mouse_init(void) {
  x_reg = y_reg = MOUSE_MASK;

  // Randomize starting phase (matches the working code’s idea)
  x_reg = rotl32(x_reg, rand() & 15);
  y_reg = rotl32(y_reg, rand() & 15);

  last_x_us = last_y_us = last_input_us = get_absolute_time();
}

// ganancia lineal por sensibilidad 0..9
static inline int apply_gain(int v, int level) {
  // 1.0, 1.3, 1.6, ..., 4.0
  static const float g[10] = {1.0f, 1.3f, 1.6f, 1.9f, 2.2f,
                              2.5f, 3.0f, 3.2f, 3.6f, 4.0f};
  return (int)(v * g[level]);
}

static inline int with_deadzone(int v) {
  if (v >= -DEADZONE_SPEED && v <= DEADZONE_SPEED) return 0;
  return v;
}

static inline void map_speed_to_period_axis(int speed, volatile int* period_us,
                                            int min_speed_us, float freq_mul) {
  if (speed == 0) {
    *period_us = 0;
    return;
  }

  double mag = MAX_SPEED / (speed >= 0 ? (double)speed : -(double)speed);
  int p = (int)mag;
  if (p < min_speed_us) p = min_speed_us;

  // Apply frequency multiplier (e.g., 2.0 for 2× faster)
  p = (int)(p / (freq_mul > 0 ? freq_mul : 1.0f));
  if (p < 1) p = 1;

  *period_us = (speed > 0) ? +p : -p;
}

void mouse_set_speed(int x, int y) {
  x = apply_gain(x, mouse_sensitivity);
  y = apply_gain(y, mouse_sensitivity);

  x = with_deadzone(x);
  y = with_deadzone(y);

  // X normal, Y is 2× faster
  map_speed_to_period_axis(x, &x_period_us, MIN_SPEED, 1.0f);
  map_speed_to_period_axis(y, &y_period_us, MIN_SPEED, 1.0f);

  if (abs(x_period_us) > STOP_IF_PERIOD_US) x_period_us = 0;
  if (abs(y_period_us) > STOP_IF_PERIOD_US) y_period_us = 0;

  last_input_us = get_absolute_time();
}

// Call this periodically (your main loop or timer). It advances the quadrature
// when the per-axis period elapses. (This is your old AtariSTMouse::update()).
void mouse_update(void) {
  absolute_time_t now = get_absolute_time();

  // If no fresh HID in a while, force stop (prevents residual creep)
  if (absolute_time_diff_us(last_input_us, now) > IDLE_TIMEOUT_US) {
    x_period_us = 0;
    y_period_us = 0;
  }

  if (x_period_us != 0) {
    int step = (x_period_us > 0) ? x_period_us : -x_period_us;
    absolute_time_t due = delayed_by_us(last_x_us, step);
    if (time_reached(due)) {
      last_x_us = now;
      x_reg = (x_period_us > 0) ? rotr32(x_reg, 1) : rotl32(x_reg, 1);
    }
  } else {
    // If stopped, keep the schedule anchored to "now"
    last_x_us = now;
  }

  if (y_period_us != 0) {
    int step = (y_period_us > 0) ? y_period_us : -y_period_us;
    absolute_time_t due = delayed_by_us(last_y_us, step);
    if (time_reached(due)) {
      last_y_us = now;
      y_reg = (y_period_us > 0) ? rotr32(y_reg, 1) : rotl32(y_reg, 1);
    }
  } else {
    last_y_us = now;
  }
}

// IKBD 6301 emulator calls this to read the current quadrature registers.
// dr4_getb() will mask with &3 and place X on bits[1:0], Y on bits[3:2].
void mouse_tick(int64_t /*cpu_cycles*/, int* x_counter, int* y_counter) {
  *x_counter = (int)x_reg;
  *y_counter = (int)y_reg;
}
