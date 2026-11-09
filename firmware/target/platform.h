#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>

struct coordinator;

void aes_hw_init(void);

/* UART0 to the host: 460800 8N1, interrupt-driven RX, buffered TX. */
void uart_init(void);
size_t uart_read(uint8_t *buf, size_t max);
void uart_flush(void); /* moves buffered TX bytes into the FIFO; call often */

/* Radio: opens the RF driver and starts continuous RX. */
void radio_init(void);
/* Delivers received frames and finished jobs to the coordinator. */
void radio_poll(struct coordinator *c);

#endif
