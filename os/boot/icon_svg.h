/*
 * Zeos -- D.9 build-time-rasterized icons.
 *
 * Icons live as SVGs in assets/icons/. The Makefile rasterizes selected ones to
 * 48x48 RGBA PNGs at build time (rsvg-convert) and objcopy-embeds them. Each is
 * a black glyph on transparent -- an ALPHA MASK. At runtime we lodepng-decode
 * once (cached) and blit tinted with the caller's accent, so icons pick up the
 * active persona accent (the "persona-tinted" desktop icons of D.9).
 */
#ifndef ZEOS_ICON_SVG_H
#define ZEOS_ICON_SVG_H

#include <stdint.h>

/* Draw an embedded icon PNG (alpha mask) tinted with `accent` (0x00RRGGBB),
 * nearest-scaled into a size x size box at (x,y). Decodes once, then caches. */
void icon_svg_draw(const unsigned char *png, unsigned long len,
                   int x, int y, int size, uint32_t accent);

/* Map a desktop/app name to its embedded icon PNG. Returns NULL (and leaves
 * *len untouched) when there's no icon -- callers fall back to initials. */
const unsigned char *icon_svg_for_name(const char *name, unsigned long *len);

#endif /* ZEOS_ICON_SVG_H */
