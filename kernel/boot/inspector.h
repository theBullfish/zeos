/*
 * Zeos -- Signal Inspector
 *
 * "Tap any chain, see its state." The inspector is a right-side panel
 * that shows the full state of any chain: identity, B3 belief, nodes,
 * routes, and timing. This is what makes Z-OS inspectable.
 *
 * Static state. No malloc. Bare-metal.
 */

#ifndef ZEOS_INSPECTOR_H
#define ZEOS_INSPECTOR_H

#include <stdint.h>

/* ── Inspector State ─────────────────────────────────────────────── */

typedef struct {
    int visible;
    int target_chain_id;    /* Which chain we're inspecting */
    int x, y, w, h;        /* Panel position/size */
    int scroll_y;           /* Vertical scroll offset */
} inspector_state_t;

/* ── API ─────────────────────────────────────────────────────────── */

/* Initialize the inspector (hidden, no target) */
void inspector_init(void);

/* Open the inspector for a specific chain */
void inspector_show(int chain_id);

/* Close the inspector */
void inspector_hide(void);

/* Toggle visibility for a specific chain */
void inspector_toggle(int chain_id);

/* Render the inspector panel to the framebuffer */
void inspector_draw(void);

/* Scroll the inspector content */
void inspector_scroll(int delta);

/* Get current inspector state (read-only) */
const inspector_state_t *inspector_get_state(void);

#endif /* ZEOS_INSPECTOR_H */
