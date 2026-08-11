/*
 * Zeos — Design Tokens
 *
 * Central file for all design constants: grid units, layout metrics,
 * spring physics, type scale, interaction states, shadows, colors.
 *
 * Fractal grid: base unit z = 4px. Everything snaps to z multiples.
 * Colors are 0xAARRGGBB format (alpha-premultiplied where applicable).
 *
 * Placeholders marked until Brad picks the final palette.
 */

#ifndef ZEOS_THEME_H
#define ZEOS_THEME_H

#include <stdint.h>

/* ── Fractal Grid (base unit z = 4px) ── */
#define Z_UNIT       4
#define Z1           4
#define Z2           8
#define Z3          12
#define Z4          16
#define Z6          24
#define Z8          32
#define Z12         48
#define Z16         64

/* ── Layout metrics ── */
#define TOOLBAR_HEIGHT    Z12
#define SIDEBAR_WIDTH     256
#define CONTENT_PADDING   Z4
#define CONTROL_HEIGHT_SM Z6
#define CONTROL_HEIGHT    Z8
#define ICON_SM           Z4
#define ICON_MD           Z6
#define ICON_LG           Z8
#define BORDER_RADIUS_SM  Z1
#define BORDER_RADIUS     Z2
#define BORDER_RADIUS_LG  Z3
#define SEPARATOR_PX      1

/* ── Spring physics constants ── */
#define SPRING_STIFFNESS   120.0f
#define SPRING_DAMPING      14.0f
#define SPRING_MASS          1.0f
#define SPRING_SETTLE_THRESHOLD 0.5f  /* pixels - stop when closer than this */

/* Spring presets (stiffness, damping pairs) */
#define SPRING_SNAPPY_S    200.0f
#define SPRING_SNAPPY_D     20.0f
#define SPRING_SMOOTH_S    100.0f
#define SPRING_SMOOTH_D     16.0f
#define SPRING_BOUNCY_S    120.0f
#define SPRING_BOUNCY_D      8.0f
#define SPRING_INTERACTIVE_S 400.0f
#define SPRING_INTERACTIVE_D  28.0f

/* ── Type scale (sizes in pixels) ── */
#define TYPE_DISPLAY     32
#define TYPE_TITLE       24
#define TYPE_HEADING     20
#define TYPE_BODY        16
#define TYPE_LABEL       14
#define TYPE_CAPTION     12
#define TYPE_MICRO       10

/* ── Interaction state opacity multipliers (0-255) ── */
#define STATE_IDLE        0     /* no overlay */
#define STATE_HOVER      20     /* ~8% overlay */
#define STATE_PRESSED    41     /* ~16% overlay */
#define STATE_FOCUSED   255     /* ring, not overlay */
#define STATE_DISABLED   97     /* ~38% opacity */

/* ── Text opacity hierarchy (alpha 0-255) ── */
#define TEXT_PRIMARY     224    /* ~88% */
#define TEXT_SECONDARY   158    /* ~62% */
#define TEXT_TERTIARY    102    /* ~40% */
#define TEXT_QUATERNARY   64    /* ~25% */

/* ── Elevation shadow params ── */
typedef struct {
    int offset_y;
    int blur_radius;
    uint8_t opacity;  /* 0-255 */
} shadow_t;

static const shadow_t SHADOW_NONE  = {0, 0, 0};
static const shadow_t SHADOW_L1    = {2, 8, 38};    /* windows */
static const shadow_t SHADOW_L2    = {4, 16, 51};   /* panels */
static const shadow_t SHADOW_L3    = {6, 24, 64};   /* sheets */
static const shadow_t SHADOW_L4    = {8, 32, 77};   /* alerts */
static const shadow_t SHADOW_L5    = {10, 40, 89};  /* notifications */

/* ── Color Palette ── */
/* Colors are 0xAARRGGBB format.
 *
 * Brad's palette (March 25, 2026):
 *   Zeros:  #09BC8A  hue 163°  teal-green (physical, robotics)
 *   DereZ:  #7A306C  hue 311°  magenta-purple (digital, code)
 *   Full:   #2E86AB  hue 198°  steel blue (neutral, complete)
 *
 * One accent per persona. Everything else derived via opacity math:
 *   - Text: white at 88% / 62% / 40% / 25%
 *   - Hover: accent at 8%
 *   - Pressed: accent at 16%
 *   - Selected: accent at 24%
 *   - Danger: always red, never derived from accent
 */

/* ── Persona accent colors (the ONE color each) ── */
#define COLOR_ZEROS_ACCENT   0xFF09BC8A   /* #09BC8A — teal-green */
#define COLOR_DEREZ_ACCENT   0xFF7A306C   /* #7A306C — magenta-purple */
#define COLOR_FULL_ACCENT    0xFF2E86AB   /* #2E86AB — steel blue */

/* ── Persona accent dimmed (35% lightness, 50% saturation) ── */
#define COLOR_ZEROS_DIM      0xFF067A5A   /* darkened teal */
#define COLOR_DEREZ_DIM      0xFF4E1E44   /* darkened magenta */
#define COLOR_FULL_DIM       0xFF1D5670   /* darkened blue */

/* ── Active persona colors (default to Full, shell switches these) ── */
#define COLOR_PRIMARY        COLOR_FULL_ACCENT
#define COLOR_PRIMARY_DIM    COLOR_FULL_DIM

/* ── Surfaces (dark background, persona-tinted) ── */
#define COLOR_SURFACE        0xFF0D1117   /* near-black with slight blue */
#define COLOR_SURFACE_HIGH   0xFF161B22   /* raised surface */
#define COLOR_SURFACE_TOP    0xFF21262D   /* highest elevation */

/* ── Text (white at opacity tiers) ── */
#define COLOR_ON_SURFACE     0xFFE0E4E8   /* primary text — 88% */
#define COLOR_ON_SURFACE_2   0xFF9EA3A8   /* secondary — 62% */
#define COLOR_ON_SURFACE_3   0xFF656A6E   /* tertiary — 40% */
#define COLOR_ON_SURFACE_4   0xFF3F4347   /* quaternary — 25% */

/* ── Semantic (never change with persona) ── */
#define COLOR_DANGER         0xFFE5534B   /* red — destructive actions */
#define COLOR_WARNING        0xFFD4A72C   /* amber — caution */
#define COLOR_SUCCESS        0xFF09BC8A   /* uses Zeros teal — confirmation */
#define COLOR_SEPARATOR      0xFF21262D   /* matches surface_top */

/* ── Light Theme Surfaces ── */
#define COLOR_LIGHT_SURFACE       0xFFFAFAF8   /* warm off-white */
#define COLOR_LIGHT_SURFACE_HIGH  0xFFEFEFED   /* slightly darker */
#define COLOR_LIGHT_SURFACE_TOP   0xFFE4E4E2   /* highest elevation */

/* ── Light Theme Text ── */
#define COLOR_LIGHT_ON_SURFACE    0xFF333333   /* primary text — dark gray */
#define COLOR_LIGHT_ON_SURFACE_2  0xFF666666   /* secondary */
#define COLOR_LIGHT_ON_SURFACE_3  0xFF999999   /* tertiary */
#define COLOR_LIGHT_ON_SURFACE_4  0xFFBBBBBB   /* quaternary */

/* ── Light Theme Semantic ── */
#define COLOR_LIGHT_SEPARATOR     0xFFE4E4E2

/* ── TRISA decision (universal, never change) ── */
#define COLOR_TRISA_IS       0xFF09BC8A   /* signal green (Zeros teal) */
#define COLOR_TRISA_ISNT     0xFF484F58   /* signal gray */

/* ── Tier tints (subtle border/accent on sovereign/internal/reference) ── */
#define COLOR_TIER_SOVEREIGN 0xFFD4A72C   /* warm gold — private, sacred */
#define COLOR_TIER_INTERNAL  0xFF656A6E   /* neutral gray — your data */
#define COLOR_TIER_REFERENCE 0xFF2E86AB   /* cool blue — shared, public */

/* ── Persona seed hues (degrees, for runtime HSL derivation) ── */
#define PERSONA_ZEROS_HUE    163   /* #09BC8A */
#define PERSONA_DEREZ_HUE    311   /* #7A306C */
#define PERSONA_FULL_HUE     198   /* #2E86AB */

#endif /* ZEOS_THEME_H */
