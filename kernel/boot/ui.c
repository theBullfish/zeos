/*
 * Zeos — UI Primitive Helpers
 *
 * Stateful drawing using theme tokens. Each primitive reads the
 * interaction state and applies the correct overlay, color, or
 * ring. All rendering goes through fb.h — no direct framebuffer
 * access here.
 */

#include "ui.h"
#include "fb.h"
#include "theme.h"

/* String length — no stdlib */
static int ui_strlen(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    return len;
}

/*
 * Build a color with a given alpha channel (for overlays).
 * Base color has alpha replaced with the specified value.
 */
static uint32_t color_with_alpha(uint32_t color, uint8_t alpha)
{
    return ((uint32_t)alpha << 24) | (color & 0x00FFFFFF);
}

void ui_button(int x, int y, int w, int h, const char *label, enum ui_state state)
{
    uint32_t bg;
    uint32_t fg;
    uint8_t text_alpha;

    /* Base colors */
    bg = COLOR_SURFACE_HIGH;
    fg = COLOR_ON_SURFACE;
    text_alpha = TEXT_PRIMARY;

    /* State-specific adjustments */
    switch (state) {
    case UI_DISABLED:
        text_alpha = STATE_DISABLED;
        break;
    case UI_FOCUSED:
        /* Focused: draw ring after, no bg change */
        break;
    case UI_HOVER:
    case UI_PRESSED:
    case UI_IDLE:
        break;
    }

    /* Draw button background */
    fb_rect(x, y, w, h, bg);

    /* Border */
    fb_rect_outline(x, y, w, h, COLOR_SEPARATOR, 1);

    /* State overlays */
    switch (state) {
    case UI_HOVER:
        fb_rect_blend(x, y, w, h,
                      color_with_alpha(COLOR_ON_SURFACE, STATE_HOVER));
        break;
    case UI_PRESSED:
        fb_rect_blend(x, y, w, h,
                      color_with_alpha(COLOR_ON_SURFACE, STATE_PRESSED));
        break;
    case UI_FOCUSED:
        ui_draw_focus_ring(x, y, w, h);
        break;
    case UI_DISABLED:
    case UI_IDLE:
        break;
    }

    /* Center the label text */
    if (label) {
        int text_len = ui_strlen(label);
        int text_w = text_len * 8;   /* 8px per glyph */
        int text_h = 16;             /* 16px glyph height */
        int tx = x + (w - text_w) / 2;
        int ty = y + (h - text_h) / 2;

        /* Apply text alpha to foreground color */
        uint32_t label_color = (fg & 0x00FFFFFF) |
                               ((uint32_t)text_alpha << 24);

        /* Use alpha-blended text if not fully opaque */
        if (text_alpha == TEXT_PRIMARY) {
            /* Close enough to opaque — use direct text */
            fb_text(tx, ty, label, fg & 0x00FFFFFF);
        } else {
            /* Reduced opacity — blend */
            fb_text(tx, ty, label, label_color);
        }
    }
}

void ui_draw_focus_ring(int x, int y, int w, int h)
{
    /* 2px ring offset by 2px outside the element */
    int rx = x - 2;
    int ry = y - 2;
    int rw = w + 4;
    int rh = h + 4;

    fb_rect_outline(rx, ry, rw, rh, COLOR_PRIMARY, 2);
}

void ui_hover_overlay(int x, int y, int w, int h)
{
    fb_rect_blend(x, y, w, h,
                  color_with_alpha(COLOR_ON_SURFACE, STATE_HOVER));
}

void ui_separator_h(int x, int y, int w)
{
    fb_hline(x, y, w, COLOR_SEPARATOR);
}

void ui_separator_v(int x, int y, int h)
{
    fb_vline(x, y, h, COLOR_SEPARATOR);
}
