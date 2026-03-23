/*
 * Zeos — Kernel print (dual output)
 */

#include "kprint.h"
#include "fb.h"
#include "serial.h"

static int serial_ready;

void kprint_init(void)
{
    serial_ready = 1;
}

void kputc(char c)
{
    fb_putc(c);
    if (serial_ready)
        serial_putc(c);
}

void kputs(const char *s)
{
    while (*s) {
        fb_putc(*s);
        if (serial_ready)
            serial_putc(*s);
        s++;
    }
}

void kput_hex(uint64_t val)
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
    kputs(p);
}

void kput_dec(uint64_t val)
{
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (val == 0) {
        kputc('0');
        return;
    }
    while (val > 0) {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    }
    kputs(&buf[i]);
}
