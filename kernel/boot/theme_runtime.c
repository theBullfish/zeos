/*
 * Zeos -- Runtime Theme Resolution
 *
 * Returns the correct color for the active scheme (dark/light/auto).
 * AUTO uses a simple TSC-based heuristic: assume boot = morning,
 * flip to dark after ~12 hours of uptime. Crude but functional
 * until we have RTC or NTP.
 */

#include "theme_runtime.h"
#include "theme.h"
#include "access.h"
#include "timer.h"
#include "fb.h"
#include "persona.h"
#include "persona_filter.h"

/* ── AUTO heuristic ── */

/*
 * For SCHEME_AUTO: light during "daytime" hours.
 * Without RTC we estimate from TSC uptime.
 * Assume boot happens in the morning — light for first 12h,
 * dark for next 12h, repeat. This is a placeholder until
 * we have real time-of-day from NTP or RTC.
 */
static int auto_is_light(void)
{
    uint64_t freq = timer_tsc_freq();
    if (freq == 0)
        return 1;  /* No freq calibrated — default light */

    uint64_t now = timer_read_tsc();
    uint64_t uptime_sec = now / freq;
    uint64_t day_cycle  = uptime_sec % (24 * 3600);

    /* Light for hours 0-12 of each 24h cycle from boot */
    return day_cycle < (12 * 3600);
}

/* ── Resolve active scheme to dark (0) or light (1) ── */

static int is_light_active(void)
{
    access_config_t *cfg = access_get();
    switch (cfg->scheme) {
    case SCHEME_LIGHT: return 1;
    case SCHEME_DARK:  return 0;
    case SCHEME_AUTO:  return auto_is_light();
    default:           return 0;
    }
}

/* ── Color queries ── */

uint32_t theme_surface(void)
{
    return is_light_active() ? COLOR_LIGHT_SURFACE : COLOR_SURFACE;
}

uint32_t theme_surface_high(void)
{
    return is_light_active() ? COLOR_LIGHT_SURFACE_HIGH : COLOR_SURFACE_HIGH;
}

uint32_t theme_surface_top(void)
{
    return is_light_active() ? COLOR_LIGHT_SURFACE_TOP : COLOR_SURFACE_TOP;
}

uint32_t theme_on_surface(void)
{
    return is_light_active() ? COLOR_LIGHT_ON_SURFACE : COLOR_ON_SURFACE;
}

uint32_t theme_on_surface_2(void)
{
    return is_light_active() ? COLOR_LIGHT_ON_SURFACE_2 : COLOR_ON_SURFACE_2;
}

uint32_t theme_on_surface_3(void)
{
    return is_light_active() ? COLOR_LIGHT_ON_SURFACE_3 : COLOR_ON_SURFACE_3;
}

uint32_t theme_on_surface_4(void)
{
    return is_light_active() ? COLOR_LIGHT_ON_SURFACE_4 : COLOR_ON_SURFACE_4;
}

uint32_t theme_separator(void)
{
    return is_light_active() ? COLOR_LIGHT_SEPARATOR : COLOR_SEPARATOR;
}

/* ── Active persona accent ── */

uint32_t theme_active_accent(void)
{
    switch (persona_get_active()) {
    case PERSONA_ZEROS: return COLOR_ZEROS_ACCENT;
    case PERSONA_DEREZ: return COLOR_DEREZ_ACCENT;
    case PERSONA_FULL:  return COLOR_FULL_ACCENT;
    }
    return COLOR_FULL_ACCENT;
}

uint32_t theme_active_accent_dim(void)
{
    switch (persona_get_active()) {
    case PERSONA_ZEROS: return COLOR_ZEROS_DIM;
    case PERSONA_DEREZ: return COLOR_DEREZ_DIM;
    case PERSONA_FULL:  return COLOR_FULL_DIM;
    }
    return COLOR_FULL_DIM;
}

/* ── Scheme control ── */

void theme_set_scheme(color_scheme_t scheme)
{
    access_set_scheme(scheme);
}

color_scheme_t theme_get_scheme(void)
{
    return access_get()->scheme;
}

/* ── Night shift ── */

/*
 * Warm tint overlay: 0x10FF8800 = ~6% opacity amber.
 * Single fb_rect_blend call over the entire screen.
 * Called after all layers, before cursor.
 */
#define NIGHT_SHIFT_TINT  0x10FF8800

void theme_apply_night_shift(void)
{
    access_config_t *cfg = access_get();
    if (!cfg->night_shift)
        return;

    uint32_t w = fb_width();
    uint32_t h = fb_height();
    if (w == 0 || h == 0)
        return;

    fb_rect_blend(0, 0, (int)w, (int)h, NIGHT_SHIFT_TINT);
}
