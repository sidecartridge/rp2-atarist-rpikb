#include "joystick.h"

#include "debug.h"
#include "mouse.h"

#define JOY_GPIO_INIT(io)    \
  gpio_init(io);             \
  gpio_set_dir(io, GPIO_IN); \
  gpio_pull_up(io);

static uint8_t axis_state = 0;
static uint8_t fire_state = 0;
static bool usb_joystick_enabled = false;
static uint8_t usb_joystick_port = 0;

// Return +1, -1, or 0 from a quadrature transition (prev->cur), both in [0..3]
// (bit0=A, bit1=B)
static inline int8_t quad_delta(uint8_t prev, uint8_t cur) {
  static const int8_t lut[16] = {/* p<<2|c :    c=00  01  10  11 */
                                 /* p=00 */ 0,  +1, -1, 0,
                                 /* p=01 */ -1, 0,  0,  +1,
                                 /* p=10 */ +1, 0,  0,  -1,
                                 /* p=11 */ 0,  -1, +1, 0};
  return lut[((prev & 3) << 2) | (cur & 3)];
}

void joystick_init_usb(bool enabled, int8_t port) {
  usb_joystick_enabled = enabled;
  usb_joystick_port = port;
}

void joystick_init() {
  // Initialize joystick GPIOs here if needed
  JOY_GPIO_INIT(JOY0_UP);
  JOY_GPIO_INIT(JOY0_DOWN);
  JOY_GPIO_INIT(JOY0_LEFT);
  JOY_GPIO_INIT(JOY0_RIGHT);
  JOY_GPIO_INIT(JOY0_FIRE);
  JOY_GPIO_INIT(JOY1_UP);
  JOY_GPIO_INIT(JOY1_DOWN);
  JOY_GPIO_INIT(JOY1_LEFT);
  JOY_GPIO_INIT(JOY1_RIGHT);
  JOY_GPIO_INIT(JOY1_FIRE);

  // Initial state
  axis_state = 0;
  fire_state = 0;

  DPRINTF("Joystick initialized\n");
}

void joystick_update(uint8_t port) {
  uint8_t prev_axis = axis_state;
  uint8_t prev_fire = fire_state;
  uint8_t axis_tmp = 0;
  uint8_t button = 0;
  switch (port) {
    case 0: {
      fire_state = (fire_state & 0xfd) | (gpio_get(JOY0_FIRE) ? 0 : 2);
      axis_tmp |= (gpio_get(JOY0_UP)) ? 0 : 1;
      axis_tmp |= (gpio_get(JOY0_DOWN)) ? 0 : 2;
      axis_tmp |= (gpio_get(JOY0_LEFT)) ? 0 : 4;
      axis_tmp |= (gpio_get(JOY0_RIGHT)) ? 0 : 8;
      axis_state &= ~0xf;
      axis_state |= axis_tmp;
      break;
    }
    case 1: {
      fire_state = (fire_state & 0xfe) | (gpio_get(JOY1_FIRE) ? 0 : 1);
      axis_tmp |= (gpio_get(JOY1_UP)) ? 0 : 1;
      axis_tmp |= (gpio_get(JOY1_DOWN)) ? 0 : 2;
      axis_tmp |= (gpio_get(JOY1_LEFT)) ? 0 : 4;
      axis_tmp |= (gpio_get(JOY1_RIGHT)) ? 0 : 8;
      axis_state &= ~(0xf << 4);
      axis_state |= (axis_tmp << 4);
      break;
    }
    case 2: {  // Original Atari ST mouse on GPIOs → feed IKBD mouse
      // --- static state ---
      static bool init = false;
      static uint8_t px = 0, py = 0;  // previous AB states (bit0=A, bit1=B)
      static int prev_sx = 0, prev_sy = 0;  // for light smoothing

      // Update left/right buttons (active low like joystick fire inputs)
      fire_state = (fire_state & 0xfd) | (gpio_get(JOY0_FIRE) ? 0 : 2);
      fire_state = (fire_state & 0xfe) | (gpio_get(JOY1_FIRE) ? 0 : 1);

      // Map “edges per sample” -> mouse_set_speed units (tune to taste).
      // Use a small piecewise mapping so that 1 edge gives a stable,
      // predictable speed, and larger edge bursts are capped.
      enum {
        BASE_X1 = 50,   // speed units for |edges| == 1 on X
        BASE_X2 = 100,  // saturated speed for |edges| >= 2 on X
        BASE_Y1 = 50,   // speed units for |edges| == 1 on Y
        BASE_Y2 = 100   // saturated speed for |edges| >= 2 on Y
      };
      // Simple smoothing (0..7). 0 = no smoothing, 3 = gentle
      enum { SMOOTH_SHIFT = 0 };  // alpha = 3/4 previous + 1/4 new

      // Init: read initial phases (active-low → 1 when grounded)
      if (!init) {
        uint8_t xa = !gpio_get(MOUSE_X_A_PIN);
        uint8_t xb = !gpio_get(MOUSE_X_B_PIN);
        uint8_t ya = !gpio_get(MOUSE_Y_A_PIN);
        uint8_t yb = !gpio_get(MOUSE_Y_B_PIN);
        px = (xa) | (xb << 1);
        py = (ya) | (yb << 1);
        init = true;
      }

      // Current phases (active-low)
      uint8_t xa = !gpio_get(MOUSE_X_A_PIN);
      uint8_t xb = !gpio_get(MOUSE_X_B_PIN);
      uint8_t ya = !gpio_get(MOUSE_Y_A_PIN);
      uint8_t yb = !gpio_get(MOUSE_Y_B_PIN);
      uint8_t cx = (xa) | (xb << 1);
      uint8_t cy = (ya) | (yb << 1);

      // Decode +1/-1/0 edges per sample
      int8_t dx_edges = quad_delta(px, cx);
      int8_t dy_edges = quad_delta(py, cy);

      // Guard against clearly invalid/skipped quadrature sequences:
      // allow up to |2| edges per sample (fast but plausible), and
      // saturate anything larger to ±2 instead of dropping it. This
      // avoids bogus spikes while still preserving "fast" motion.
      if (dx_edges > 2) dx_edges = 2;
      if (dx_edges < -2) dx_edges = -2;
      if (dy_edges > 2) dy_edges = 2;
      if (dy_edges < -2) dy_edges = -2;
      px = cx;
      py = cy;

      // Convert “edges this sample” to speed units using a capped mapping.
      int sx = 0;
      int ax = dx_edges >= 0 ? dx_edges : -dx_edges;
      if (ax == 1) {
        sx = (dx_edges > 0) ? BASE_X1 : -BASE_X1;
      } else if (ax >= 2) {
        sx = (dx_edges > 0) ? BASE_X2 : -BASE_X2;
      }

      int sy = 0;
      int ay = dy_edges >= 0 ? dy_edges : -dy_edges;
      if (ay == 1) {
        sy = (dy_edges > 0) ? BASE_Y1 : -BASE_Y1;
      } else if (ay >= 2) {
        sy = (dy_edges > 0) ? BASE_Y2 : -BASE_Y2;
      }

      // Light smoothing to avoid jitter / stutter
      sx = (prev_sx * ((1 << SMOOTH_SHIFT) - 1) + sx) >> SMOOTH_SHIFT;
      sy = (prev_sy * ((1 << SMOOTH_SHIFT) - 1) + sy) >> SMOOTH_SHIFT;
      prev_sx = sx;
      prev_sy = sy;

      // Clamp to IKBD/your API expectations
      if (sx > 127) sx = 127;
      if (sx < -127) sx = -127;
      if (sy > 127) sy = 127;
      if (sy < -127) sy = -127;

      // Send speed every sample; when there are no edges, sx/sy will
      // naturally decay toward zero via smoothing (or be exactly zero
      // if SMOOTH_SHIFT == 0).
      mouse_set_speed(sx, sy);
      break;
    }
    case 3:  // Parse USB joystick report → feed IKBD joystick
             // (not implemented yet)
      break;
    case 4:  // Parse USB joystick report → feed IKBD joystick
      // (not implemented yet)
      break;
    default:
      return;
  }

  // if (axis_state != prev_axis || fire_state != prev_fire) {
  //   DPRINTF("Joystick port %u state changed: axis=0x%02x fire=0x%02x\n",
  //   port,
  //           axis_state, fire_state);
  // }
}

void joystick_set_state(uint8_t fire_state_arg, uint8_t axis_state_arg) {
  if (usb_joystick_port == 1) {
    fire_state = fire_state_arg >> 1;
    axis_state = axis_state_arg << 4;
  }
}

void joystick_get_state(uint8_t* fire_state_arg, uint8_t* axis_state_arg) {
  *fire_state_arg = fire_state;
  *axis_state_arg = axis_state;
}
