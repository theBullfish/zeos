/*
 * Zeos -- Runtime Theme Resolution
 *
 * Resolves dark/light colors based on the active color_scheme_t
 * from the accessibility config. For SCHEME_AUTO, uses an
 * approximate time-of-day heuristic from TSC uptime.
 *
 * All functions return 0xAARRGGBB colors matching theme.h defines.
 * Consumers should call these instead of using COLOR_SURFACE etc.
 * directly so that light theme and night shift work transparently.
 */

#ifndef ZEOS_THEME_RUNTIME_H
#define ZEOS_THEME_RUNTIME_H

#include <stdint.h>
#include "access.h"

/* ── Color queries (resolve dark vs light) ── */
uint32_t theme_surface(void);
uint32_t theme_surface_high(void);
uint32_t theme_surface_top(void);
uint32_t theme_on_surface(void);
uint32_t theme_on_surface_2(void);
uint32_t theme_on_surface_3(void);
uint32_t theme_on_surface_4(void);
uint32_t theme_separator(void);

/* ── Scheme control ── */
void           theme_set_scheme(color_scheme_t scheme);
color_scheme_t theme_get_scheme(void);

/* ── Night shift ── */

/*
 * Apply night shift warm tint to the entire framebuffer.
 * Call once per frame AFTER all layers are drawn, BEFORE cursor.
 * No-op if night_shift is disabled in access config.
 */
void theme_apply_night_shift(void);

#endif /* ZEOS_THEME_RUNTIME_H */
