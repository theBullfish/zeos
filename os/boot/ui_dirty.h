/*
 * Zeos — Dirty Buffer / "Save Changes?" Tracker
 *
 * Doctrine: data loss on close is the single worst UI failure mode.
 * Every editable buffer that survives a close should register here.
 * On close, the WM consults dirty_get() and presents a Save / Discard
 * / Cancel modal before destroying the surface.
 *
 * The modal here is a lightweight text-only dialog drawn over the
 * compositor — no chain surface, no input to other layers.
 */

#ifndef ZEOS_UI_DIRTY_H
#define ZEOS_UI_DIRTY_H

#include <stdint.h>

#define UI_DIRTY_MAX  64

typedef enum {
    DIRTY_SAVE,
    DIRTY_DISCARD,
    DIRTY_CANCEL,
} dirty_response_t;

/* Modal callback — the host receives the user's choice. */
typedef void (*dirty_modal_cb_fn)(int window_id, dirty_response_t response,
                                  void *ctx);

/* Mark a window as having unsaved changes. Idempotent. */
void dirty_register(int window_id);

/* Clear the dirty flag (typically after a successful save). */
void dirty_clear(int window_id);

/* Bump the modified timestamp without changing dirty flag. */
void dirty_touch(int window_id);

/* Query state. */
int      dirty_get(int window_id);
uint64_t dirty_modified_at(int window_id);
int      dirty_count(void);

/* Open the close-confirm modal for a window. The callback fires once
 * the user picks Save / Discard / Cancel. While the modal is up,
 * mouse / key dispatch is consumed by the modal (see dirty_*_handlers). */
void dirty_open_close_modal(int window_id,
                            const char *title,
                            dirty_modal_cb_fn cb, void *ctx);

/* Modal active? */
int  dirty_modal_active(void);

/* Input dispatch for the modal. Return 1 if consumed. */
int  dirty_modal_mouse_move(int x, int y);
int  dirty_modal_mouse_down(int x, int y, int button);
int  dirty_modal_key(int ascii);

/* Render. */
void dirty_modal_draw(void);

#endif /* ZEOS_UI_DIRTY_H */
