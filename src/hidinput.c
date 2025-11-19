#include "hidinput.h"

// Atari ST key matrix indices for modifier keys
#define ATARI_LSHIFT 42
#define ATARI_RSHIFT 54
#define ATARI_ALT 56
#define ATARI_CTRL 29

_Atomic int16_t pend_dx = 0;
_Atomic int16_t pend_dy = 0;

#define GET_I32_VALUE(item)                                        \
  (int32_t)(item->Value |                                          \
            ((item->Value & (1 << (item->Attributes.BitSize - 1))) \
                 ? ~((1 << item->Attributes.BitSize) - 1)          \
                 : 0))

static int mouse_state = 0;
static uint8_t mouse_buttons_hid = 0;
static uint8_t joystick_fire_mask = 0;

// ---- HID interface info ring implementation ----
#ifndef HID_IF_RING_CAP
#define HID_IF_RING_CAP 16
#endif

typedef struct {
  uint8_t dev_addr;
  uint8_t instance;
} hid_if_entry_t;

static hid_if_entry_t s_hid_if_ring[HID_IF_RING_CAP];
static int s_hid_if_head = 0;  // next write position
static int s_hid_if_tail = 0;  // oldest entry
static int s_hid_if_count = 0;

void hidinput_if_ring_init(void) {
  s_hid_if_head = 0;
  s_hid_if_tail = 0;
  s_hid_if_count = 0;
}

bool hidinput_if_ring_push(uint8_t dev_addr, uint8_t instance) {
  s_hid_if_ring[s_hid_if_head].dev_addr = dev_addr;
  s_hid_if_ring[s_hid_if_head].instance = instance;
  s_hid_if_head = (s_hid_if_head + 1) % HID_IF_RING_CAP;
  if (s_hid_if_count == HID_IF_RING_CAP) {
    // overwrite oldest
    s_hid_if_tail = (s_hid_if_tail + 1) % HID_IF_RING_CAP;
  } else {
    s_hid_if_count++;
  }
  return true;
}

bool hidinput_if_ring_pop(uint8_t* dev_addr, uint8_t* instance) {
  if (s_hid_if_count == 0) return false;
  hid_if_entry_t* e = &s_hid_if_ring[s_hid_if_tail];
  if (dev_addr) *dev_addr = e->dev_addr;
  if (instance) *instance = e->instance;
  s_hid_if_tail = (s_hid_if_tail + 1) % HID_IF_RING_CAP;
  s_hid_if_count--;
  return true;
}

int hidinput_if_ring_size(void) { return s_hid_if_count; }

bool hidinput_if_ring_peek(int index_from_oldest, uint8_t* dev_addr,
                           uint8_t* instance) {
  if (index_from_oldest < 0 || index_from_oldest >= s_hid_if_count)
    return false;
  int pos = (s_hid_if_tail + index_from_oldest) % HID_IF_RING_CAP;
  if (dev_addr) *dev_addr = s_hid_if_ring[pos].dev_addr;
  if (instance) *instance = s_hid_if_ring[pos].instance;
  return true;
}

// Callback for when string descriptor is received
void tuh_descriptor_get_string_complete_cb(tuh_xfer_t* xfer) {
  if (xfer->result == XFER_RESULT_SUCCESS) {
    uint8_t* desc_string = (uint8_t*)xfer->buffer;
    uint16_t language_id = xfer->setup->wIndex;
    uint8_t dev_addr =
        (uint8_t)(xfer->user_data);  // Get device address from user_data

    if (language_id == 0x0409) {  // English language ID
      DPRINTF("");

      // String descriptor format:
      // [0] = length, [1] = descriptor type (0x03), [2..] = UTF-16LE string
      uint8_t desc_len = desc_string[0];

      // Process UTF-16LE string (skip first 2 bytes of descriptor header)
      for (int i = 2; i < desc_len && i < xfer->actual_len; i += 2) {
        // Check if we have both bytes of the UTF-16 character
        if (i + 1 < desc_len) {
          uint16_t unicode_char = (desc_string[i + 1] << 8) | desc_string[i];

          if (unicode_char == 0) break;  // End of string

          // Simple conversion: if it's ASCII, print it
          if (unicode_char < 128 && unicode_char >= 32) {
            DPRINTFRAW("%c", (char)unicode_char);
          } else if (unicode_char >= 128) {
            DPRINTFRAW("?");  // Non-ASCII character
          }
        }
      }
      DPRINTFRAW("\r\n");
    }
  } else {
    DPRINTF(
        "String descriptor request failed with result: %d, actual_len: %u\n",
        xfer->result, xfer->actual_len);
  }
}

// Callback when device descriptor retrieval completes
void hidinput_device_descriptor_complete_cb(tuh_xfer_t* xfer) {
  DPRINTF("Device descriptor received for device %u\r\n", xfer->daddr);
  if (xfer->result == XFER_RESULT_SUCCESS) {
    tusb_desc_device_t* desc = (tusb_desc_device_t*)xfer->buffer;
    uint8_t daddr = xfer->daddr;

    DPRINTF("Device %u: ID %04x:%04x\n", daddr, desc->idVendor,
            desc->idProduct);
    DPRINTF("  iManufacturer = %u\n", desc->iManufacturer);
    DPRINTF("  iProduct      = %u\n", desc->iProduct);
    DPRINTF("  iSerialNumber = %u\n", desc->iSerialNumber);
    DPRINTF("  idVendor      = 0x%04X\n", desc->idVendor);
    DPRINTF("  idProduct     = 0x%04X\n", desc->idProduct);

    // Request strings if available
#define LANGUAGE_ID 0x0409
    if (desc->iManufacturer > 0) {
      static uint8_t buf_man[128];
      tuh_descriptor_get_string(
          daddr, desc->iManufacturer, LANGUAGE_ID, buf_man, sizeof(buf_man),
          tuh_descriptor_get_string_complete_cb, (uintptr_t)daddr);
    }
    if (desc->iProduct > 0) {
      static uint8_t buf_prod[128];
      tuh_descriptor_get_string(
          daddr, desc->iProduct, LANGUAGE_ID, buf_prod, sizeof(buf_prod),
          tuh_descriptor_get_string_complete_cb, (uintptr_t)daddr);
    }
  } else {
    DPRINTF("Device descriptor request failed for device %u, result = %d\n",
            xfer->daddr, xfer->result);
  }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const* report_desc, uint16_t desc_len) {
  DPRINTF("HID device mounted: addr=%d (instance=%d)\r\n", dev_addr, instance);

  // Interface protocol (hid_interface_protocol_enum_t)
  const char* protocol_str[] = {"None", "Keyboard", "Mouse"};
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  DPRINTF("HID Interface Protocol = %s\r\n", protocol_str[itf_protocol]);

  // Store this interface id (addr+instance) into the ring for external
  // consumers
  (void)hidinput_if_ring_push(dev_addr, instance);
  // Start receiving reports
  if (!tuh_hid_receive_report(dev_addr, instance)) {
    DPRINTF("Failed to start receiving reports\r\n");
  }

  // Get device usage info using the correct function
  // uint8_t const* report_descriptor = report_desc;
  // uint16_t report_length = desc_len;
  // static uint8_t dev_desc_buf[18] __attribute__((aligned(4)));
  // bool ok =
  //     tuh_descriptor_get_device(dev_addr, dev_desc_buf, sizeof(dev_desc_buf),
  //                               device_descriptor_complete_cb,
  //                               /* user_data */ 0);
  // if (!ok) {
  //   DPRINTF("Failed to queue device descriptor request for device %u\n",
  //           dev_addr);
  // }
}

void tuh_hid_unmount_cb(uint8_t dev_addr, uint8_t instance) {
  DPRINTF("A device (address %d) is unmounted. Index: %d\r\n", dev_addr,
          instance);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                const uint8_t* report, uint16_t len) {
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  switch (itf_protocol) {
    case HID_ITF_PROTOCOL_KEYBOARD: {
      // previous report to check key released
      static hid_keyboard_report_t prev_kbd_report = {0, 0, {0}};
      hid_keyboard_report_t const* cur = (hid_keyboard_report_t const*)report;

      // If report length matches and contents are identical, skip work
      if (len >= sizeof(hid_keyboard_report_t) &&
          memcmp(cur, &prev_kbd_report, sizeof(hid_keyboard_report_t)) == 0) {
        break;  // nothing changed
      }

      // Clear keys that were in the previous report but not in the current
      for (int i = 0; i < 6; i++) {
        uint8_t prev_code = prev_kbd_report.keycode[i];
        if (!prev_code) continue;
        bool still_pressed = false;
        for (int j = 0; j < 6; j++) {
          if (cur->keycode[j] == prev_code) {
            still_pressed = true;
            break;
          }
        }
        if (!still_pressed) {
          uint8_t st = (prev_code < 128) ? stkeys_lookup_hid_gb[prev_code] : 0;
          if (st) key_states[st] = 0;
        }
      }

      // Set keys that are now pressed in the current report
      for (int i = 0; i < 6; i++) {
        uint8_t cur_code = cur->keycode[i];
        if (!cur_code) continue;
        uint8_t st = (cur_code < 128) ? stkeys_lookup_hid_gb[cur_code] : 0;
        if (st) key_states[st] = 1;
      }

      // Handle modifier keys (set every time)
      key_states[ATARI_LSHIFT] =
          (cur->modifier & KEYBOARD_MODIFIER_LEFTSHIFT) ? 1 : 0;
      key_states[ATARI_RSHIFT] =
          (cur->modifier & KEYBOARD_MODIFIER_RIGHTSHIFT) ? 1 : 0;
      key_states[ATARI_CTRL] = ((cur->modifier & KEYBOARD_MODIFIER_LEFTCTRL) ||
                                (cur->modifier & KEYBOARD_MODIFIER_RIGHTCTRL))
                                   ? 1
                                   : 0;
      key_states[ATARI_ALT] = ((cur->modifier & KEYBOARD_MODIFIER_LEFTALT) ||
                               (cur->modifier & KEYBOARD_MODIFIER_RIGHTALT))
                                  ? 1
                                  : 0;

      // Save as previous for next time
      if (len >= sizeof(hid_keyboard_report_t)) {
        prev_kbd_report = *cur;
      } else {
        // Fallback: clear prev if length unexpected
        memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
      }
      break;
    }
    case HID_ITF_PROTOCOL_MOUSE: {
      hid_mouse_report_t const* cur = (hid_mouse_report_t const*)report;
      bool left = (cur->buttons & MOUSE_BUTTON_LEFT) != 0;
      bool right = (cur->buttons & MOUSE_BUTTON_RIGHT) != 0;
      hidinput_update_mouse((int16_t)cur->x, (int16_t)cur->y, left, right);
      break;
    }

    default: {
      const uint8_t* r = report;
      uint16_t n = len;

      // Debug dump
      // DPRINTF("JOY rpt len=%u : ", n);
      // for (uint16_t i = 0; i < n; i++) DPRINTFRAW("%02x ", r[i]);
      // DPRINTFRAW(": ");

      if (n >= 7) {
        // Simple mapping for standard 7-byte joystick report
        // Byte 0: buttons
        // Byte 1: X low
        // Byte 2: Y low
        // Byte 3: X high
        // Byte 4: Y high
        // Byte 5: Hat switch
        // Byte 6: Reserved

        int16_t x = 0, y = 0;
        if (r[3] != 0x7F) {
          x = (int16_t)((r[3] << 8) | r[1]);
        }
        if (r[4] != 0x7F) {
          y = (int16_t)((r[4] << 8) | r[2]);
        }

        // Read buttons starting in byte 5
        uint8_t buttons = r[5] & (0xFF - 0x0F);  // mask out hat switch
        buttons |= r[0] & 0xFE;  // keep other buttons from byte 0
        buttons |= r[6];

        uint8_t fire_state = buttons ? 0x02 : 0;  // Joy 0 fire on bit 1
        uint8_t axis_state = 0;
        // Simple axis mapping: up/down/left/right based on sign of x/y
        if (y > 0)
          axis_state |= 0x01;  // up
        else if (y < 0)
          axis_state |= 0x02;  // down
        if (x > 0)
          axis_state |= 0x04;  // left
        else if (x < 0)
          axis_state |= 0x08;  // right

        // Axis state for joystick port 0 is in the lower nibble
        // Axis state for joystick port 1 is in the upper nibble
        joystick_set_state(fire_state, axis_state);

        // DPRINTF(" - Joystick report: x=%d y=%d btns=0x%02x\n", x, y,
        // buttons);
      }
    }
  }

  // continue to request to receive report
  if (!tuh_hid_receive_report(dev_addr, instance)) {
    DPRINTF("Error: cannot request to receive report\r\n");
  }
}

unsigned char st_keydown(const unsigned char code) {
  if (code > 0 && code < 128) {
    // if (key_states[code] && code) {
    //   DPRINTF("st_keydown: code=%u state=%u\r\n", code, key_states[code]);
    // }
    return key_states[code];
  }
  return 0;
}

int st_mouse_buttons() {
  int other_bits = mouse_state & ~0x03;
  return other_bits | mouse_buttons_hid | joystick_fire_mask;
}

unsigned char st_joystick() {
  uint8_t fire_state;
  uint8_t axis_state;
  joystick_get_state(&fire_state, &axis_state);
  uint8_t fire_bits = fire_state & 0x03;
  joystick_fire_mask |= fire_bits;
  if ((fire_bits & 0x01) == 0) {
    joystick_fire_mask &= (uint8_t)~0x01;
  }
  if ((fire_bits & 0x02) == 0) {
    joystick_fire_mask &= (uint8_t)~0x02;
  }

  int other_bits = mouse_state & ~0x03;
  mouse_state = other_bits | mouse_buttons_hid | joystick_fire_mask;
  return axis_state;
}

int st_mouse_enabled() {
  return 1;  // always enabled for now
}

void hidinput_update_mouse(int16_t dx, int16_t dy, bool left_down,
                           bool right_down) {
  int new_btns = 0;
  if (right_down) new_btns |= 0x01;
  if (left_down) new_btns |= 0x02;

  int other_bits = mouse_state & ~0x03;
  mouse_buttons_hid = (uint8_t)(new_btns & 0x03);
  mouse_state = other_bits | mouse_buttons_hid | joystick_fire_mask;

  mouse_set_speed(dx, dy);
}
