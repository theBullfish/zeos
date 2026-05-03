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

/* Inject a scancode (set 1) from an alternate input source -- e.g. a
 * USB HID boot keyboard. Behaves exactly as if the byte arrived on
 * port 0x60: routes through the keybinds layer, updates shift/caps
 * state, and pushes ASCII into the shell buffer. */
void keyboard_inject_scancode(uint8_t scancode, int extended);

#endif /* ZEOS_KEYBOARD_H */
