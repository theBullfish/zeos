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

/* Non-blocking RX poll. Returns 1 + char if a byte is queued, 0 otherwise. */
int  serial_try_getc(char *out);

#endif /* ZEOS_SERIAL_H */
