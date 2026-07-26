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

#ifdef ZEOS_DIAG_M4
#include "kprint.h"
/*
 * M.4 selftest: prove the sensory-mode consumer layer actually transforms a
 * known accent color and the geometry queries respond, across all three modes.
 * Measured (real access_apply_sensory over the real COLOR_FULL_ACCENT constant)
 * and printed to serial so the transform is observable against a stable
 * baseline (the STANDARD passthrough must equal the input).
 */
void access_m4_selftest(void)
{
    sensory_mode_t saved = g_access.sensory;
    uint32_t in = COLOR_FULL_ACCENT;

    g_access.sensory = SENSORY_STANDARD;
    uint32_t std  = access_apply_sensory(in);
    g_access.sensory = SENSORY_LOW_STIMULI;
    uint32_t low  = access_apply_sensory(in);
    int      low_border = access_border_width();
    int      low_deco   = access_decorative_anims_enabled();
    g_access.sensory = SENSORY_HIGH_CONTRAST;
    uint32_t high = access_apply_sensory(in);
    uint32_t htxt = access_text_color(0x00223344u);
    int      high_border = access_border_width();

    g_access.sensory = saved;

    /* STANDARD must be a pure passthrough (the stable baseline). LOW_STIMULI
     * must actually change the color (muted). HIGH_CONTRAST text must be white. */
    int pass = (std == in) && (low != in) && (htxt == 0xFFFFFFFFu) &&
               (low_border == 1) && (high_border == 2) && (low_deco == 0);

    kputs("[M4] accent in=");        kput_hex(in);
    kputs(" std=");                  kput_hex(std);
    kputs(" low=");                  kput_hex(low);
    kputs(" high=");                 kput_hex(high);
    kputs(" htext=");                kput_hex(htxt);
    kputs(" border(low/high)=");     kput_dec((uint64_t)low_border);
    kputs("/");                      kput_dec((uint64_t)high_border);
    kputs(" deco(low)=");            kput_dec((uint64_t)low_deco);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
}
#endif /* ZEOS_DIAG_M4 */

#ifdef ZEOS_DIAG_D5
#include "kprint.h"
/*
 * D.5 selftest: prove panel height follows density live (48/40/32 for
 * COMFORTABLE/STANDARD/COMPACT). Drives the real access_set_density() and reads
 * back both the density->height map and the panel's actually-applied height,
 * proving the mapping is wired end-to-end (not just an init param). Restores
 * the original density. Measured + printed to serial (observed on real boot).
 */
void access_d5_selftest(void)
{
    extern int panel_get_height(void);
    density_mode_t saved = g_access.density;
    static const int expect[3] = { 48, 40, 32 };  /* COMFORTABLE/STANDARD/COMPACT */
    int pass = 1;

    kputs("[D5]");
    for (int d = 0; d <= DENSITY_COMPACT; d++) {
        access_set_density((density_mode_t)d);
        int mapped = access_get_panel_height();
        int live   = panel_get_height();
        if (mapped != expect[d] || live != expect[d]) pass = 0;
        kputs(" d="); kput_dec((uint64_t)d);
        kputs(":map="); kput_dec((uint64_t)mapped);
        kputs(",live="); kput_dec((uint64_t)live);
    }

    access_set_density(saved);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
}
#endif /* ZEOS_DIAG_D5 */

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

#ifdef ZEOS_DIAG_M4_FORCE_LOW
    /* M.4 render observation: force LOW_STIMULI so the panel accent renders
     * muted for a screenshot diff vs STANDARD. Not persisted. */
    g_access.sensory = SENSORY_LOW_STIMULI;
#endif
#ifdef ZEOS_DIAG_M4_FORCE_HIGH
    g_access.sensory = SENSORY_HIGH_CONTRAST;
#endif

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

/* ── Sensory-mode runtime color/geometry transforms (M.4 consumers) ──
 *
 * The theme palette is compile-time (theme.h COLOR_* constants), so the
 * sensory modes are applied by running these transforms at the color source
 * (accent getters, text getters) and geometry source (border width, spring
 * stiffness). This is the consumer layer the design in apply_sensory_mode()
 * referred to — centralised here so callers just pipe their color through.
 *
 * ARGB byte order is 0xAARRGGBB; alpha is preserved by every transform. */

static int clamp_byte(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return v;
}

/* Rec.601-ish luminance, integer (r*0.30 + g*0.59 + b*0.11). */
static uint32_t luma601(uint32_t r, uint32_t g, uint32_t b)
{
    return (r * 77u + g * 150u + b * 29u) >> 8;
}

/*
 * Apply the active sensory mode to an accent/decorative color.
 *   STANDARD:      unchanged.
 *   LOW_STIMULI:   mix 50% toward luminance -> 50% saturation (muted).
 *   HIGH_CONTRAST: push away from luminance (~1.4x sat) so accents read
 *                  hard against the ground.
 */
uint32_t access_apply_sensory(uint32_t argb)
{
    uint32_t a = (argb >> 24) & 0xFFu;
    int r = (int)((argb >> 16) & 0xFFu);
    int g = (int)((argb >>  8) & 0xFFu);
    int b = (int)( argb        & 0xFFu);

    switch (g_access.sensory) {
    case SENSORY_LOW_STIMULI: {
        int l = (int)luma601((uint32_t)r, (uint32_t)g, (uint32_t)b);
        r = (r + l) >> 1;
        g = (g + l) >> 1;
        b = (b + l) >> 1;
        break;
    }
    case SENSORY_HIGH_CONTRAST: {
        int l = (int)luma601((uint32_t)r, (uint32_t)g, (uint32_t)b);
        r = clamp_byte(l + ((r - l) * 7) / 5);
        g = clamp_byte(l + ((g - l) * 7) / 5);
        b = clamp_byte(l + ((b - l) * 7) / 5);
        break;
    }
    case SENSORY_STANDARD:
    default:
        return argb;
    }

    return (a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/*
 * Text color under the active sensory mode. HIGH_CONTRAST forces pure white
 * (opaque) for maximum legibility; other modes return the caller's color
 * unchanged.
 */
uint32_t access_text_color(uint32_t normal)
{
    if (g_access.sensory == SENSORY_HIGH_CONTRAST)
        return 0xFFFFFFFFu;
    return normal;
}

/* Border/separator width: 2px under HIGH_CONTRAST, 1px otherwise. */
int access_border_width(void)
{
    return (g_access.sensory == SENSORY_HIGH_CONTRAST) ? 2 : 1;
}

/* Decorative (non-essential) animations are suppressed under LOW_STIMULI. */
int access_decorative_anims_enabled(void)
{
    return (g_access.sensory == SENSORY_LOW_STIMULI) ? 0 : 1;
}

/* Spring stiffness multiplier: gentler (0.5x) motion under LOW_STIMULI. */
float access_spring_stiffness_scale(void)
{
    return (g_access.sensory == SENSORY_LOW_STIMULI) ? 0.5f : 1.0f;
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

/* Minimum interactive touch-target size (px). Hit-testing consumers expand a
 * control's clickable catch-zone toward this so small glyphs stay easy to hit
 * (M.5). Clamped 32..64 at load. */
int access_min_touch_target(void)
{
    return g_access.min_touch_target;
}
