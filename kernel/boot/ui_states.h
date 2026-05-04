/*
 * Zeos — List Empty / Loading / Error States
 *
 * Doctrine: every list has four states, not one. The naked "empty
 * rectangle" is a bug. A list that doesn't draw a state is lying to
 * the user about whether the data was there, isn't yet, or failed.
 */

#ifndef ZEOS_UI_STATES_H
#define ZEOS_UI_STATES_H

#include <stdint.h>

typedef enum {
    LIST_OK,        /* Has rows — caller draws them itself */
    LIST_LOADING,
    LIST_EMPTY,
    LIST_ERROR,
} list_state_t;

/* Optional retry callback for LIST_ERROR. NULL = no retry button. */
typedef void (*list_retry_fn)(void *ctx);

typedef struct {
    int           x, y, w, h;
    list_state_t  state;
    const char   *message;       /* May be NULL — defaults are used. */
    const char   *cta;           /* Empty-state call-to-action (NULL ok). */
    list_retry_fn retry_cb;
    void         *retry_ctx;
} list_state_ctx_t;

/* Render a state into the rect. Caller passes their list rect; this
 * draws spinner / message / retry button as appropriate. Returns 1 if
 * a state was drawn (state != LIST_OK), else 0. */
int  list_render_state(const list_state_ctx_t *c);

/* Mouse hit-test for the retry button. Returns 1 and invokes the
 * callback if the click landed on retry. */
int  list_state_mouse_down(const list_state_ctx_t *c, int x, int y);

/* Counter for selftest. */
uint32_t list_state_total_renders(void);

#endif /* ZEOS_UI_STATES_H */
