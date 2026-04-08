/*
 * Zeos — Hot Corners
 *
 * Cursor-dwell activation zones at the four screen corners.
 * Called every compositor frame with the current mouse position.
 *
 * Sacred corners (DESKTOP.md):
 *   Top-Left:     Command Palette
 *   Top-Right:    Notifications / Signal Status
 *   Bottom-Left:  Workspaces (cycle)
 *   Bottom-Right: Show Desktop (minimize all)
 *
 * Dwell time prevents accidental triggers — cursor must stay in the
 * corner zone for dwell_ms before the action fires. The action fires
 * once; cursor must leave the zone before it can re-trigger.
 */

#ifndef ZEOS_HOTCORNERS_H
#define ZEOS_HOTCORNERS_H

#include <stdint.h>

/* ── Corner identifiers ── */
typedef enum {
    CORNER_TL,      /* Top-left:     Command Palette */
    CORNER_TR,      /* Top-right:    Notifications */
    CORNER_BL,      /* Bottom-left:  Workspaces */
    CORNER_BR,      /* Bottom-right: Show Desktop */
    CORNER_COUNT
} corner_id_t;

/* ── Hot corners state ── */
typedef struct {
    int      zone_size;                  /* Activation zone in pixels (default 8) */
    int      dwell_ms;                   /* Required dwell time in ms (default 150) */
    int      enabled;                    /* Global enable/disable */
    int      active_corner;              /* Currently dwelling corner (-1 = none) */
    uint64_t enter_tsc;                  /* TSC when cursor entered active corner */
    int      triggered[CORNER_COUNT];    /* Prevent re-trigger until cursor leaves */

    /* Screen dimensions */
    int      screen_w;
    int      screen_h;
} hotcorners_state_t;

/* Initialize hot corners. Must be called after timer_init(). */
void hotcorners_init(int screen_w, int screen_h);

/* Tick — called every compositor frame with current mouse position. */
void hotcorners_tick(int mouse_x, int mouse_y);

/* Enable or disable hot corners globally. */
void hotcorners_enable(int enabled);

#endif /* ZEOS_HOTCORNERS_H */
