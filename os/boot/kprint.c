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
#include "spinlock.h"

static int serial_ready;
static int splash_mode;   /* 1 = serial-only, 0 = dual */
static uint64_t s_first_tsc;  /* TSC at first kprint_log_prefix call -- "boot" anchor */

/* IRQ-safe lock: kputs may be called from the LAPIC timer ISR (preempt
 * path) on the same CPU that's currently mid-write. spin_lock_irqsave
 * disables IF for the critical section so the ISR can't be re-entered
 * while we hold the lock. */
static zeos_spinlock_t g_kprint_lock = ZEOS_SPINLOCK_INIT;

/* ── Terminal console ring (see kprint.h) ──────────────────────────────
 * Captures output for the desktop Terminal window. Written under
 * g_kprint_lock (inside kputc/kputs). Kernel-log lines beginning with '['
 * are suppressed so the window shows the shell session, not debug spam. */
static char g_term[TERM_CONSOLE_ROWS][TERM_CONSOLE_COLS + 1];
static int  g_term_row = 0;
static int  g_term_col = 0;
static int  g_term_on  = 0;
static int  g_term_shell = 0;      /* >0 while shell I/O is being printed */

void term_console_capture(int on) { g_term_on = on ? 1 : 0; }
void term_console_shell(int on)   { g_term_shell += on ? 1 : -1;
                                    if (g_term_shell < 0) g_term_shell = 0; }
int  term_console_cur_row(void)   { return g_term_row; }
const char *term_console_row(int idx)
{
    if (idx < 0 || idx >= TERM_CONSOLE_ROWS) return "";
    return g_term[idx];
}

/* Append one char to the ring. Caller holds g_kprint_lock. Captures ONLY shell
 * I/O (g_term_shell set around the shell prompt/echo/dispatch), so the window
 * shows the live shell session with zero kernel-log spam -- no fragile filter,
 * no shared-kputs interference. Serial still gets everything. */
static void term_console_putc(char c)
{
    if (!g_term_on || !g_term_shell || c == '\r') return;
    if (c == '\n') {
        g_term_row = (g_term_row + 1) % TERM_CONSOLE_ROWS;
        g_term_col = 0;
        g_term[g_term_row][0] = '\0';
        return;
    }
    if (g_term_col >= TERM_CONSOLE_COLS) {
        g_term_row = (g_term_row + 1) % TERM_CONSOLE_ROWS;
        g_term_col = 0;
        g_term[g_term_row][0] = '\0';
    }
    g_term[g_term_row][g_term_col++] = c;
    g_term[g_term_row][g_term_col] = '\0';
}

void kprint_init(void)
{
    serial_ready = 1;
    splash_mode = 0;
    g_term_on = 1;   /* capture the console for the Terminal window */
}

void kprint_set_splash_mode(int on)
{
    splash_mode = on ? 1 : 0;
}

void kputc(char c)
{
    uint64_t f = spin_lock_irqsave(&g_kprint_lock);
    if (!splash_mode)
        fb_putc(c);
    if (serial_ready)
        serial_putc(c);
    term_console_putc(c);
    spin_unlock_irqrestore(&g_kprint_lock, f);
}

void kputs(const char *s)
{
    uint64_t f = spin_lock_irqsave(&g_kprint_lock);
    while (*s) {
        if (!splash_mode)
            fb_putc(*s);
        if (serial_ready)
            serial_putc(*s);
        term_console_putc(*s);
        s++;
    }
    spin_unlock_irqrestore(&g_kprint_lock, f);
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
