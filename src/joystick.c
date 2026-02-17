#include "joystick.h"

#include "debug.h"
#include "mouse.h"
#include "pico/time.h"

#define JOY_GPIO_INIT(io)    \
  gpio_init(io);             \
  gpio_set_dir(io, GPIO_IN); \
  gpio_pull_up(io);

static uint8_t axis_state = 0;
static uint8_t fire_state = 0;
static bool usb_joystick_enabled = false;
static uint8_t usb_joystick_port = 0;
static bool usb_autoshoot_enabled = false;
static uint8_t usb_autoshoot_speed = 0;
static uint32_t usb_autoshoot_toggle_interval_us = 0;
static bool usb_autoshoot_phase = false;
static bool usb_autoshoot_active = false;
static bool usb_autoshoot_fire_was_pressed = false;
static absolute_time_t usb_autoshoot_last_toggle;
static absolute_time_t usb_autoshoot_press_start;

static inline bool gpio_active_low_pressed_from_levels(uint32_t levels,
                                                       uint pin) {
  return (((levels >> pin) & 1u) == 0u);
}

static uint8_t joystick_clamp_autoshoot_speed(int value) {
  if (value <= 0) {
    return 0;
  }
  if (value > 10) {
    return 10;
  }
  return (uint8_t)value;
}

static uint32_t joystick_autoshoot_toggle_interval_us(uint8_t speed) {
  if (speed == 0) {
    return 0;
  }

  // 1 -> 1 shot/second, 10 -> 10 shots/second.
  // One shot is one full press+release cycle, so toggle at 2x shot rate.
  const uint32_t toggle_hz = ((uint32_t)speed) * 2u;
  return 1000000u / toggle_hz;
}

// Return +1, -1, or 0 from a quadrature transition (prev->cur), both in [0..3]
// (bit0=A, bit1=B)
static inline int8_t quad_delta(uint8_t prev, uint8_t cur) {
  static const int8_t lut[16] = {/* p<<2|c :    c=00  01  10  11 */
                                 /* p=00 */ 0,  +1, -1, 0,
                                 /* p=01 */ -1, 0,  0,  +1,
                                 /* p=10 */ +1, 0,  0,  -1,
                                 /* p=11 */ 0,  -1, +1, 0};
  int8_t delta = lut[((prev & 3) << 2) | (cur & 3)];
  return delta;
}

#ifndef JOYSTICK_EDGE_RING_SIZE
#define JOYSTICK_EDGE_RING_SIZE 1024u
#endif

#if JOYSTICK_EDGE_RING_SIZE < 2
#error "JOYSTICK_EDGE_RING_SIZE must be at least 2"
#endif

typedef struct {
  uint8_t xa;
  uint8_t xb;
  uint8_t ya;
  uint8_t yb;
  uint8_t left_btn;
  uint8_t right_btn;
} joystick_edge_sample_t;

static joystick_edge_sample_t edge_ring[JOYSTICK_EDGE_RING_SIZE];
static uint32_t edge_ring_head = 0;
static uint32_t edge_ring_tail = 0;

static inline uint32_t edge_ring_next_index(uint32_t idx) {
#if (JOYSTICK_EDGE_RING_SIZE & (JOYSTICK_EDGE_RING_SIZE - 1u)) == 0
  return (idx + 1u) & (JOYSTICK_EDGE_RING_SIZE - 1u);
#else
  idx++;
  return (idx >= JOYSTICK_EDGE_RING_SIZE) ? 0u : idx;
#endif
}

static inline void edge_ring_push(joystick_edge_sample_t sample) {
  uint32_t next = edge_ring_next_index(edge_ring_head);

  // Full ring: drop the oldest sample to keep newest timing information.
  if (next == edge_ring_tail) {
    edge_ring_tail = edge_ring_next_index(edge_ring_tail);
  }

  edge_ring[edge_ring_head] = sample;
  edge_ring_head = next;
}

static inline bool edge_ring_pop(joystick_edge_sample_t* sample) {
  if (edge_ring_tail == edge_ring_head) return false;
  *sample = edge_ring[edge_ring_tail];
  edge_ring_tail = edge_ring_next_index(edge_ring_tail);
  return true;
}

void __not_in_flash_func(joystick_read_edges)(void) {
  // bits: 0=xa 1=xb 2=ya 3=yb 4=left_btn 5=right_btn
  static uint8_t prev_packed = 0xFFu;  // impossible value for first sample

  uint32_t levels = gpio_get_all();
  uint8_t xa = (uint8_t)(((levels >> MOUSE_X_A_PIN) & 1u) ^ 1u);
  uint8_t xb = (uint8_t)(((levels >> MOUSE_X_B_PIN) & 1u) ^ 1u);
  uint8_t ya = (uint8_t)(((levels >> MOUSE_Y_A_PIN) & 1u) ^ 1u);
  uint8_t yb = (uint8_t)(((levels >> MOUSE_Y_B_PIN) & 1u) ^ 1u);
  uint8_t left_btn = (uint8_t)(((levels >> MOUSE_BTN_L_PIN) & 1u) ^ 1u);
  uint8_t right_btn =
      (uint8_t)((((levels >> MOUSE_BTN_R_PIN) & 1u) == 0u)
#if defined(MOUSE_BTN_R_PIN_ALT)
                || (((levels >> MOUSE_BTN_R_PIN_ALT) & 1u) == 0u)
#endif
      );

  uint8_t packed = (uint8_t)(xa | (xb << 1u) | (ya << 2u) | (yb << 3u) |
                             (left_btn << 4u) | (right_btn << 5u));

  if (packed == prev_packed) return;
  prev_packed = packed;

  joystick_edge_sample_t sample = {.xa = xa,
                                   .xb = xb,
                                   .ya = ya,
                                   .yb = yb,
                                   .left_btn = left_btn,
                                   .right_btn = right_btn};

  edge_ring_push(sample);
}

void __not_in_flash_func(joystick_process_mouse_edges)(void) {
  // --- static state ---
  static bool init = false;
  static uint8_t px = 0, py = 0;        // previous AB states (bit0=A, bit1=B)
  static int prev_sx = 0, prev_sy = 0;  // for light smoothing
  static uint32_t invalid_x_count = 0;
  static uint32_t invalid_y_count = 0;

  // Map "edges per sample" -> mouse_set_speed units.
  enum {
    BASE_X1 = 50,   // speed units for |edges| == 1 on X
    BASE_X2 = 127,  // saturated speed for |edges| >= 2 on X
    BASE_Y1 = 50,   // speed units for |edges| == 1 on Y
    BASE_Y2 = 100,  // speed units for |edges| == 2 on Y
    STEP_X_AFTER_2 = 8,
    STEP_Y_AFTER_2 = 8
  };
  // 0 = no smoothing.
  enum { SMOOTH_SHIFT = 0 };

  joystick_edge_sample_t sample;
  int sum_dx_edges = 0;
  int sum_dy_edges = 0;
  while (edge_ring_pop(&sample)) {
    // Update left/right buttons (active low like joystick fire inputs)
    fire_state = (fire_state & 0xfd) | (sample.left_btn ? 2 : 0);
    fire_state = (fire_state & 0xfe) | (sample.right_btn ? 1 : 0);

    uint8_t cx = (sample.xa) | (sample.xb << 1);
    uint8_t cy = (sample.ya) | (sample.yb << 1);

    // Init: first sample seeds phase state without creating fake edges.
    if (!init) {
      px = cx;
      py = cy;
      init = true;
      continue;
    }

    // Decode +1/-1/0 edges per sample
    int8_t dx_edges = quad_delta(px, cx);
    int8_t dy_edges = quad_delta(py, cy);

    bool x_changed = (px != cx);
    bool y_changed = (py != cy);

    // Impossible transition: both phase bits changed at once.
    // Drop movement for this sample and resync to current phase.
    if (x_changed && dx_edges == 0) {
      invalid_x_count++;
      px = cx;
    } else {
      sum_dx_edges += dx_edges;
      px = cx;
    }

    if (y_changed && dy_edges == 0) {
      invalid_y_count++;
      py = cy;
    } else {
      sum_dy_edges += dy_edges;
      py = cy;
    }
  }

  // Convert decoded edges to capped speed.
  int sx = 0;
  int ax = (sum_dx_edges >= 0) ? sum_dx_edges : -sum_dx_edges;
  if (ax == 1) {
    sx = (sum_dx_edges > 0) ? BASE_X1 : -BASE_X1;
  } else if (ax >= 2) {
    int mag = BASE_X2 + ((ax - 2) * STEP_X_AFTER_2);
    if (mag > 127) mag = 127;
    sx = (sum_dx_edges > 0) ? mag : -mag;
  }

  int sy = 0;
  int ay = (sum_dy_edges >= 0) ? sum_dy_edges : -sum_dy_edges;
  if (ay == 1) {
    sy = (sum_dy_edges > 0) ? BASE_Y1 : -BASE_Y1;
  } else if (ay >= 2) {
    int mag = BASE_Y2 + ((ay - 2) * STEP_Y_AFTER_2);
    if (mag > 127) mag = 127;
    sy = (sum_dy_edges > 0) ? mag : -mag;
  }

  // Light smoothing to avoid jitter / stutter.
  sx = (prev_sx * ((1 << SMOOTH_SHIFT) - 1) + sx) >> SMOOTH_SHIFT;
  sy = (prev_sy * ((1 << SMOOTH_SHIFT) - 1) + sy) >> SMOOTH_SHIFT;
  prev_sx = sx;
  prev_sy = sy;

  // Clamp to expected range.
  if (sx > 127) sx = 127;
  if (sx < -127) sx = -127;
  if (sy > 127) sy = 127;
  if (sy < -127) sy = -127;

  // Send speed every process tick.
  mouse_set_speed_native(sx, sy);
}

void joystick_init_usb(bool enabled, int8_t port) {
  usb_joystick_enabled = enabled;
  usb_joystick_port = port;
}

void joystick_set_autoshoot(bool enabled, int speed) {
  usb_autoshoot_enabled = enabled;
  usb_autoshoot_speed = joystick_clamp_autoshoot_speed(speed);
  usb_autoshoot_toggle_interval_us =
      joystick_autoshoot_toggle_interval_us(usb_autoshoot_speed);
  usb_autoshoot_phase = false;
  usb_autoshoot_active = false;
  usb_autoshoot_fire_was_pressed = false;
  usb_autoshoot_last_toggle = get_absolute_time();
  usb_autoshoot_press_start = get_absolute_time();
}

void joystick_init() {
  // Initialize joystick GPIOs here if needed
  JOY_GPIO_INIT(JOY0_UP);
  JOY_GPIO_INIT(JOY0_DOWN);
  JOY_GPIO_INIT(JOY0_LEFT);
  JOY_GPIO_INIT(JOY0_RIGHT);
  JOY_GPIO_INIT(MOUSE_BTN_L_PIN);
  JOY_GPIO_INIT(JOY1_UP);
  JOY_GPIO_INIT(JOY1_DOWN);
  JOY_GPIO_INIT(JOY1_LEFT);
  JOY_GPIO_INIT(JOY1_RIGHT);
  JOY_GPIO_INIT(MOUSE_BTN_R_PIN);
#if defined(MOUSE_BTN_R_PIN_ALT)
  JOY_GPIO_INIT(MOUSE_BTN_R_PIN_ALT);
#endif

  // Initial state
  axis_state = 0;
  fire_state = 0;

  DPRINTF("Joystick initialized\n");
}

void __not_in_flash_func(joystick_update)(uint8_t port) {
  uint8_t axis_tmp = 0;
  switch (port) {
    case 0: {
      uint32_t levels = gpio_get_all();
      fire_state =
          (fire_state & 0xfd) |
          (gpio_active_low_pressed_from_levels(levels, JOY0_FIRE) ? 2 : 0);
      axis_tmp |= gpio_active_low_pressed_from_levels(levels, JOY0_UP) ? 1 : 0;
      axis_tmp |=
          gpio_active_low_pressed_from_levels(levels, JOY0_DOWN) ? 2 : 0;
      axis_tmp |=
          gpio_active_low_pressed_from_levels(levels, JOY0_LEFT) ? 4 : 0;
      axis_tmp |=
          gpio_active_low_pressed_from_levels(levels, JOY0_RIGHT) ? 8 : 0;
      axis_state &= ~0xf;
      axis_state |= axis_tmp;
      break;
    }
    case 1: {
      uint32_t levels = gpio_get_all();
      bool joystick1_fire_pressed =
          gpio_active_low_pressed_from_levels(levels, JOY1_FIRE);
      bool existing_right_button = (fire_state & 0x01) != 0;
      fire_state = (fire_state & 0xfe) |
                   ((joystick1_fire_pressed || existing_right_button) ? 1 : 0);
      axis_tmp |= gpio_active_low_pressed_from_levels(levels, JOY1_UP) ? 1 : 0;
      axis_tmp |=
          gpio_active_low_pressed_from_levels(levels, JOY1_DOWN) ? 2 : 0;
      axis_tmp |=
          gpio_active_low_pressed_from_levels(levels, JOY1_LEFT) ? 4 : 0;
      axis_tmp |=
          gpio_active_low_pressed_from_levels(levels, JOY1_RIGHT) ? 8 : 0;
      axis_state &= ~(0xf << 4);
      axis_state |= (axis_tmp << 4);
      break;
    }
    case 2:  // Original Atari ST mouse on GPIOs → feed IKBD mouse
             // (handled separately in joystick_read_edges)
    case 3:  // Parse USB joystick report → feed IKBD joystick
             // (not implemented yet)
    case 4:  // Parse USB joystick report → feed IKBD joystick
             // (not implemented yet)
    default:
      return;
  }
}

void __not_in_flash_func(joystick_set_state)(uint8_t fire_state_arg,
                                             uint8_t axis_state_arg) {
  if (!usb_joystick_enabled) return;

  uint8_t usb_fire0_in = fire_state_arg & 0x02;  // Joy0 fire encoding
  uint8_t usb_axis0 = axis_state_arg & 0x0F;  // Joy0 axis nibble
  uint8_t usb_fire0_out = usb_fire0_in;

  if (usb_autoshoot_enabled) {
    bool fire_pressed = (usb_fire0_in != 0u);

    if (usb_autoshoot_speed == 0) {
      usb_fire0_out = fire_pressed ? 0x02 : 0x00;
      usb_autoshoot_phase = false;
      usb_autoshoot_active = false;
    } else {
      enum {
        USB_GAMEPAD_AUTOSHOOT_HOLD_START_US = 2000000,
      };
      absolute_time_t now = get_absolute_time();
      if (fire_pressed) {
        if (!usb_autoshoot_fire_was_pressed) {
          // Normal shot behavior starts immediately.
          usb_autoshoot_press_start = now;
          usb_autoshoot_active = false;
          usb_autoshoot_phase = false;
          usb_autoshoot_last_toggle = now;
          usb_fire0_out = 0x02;
        } else if (!usb_autoshoot_active) {
          if (absolute_time_diff_us(usb_autoshoot_press_start, now) >=
              USB_GAMEPAD_AUTOSHOOT_HOLD_START_US) {
            usb_autoshoot_active = true;
            usb_autoshoot_phase = true;
            usb_autoshoot_last_toggle = now;
          }
          // Keep normal pressed behavior before 2s.
          usb_fire0_out = 0x02;
        } else {
          if (absolute_time_diff_us(usb_autoshoot_last_toggle, now) >=
              (int64_t)usb_autoshoot_toggle_interval_us) {
            usb_autoshoot_phase = !usb_autoshoot_phase;
            usb_autoshoot_last_toggle = now;
          }

          usb_fire0_out = usb_autoshoot_phase ? 0x02 : 0x00;
        }
      } else {
        usb_autoshoot_active = false;
        usb_autoshoot_phase = false;
        usb_autoshoot_press_start = now;
        usb_fire0_out = 0x00;
      }
    }

    usb_autoshoot_fire_was_pressed = fire_pressed;
  }

  if (usb_joystick_port == 1) {
    // Map to joystick port 1 (upper nibble + fire bit0), preserve port 0 bits.
    fire_state = (fire_state & 0x02) | (usb_fire0_out >> 1);
    axis_state = (axis_state & 0x0F) | (uint8_t)(usb_axis0 << 4);
  } else {
    // Map to joystick port 0 (lower nibble + fire bit1), preserve port 1 bits.
    fire_state = (fire_state & 0x01) | usb_fire0_out;
    axis_state = (axis_state & 0xF0) | usb_axis0;
  }
}

void __not_in_flash_func(joystick_get_state)(uint8_t* fire_state_arg,
                                             uint8_t* axis_state_arg) {
  *fire_state_arg = fire_state;
  *axis_state_arg = axis_state;
}
