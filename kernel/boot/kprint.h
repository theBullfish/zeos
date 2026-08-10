/*
 * Zeos — Kernel print (dual output)
 *
 * Writes to both framebuffer and serial simultaneously.
 * Use these instead of fb_puts/serial_puts for all kernel messages.
 */

#ifndef ZEOS_KPRINT_H
#define ZEOS_KPRINT_H

#include <stdint.h>

/* Initialize dual output (call after fb_init and serial_init) */
void kprint_init(void);

/*
 * Splash mode: when on, fb writes are suppressed and only serial
 * receives output. Set by splash_init(), cleared by splash_dismiss().
 */
void kprint_set_splash_mode(int on);

/* Print to both fb and serial */
void kputc(char c);
void kputs(const char *s);
void kput_hex(uint64_t val);
void kput_dec(uint64_t val);

/* ── Terminal console ring ────────────────────────────────────────────
 * Captures shell I/O so the desktop Terminal window can render the LIVE shell
 * session instead of a static mockup. Only output printed while the shell-gate
 * is set (term_console_shell, bracketed around the prompt/echo/dispatch) is
 * captured, so the window shows the shell session with zero kernel-log spam;
 * full output still goes to serial. Capture master switch is on by default. */
#define TERM_CONSOLE_ROWS 64
#define TERM_CONSOLE_COLS 128
void        term_console_capture(int on);  /* enable/disable the tee */
void        term_console_shell(int on);     /* bracket shell I/O: on=capture the prompt/echo/output that follows */
int         term_console_cur_row(void);     /* ring index of the newest row */
const char *term_console_row(int idx);      /* NUL-terminated text of ring row idx */

/* Emit a wall-clock + TSC delta prefix of the form
 *   "[14:23:07.123 +N us] "
 * suitable for log lines. If tod_init hasn't run, falls back to
 *   "[+N us] "
 * where N is microseconds since boot. Cheap; safe to call from any
 * subsystem after timer_init. */
void kprint_log_prefix(void);

#endif /* ZEOS_KPRINT_H */
