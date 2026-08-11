/*
 * Zeos -- Settings App
 *
 * System preferences as a chain surface (window).
 * Lets the user change everything they picked at first boot,
 * plus accessibility, network info, and about.
 *
 * Changes apply immediately and persist to VAULT.
 *
 * NOTE: Add kernel/boot/settings.c to SRCS in the Makefile.
 */

#ifndef ZEOS_SETTINGS_H
#define ZEOS_SETTINGS_H

#include <stdint.h>
#include "access.h"   /* J.2: single source of truth for accessibility config
                       * (sensory_mode_t, density_mode_t, access_config_t).
                       * settings.h used to define DUPLICATE copies of these,
                       * so the settings GUI mutated a private struct that never
                       * reached the live access subsystem -- changing Sensory
                       * Mode etc. did nothing. Now both use access.h's config. */

/* ── Settings pages ── */
typedef enum {
    SETTINGS_PAGE_DISPLAY,
    SETTINGS_PAGE_INPUT,
    SETTINGS_PAGE_ACCESSIBILITY,
    SETTINGS_PAGE_NETWORK,
    SETTINGS_PAGE_ABOUT,
    SETTINGS_PAGE_COUNT
} settings_page_t;

/* ── Settings state ── */
typedef struct {
    int              surface_id;     /* WM surface this lives in */
    settings_page_t  page;           /* Current page */
    int              selected_item;  /* Currently highlighted setting */
    int              scroll_y;
} settings_state_t;

/* ── API ── */

/* Open the settings window (creates a WM chain surface) */
void settings_open(void);

/* J.4: open Settings on a specific page (SETTINGS_PAGE_*). */
void settings_open_page(int page);
int  settings_current_page(void);
int  settings_is_open(void);

/* Close (detach) the settings surface */
void settings_close(void);

/* WM draw_content callback — renders settings content */
void settings_draw(int id, int x, int y, int w, int h);

/* Handle keyboard input within settings */
void settings_key(int scancode);

/* Handle mouse click within settings */
void settings_click(int x, int y);

/* Get current accessibility config (for other subsystems to query) */
const access_config_t *settings_get_access(void);

#endif /* ZEOS_SETTINGS_H */
