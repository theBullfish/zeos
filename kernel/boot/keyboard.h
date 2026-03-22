/*
 * Zeos — PS/2 keyboard driver
 *
 * Handles IRQ1, translates scan codes to ASCII,
 * feeds a character buffer for the shell.
 */

#ifndef ZEOS_KEYBOARD_H
#define ZEOS_KEYBOARD_H

#include <stdint.h>

/* Initialize keyboard (register IRQ1 handler, unmask IRQ1) */
void keyboard_init(void);

/* Get a character from the keyboard buffer (blocking) */
char keyboard_getc(void);

/* Check if a character is available (non-blocking) */
int keyboard_has_char(void);

#endif /* ZEOS_KEYBOARD_H */
