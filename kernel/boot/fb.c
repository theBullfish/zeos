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

/* Text colors — warm, not clinical */
#define FG_COLOR  0x00E0D8D0   /* Warm light text */
#define BG_COLOR  0x001A1A1A   /* Dark warm gray */

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
