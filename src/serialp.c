#include "serialp.h"

#define UART_IRQ UART1_IRQ

// Buffer for received data
static volatile uint8_t rx_buffer[256];
static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;

void rx_buffer_put(uint8_t data) {
  uint16_t next_head = (rx_head + 1) & 0xFF;
  if (next_head != rx_tail) {  // Buffer not full
    rx_buffer[rx_head] = data;
    rx_head = next_head;
  }
}

bool rx_buffer_get(uint8_t *data) {
  if (rx_head == rx_tail) {
    return false;  // Buffer empty
  }
  *data = rx_buffer[rx_tail];
  rx_tail = (rx_tail + 1) & 0xFF;
  return true;
}

uint16_t rx_available(void) { return (rx_head - rx_tail) & 0xFF; }

// ISR for UART receive
static void on_uart_irq(void) {
  // Check if this is an RX interrupt
  if (uart_get_hw(UART_DEVICE)->mis & UART_UARTMIS_RXMIS_BITS) {
    // Clear RX interrupt
    uart_get_hw(UART_DEVICE)->icr = UART_UARTICR_RXIC_BITS;

    // Read all available data
    while (uart_is_readable(UART_DEVICE)) {
      uint8_t ch = uart_getc(UART_DEVICE);
      rx_buffer_put(ch);
      // DPRINTF("ST -> 6301 0x%02X (buffered)\n", ch);
    }
  }
}

void serialp_open(void) {
  // Initialize UART with precise baud rate configuration
  int actual = uart_init(UART_DEVICE, BAUD_RATE);
  DPRINTF("Serial port opened at %d baud, defined baud rate %d\n", actual,
          BAUD_RATE);

  gpio_set_function(UART_TX, GPIO_FUNC_UART);
  gpio_set_function(UART_RX, GPIO_FUNC_UART);

  // Set pull-up resistors on RX line if needed
  gpio_pull_up(UART_RX);

  // Disable pulls on TX line
  gpio_disable_pulls(UART_TX);

  // Set drive strength if needed
  gpio_set_drive_strength(UART_TX, GPIO_DRIVE_STRENGTH_12MA);

  // Verify and set baud rate
  // int actual = uart_set_baudrate(UART_DEVICE, BAUD_RATE);
  // DPRINTF("Serial initialized at %d baud (target: %d)\n", actual, BAUD_RATE);

  // Data format: data bits, stop bits, parity
  uart_set_format(UART_DEVICE, DATA_BITS, STOP_BITS, PARITY);

  // Disable hardware flow control
  uart_set_hw_flow(UART_DEVICE, false, false);

  // Use raw mode (no CRLF translation)
  uart_set_translate_crlf(UART_DEVICE, false);

  // Better use fifo disabled
  uart_set_fifo_enabled(UART_DEVICE, false);

  // // Reset buffer
  rx_head = 0;
  rx_tail = 0;

  // // Set up interrupt handler for UART
  irq_set_exclusive_handler(UART_IRQ, on_uart_irq);
  irq_set_priority(UART_IRQ, 0);  // 0 = highest priority

  // // Enable UART RX interrupt (but not TX)
  uart_set_irq_enables(UART_DEVICE, true, false);

  // // Enable the IRQ at NVIC level - THIS WAS MISSING!
  irq_set_enabled(UART_IRQ, true);
}

void serialp_close(void) {
  // Disable interrupts
  irq_set_enabled(UART_IRQ, false);
  uart_set_irq_enables(UART_DEVICE, false, false);
  uart_deinit(UART_DEVICE);
}

void serialp_send(const unsigned char data) {
  DPRINTF("6301 -> ST 0x%02X\n", data);
  // Write the byte and wait until the UART finishes transmitting it.
  // At 7812 baud this is safe and ensures the bit actually leaves the pin.
  uart_putc_raw(UART_DEVICE, data);
}
