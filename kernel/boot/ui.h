/*
 * Zeos — UI Primitive Helpers
 *
 * Draws buttons, focus rings, overlays, and separators using
 * theme.h tokens and fb.h drawing primitives. These are the
 * building blocks for all Zeos UI — signal visualizer, shell,
 * persona overlays, everything.
 *
 * State-driven: pass the interaction state and the correct
 * visual treatment is applied automatically.
 */

#ifndef ZEOS_UI_H
#define ZEOS_UI_H

#include <stdint.h>

/* Interaction states (matches theme.h STATE_* values) */
enum ui_state {
    UI_IDLE,
    UI_HOVER,
    UI_PRESSED,
    UI_FOCUSED,
    UI_DISABLED,
};

/* Draw a button with label and interaction state */
void ui_button(int x, int y, int w, int h, const char *label, enum ui_state state);

/* Draw a 2px focus ring in primary color */
void ui_draw_focus_ring(int x, int y, int w, int h);

/* Draw a hover-state overlay (semi-transparent) */
void ui_hover_overlay(int x, int y, int w, int h);

/* Horizontal separator line */
void ui_separator_h(int x, int y, int w);

/* Vertical separator line */
void ui_separator_v(int x, int y, int h);

#endif /* ZEOS_UI_H */
