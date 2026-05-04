/*
 * Zeos — Serial console (16550 UART)
 *
 * COM1 at 0x3F8, 115200 baud, 8N1.
 * No interrupts — polling TX only (debug output).
 */

#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);  /* Disable interrupts */
    outb(COM1 + 3, 0x80);  /* Enable DLAB (set baud divisor) */
    outb(COM1 + 0, 0x01);  /* Divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);  /* 8 bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);  /* Enable FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B);  /* IRQs enabled, RTS/DSR set */

    /* Test: send a null byte and check loopback */
    outb(COM1 + 4, 0x1E);  /* Loopback mode */
    outb(COM1 + 0, 0xAE);  /* Send test byte */
    if (inb(COM1 + 0) != 0xAE) {
        /* Serial port doesn't exist or is broken — continue anyway */
    }
    outb(COM1 + 4, 0x0F);  /* Normal operation */
}

static int serial_tx_ready(void)
{
    return inb(COM1 + 5) & 0x20;  /* THR empty */
}

void serial_putc(char c)
{
    /* Wait for transmit buffer to be empty */
    while (!serial_tx_ready())
        ;
    outb(COM1, c);

    /* Send \r before \n for terminal compatibility */
    if (c == '\n') {
        while (!serial_tx_ready())
            ;
        outb(COM1, '\r');
    }
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

void serial_put_hex(uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    int i;
    for (i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xf];
        val >>= 4;
    }
    buf[16] = '\0';
    char *p = buf;
    while (*p == '0' && p < buf + 15)
        p++;
    serial_puts(p);
}

/* Returns 1 + char if a byte is waiting on the UART, 0 otherwise. */
int serial_try_getc(char *out)
{
    if (!(inb(COM1 + 5) & 0x01)) return 0;  /* RX data ready? */
    *out = (char)inb(COM1 + 0);
    return 1;
}

void serial_put_dec(uint64_t val)
{
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (val == 0) {
        serial_putc('0');
        return;
    }
    while (val > 0) {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    }
    serial_puts(&buf[i]);
}
