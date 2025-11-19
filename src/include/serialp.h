#ifndef SERIALP_H
#define SERIALP_H

#include <stdio.h>

#include "6301/6301.h"
#include "constants.h"
#include "debug.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#define BAUD_RATE 7812
#define DATA_BITS 8
#define STOP_BITS 1
#define PARITY UART_PARITY_NONE

void serialp_open(void);
void serialp_close(void);
void serialp_send(const unsigned char data);

// RX ring buffer helpers
uint16_t rx_available(void);
bool rx_buffer_get(uint8_t *data);

#endif  // SERIALP_H