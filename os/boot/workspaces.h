/*
 * Zeos — Virtual Desktops / Workspaces
 *
 * N independent window sets (default 4, max 8). Each workspace owns
 * its own focused window, optional wallpaper override, and an ordered
 * window list. Switching slides the WM contents horizontally via the
 * spring system. Mission-Control style overview tiles every workspace
 * onto one screen.
 *
 * Window ↔ workspace mapping is owned by chain_surface_t.workspace
 * (in wm.h). This module owns the *workspace-side* state: names,
 * wallpaper paths, focused id, count, slide animation, overview mode.
 *
 * Persistence: per-workspace wallpaper at /wm/wallpaper/<id>, the
 * window→workspace map at /wm/window_workspace_map, and the active
 * workspace + window stack at /wm/workspaces_state. All via VAULT.
 */

#ifndef ZEOS_WORKSPACES_H
#define ZEOS_WORKSPACES_H

#include <stdint.h>

#define WS_MAX_COUNT      8
#define WS_DEFAULT_COUNT  4
#define WS_NAME_MAX       32
#define WS_WALLPAPER_MAX  128
#define WS_MAX_WINDOWS    32          /* mirror WM_MAX_SURFACES */

typedef struct {
    int      id;
    char     name[WS_NAME_MAX];
    char     wallpaper_path[WS_WALLPAPER_MAX];
    int      window_ids[WS_MAX_WINDOWS]; /* ordered by stack, low->high z */
    int      window_count;
    int      focused_window_id;
} workspace_t;

/* ── Lifecycle ── */
void workspaces_init(int screen_w, int screen_h);

/* Apply a count change (1..WS_MAX_COUNT). Re-homes any windows whose
 * workspace id is now out of range to workspace 0. */
void workspaces_set_count(int n);
int  workspaces_get_count(void);

/* Animation tunable (settings: wm.workspace_animation_ms). */
void workspaces_set_animation_ms(int ms);
int  workspaces_get_animation_ms(void);

/* ── Active workspace ── */
int  workspace_active(void);
const workspace_t *workspace_get(int id);
workspace_t *workspace_get_mut(int id);

/* Switch with a horizontal spring slide. Recomputes window visibility,
 * focuses the workspace's saved focus, animates direction by id delta. */
void workspace_switch(int target_id);

/* Move the focused window (or any window) to another workspace. The
 * window's per-surface workspace field is updated and persisted. */
void workspace_move_window(int window_id, int target_id);
void workspace_move_focused_and_switch(int target_id);

/* ── Per-workspace wallpaper ── */
void        workspace_set_wallpaper(int id, const char *path);
const char *workspace_get_wallpaper(int id);  /* "" if unset */

/* ── Overview mode (Mission Control) ── */
void workspaces_overview_enter(void);
void workspaces_overview_exit(void);
int  workspaces_overview_active(void);
/* Compositor calls this after notifications and before context menus. */
void workspaces_overview_draw(void);
/* Mouse hooks. Returns 1 if the event was consumed. */
int  workspaces_overview_mouse_down(int x, int y, int button);
int  workspaces_overview_mouse_move(int x, int y);
/* Esc handler. Returns 1 if overview was active and got dismissed. */
int  workspaces_overview_handle_escape(void);

/* ── Slide animation hook for compositor ── */
/* Render-time x offset for windows on the *current* workspace
 * during a slide. WM uses this to translate draws. */
int workspaces_slide_offset_active(void);
int workspaces_slide_offset_incoming(void);
int workspaces_slide_incoming_id(void);   /* -1 if not sliding */
int workspaces_is_sliding(void);

/* Re-home book-keeping: WM calls this after a window's workspace
 * field is set so the workspace can update its window_ids list. */
void workspaces_window_assigned(int window_id, int workspace_id);
void workspaces_window_removed(int window_id);
void workspaces_window_focused(int window_id);

/* ── Indicator (panel.c calls) ── */
/* Render N dots starting at (x, y_center). Returns total pixel width. */
int  workspaces_draw_indicator(int x, int y_center);
/* Hit-test a click on the indicator. Returns workspace id or -1. */
int  workspaces_indicator_hit(int x, int y, int strip_x, int y_center);

/* ── Persistence ── */
void workspaces_save(void);
void workspaces_load(void);

/* ── Selftest ── */
int  workspaces_total_windows(void);  /* sum across all workspaces */

#endif /* ZEOS_WORKSPACES_H */
