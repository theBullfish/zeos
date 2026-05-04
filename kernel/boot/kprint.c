/*
 * Zeos — Kernel print (dual output)
 *
 * In normal mode, kputs/kputc write to BOTH the framebuffer console
 * and the serial port. In splash mode (set by splash_init), fb output
 * is suppressed so the splash stays clean; serial output is unchanged
 * so the boot log is fully captured for debug.
 */

#include "kprint.h"
#include "fb.h"
#include "serial.h"
#include "timer.h"
#include "timeofday.h"

static int serial_ready;
static int splash_mode;   /* 1 = serial-only, 0 = dual */
static uint64_t s_first_tsc;  /* TSC at first kprint_log_prefix call -- "boot" anchor */

void kprint_init(void)
{
    serial_ready = 1;
    splash_mode = 0;
}

void kprint_set_splash_mode(int on)
{
    splash_mode = on ? 1 : 0;
}

void kputc(char c)
{
    if (!splash_mode)
        fb_putc(c);
    if (serial_ready)
        serial_putc(c);
}

void kputs(const char *s)
{
    while (*s) {
        if (!splash_mode)
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

void kprint_log_prefix(void)
{
    uint64_t now = timer_read_tsc();
    if (s_first_tsc == 0) s_first_tsc = now;

    uint64_t freq = timer_tsc_freq();
    uint64_t us   = 0;
    if (freq > 0 && now >= s_first_tsc) {
        /* freq is Hz; us = delta * 1e6 / freq. Avoid 128-bit math by
         * dividing freq down to MHz first; loses sub-us precision but
         * a log prefix doesn't need it. */
        uint64_t mhz = freq / 1000000ULL;
        if (mhz == 0) mhz = 1;
        us = (now - s_first_tsc) / mhz;
    }

    char tbuf[16];
    int n = tod_format_log(tbuf, sizeof(tbuf));
    if (n > 0 && tod_ready()) {
        /* "[HH:MM:SS.mmm +N us] " */
        tbuf[n - 1] = '\0';     /* strip trailing ']' */
        kputs(tbuf);
        kputs(" +");
        kput_dec(us);
        kputs(" us] ");
    } else {
        kputs("[+");
        kput_dec(us);
        kputs(" us] ");
    }
}
