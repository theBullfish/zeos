/*
 * Zeos framebuffer console — bare metal text output.
 *
 * Uses an embedded 8x16 bitmap font. No dependencies.
 * After ExitBootServices, this is our only way to talk to the human.
 */

#include "fb.h"
#include "font8x16.h"

static struct zeos_framebuffer *g_fb;
static uint32_t cursor_x;  /* Character column */
static uint32_t cursor_y;  /* Character row */
static uint32_t max_cols;
static uint32_t max_rows;

/* Text colors — from design system */
#include "theme.h"
#define FG_COLOR  COLOR_ON_SURFACE   /* Primary text */
#define BG_COLOR  COLOR_SURFACE      /* Background */

void fb_init(struct zeos_framebuffer *fb)
{
    g_fb = fb;
    cursor_x = 0;
    cursor_y = 0;
    max_cols = fb->width / 8;
    max_rows = fb->height / 16;
}

void fb_clear(uint32_t color)
{
    if (!g_fb || !g_fb->base)
        return;

    for (uint32_t y = 0; y < g_fb->height; y++) {
        uint32_t *row = g_fb->base + y * g_fb->pitch;
        for (uint32_t x = 0; x < g_fb->width; x++) {
            row[x] = color;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

/*
 * Scroll the screen up by one text row (16 pixels).
 */
static void fb_scroll(void)
{
    if (!g_fb || !g_fb->base)
        return;

    /* Move everything up by 16 pixel rows */
    for (uint32_t y = 0; y < g_fb->height - 16; y++) {
        uint32_t *dst = g_fb->base + y * g_fb->pitch;
        uint32_t *src = g_fb->base + (y + 16) * g_fb->pitch;
        for (uint32_t x = 0; x < g_fb->width; x++) {
            dst[x] = src[x];
        }
    }

    /* Clear the bottom row */
    for (uint32_t y = g_fb->height - 16; y < g_fb->height; y++) {
        uint32_t *row = g_fb->base + y * g_fb->pitch;
        for (uint32_t x = 0; x < g_fb->width; x++) {
            row[x] = BG_COLOR;
        }
    }
}

/*
 * Draw a single glyph at pixel position (px, py).
 * Font is 8 wide, 16 tall, 1 bit per pixel, stored row-major.
 */
static void draw_glyph(uint32_t px, uint32_t py, uint8_t ch)
{
    if (!g_fb || !g_fb->base)
        return;

    const uint8_t *glyph = font8x16 + (uint32_t)ch * 16;

    for (uint32_t row = 0; row < 16; row++) {
        uint32_t y = py + row;
        if (y >= g_fb->height)
            break;
        uint32_t *pixel_row = g_fb->base + y * g_fb->pitch;
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < 8; col++) {
            uint32_t x = px + col;
            if (x >= g_fb->width)
                break;
            pixel_row[x] = (bits & (0x80 >> col)) ? FG_COLOR : BG_COLOR;
        }
    }
}

void fb_putc(char c)
{
    if (!g_fb)
        return;

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 4) & ~3;
    } else {
        draw_glyph(cursor_x * 8, cursor_y * 16, (uint8_t)c);
        cursor_x++;
    }

    /* Line wrap */
    if (cursor_x >= max_cols) {
        cursor_x = 0;
        cursor_y++;
    }

    /* Scroll if needed */
    if (cursor_y >= max_rows) {
        fb_scroll();
        cursor_y = max_rows - 1;
    }
}

void fb_puts(const char *s)
{
    while (*s)
        fb_putc(*s++);
}

void fb_put_hex(uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    int i;

    for (i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xf];
        val >>= 4;
    }
    buf[16] = '\0';

    /* Skip leading zeros but always print at least one digit */
    char *p = buf;
    while (*p == '0' && p < buf + 15)
        p++;
    fb_puts(p);
}

void fb_put_dec(uint64_t val)
{
    char buf[21];
    int i = 20;
    buf[i] = '\0';

    if (val == 0) {
        fb_putc('0');
        return;
    }

    while (val > 0) {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    }

    fb_puts(&buf[i]);
}

/* ── Pixel-level drawing primitives ─────────── */

uint32_t fb_width(void)
{
    return g_fb ? g_fb->width : 0;
}

uint32_t fb_height(void)
{
    return g_fb ? g_fb->height : 0;
}

uint64_t fb_phys_base(void)
{
    /* Identity-mapped: virtual == physical for the FB on UEFI boot. */
    return g_fb ? (uint64_t)(uintptr_t)g_fb->base : 0;
}

uint32_t fb_pitch_pixels(void)
{
    return g_fb ? g_fb->pitch : 0;
}

void fb_pixel(int x, int y, uint32_t color)
{
    if (!g_fb || !g_fb->base)
        return;
    if (x < 0 || y < 0 || (uint32_t)x >= g_fb->width || (uint32_t)y >= g_fb->height)
        return;
    g_fb->base[(uint32_t)y * g_fb->pitch + (uint32_t)x] = color;
}

void fb_rect(int x, int y, int w, int h, uint32_t color)
{
    if (!g_fb || !g_fb->base)
        return;

    /* Clip to screen bounds */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > (int)g_fb->width) x1 = (int)g_fb->width;
    if (y1 > (int)g_fb->height) y1 = (int)g_fb->height;

    for (int py = y0; py < y1; py++) {
        uint32_t *row = g_fb->base + (uint32_t)py * g_fb->pitch;
        for (int px = x0; px < x1; px++) {
            row[px] = color;
        }
    }
}

void fb_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness)
{
    if (thickness <= 0) thickness = 1;
    /* Top edge */
    fb_rect(x, y, w, thickness, color);
    /* Bottom edge */
    fb_rect(x, y + h - thickness, w, thickness, color);
    /* Left edge */
    fb_rect(x, y, thickness, h, color);
    /* Right edge */
    fb_rect(x + w - thickness, y, thickness, h, color);
}

void fb_hline(int x, int y, int w, uint32_t color)
{
    fb_rect(x, y, w, 1, color);
}

void fb_vline(int x, int y, int h, uint32_t color)
{
    fb_rect(x, y, 1, h, color);
}

void fb_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    /* Bresenham's line algorithm */
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1;
    int sy = dy > 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    int err = dx - dy;

    for (;;) {
        fb_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void fb_circle(int cx, int cy, int r, uint32_t color)
{
    /* Midpoint circle algorithm */
    int x = r;
    int y = 0;
    int d = 1 - r;

    while (x >= y) {
        fb_pixel(cx + x, cy + y, color);
        fb_pixel(cx - x, cy + y, color);
        fb_pixel(cx + x, cy - y, color);
        fb_pixel(cx - x, cy - y, color);
        fb_pixel(cx + y, cy + x, color);
        fb_pixel(cx - y, cy + x, color);
        fb_pixel(cx + y, cy - x, color);
        fb_pixel(cx - y, cy - x, color);
        y++;
        if (d <= 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

void fb_circle_filled(int cx, int cy, int r, uint32_t color)
{
    /* Fill by drawing horizontal lines for each row. The "+ r" bias rounds
     * the poles so small-radius dots read as smooth circles instead of
     * having a 1px point/spur at top and bottom. */
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while (dx * dx + dy * dy <= r * r + r)
            dx++;
        dx--;
        fb_hline(cx - dx, cy + dy, 2 * dx + 1, color);
    }
}

/* Draw a glyph at arbitrary pixel position with custom colors */
static void draw_glyph_at(int px, int py, uint8_t ch, uint32_t fg)
{
    if (!g_fb || !g_fb->base)
        return;

    const uint8_t *glyph = font8x16 + (uint32_t)ch * 16;

    for (int row = 0; row < 16; row++) {
        int y = py + row;
        if (y < 0 || (uint32_t)y >= g_fb->height)
            continue;
        uint32_t *pixel_row = g_fb->base + (uint32_t)y * g_fb->pitch;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int x = px + col;
            if (x < 0 || (uint32_t)x >= g_fb->width)
                continue;
            if (bits & (0x80 >> col))
                pixel_row[x] = fg;
            /* Don't touch background — transparent text rendering */
        }
    }
}

/* Draw an 8x16 glyph scaled by an integer factor (each source pixel becomes a
 * scale x scale block). scale=1 is identical to draw_glyph_at. Transparent bg. */
static void draw_glyph_scaled(int px, int py, uint8_t ch, uint32_t fg, int scale)
{
    if (!g_fb || !g_fb->base || scale < 1)
        return;
    const uint8_t *glyph = font8x16 + (uint32_t)ch * 16;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col)))
                continue;
            for (int sy = 0; sy < scale; sy++) {
                int y = py + row * scale + sy;
                if (y < 0 || (uint32_t)y >= g_fb->height)
                    continue;
                uint32_t *prow = g_fb->base + (uint32_t)y * g_fb->pitch;
                for (int sx = 0; sx < scale; sx++) {
                    int x = px + col * scale + sx;
                    if (x < 0 || (uint32_t)x >= g_fb->width)
                        continue;
                    prow[x] = fg;
                }
            }
        }
    }
}

/* Public: scaled boot-font text. Advance = 8*scale per glyph, 16*scale per line. */
void fb_text_scaled(int x, int y, const char *s, uint32_t color, int scale)
{
    if (scale < 1) scale = 1;
    int px = x;
    while (*s) {
        if (*s == '\n') { px = x; y += 16 * scale; }
        else { draw_glyph_scaled(px, y, (uint8_t)*s, color, scale); px += 8 * scale; }
        s++;
    }
}

static void draw_glyph_bg(int px, int py, uint8_t ch, uint32_t fg, uint32_t bg)
{
    if (!g_fb || !g_fb->base)
        return;

    const uint8_t *glyph = font8x16 + (uint32_t)ch * 16;

    for (int row = 0; row < 16; row++) {
        int y = py + row;
        if (y < 0 || (uint32_t)y >= g_fb->height)
            continue;
        uint32_t *pixel_row = g_fb->base + (uint32_t)y * g_fb->pitch;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int x = px + col;
            if (x < 0 || (uint32_t)x >= g_fb->width)
                continue;
            pixel_row[x] = (bits & (0x80 >> col)) ? fg : bg;
        }
    }
}

void fb_text(int x, int y, const char *s, uint32_t color)
{
    int px = x;
    while (*s) {
        if (*s == '\n') {
            px = x;
            y += 16;
        } else {
            draw_glyph_at(px, y, (uint8_t)*s, color);
            px += 8;
        }
        s++;
    }
}

void fb_text_bg(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    int px = x;
    while (*s) {
        if (*s == '\n') {
            px = x;
            y += 16;
        } else {
            draw_glyph_bg(px, y, (uint8_t)*s, fg, bg);
            px += 8;
        }
        s++;
    }
}

/* Read a w*h rect of the framebuffer into `out` (row-major, w stride).
 * Symmetric with fb_blit: out-of-bounds pixels are skipped in both, so a
 * read-then-blit of the same rect round-trips the on-screen region. */
void fb_read_rect(int x, int y, int w, int h, uint32_t *out)
{
    if (!g_fb || !g_fb->base || !out)
        return;

    for (int py = 0; py < h; py++) {
        int sy = y + py;
        uint32_t *dst = out + py * w;
        if (sy < 0 || (uint32_t)sy >= g_fb->height)
            continue;
        uint32_t *src = g_fb->base + (uint32_t)sy * g_fb->pitch;
        for (int px = 0; px < w; px++) {
            int sx = x + px;
            if (sx < 0 || (uint32_t)sx >= g_fb->width)
                continue;
            dst[px] = src[sx];
        }
    }
}

void fb_blit(int x, int y, int w, int h, const uint32_t *pixels)
{
    if (!g_fb || !g_fb->base || !pixels)
        return;

    for (int py = 0; py < h; py++) {
        int sy = y + py;
        if (sy < 0 || (uint32_t)sy >= g_fb->height)
            continue;
        uint32_t *dst = g_fb->base + (uint32_t)sy * g_fb->pitch;
        const uint32_t *src = pixels + py * w;
        for (int px = 0; px < w; px++) {
            int sx = x + px;
            if (sx < 0 || (uint32_t)sx >= g_fb->width)
                continue;
            dst[sx] = src[px];
        }
    }
}

void fb_pixel_blend(int x, int y, uint32_t color)
{
    if (!g_fb || !g_fb->base)
        return;
    if (x < 0 || y < 0 || (uint32_t)x >= g_fb->width || (uint32_t)y >= g_fb->height)
        return;

    uint8_t alpha = (color >> 24) & 0xFF;
    if (alpha == 0xFF) {
        g_fb->base[(uint32_t)y * g_fb->pitch + (uint32_t)x] = color;
        return;
    }
    if (alpha == 0x00)
        return;

    uint32_t dst = g_fb->base[(uint32_t)y * g_fb->pitch + (uint32_t)x];
    uint8_t inv = 255 - alpha;

    uint8_t r = ((((color >> 16) & 0xFF) * alpha) + (((dst >> 16) & 0xFF) * inv)) >> 8;
    uint8_t g = ((((color >> 8) & 0xFF) * alpha) + (((dst >> 8) & 0xFF) * inv)) >> 8;
    uint8_t b = (((color & 0xFF) * alpha) + ((dst & 0xFF) * inv)) >> 8;

    g_fb->base[(uint32_t)y * g_fb->pitch + (uint32_t)x] = (r << 16) | (g << 8) | b;
}

void fb_rect_blend(int x, int y, int w, int h, uint32_t color)
{
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            fb_pixel_blend(px, py, color);
}

void fb_cursor_pos(uint32_t *col, uint32_t *row)
{
    if (col) *col = cursor_x;
    if (row) *row = cursor_y;
}

void fb_set_cursor(uint32_t col, uint32_t row)
{
    cursor_x = col;
    cursor_y = row;
}
