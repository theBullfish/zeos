/*
 * Zeos — Dock
 *
 * Bottom-edge launcher. Auto-hides by default.
 * Shows pinned items on the left, running chains on the right,
 * separated by a vertical divider.
 *
 * Spring-animated slide: hidden (off-screen) ↔ visible (docked).
 * Each item is a 40x40 cell with a letter icon and optional state dot.
 */

#ifndef ZEOS_DOCK_H
#define ZEOS_DOCK_H

#include <stdint.h>
#include "chain.h"

/* ── Limits ── */
#define DOCK_MAX_PINNED  12
#define DOCK_MAX_RUNNING 16

/* ── Item types ── */
typedef enum {
    DOCK_ITEM_PINNED,
    DOCK_ITEM_RUNNING,
} dock_item_type_t;

/* ── A single dock item ── */
typedef struct {
    dock_item_type_t type;
    char             name[32];
    int              chain_id;       /* For running chains */
    int              surface_id;     /* For WM surface link */
    uint32_t         accent;         /* State color */
    int              has_state_dot;  /* Show live/paused/error indicator */
    chain_status_t   state;
} dock_item_t;

/* ── Dock state ── */
typedef struct {
    int   visible;
    int   auto_hide;
    float slide_y;       /* Spring-animated slide (0 = hidden, 1 = visible) */
    int   anim_id;       /* Spring animation ID (-1 = none) */
    int   hover;         /* Mouse is over dock area */

    dock_item_t pinned[DOCK_MAX_PINNED];
    int         pinned_count;

    dock_item_t running[DOCK_MAX_RUNNING];
    int         running_count;

    int   selected;      /* Hovered item index (-1 = none) */
    int   dock_h;        /* Total height */
    int   dock_w;        /* Computed width based on item count */
} dock_state_t;

/* ── API ── */

/* Initialize the dock. auto_hide: 1 = hide when mouse leaves */
void dock_init(int auto_hide);

/* Pin an item (launcher shortcut). Returns 0 on success. */
int  dock_pin(const char *name, int chain_id);

/* Remove pinned item at index. */
void dock_unpin(int index);

/* Refresh running-chains list from WM surface list. */
void dock_update(void);

/* Spring-animate the dock into view. */
void dock_show(void);

/* Spring-animate the dock off screen. */
void dock_hide(void);

/* Toggle visible ↔ hidden. */
void dock_toggle(void);

/* Mouse entered the dock trigger zone. */
void dock_mouse_enter(void);

/* Mouse left the dock area. */
void dock_mouse_leave(void);

/* Click at screen-space x. Activates the item (focus or launch). */
void dock_click(int x);

/* Render the dock to the framebuffer. Called by compositor. */
void dock_draw(void);

/* Current visible height in pixels (0 when fully hidden). */
int  dock_get_height(void);

/* Get dock state pointer (for compositor / input routing). */
dock_state_t *dock_get_state(void);

#endif /* ZEOS_DOCK_H */
