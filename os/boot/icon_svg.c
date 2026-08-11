/*
 * Zeos -- D.9 build-time-rasterized, persona-tinted icons. See icon_svg.h.
 */
#include "icon_svg.h"
#include "fb.h"
#include "lodepng/lodepng.h"

/* objcopy-embedded 48x48 RGBA PNGs (see Makefile ICON_OBJS). */
extern const unsigned char _binary_icon_folder_png_start[];
extern const unsigned char _binary_icon_folder_png_end[];
extern const unsigned char _binary_icon_code_png_start[];
extern const unsigned char _binary_icon_code_png_end[];
extern const unsigned char _binary_icon_gear_png_start[];
extern const unsigned char _binary_icon_gear_png_end[];

/* ── decode-once cache (icons persist for the session; never freed) ── */
#define ICON_CACHE_MAX 16
typedef struct {
    const unsigned char *key;
    const uint8_t       *rgba;   /* lodepng RGBA, row-major, 4 bytes/px */
    unsigned             w, h;
} icon_cache_t;
static icon_cache_t s_cache[ICON_CACHE_MAX];
static int          s_cache_n;

static const uint8_t *decode_cached(const unsigned char *png, unsigned long len,
                                    unsigned *w, unsigned *h)
{
    for (int i = 0; i < s_cache_n; i++)
        if (s_cache[i].key == png) { *w = s_cache[i].w; *h = s_cache[i].h; return s_cache[i].rgba; }

    unsigned char *pixels = 0; unsigned pw = 0, ph = 0;
    if (lodepng_decode32(&pixels, &pw, &ph, png, (size_t)len) != 0 || !pixels)
        return 0;
    if (s_cache_n < ICON_CACHE_MAX) {
        s_cache[s_cache_n].key = png; s_cache[s_cache_n].rgba = pixels;
        s_cache[s_cache_n].w = pw; s_cache[s_cache_n].h = ph; s_cache_n++;
    }
    *w = pw; *h = ph;
    return pixels;
}

void icon_svg_draw(const unsigned char *png, unsigned long len,
                   int x, int y, int size, uint32_t accent)
{
    if (!png || len == 0 || size <= 0) return;
    unsigned w = 0, h = 0;
    const uint8_t *rgba = decode_cached(png, len, &w, &h);
    if (!rgba || !w || !h) return;

    uint32_t ar = (accent >> 16) & 0xFF, ag = (accent >> 8) & 0xFF, ab = accent & 0xFF;
    for (int py = 0; py < size; py++) {
        int sy = py * (int)h / size;
        if (sy < 0 || sy >= (int)h) continue;
        for (int px = 0; px < size; px++) {
            int sx = px * (int)w / size;
            if (sx < 0 || sx >= (int)w) continue;
            uint8_t a = rgba[((unsigned)sy * w + (unsigned)sx) * 4 + 3];  /* alpha mask */
            if (a == 0) continue;
            fb_pixel_blend(x + px, y + py, ((uint32_t)a << 24) | (ar << 16) | (ag << 8) | ab);
        }
    }
}

const unsigned char *icon_svg_for_name(const char *name, unsigned long *len)
{
    if (!name) return 0;
#define ICON(sym) do { \
        *len = (unsigned long)(_binary_##sym##_end - _binary_##sym##_start); \
        return _binary_##sym##_start; } while (0)
    switch (name[0]) {
    case 'F': case 'f': ICON(icon_folder_png);   /* Files / Folder */
    case 'T': case 't': ICON(icon_code_png);     /* Terminal */
    case 'S': case 's': ICON(icon_gear_png);     /* Settings */
    default: return 0;                            /* fall back to initials */
    }
#undef ICON
}
