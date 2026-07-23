/*
 * Zeos — Accessibility Settings
 *
 * System-wide accessibility that affects every subsystem.
 * Persisted to VAULT. Loaded on boot.
 */

#include "access.h"
#include "theme.h"
#include "vault.h"
#include "anim.h"
#include "compositor.h"

#define ACCESS_VAULT_KEY  "accessibility"

/* ── Static config (single instance, entire OS) ── */
static access_config_t g_access;
static int g_initialized = 0;

/* ── Density lookup tables ── */

/* Control height: COMFORTABLE=48, STANDARD=40, COMPACT=32 */
static const int density_control_height[] = { 48, 40, 32 };

/* Panel height: matches control height per density */
static const int density_panel_height[] = { 48, 40, 32 };

/* Icon size: matches control height per density */
static const int density_icon_size[] = { 48, 40, 32 };

/* ── Defaults ── */

static void access_set_defaults(void)
{
    g_access.sensory        = SENSORY_STANDARD;
    g_access.density        = DENSITY_STANDARD;
    g_access.scheme         = SCHEME_DARK;

    g_access.letter_spacing = 0;
    g_access.word_spacing   = 0;
    g_access.line_spacing   = 0;

    g_access.reduced_motion = 0;
    g_access.anim_speed     = 1.0f;

    g_access.focus_mode     = 0;

    g_access.night_shift    = 0;
    g_access.color_temp     = 0x00000000;  /* No tint */

    g_access.min_touch_target = 44;
}

/* ── Clamp helpers ── */

static int clamp_int(int val, int lo, int hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static float clamp_float(float val, float lo, float hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

/* ── Sensory mode application ── */

/*
 * Apply sensory mode side-effects to the running system.
 * This is where we reach into other subsystems.
 */
static void apply_sensory_mode(sensory_mode_t mode)
{
    switch (mode) {
    case SENSORY_LOW_STIMULI:
        /*
         * Mute persona accents to 50% saturation.
         * Disable decorative animations.
         * Reduce spring stiffness for gentler motion.
         * Warm color temperature.
         *
         * Accent muting is done by consumers who call access_get()
         * and check sensory == SENSORY_LOW_STIMULI. The theme
         * constants are compile-time, so runtime consumers must
         * derive muted variants themselves.
         *
         * Spring stiffness reduction: consumers should multiply
         * stiffness by 0.5 when sensory == LOW_STIMULI.
         */
        g_access.color_temp = 0x18331A00;  /* Warm amber tint */
        break;

    case SENSORY_HIGH_CONTRAST:
        /*
         * Pure white text (consumers override TEXT_PRIMARY to 0xFF).
         * 2px borders (consumers check sensory == HIGH_CONTRAST).
         * No gradients, no transparency.
         *
         * Color temperature reset — high contrast needs clarity.
         */
        g_access.color_temp = 0x00000000;  /* No tint */
        break;

    case SENSORY_STANDARD:
    default:
        /* Theme defaults. No color temperature override. */
        if (!g_access.night_shift)
            g_access.color_temp = 0x00000000;
        break;
    }

    /* Mark entire screen dirty — everything needs redraw */
    compositor_dirty_all();
}

/* ── Init ── */

void access_init(void)
{
    int loaded;

    /* Set sane defaults first */
    access_set_defaults();

    /* Try to load from VAULT */
    loaded = vault_load_config(ACCESS_VAULT_KEY, &g_access,
                               (uint32_t)sizeof(access_config_t));

    if (loaded != (int)sizeof(access_config_t)) {
        /* First boot or corrupted — use defaults and save them */
        access_set_defaults();
        vault_save_config(ACCESS_VAULT_KEY, &g_access,
                          (uint32_t)sizeof(access_config_t));
    }

    /* Validate loaded values (guard against corrupted VAULT data) */
    if (g_access.sensory > SENSORY_HIGH_CONTRAST)
        g_access.sensory = SENSORY_STANDARD;
    if (g_access.density > DENSITY_COMPACT)
        g_access.density = DENSITY_STANDARD;
    if (g_access.scheme > SCHEME_AUTO)
        g_access.scheme = SCHEME_DARK;

    g_access.letter_spacing = clamp_int(g_access.letter_spacing, 0, 4);
    g_access.word_spacing   = clamp_int(g_access.word_spacing, 0, 8);
    g_access.line_spacing   = clamp_int(g_access.line_spacing, 0, 8);
    g_access.reduced_motion = g_access.reduced_motion ? 1 : 0;
    g_access.anim_speed     = clamp_float(g_access.anim_speed, 0.0f, 2.0f);
    g_access.focus_mode     = g_access.focus_mode ? 1 : 0;
    g_access.night_shift    = g_access.night_shift ? 1 : 0;
    g_access.min_touch_target = clamp_int(g_access.min_touch_target, 32, 64);

    /* Apply sensory mode side-effects */
    apply_sensory_mode(g_access.sensory);

    g_initialized = 1;
}

/* ── Save ── */

void access_save(void)
{
    vault_save_config(ACCESS_VAULT_KEY, &g_access,
                      (uint32_t)sizeof(access_config_t));
}

/* ── Get ── */

access_config_t *access_get(void)
{
    return &g_access;
}

/* ── Setters ── */

void access_set_sensory(sensory_mode_t mode)
{
    if (mode > SENSORY_HIGH_CONTRAST)
        return;

    g_access.sensory = mode;
    apply_sensory_mode(mode);
    access_save();
}

void access_set_density(density_mode_t mode)
{
    if (mode > DENSITY_COMPACT)
        return;

    g_access.density = mode;

    /* Re-layout to the new density: panel height (48/40/32) live. */
    { extern void panel_set_height(int); extern void compositor_set_panel_h(int);
      int ph = access_get_panel_height();
      panel_set_height(ph);
      compositor_set_panel_h(ph); }

    /* Mark everything dirty — control sizes change globally */
    compositor_dirty_all();
    access_save();
}

void access_set_scheme(color_scheme_t scheme)
{
    if (scheme > SCHEME_AUTO)
        return;

    g_access.scheme = scheme;
    compositor_dirty_all();
    access_save();
}

void access_set_reduced_motion(int on)
{
    g_access.reduced_motion = on ? 1 : 0;
    access_save();
}

void access_set_anim_speed(float speed)
{
    g_access.anim_speed = clamp_float(speed, 0.0f, 2.0f);
    access_save();
}

void access_set_focus_mode(int on)
{
    g_access.focus_mode = on ? 1 : 0;
    access_save();
}

/* ── Density queries ── */

int access_get_control_height(void)
{
    return density_control_height[g_access.density];
}

int access_get_panel_height(void)
{
    return density_panel_height[g_access.density];
}

int access_get_icon_size(void)
{
    return density_icon_size[g_access.density];
}
