/*
 * Zeos -- Desktop Icons
 *
 * Grid-snapped icon system for the desktop surface.
 * Icons represent chain launchers -- double-click creates
 * a chain of the specified type and opens a surface for it.
 *
 * Persistence via VAULT config: icon positions, names, and
 * chain types survive reboot.
 *
 * Ships empty. User adds icons via shell or drag.
 */

#ifndef ZEOS_DESKTOP_H
#define ZEOS_DESKTOP_H

#include <stdint.h>

#define DESKTOP_MAX_ICONS 32

typedef struct {
    char name[32];
    char chain_type[32];    /* What chain to create when launched */
    int grid_x, grid_y;     /* Grid position (in Z8=32px units) */
    int selected;
    uint32_t accent;        /* Icon tint color */
} desktop_icon_t;

typedef struct {
    desktop_icon_t icons[DESKTOP_MAX_ICONS];
    int icon_count;
    int icon_size;          /* 32/40/48 based on density */
    int grid_spacing;       /* Z8 = 32px */
    uint32_t wallpaper_color;
    int dragging;           /* Currently dragging an icon */
    int drag_icon;          /* Which icon */
    int drag_x, drag_y;     /* Current drag position */
} desktop_state_t;

/* ── API ── */

/*
 * Initialize the desktop icon system.
 * wallpaper_color: background fill (0xAARRGGBB)
 * density: 0=small(32), 1=medium(40), 2=large(48)
 * Loads saved icons from VAULT if they exist.
 */
void desktop_init(uint32_t wallpaper_color, int density);

/* D.10: 1 if a wallpaper image was decoded from VAULT (not the solid fallback). */
int desktop_wallpaper_loaded(void);

/*
 * Add an icon to the desktop.
 * Returns icon index, or -1 if full.
 */
int desktop_add_icon(const char *name, const char *chain_type,
                     int grid_x, int grid_y);

/*
 * Remove an icon by index.
 */
void desktop_remove_icon(int index);

/*
 * Draw the desktop: wallpaper + all icons.
 * Called by the compositor's draw_desktop().
 */
void desktop_draw(void);

/*
 * Handle a single click at (x, y).
 * Selects the icon under the cursor, or deselects all if empty space.
 */
void desktop_click(int x, int y);

/*
 * Handle a double-click at (x, y).
 * Launches the chain associated with the icon under the cursor.
 */
void desktop_double_click(int x, int y);

/*
 * Begin dragging the selected icon from (x, y).
 */
void desktop_drag_start(int x, int y);

/*
 * Update the drag position to (x, y).
 */
void desktop_drag_move(int x, int y);

/*
 * End the drag at (x, y). Snaps icon to nearest grid cell.
 * Saves positions to VAULT.
 */
void desktop_drag_end(int x, int y);

/*
 * Persist all icon positions/names/types to VAULT.
 */
void desktop_save(void);

/*
 * Load icon positions/names/types from VAULT.
 */
void desktop_load(void);

/*
 * Get the desktop state (for compositor access).
 */
desktop_state_t *desktop_get_state(void);

/* Right-click on empty desktop — opens "New folder / Settings / Wallpaper". */
int desktop_right_click(int x, int y);

/* Open Quick Look on the currently-selected desktop icon (if any). */
void desktop_quick_look_selected(void);

#endif /* ZEOS_DESKTOP_H */
