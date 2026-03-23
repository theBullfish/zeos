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

/* Print to both fb and serial */
void kputc(char c);
void kputs(const char *s);
void kput_hex(uint64_t val);
void kput_dec(uint64_t val);

#endif /* ZEOS_KPRINT_H */
