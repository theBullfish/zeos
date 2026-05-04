/*
 * Zeos — Hover Registry
 *
 * Doctrine: hover state is the cheapest signal a UI can emit, and its
 * absence is the loudest "this OS feels dead" smell. Every clickable
 * widget registers a rectangle and an enter/leave callback. Mouse-move
 * dispatches; cursor switches to pointer over registered hot zones.
 *
 * The registry is an array (no allocations). Slots are reusable —
 * widgets re-register on layout change. A widget that doesn't register
 * costs nothing.
 */

#ifndef ZEOS_UI_HOVER_H
#define ZEOS_UI_HOVER_H

#include <stdint.h>

#define UI_HOVER_MAX  128

/* Cursor hint for the hover zone (mapped to cursor_state_t at dispatch). */
typedef enum {
    HOVER_CURSOR_DEFAULT,
    HOVER_CURSOR_POINTER,    /* hand over a clickable */
    HOVER_CURSOR_TEXT,       /* I-beam over editable text */
    HOVER_CURSOR_RESIZE,
} hover_cursor_hint_t;

typedef void (*hover_cb_fn)(void *ctx, int entering);

typedef struct {
    int                 used;
    int                 x, y, w, h;
    hover_cursor_hint_t cursor_hint;
    hover_cb_fn         cb;     /* fires on enter (1) and leave (0); may be NULL */
    void               *ctx;
    int                 active; /* internal: cursor currently inside */
    uint64_t            token;  /* unique handle for unregister */
} hover_zone_t;

/* Register a clickable hover zone. Returns a non-zero token on success,
 * 0 on failure (table full). The token is needed to unregister. */
uint64_t hover_register(int x, int y, int w, int h,
                        hover_cursor_hint_t hint,
                        hover_cb_fn cb, void *ctx);

/* Update the rect for an existing zone (e.g. after layout change). */
void hover_update_rect(uint64_t token, int x, int y, int w, int h);

/* Unregister a zone. */
void hover_unregister(uint64_t token);

/* Clear all zones (e.g. between scene rebuilds). */
void hover_clear_all(void);

/* Mouse-move dispatch. Fires enter/leave callbacks and updates cursor. */
void hover_dispatch(int x, int y);

/* Query: is the cursor over any registered hover zone? */
int  hover_any_active(void);

/* Counters for selftest. */
uint32_t hover_total_zones(void);
uint32_t hover_total_enters(void);

#endif /* ZEOS_UI_HOVER_H */
