/*
 * Zeos — Font Rendering System
 *
 * Uses stb_truetype for TTF rasterization.
 * Glyphs are rendered to grayscale bitmaps, cached, then
 * blended onto the framebuffer with the requested color.
 *
 * stb_truetype is public domain (Unlicense / MIT).
 */

/* stb_truetype implementation (single compilation unit) */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
/* stb_truetype needs these from our platform shim */
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);
extern unsigned long strlen(const char *s);
/* stb uses assert — stub it for bare-metal */
#define assert(x) ((void)0)
/* stb uses math functions — provide minimal versions */
static double stbtt_floor(double x) { return (double)(long)x - (x < 0 && x != (long)x); }
static double stbtt_ceil(double x) { return (double)(long)x + (x > 0 && x != (long)x); }
static double stbtt_fabs(double x) { return x < 0 ? -x : x; }
/* Minimal sqrt via Newton's method */
static double stbtt_sqrt(double x) {
    if (x <= 0) return 0;
    double g = x / 2;
    for (int i = 0; i < 20; i++) g = (g + x / g) / 2;
    return g;
}
#define STBTT_ifloor(x)  ((int)stbtt_floor(x))
#define STBTT_iceil(x)   ((int)stbtt_ceil(x))
#define STBTT_fabs(x)    stbtt_fabs(x)
#define STBTT_sqrt(x)    stbtt_sqrt(x)
#define STBTT_pow(x,y)   stbtt_pow(x,y)
/* Minimal pow for stb (only used for gamma, which we skip) */
static double stbtt_pow(double x, double y) {
    (void)x; (void)y;
    return 1.0; /* Gamma not needed for grayscale AA */
}
/* Forward-declare heap functions so stb_truetype can use them.
 * Must match heap.h signatures (uint64_t = unsigned long on x86_64). */
#include <stdint.h>
#include "access.h"
void *kmalloc(uint64_t size);
void  kfree(void *ptr);
#define STBTT_malloc(x,u)  kmalloc(x)
#define STBTT_free(x,u)    kfree(x)

#include "../lib/stb/stb_truetype.h"

#include "font.h"
#include "font_data.h"
#include "fb.h"
#include "heap.h"
#include "theme.h"
#include "kprint.h"

/* ── Embedded font data ──
 *
 * TTF files are linked into the binary via objcopy (see Makefile).
 * Symbols _binary_inter_regular_ttf_start etc. are declared in font_data.h.
 */

/* Font data pointers (set during font_init) */
static const uint8_t *font_data[FONT_COUNT];
static stbtt_fontinfo  font_info[FONT_COUNT];
static int             font_loaded[FONT_COUNT];

/* ── Glyph cache ── */

#define GLYPH_CACHE_SIZE 256

static glyph_t glyph_cache[GLYPH_CACHE_SIZE];
static int     glyph_cache_count = 0;

void font_cache_clear(void) {
    for (int i = 0; i < glyph_cache_count; i++) {
        if (glyph_cache[i].bitmap)
            kfree(glyph_cache[i].bitmap);
    }
    glyph_cache_count = 0;
}

/* Look up a cached glyph */
static glyph_t *cache_find(font_id_t font, int size_px, uint32_t codepoint) {
    for (int i = 0; i < glyph_cache_count; i++) {
        glyph_t *g = &glyph_cache[i];
        if (g->font == font && g->size_px == size_px && g->codepoint == codepoint)
            return g;
    }
    return 0;
}

/* Render and cache a glyph */
static glyph_t *cache_render(font_id_t font, int size_px, uint32_t codepoint) {
    if (!font_loaded[font]) return 0;
    if (glyph_cache_count >= GLYPH_CACHE_SIZE) {
        /* Evict oldest half */
        for (int i = 0; i < GLYPH_CACHE_SIZE / 2; i++) {
            if (glyph_cache[i].bitmap) kfree(glyph_cache[i].bitmap);
        }
        int keep = GLYPH_CACHE_SIZE - GLYPH_CACHE_SIZE / 2;
        for (int i = 0; i < keep; i++)
            glyph_cache[i] = glyph_cache[i + GLYPH_CACHE_SIZE / 2];
        glyph_cache_count = keep;
    }

    stbtt_fontinfo *info = &font_info[font];
    float scale = stbtt_ScaleForPixelHeight(info, (float)size_px);

    int w, h, xoff, yoff;
    uint8_t *bitmap = stbtt_GetCodepointBitmap(info, 0, scale,
                                                (int)codepoint, &w, &h, &xoff, &yoff);
    if (!bitmap) return 0;

    /* Copy to our heap (stb uses our kmalloc, but we want to track it) */
    int bmp_size = w * h;
    uint8_t *our_bmp = (uint8_t *)kmalloc((uint64_t)bmp_size);
    if (!our_bmp) {
        stbtt_FreeBitmap(bitmap, 0);
        return 0;
    }
    for (int i = 0; i < bmp_size; i++) our_bmp[i] = bitmap[i];
    stbtt_FreeBitmap(bitmap, 0);

    /* Get advance width */
    int advance, lsb;
    stbtt_GetCodepointHMetrics(info, (int)codepoint, &advance, &lsb);

    glyph_t *g = &glyph_cache[glyph_cache_count++];
    g->bitmap = our_bmp;
    g->w = w;
    g->h = h;
    g->xoff = xoff;
    g->yoff = yoff;
    g->advance = (int)((float)advance * scale);
    g->codepoint = codepoint;
    g->size_px = size_px;
    g->font = font;

    return g;
}

/* ── Public API ── */

int font_init(void) {
    kputs("FONT: initializing TTF system\n");

    /* Mark all as unloaded */
    for (int i = 0; i < FONT_COUNT; i++) {
        font_data[i] = 0;
        font_loaded[i] = 0;
    }

    /* Boot font is always "loaded" */
    font_loaded[FONT_BOOT] = 1;

    /* ── Load embedded TTF data ── */

    /* Inter Regular -> FONT_UI */
    font_data[FONT_UI] = _binary_inter_regular_ttf_start;
    if (stbtt_InitFont(&font_info[FONT_UI], font_data[FONT_UI], 0)) {
        font_loaded[FONT_UI] = 1;
        kputs("FONT: Inter Regular loaded\n");
    } else {
        kputs("FONT: WARN - Inter Regular failed to parse\n");
    }

    /* JetBrains Mono Regular -> FONT_CODE */
    font_data[FONT_CODE] = _binary_jbmono_regular_ttf_start;
    if (stbtt_InitFont(&font_info[FONT_CODE], font_data[FONT_CODE], 0)) {
        font_loaded[FONT_CODE] = 1;
        kputs("FONT: JetBrains Mono Regular loaded\n");
    } else {
        kputs("FONT: WARN - JetBrains Mono Regular failed to parse\n");
    }

    kputs("FONT: TTF engine ready (stb_truetype)\n");
    return 0;
}

const glyph_t *font_get_glyph(font_id_t font, int size_px, uint32_t codepoint) {
    glyph_t *g = cache_find(font, size_px, codepoint);
    if (g) return g;
    return cache_render(font, size_px, codepoint);
}

int font_draw(int x, int y, const char *text, font_id_t font,
              int size_px, uint32_t color)
{
    /* Fallback to boot font if TTF not loaded */
    if (!font_loaded[font] || font == FONT_BOOT) {
        fb_text(x, y, text, color);
        return x + (int)strlen(text) * 8;
    }

    int cx = x;
    int lsp = access_get()->letter_spacing;   /* M.6: accessibility spacing */
    int wsp = access_get()->word_spacing;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g_c = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    while (*text) {
        uint32_t cp = (uint32_t)(uint8_t)*text++;  /* ASCII for now */

        const glyph_t *glyph = font_get_glyph(font, size_px, cp);
        if (!glyph) {
            cx += size_px / 2;  /* Skip unknown chars */
            continue;
        }

        /* Blend glyph bitmap onto framebuffer */
        int gx = cx + glyph->xoff;
        int gy = y + size_px + glyph->yoff;  /* Baseline offset */

        for (int row = 0; row < glyph->h; row++) {
            for (int col = 0; col < glyph->w; col++) {
                uint8_t alpha = glyph->bitmap[row * glyph->w + col];
                if (alpha == 0) continue;

                /* Alpha blend with persona color */
                uint32_t blended = ((uint32_t)alpha << 24) |
                                   ((uint32_t)r << 16) |
                                   ((uint32_t)g_c << 8) |
                                   (uint32_t)b;

                fb_pixel_blend(gx + col, gy + row, blended);
            }
        }

        cx += glyph->advance + lsp + (cp == ' ' ? wsp : 0);   /* M.6 spacing */
    }

    return cx;
}

int font_measure(const char *text, font_id_t font, int size_px) {
    if (!font_loaded[font] || font == FONT_BOOT) {
        return (int)strlen(text) * 8;
    }

    int width = 0;
    while (*text) {
        uint32_t cp = (uint32_t)(uint8_t)*text++;
        const glyph_t *glyph = font_get_glyph(font, size_px, cp);
        if (glyph)
            width += glyph->advance;
        else
            width += size_px / 2;
    }
    return width;
}

int font_line_height(font_id_t font, int size_px) {
    if (!font_loaded[font] || font == FONT_BOOT)
        return 16;  /* Boot font height */

    stbtt_fontinfo *info = &font_info[font];
    float scale = stbtt_ScaleForPixelHeight(info, (float)size_px);

    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);

    return (int)((float)(ascent - descent + line_gap) * scale);
}
