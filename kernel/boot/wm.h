/*
 * Zeos — Window Manager
 *
 * Manages chain surfaces: chrome, stacking, drag, resize, snap/tile.
 * Each "window" is a chain_surface — a visual region bound to a signal chain.
 *
 * Window controls: [×] detach  [−] minimize  [□] maximize  [⚡] signal status
 * Position: Left or Right, user-configurable (first boot + settings).
 *
 * Stacking model: floating by default, tiling opt-in per workspace.
 * Drag-to-edge snapping with ghost preview and spring animation.
 */

#ifndef ZEOS_WM_H
#define ZEOS_WM_H

#include <stdint.h>
#include "theme.h"

/* ── Limits ── */
#define WM_MAX_SURFACES   32
#define WM_MAX_WORKSPACES  8

/* ── Window control placement ── */
typedef enum {
    WM_CONTROLS_LEFT,
    WM_CONTROLS_RIGHT,
} wm_controls_side_t;

/* ── Surface state ── */
typedef enum {
    SURFACE_NORMAL,
    SURFACE_MAXIMIZED,
    SURFACE_MINIMIZED,
    SURFACE_SNAPPED_LEFT,
    SURFACE_SNAPPED_RIGHT,
    SURFACE_SNAPPED_TL,
    SURFACE_SNAPPED_TR,
    SURFACE_SNAPPED_BL,
    SURFACE_SNAPPED_BR,
    SURFACE_SNAPPED_LEFT_2_3,    /* Left two-thirds (cycle target) */
    SURFACE_SNAPPED_RIGHT_2_3,   /* Right two-thirds (cycle target) */
    SURFACE_TILED,
} surface_state_t;

/* Public alias used by snap API. */
typedef surface_state_t snap_kind_t;

/* ── Signal health ── */
typedef enum {
    SIGNAL_LIVE,       /* Green pulse — chain resolving */
    SIGNAL_PAUSED,     /* Gray — chain paused */
    SIGNAL_ERROR,      /* Red pulse — chain has errors */
    SIGNAL_DETACHED,   /* Not rendering */
} signal_status_t;

/* ── Chain surface (window) ── */
typedef struct {
    /* Identity */
    int              id;
    char             title[128];
    int              chain_id;        /* Signal chain this surface renders */
    signal_status_t  signal;

    /* Geometry */
    int              x, y;            /* Position */
    int              w, h;            /* Dimensions (including chrome) */
    int              min_w, min_h;    /* Minimum dimensions */
    surface_state_t  state;

    /* Saved geometry (for restore from maximize/snap) */
    int              saved_x, saved_y, saved_w, saved_h;

    /* Z-order */
    int              z_index;         /* Higher = on top */

    /* Workspace */
    int              workspace;

    /* Rendering */
    int              visible;
    int              focused;
    uint32_t         accent;          /* Persona accent for this surface */

    /* Drag/resize state */
    int              dragging;
    int              resizing;
    int              drag_offset_x;
    int              drag_offset_y;
    int              resize_edge;     /* Bitmask: 1=top, 2=right, 4=bottom, 8=left */

    /* Spring animation state */
    float          anim_scale;       /* Current scale (0.0–1.0), default 1.0 */
    float          anim_opacity;     /* Current opacity (0–255), default 255 */
    int            anim_scale_id;    /* Spring anim ID for scale (-1 = none) */
    int            anim_opacity_id;  /* Spring anim ID for opacity (-1 = none) */
    int            anim_x_id;        /* Spring anim ID for x position (-1 = none) */
    int            anim_y_id;        /* Spring anim ID for y position (-1 = none) */
    int            anim_w_id;        /* Spring anim ID for width (-1 = none) */
    int            anim_h_id;        /* Spring anim ID for height (-1 = none) */
    float          anim_x;           /* Animated x (float for spring precision) */
    float          anim_y;           /* Animated y */
    float          anim_w;           /* Animated w */
    float          anim_h;           /* Animated h */
    int            closing;          /* 1 = close animation in progress */

    /* Content draw callback */
    void           (*draw_content)(int id, int x, int y, int w, int h);

    /* Right-click callback. The WM transforms the screen coord into
     * window-content-relative coordinates and dispatches here. NULL =
     * surface has no per-app context menu (falls back to generic). */
    void           (*on_right_click)(int local_x, int local_y);

    /* Persistence name. NULL = surface state is not persisted. The WM
     * uses this on detach to flush via wm_persist_save(name). */
    const char    *persist_name;

    /* D.11: last payload dropped onto this surface (a desktop icon dragged +
     * released over the window "feeds the chain"). */
    char           last_drop[32];
    int            drop_count;

    /* C.12: tabs — a surface can multiplex several chains as tabs. */
    int            tab_chain[8];      /* chain_id per tab */
    char           tab_title[8][24];
    int            tab_count;
    int            active_tab;

    /* C.14: parent/child chain stacking. */
    int            parent_surface;    /* -1 = top-level; else stacks on parent */
} chain_surface_t;

/* ── Snap zone ghost ── */
typedef struct {
    int  active;
    int  x, y, w, h;
    surface_state_t target_state;
} snap_ghost_t;

/* ── Window Manager State ── */
typedef struct {
    chain_surface_t  surfaces[WM_MAX_SURFACES];
    int              surface_count;
    int              focused_id;       /* Currently focused surface ID */
    int              next_id;

    /* Settings */
    wm_controls_side_t controls_side;
    int              active_workspace;
    int              tiling_enabled[WM_MAX_WORKSPACES];

    /* Snap preview */
    snap_ghost_t     ghost;

    /* Screen dimensions */
    int              screen_w, screen_h;
    int              panel_h;          /* Top panel height */
    int              dock_h;           /* Bottom dock height (when visible) */
} wm_state_t;

/* ── Sizing constants ── */
#define WM_TITLEBAR_HEIGHT  40
#define WM_BORDER_WIDTH      1
#define WM_CONTROL_SIZE     22        /* Window control button size */
#define WM_CONTROL_SPACING  10
#define WM_CONTROL_MARGIN   14
#define WM_SNAP_ZONE        32        /* Edge activation area in pixels */
#define WM_MIN_SURFACE_W   200
#define WM_MIN_SURFACE_H   150

/* ── API ── */

/* Initialize the window manager */
void wm_init(int screen_w, int screen_h, int panel_h);

/* Set window controls side (Left or Right) */
void wm_set_controls_side(wm_controls_side_t side);
wm_controls_side_t wm_get_controls_side(void);

/* ── Surface lifecycle ── */

/* Create a new chain surface. Returns surface ID or -1. */
int wm_create_surface(const char *title, int chain_id,
                      int x, int y, int w, int h,
                      void (*draw_content)(int, int, int, int, int));

/* Close (detach) a surface — chain keeps running */
void wm_detach_surface(int id);
/* Force a surface to fully-open (skip open animation) for synchronous paints. */
void wm_force_visible(int id);

/* Minimize a surface */
void wm_minimize_surface(int id);

/* Restore a minimized surface */
void wm_restore_surface(int id);

/* Maximize / restore toggle */
void wm_maximize_surface(int id);

/* ── Focus ── */
void wm_focus_surface(int id);
int  wm_get_focused(void);

/* ── Movement / resize ── */
void wm_move_surface(int id, int x, int y);
void wm_resize_surface(int id, int w, int h);

/* ── Snap / tile ── */
void wm_snap_surface(int id, surface_state_t snap);
/* New canonical snap entry — sets target geometry for the kind, animates
 * via spring, multi-display aware. Saves pre-snap geometry on transition
 * out of NORMAL so SURFACE_NORMAL un-snaps back to it. */
void wm_snap(int window_id, snap_kind_t kind);
/* Cycle Super+Left/Right: half -> two-thirds -> normal restore. */
void wm_snap_cycle_left(int window_id);
void wm_snap_cycle_right(int window_id);
/* Move focused window to next display (multi-display awareness). */
void wm_move_to_next_display(int window_id);
/* Cancel an in-progress drag (Escape key). Returns 1 if a drag was cancelled. */
int  wm_cancel_drag(void);
/* Tunables (settings registry). */
void wm_set_snap_drag_threshold(int px);
int  wm_get_snap_drag_threshold(void);
void wm_set_snap_animation_ms(int ms);
int  wm_get_snap_animation_ms(void);
void wm_toggle_tiling(void);

/* ── Workspace ── */
void wm_switch_workspace(int workspace);
int  wm_get_workspace(void);
void wm_move_to_workspace(int id, int workspace);

/* ── Input handling ── */

/* Mouse events — the WM handles drag, resize, button clicks */
int  wm_mouse_down(int x, int y, int button);   /* returns hit surface id, or -1 */
void wm_mouse_up(int x, int y, int button);
void wm_mouse_move(int x, int y);

/* Keyboard shortcuts */
void wm_key_event(int keycode, int modifiers);

/* ── Rendering ── */

/* Set the right-click callback for a surface. Pass NULL to clear. */
void wm_set_right_click(int id, void (*cb)(int local_x, int local_y));

/* Set the persistence name for a surface (must be a static string;
 * stored by reference). The WM auto-saves on detach and queues this
 * surface for the 60s autosave sweep. */
void wm_set_persist_name(int id, const char *name);

/* Translate a screen coordinate to a window-content-relative coord.
 * Returns 1 if the screen point falls inside the content area of the
 * given window and writes the local coord to out_x/out_y; 0 otherwise. */
int  wm_screen_to_window(int window_id, int screen_x, int screen_y,
                         int *out_x, int *out_y);

/* Find the topmost visible window whose content area contains (sx,sy).
 * Returns the surface id, or -1 if none. */
int  wm_window_at(int sx, int sy);

/* Draw all visible surfaces (called by compositor) */
void wm_draw_all(void);

/* Serial dump of all surfaces' live state (id/ws/geometry/z/state/vis/focus).
 * Ground truth for interaction verification; called gated by ZEOS_DIAG_WM_STATE. */
void wm_dump_state(void);

/* Draw a single surface's chrome (title bar, controls, border) */
void wm_draw_chrome(chain_surface_t *s);

/* Draw snap ghost preview */
void wm_draw_ghost(void);

/* ── Show Desktop ── */
void wm_show_desktop(void);      /* Spring-animate all surfaces to hidden */
void wm_restore_desktop(void);   /* Spring-animate all surfaces back */
int  wm_is_desktop_shown(void);  /* 1 if show-desktop mode is active */

/* ── Queries ── */
chain_surface_t *wm_get_surface(int id);

/* D.11: topmost visible surface containing (x,y), or -1. */
int  wm_surface_at(int x, int y);
/* D.11: deliver a dropped payload (desktop icon name) to a surface's chain. */
int  wm_feed_surface(int id, const char *payload);

/* C.13: dock a surface beside a same-chain neighbor (magnetic adjacency). */
int  wm_snap_adjacent(int id);

/* C.12: tabs. */
int  wm_tab_add(int id, int chain_id, const char *title);
int  wm_tab_switch(int id, int index);
int  wm_tab_count(int id);
int  wm_tab_active(int id);

/* C.14: parent/child chain stacking. */
int  wm_set_parent(int id, int parent_id);
int  wm_get_parent(int id);
int  wm_is_ancestor(int a, int b);
chain_surface_t *wm_get_surface_by_index(int index);  /* Index into surface array */
int wm_surface_count(void);
int wm_visible_count(void);    /* On current workspace */

#endif /* ZEOS_WM_H */
