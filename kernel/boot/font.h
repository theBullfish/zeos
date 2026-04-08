/*
 * Zeos — Font Rendering System
 *
 * Two-tier font system:
 *   1. Boot font: 8x16 bitmap (font8x16.h) — always available, zero deps
 *   2. UI font: TTF rendering via stb_truetype — Inter for UI, JetBrains Mono for code
 *
 * The boot font is used during early boot and as fallback.
 * Once heap is initialized, TTF fonts are loaded from embedded data.
 *
 * Rendering: grayscale antialiasing only (no subpixel — spec decision).
 * Glyphs are cached in a bitmap cache for performance.
 */

#ifndef ZEOS_FONT_H
#define ZEOS_FONT_H

#include <stdint.h>

/* ── Font IDs ── */
typedef enum {
    FONT_BOOT,           /* 8x16 bitmap, always available */
    FONT_UI,             /* Inter — UI text */
    FONT_UI_BOLD,        /* Inter Bold */
    FONT_UI_LIGHT,       /* Inter Light */
    FONT_CODE,           /* JetBrains Mono — code/terminal */
    FONT_CODE_BOLD,      /* JetBrains Mono Bold */
    FONT_COUNT
} font_id_t;

/* ── Glyph cache entry ── */
typedef struct {
    uint8_t *bitmap;     /* Grayscale bitmap (w * h bytes, 0-255) */
    int      w, h;       /* Bitmap dimensions */
    int      xoff, yoff; /* Offset from baseline */
    int      advance;    /* Horizontal advance (pixels) */
    uint32_t codepoint;  /* Unicode codepoint */
    int      size_px;    /* Font size this was rendered at */
    font_id_t font;      /* Which font */
} glyph_t;

/* ── API ── */

/*
 * Initialize the font system. Loads TTF data for Inter and JetBrains Mono.
 * Must be called after heap_init().
 */
int font_init(void);

/*
 * Render a string to the framebuffer.
 * x, y: top-left position
 * text: UTF-8 string
 * font: which font to use
 * size_px: font size in pixels (from theme.h TYPE_* constants)
 * color: 0xAARRGGBB
 *
 * Returns the x position after the last character (for cursor positioning).
 */
int font_draw(int x, int y, const char *text, font_id_t font,
              int size_px, uint32_t color);

/*
 * Measure text width without drawing.
 */
int font_measure(const char *text, font_id_t font, int size_px);

/*
 * Get line height for a font at a given size.
 */
int font_line_height(font_id_t font, int size_px);

/*
 * Get a single glyph (cached). Returns NULL if not renderable.
 */
const glyph_t *font_get_glyph(font_id_t font, int size_px, uint32_t codepoint);

/*
 * Clear the glyph cache (e.g., on font size change).
 */
void font_cache_clear(void);

#endif /* ZEOS_FONT_H */
