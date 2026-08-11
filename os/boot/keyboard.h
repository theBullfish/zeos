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

/* Inject an ASCII char straight into kb_buf -- used by sources that
 * already produce ASCII (UART serial, future telnet/SSH consoles).
 * Bypasses the scancode pipeline; modifier state is left untouched. */
void keyboard_inject_char(char c);

/* Non-blocking variant of keyboard_getc. Returns 1 + writes char if
 * one is queued, 0 otherwise. Used by the scheduler-driven shell pump. */
int  keyboard_try_getc(char *out);

/* CHAIN_KEYBOARD plumbing. */
int      keyboard_chain_pending(void);   /* scancodes waiting in ring */
uint32_t keyboard_chain_drain(void);     /* drain through process_scancode */
uint32_t keyboard_chain_total(void);     /* lifetime drained count */
uint32_t keyboard_pending_irqs(void);    /* lifetime IRQ count */


/* E.9: input-method (compose/dead-key) framework. */
void ime_reset(void);
int  ime_pending(void);
int  ime_feed(int cp);
#endif /* ZEOS_KEYBOARD_H */
