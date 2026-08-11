/*
 * Zeos -- Procedural Icon Rendering
 *
 * Simple geometric icons drawn with framebuffer primitives.
 * Each icon scales cleanly to ICON_SM (16), ICON_MD (24), ICON_LG (32).
 *
 * Alpha 0.1: procedural draw functions.
 * Future: pre-rasterized bitmaps from SVG assets at build time.
 */

#ifndef ZEOS_ICON_RENDER_H
#define ZEOS_ICON_RENDER_H

#include <stdint.h>

typedef enum {
    ICON_FILE,
    ICON_FOLDER,
    ICON_TERMINAL,
    ICON_BROWSER,
    ICON_SETTINGS,
    ICON_CHAIN,
    ICON_SIGNAL,
    ICON_LOCK,
    ICON_UNLOCK,
    ICON_NETWORK,
    ICON_AUDIO,
    ICON_DISPLAY,
    ICON_STORAGE,
    ICON_USB,
    ICON_SEARCH,
    ICON_HOME,
    ICON_BACK,
    ICON_FORWARD,
    ICON_REFRESH,
    ICON_CLOSE,
    ICON_MINIMIZE,
    ICON_MAXIMIZE,
    ICON_BOOKMARK,
    ICON_ALERT,
    ICON_CHECK,
    ICON_PLUS,
    ICON_MINUS,
    ICON_GEAR,
    ICON_USER,
    ICON_PENCIL,
    ICON_TRASH,
    ICON_COUNT
} icon_id_t;

/* Draw an icon at (x, y) with given size and color */
void icon_draw(icon_id_t icon, int x, int y, int size, uint32_t color);

/* Draw an icon tinted with the current persona accent */
void icon_draw_accent(icon_id_t icon, int x, int y, int size);

#endif /* ZEOS_ICON_RENDER_H */
