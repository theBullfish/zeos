/*
 * Zeos framebuffer console — bare metal text output.
 * No UEFI, no firmware. Just pixels.
 */

#ifndef ZEOS_FB_H
#define ZEOS_FB_H

#include "zeos_boot.h"

/* Initialize framebuffer console from boot info */
void fb_init(struct zeos_framebuffer *fb);

/* Clear screen to a 32-bit ARGB color */
void fb_clear(uint32_t color);

/* Write a single character at the current cursor position */
void fb_putc(char c);

/* Write a null-terminated string */
void fb_puts(const char *s);

/* Print a 64-bit value as hex */
void fb_put_hex(uint64_t val);

/* Print a 64-bit value as decimal */
void fb_put_dec(uint64_t val);

#endif /* ZEOS_FB_H */
