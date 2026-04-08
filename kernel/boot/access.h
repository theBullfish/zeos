/*
 * Zeos — Accessibility Settings
 *
 * System-wide accessibility configuration that affects all of Z-OS.
 * Sensory modes, density, color scheme, motion, focus, night shift.
 *
 * Persisted to VAULT at /config/accessibility.
 * Loaded on boot, defaults applied if no saved config exists.
 */

#ifndef ZEOS_ACCESS_H
#define ZEOS_ACCESS_H

#include <stdint.h>

/* ── Sensory modes ── */
typedef enum {
    SENSORY_STANDARD,       /* Theme defaults */
    SENSORY_LOW_STIMULI,    /* Muted accents, no decorative anims, warm temp */
    SENSORY_HIGH_CONTRAST,  /* Pure white text, 2px borders, no transparency */
} sensory_mode_t;

/* ── Density modes ── */
typedef enum {
    DENSITY_COMFORTABLE,    /* Spacious, 48px controls */
    DENSITY_STANDARD,       /* Default, 40px controls */
    DENSITY_COMPACT,        /* Dense, 32px controls */
} density_mode_t;

/* ── Color scheme ── */
typedef enum {
    SCHEME_DARK,
    SCHEME_LIGHT,
    SCHEME_AUTO,
} color_scheme_t;

/* ── Master accessibility config ── */
typedef struct {
    sensory_mode_t  sensory;
    density_mode_t  density;
    color_scheme_t  scheme;

    /* Typography */
    int             letter_spacing;     /* Extra pixels between letters (0-4) */
    int             word_spacing;       /* Extra pixels between words (0-8) */
    int             line_spacing;       /* Extra pixels between lines (0-8) */

    /* Motion */
    int             reduced_motion;     /* 0=off, 1=on (instant cuts + opacity fades) */
    float           anim_speed;         /* 0.0=instant, 0.5, 1.0=normal, 2.0=slow */

    /* Focus */
    int             focus_mode;         /* 0=off, 1=on (suppress non-critical notifs) */

    /* Night shift */
    int             night_shift;        /* 0=off, 1=on */
    uint32_t        color_temp;         /* Color temperature warm tint (0xAARRGGBB) */

    /* Touch */
    int             min_touch_target;   /* Minimum interactive element size (px) */
} access_config_t;

/* ── API ── */

/*
 * Initialize accessibility system.
 * Loads config from VAULT, or applies defaults if first boot.
 */
void access_init(void);

/*
 * Persist current config to VAULT.
 */
void access_save(void);

/*
 * Return pointer to current accessibility config.
 * Never NULL after access_init().
 */
access_config_t *access_get(void);

/*
 * Set sensory mode. Adjusts colors, motion, sound system-wide.
 *   LOW_STIMULI:    mute accents to 50% sat, disable decorative anims,
 *                   reduce spring stiffness, warm color temperature.
 *   HIGH_CONTRAST:  pure white text, 2px borders, no gradients,
 *                   no transparency.
 *   STANDARD:       theme.h defaults.
 */
void access_set_sensory(sensory_mode_t mode);

/*
 * Set density mode. Adjusts control sizes, panel height, icon sizes.
 */
void access_set_density(density_mode_t mode);

/*
 * Set color scheme (dark, light, auto).
 */
void access_set_scheme(color_scheme_t scheme);

/*
 * Toggle reduced motion.
 * When on: instant cuts + opacity fades, no spring animations.
 */
void access_set_reduced_motion(int on);

/*
 * Set animation speed multiplier.
 * 0.0 = instant, 0.5 = fast, 1.0 = normal, 2.0 = slow.
 */
void access_set_anim_speed(float speed);

/*
 * Toggle focus mode.
 * When on: suppress non-critical notifications.
 */
void access_set_focus_mode(int on);

/*
 * Returns control height based on density: 32 / 40 / 48.
 */
int access_get_control_height(void);

/*
 * Returns panel height based on density: 32 / 40 / 48.
 */
int access_get_panel_height(void);

/*
 * Returns icon size based on density: 32 / 40 / 48.
 */
int access_get_icon_size(void);

#endif /* ZEOS_ACCESS_H */
