#include "timing_tests.h"

#include <stdint.h>
#include <stdio.h>  // sprintf

#include "test_runner.h"

#define IKBD_RESET_CMD1 0x80
#define IKBD_RESET_CMD2 0x01
#define IKBD_RESET_EXPECT_0 0xF1
#define IKBD_RESET_EXPECT_1 0x00

#define IKBD_BREAK_GAP_MAX_TICKS 200
#define IKBD_PACKET_GAP_MAX_TICKS 4
#define IKBD_TOD_DRIFT_SAMPLE_SECONDS 60
#define IKBD_TOD_DRIFT_MAX_TICKS 10

#define ACIA_BASE 0xFFFFFC00u
struct ACIA_INTERFACE {
  unsigned char control; /* read=status, write=control */
  unsigned char dummy1;
  unsigned char data;
  unsigned char dummy2;
};

#define acia_interface (*(volatile struct ACIA_INTERFACE*)ACIA_BASE)

// NOTE: These control values MUST match the MC6850 encoding and your machine
// setup. If these are wrong, takeover will break the IKBD stream.
#define IKBD_ACIA_CTRL_MASTER_RESET 0x03 /* typical 6850 master reset */
#define IKBD_ACIA_CTRL_POLLING 0x16      /* /64 + 8N1 + RX IRQ off */
#define IKBD_ACIA_CTRL_TOS_LIKE 0x96     /* /64 + 8N1 + RX IRQ on  */

#define ACIA_SR_RDRF (1u << 0) /* RX data register full */
#define ACIA_SR_TDRE (1u << 1) /* TX data register empty */
#define ACIA_SR_FE (1u << 4)   /* framing error */
#define ACIA_SR_OVRN (1u << 5) /* overrun */
#define ACIA_SR_PE (1u << 6)   /* parity error */

#define HZ200_ADDR 0x4BAu
static volatile uint32_t* const hz200 = (volatile uint32_t*)HZ200_ADDR;

/* stable read (prevents tearing while the longword is being updated) */
static inline uint32_t read_hz200(void) {
  uint32_t a, b;
  do {
    a = *hz200;
    b = *hz200;
  } while (a != b);
  return a;
}

static inline void acia_barrier(void) {
  (void)acia_interface.control; /* read status as a bus sync */
}

static inline int ikbd_can_read(void) {
  return (acia_interface.control & ACIA_SR_RDRF) != 0;
}

static inline int ikbd_can_write(void) {
  return (acia_interface.control & ACIA_SR_TDRE) != 0;
}

/*
 * IMPORTANT:
 * Read STATUS first, then DATA. This is the safe sequence for MC6850-style
 * ACIAs, and also lets you observe FE/OVRN/PE.
 */
static inline int ikbd_read_byte(uint8_t* out_value, uint8_t* out_status) {
  uint8_t sr = acia_interface.control; /* status */
  if ((sr & ACIA_SR_RDRF) == 0) {
    return 0;
  }

  uint8_t v = acia_interface.data; /* data */

  if (out_value) {
    *out_value = v;
  }
  if (out_status) {
    *out_status = sr;
  }

  return 1;
}

static inline void ikbd_write_byte(int value) {
  while (!ikbd_can_write()) {
    /* busy wait */
  }
  acia_interface.data = (uint8_t)value;
}

static inline void flush_ikbd(void) {
  uint8_t v, sr;
  while (ikbd_read_byte(&v, &sr)) {
    (void)v;
    (void)sr;
  }
}

static void spin(void) {
  const char spinner[] = "|/-\\";
  int spin_idx = 0;
  for (volatile int i = 0; i < 200; ++i) {
    print("%c\b", spinner[spin_idx]);
    spin_idx = (spin_idx + 1) & 3;
    for (volatile int j = 0; j < 20000; ++j) {
      __asm__ volatile("nop");
    }
  }
}

/*
 * Takeover: disable ACIA-generated IRQs so TOS stops draining the IKBD bytes.
 * Keep the rest of the system alive (HZ200 still runs).
 */
static inline void ikbd_takeover_begin(void) {
  acia_interface.control = IKBD_ACIA_CTRL_MASTER_RESET;
  acia_barrier();

  acia_interface.control = IKBD_ACIA_CTRL_POLLING;
  acia_barrier();

  flush_ikbd();
}

static inline void ikbd_takeover_end(void) {
  /* Restore ACIA so OS IRQ handler can run again */
  acia_interface.control = IKBD_ACIA_CTRL_MASTER_RESET;
  acia_barrier();

  acia_interface.control = IKBD_ACIA_CTRL_TOS_LIKE;
  acia_barrier();

  /* Drain anything left from our polling session */
  flush_ikbd();

  /* Reset IKBD so TOS resyncs cleanly (MOUSE/KEY state) */
  while (!ikbd_can_write()) {
  }
  acia_interface.data = 0x80; /* RESET command byte 1 */
  acia_barrier();

  while (!ikbd_can_write()) {
  }
  acia_interface.data = 0x01; /* RESET command byte 2 */
  acia_barrier();

  /* Optional: wait a bit and drain the reset response */
  {
    uint8_t v, sr;
    uint32_t start = read_hz200();
    while ((uint32_t)(read_hz200() - start) < 40) { /* 200ms */
      if (!ikbd_read_byte(&v, &sr)) {
        continue;
      }
      /* drop bytes; TOS will fully recover after we return */
    }
  }
}

static int wait_ikbd_byte(uint32_t timeout_ticks, uint8_t* out_value,
                          uint8_t* out_status) {
  uint32_t start = read_hz200();
  while ((uint32_t)(read_hz200() - start) < timeout_ticks) {
    uint8_t v, sr;
    if (ikbd_read_byte(&v, &sr)) {
      if (out_value) *out_value = v;
      if (out_status) *out_status = sr;
      return 1;
    }
  }
  return 0;
}

static int is_bcd_byte(int value) {
  int hi = (value >> 4) & 0x0F;
  int lo = value & 0x0F;
  return (hi <= 9) && (lo <= 9);
}

static int bcd_to_int(uint8_t value) {
  int hi = (value >> 4) & 0x0F;
  int lo = value & 0x0F;
  return (hi * 10) + lo;
}

/*
 * Robust TOD interrogate:
 * - send 0x1C
 * - resync on 0xFC header
 * - timeouts everywhere (no hard hang)
 */
static int read_time_of_day(uint8_t* out_bytes) {
  if (!out_bytes) {
    return 0;
  }

  flush_ikbd();
  ikbd_write_byte(0x1C);

  /* Find 0xFC header, ignore unrelated bytes */
  uint8_t b = 0, sr = 0;
  uint32_t start = read_hz200();
  while ((uint32_t)(read_hz200() - start) < 200) { /* 1s max */
    if (!wait_ikbd_byte(2, &b, &sr)) {             /* 10ms slices */
      continue;
    }
    if (sr & (ACIA_SR_FE | ACIA_SR_OVRN | ACIA_SR_PE)) {
      /* Drain errors; keep trying */
      continue;
    }
    if (b == 0xFC) {
      goto got_header;
    }
  }
  return 0;

got_header:
  for (int i = 0; i < 6; i++) {
    if (!wait_ikbd_byte(20, &out_bytes[i], &sr)) { /* 100ms per byte */
      return 0;
    }
    if (sr & (ACIA_SR_FE | ACIA_SR_OVRN | ACIA_SR_PE)) {
      return 0;
    }
  }

  return 1;
}

static void test_reset_response_timing(void) {
  uint8_t byte0 = 0, sr = 0;

  ikbd_takeover_begin();

  ikbd_write_byte(IKBD_RESET_CMD1);
  ikbd_write_byte(IKBD_RESET_CMD2);

  uint32_t start = read_hz200();
  int ok = wait_ikbd_byte(200, &byte0, &sr); /* 1s timeout */
  uint32_t ticks = read_hz200() - start;

  assert_result("IKBD reset response byte received", ok, 1);

  if (ok) {
    assert_result("IKBD reset response byte value correct.", byte0,
                  IKBD_RESET_EXPECT_0);
  }

  /* 60 ticks = 300ms */
  assert_result("IKBD reset response <= 300ms", ticks <= 60, 1);

  ikbd_takeover_end();
}

static void test_time_of_day_interrogate(void) {
  uint8_t tod[6] = {0};

  ikbd_takeover_begin();

  int ok = read_time_of_day(tod);
  assert_result("IKBD TOD interrogate", ok, 1);

  if (ok) {
    for (int i = 0; i < 6; ++i) {
      char label[64];
      sprintf(label, "IKBD TOD byte %d is BCD", i);
      assert_result(label, is_bcd_byte(tod[i]), 1);
    }

    /* Optional debug dump */
    print("TOD bytes: %02X %02X %02X %02X %02X %02X\r\n", tod[0], tod[1],
          tod[2], tod[3], tod[4], tod[5]);
  }

  ikbd_takeover_end();
}

static void test_time_of_day_drift(uint32_t sample_seconds) {
  {
    uint8_t tod[6] = {0};

    ikbd_takeover_begin();

    int ok = read_time_of_day(tod);
    if (!ok) {
      ikbd_takeover_end();
      assert_result("IKBD TOD initial read", 0, 1);
      return;
    }

    int prev_sec = bcd_to_int(tod[5]);
    uint32_t edges = 0;
    uint32_t tick_start = 0;
    uint32_t tick_end = 0;

    /* Global safety timeout: sample_seconds + 5 seconds */
    uint32_t global_start = read_hz200();
    uint32_t global_timeout = (sample_seconds + 5) * 200;

    while (edges < sample_seconds &&
           (uint32_t)(read_hz200() - global_start) < global_timeout) {
      ok = read_time_of_day(tod);
      if (!ok) {
        continue;
      }

      int sec = bcd_to_int(tod[5]);
      if (sec != prev_sec) {
        if (edges == 0) {
          tick_start = read_hz200();
        }
        edges++;
        prev_sec = sec;

        if (edges >= sample_seconds) {
          tick_end = read_hz200();
          break;
        }
      }
    }

    ikbd_takeover_end();

    {
      char label[64];
      sprintf(label, "IKBD TOD drift captured edges (%lus)",
              (unsigned long)sample_seconds);
      assert_result(label, (int)edges, (int)sample_seconds);
    }
    if (edges < sample_seconds) {
      return;
    }

    uint32_t ticks = tick_end - tick_start;
    uint32_t expected = sample_seconds * 200;
    long drift = (long)ticks - (long)expected;
    if (drift < 0) {
      drift = -drift;
    }

    print("IKBD TOD drift ticks (%lus): %ld\r\n", (unsigned long)sample_seconds,
          drift);
    print("IKBD TOD drift ms (%lus): %ld\r\n", (unsigned long)sample_seconds,
          drift * 5);

    assert_result("IKBD TOD drift within limit",
                  drift <= IKBD_TOD_DRIFT_MAX_TICKS, 1);
  }
}

int run_timing_tests(int presskey) {
  (void)presskey;

  print("=== IKBD Timing Test Suite ===\r\n");

  test_reset_response_timing();
  if (presskey) {
    press_key(NEWLINE);
  }

  test_time_of_day_interrogate();
  if (presskey) {
    press_key(NEWLINE);
  }

  test_time_of_day_drift((uint32_t)IKBD_TOD_DRIFT_SAMPLE_SECONDS);
  if (presskey) {
    press_key(NEWLINE);
  }

  print("=== IKBD timing tests completed ===\r\n");
  spin();
  return 0;
}
