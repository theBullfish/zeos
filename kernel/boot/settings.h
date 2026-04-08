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

/* ── Settings pages ── */
typedef enum {
    SETTINGS_PAGE_DISPLAY,
    SETTINGS_PAGE_INPUT,
    SETTINGS_PAGE_ACCESSIBILITY,
    SETTINGS_PAGE_NETWORK,
    SETTINGS_PAGE_ABOUT,
    SETTINGS_PAGE_COUNT
} settings_page_t;

/* ── Accessibility configuration ── */
typedef enum {
    SENSORY_STANDARD,
    SENSORY_LOW_STIMULI,
    SENSORY_HIGH_CONTRAST,
} sensory_mode_t;

typedef enum {
    DENSITY_COMFORTABLE,
    DENSITY_STANDARD,
    DENSITY_COMPACT,
} density_t;

typedef enum {
    ANIM_SPEED_0X,       /* No animation */
    ANIM_SPEED_HALF,     /* 0.5x */
    ANIM_SPEED_1X,       /* Normal */
    ANIM_SPEED_2X,       /* Fast */
} anim_speed_t;

typedef struct {
    sensory_mode_t sensory;
    density_t      density;
    int            reduced_motion;   /* 0 = off, 1 = on */
    anim_speed_t   anim_speed;
    int            letter_spacing;   /* 0-4 px extra */
    int            focus_mode;       /* 0 = off, 1 = on */
    int            night_shift;      /* 0 = off, 1 = on */
} access_config_t;

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
