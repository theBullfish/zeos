/*
 * Zeos — Serial console (UART 16550)
 *
 * Debug output channel that works after ExitBootServices.
 * Outputs to COM1 (0x3F8) by default.
 */

#ifndef ZEOS_SERIAL_H
#define ZEOS_SERIAL_H

#include <stdint.h>

/* Initialize COM1 at 115200 baud */
void serial_init(void);

/* Write a single character */
void serial_putc(char c);

/* Write a null-terminated string */
void serial_puts(const char *s);

/* Write a hex value */
void serial_put_hex(uint64_t val);

/* Write a decimal value */
void serial_put_dec(uint64_t val);

/* Non-blocking RX poll (legacy direct-from-UART path). */
int  serial_try_getc(char *out);

/* Wire up COM1 (IRQ4) ISR. Must be called after idt_init() / pic_init(). */
void serial_irq_init(void);

/* Chain-side RX ring API. serial_rx_pending() also tops up the ring
 * from the UART RX FIFO so the polling path keeps working when the
 * IRQ never fires. */
int      serial_rx_pending(void);
int      serial_rx_pop(uint8_t *out);
uint32_t serial_rx_dropped(void);

#endif /* ZEOS_SERIAL_H */
