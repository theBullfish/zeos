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

/* Emit a wall-clock + TSC delta prefix of the form
 *   "[14:23:07.123 +N us] "
 * suitable for log lines. If tod_init hasn't run, falls back to
 *   "[+N us] "
 * where N is microseconds since boot. Cheap; safe to call from any
 * subsystem after timer_init. */
void kprint_log_prefix(void);

#endif /* ZEOS_KPRINT_H */
