/*
 * Zeos — PS/2 mouse driver
 *
 * Handles IRQ12 via the 8042 controller auxiliary port.
 * Decodes 3-byte packets into absolute position + button state.
 * Feeds the cursor system (cursor_move / cursor_press / cursor_release).
 */

#ifndef ZEOS_MOUSE_H
#define ZEOS_MOUSE_H

#include <stdint.h>

/* ── Button bit masks ── */
#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

/* ── Mouse state ── */
typedef struct {
    int x, y;              /* absolute position, clamped to screen */
    int dx, dy;            /* last-packet delta (signed) */
    uint8_t buttons;       /* current button state (MOUSE_BTN_*) */
    uint8_t prev_buttons;  /* previous button state (for edge detection) */
} mouse_state_t;

/* Initialize PS/2 mouse (enable aux device, set defaults, enable reporting).
 * Must be called after idt_init() and before sti. */
void mouse_init(void);

/* Poll for mouse data (non-interrupt fallback, checks OBF + AuxData).
 * Call from main loop if IRQ12 is not firing. */
void mouse_poll(void);

/* Get current absolute X position */
int mouse_get_x(void);

/* Get current absolute Y position */
int mouse_get_y(void);

/* Get current button state (MOUSE_BTN_* bitmask) */
uint8_t mouse_get_buttons(void);

/* Get pointer to full mouse state (read-only) */
const mouse_state_t *mouse_get_state(void);

/* Inject a relative-motion event from an alternate input source -- e.g.
 * a USB HID boot mouse. dx/dy are signed pixel deltas already in screen
 * coordinates (positive y = down). buttons is a MOUSE_BTN_* bitmask of
 * the current state. Updates absolute position, clamps to screen, and
 * fires cursor_move/press/release exactly like the PS/2 IRQ path. */
void mouse_inject(int dx, int dy, uint8_t buttons);

/* CHAIN_MOUSE plumbing. */
int      mouse_chain_pending(void);
uint32_t mouse_chain_drain(void);
uint32_t mouse_chain_total(void);
uint32_t mouse_pending_irqs(void);

#endif /* ZEOS_MOUSE_H */
