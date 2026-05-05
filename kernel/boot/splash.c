/*
 * Boot splash. Static wordmark + step + percent. Serial logs continue
 * silently behind the splash. Dismissed once chain_registry_init
 * completes; user sees the scheduler-driven shell from there.
 * Per OS_LITTLE_THINGS.md top-20 #7.
 *
 * Constraints honored:
 *   - Runs immediately after fb_init (before pmm/heap/timer). No
 *     allocation, no TTF, no math libs. Boot 8x16 bitmap font only.
 *   - GOP framebuffer ONLY. If no fb, splash_init() returns 0 and
 *     callers fall back to serial.
 *   - kprint dual-mode toggle hides intermediate boot logs from the
 *     fb while keeping the serial log untouched for debug.
 */

#include "splash.h"
#include "fb.h"
#include "kprint.h"
#include "serial.h"
#include "theme.h"
#include "font8x16.h"

/* ── State ────────────────────────────────────────────────────────── */

static int   g_active;
static int   g_last_pct = -1;
static char  g_last_step[40];

/* Layout, derived from fb size in splash_init() */
static int   g_word_x;     /* top-left of wordmark */
static int   g_word_y;
static int   g_word_w;
static int   g_word_h;
static int   g_bar_x;
static int   g_bar_y;
static int   g_bar_w;
static int   g_bar_h;
static int   g_step_x;
static int   g_step_y;
static int   g_pct_x;

/* Wordmark glyph scale (boot font is 8x16; we draw NxN blocks per pixel) */
#define WORDMARK_SCALE 6     /* 48x96 per char */
#define WORDMARK_TEXT  "ZEOS"
#define WORDMARK_LEN   4

/* Progress bar geometry in pixels */
#define BAR_HEIGHT     6
#define BAR_WIDTH_FRAC_NUM  1   /* width = fb_w * NUM/DEN */
#define BAR_WIDTH_FRAC_DEN  3

/* Line-clear buffer width: enough for the longest sane step label */
#define STEP_FIELD_CHARS  40

/* Color choices — derived from theme.h tokens */
#define COL_BG_TOP     COLOR_SURFACE         /* 0xFF0D1117 */
/* COL_BG_BOTTOM: COLOR_SURFACE darkened ~30% for vertical gradient */
#define COL_BG_BOTTOM  0xFF06080C
#define COL_WORDMARK   COLOR_FULL_ACCENT     /* steel blue, persona Full */
#define COL_STEP       COLOR_ON_SURFACE_2    /* secondary text */
#define COL_BAR_TRACK  COLOR_SURFACE_TOP
#define COL_BAR_FILL   COLOR_FULL_ACCENT

/* ── kprint dual-mode hook ────────────────────────────────────────── */
/* Implemented in kprint.c; declared here for splash use only. */
extern void kprint_set_splash_mode(int on);

/* ── Helpers ──────────────────────────────────────────────────────── */

static int s_strlen(const char *s)
{
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void s_strcpy_clamp(char *dst, const char *src, int max)
{
    int i = 0;
    if (!src) { dst[0] = '\0'; return; }
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/*
 * Vertical gradient between two ARGB colors, full-screen.
 * Linear interpolation per row — no float, integer-only.
 */
static void paint_gradient(uint32_t top, uint32_t bot, int w, int h)
{
    uint8_t r0 = (top >> 16) & 0xFF;
    uint8_t g0 = (top >>  8) & 0xFF;
    uint8_t b0 =  top        & 0xFF;
    uint8_t r1 = (bot >> 16) & 0xFF;
    uint8_t g1 = (bot >>  8) & 0xFF;
    uint8_t b1 =  bot        & 0xFF;

    for (int y = 0; y < h; y++) {
        int t = (y * 256) / (h ? h : 1);
        uint8_t r = (uint8_t)(((int)r0 * (256 - t) + (int)r1 * t) >> 8);
        uint8_t g = (uint8_t)(((int)g0 * (256 - t) + (int)g1 * t) >> 8);
        uint8_t b = (uint8_t)(((int)b0 * (256 - t) + (int)b1 * t) >> 8);
        uint32_t c = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        fb_hline(0, y, w, c);
    }
}

/*
 * Draw one boot-font glyph, scaled by an integer factor, as a block grid.
 * px,py = top-left pixel, scale = pixels per font-pixel. Non-foreground
 * font bits are left untouched (transparent over the gradient).
 */
static void draw_glyph_scaled(int px, int py, char ch, int scale, uint32_t color)
{
    const uint8_t *glyph = font8x16 + (uint32_t)(uint8_t)ch * 16;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                fb_rect(px + col * scale, py + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_wordmark(void)
{
    int x = g_word_x;
    for (int i = 0; i < WORDMARK_LEN; i++) {
        draw_glyph_scaled(x, g_word_y, WORDMARK_TEXT[i], WORDMARK_SCALE, COL_WORDMARK);
        x += 8 * WORDMARK_SCALE;  /* no kerning, full glyph cell */
    }
}

/*
 * Erase the step + percent line region back to the gradient color,
 * row-by-row. Cheap; the line is at most STEP_FIELD_CHARS*8 wide.
 */
static void clear_step_line(void)
{
    int w = fb_width();
    int line_w = w;            /* clear full width — easiest */
    int line_h = 16;
    /* Recompute the gradient color at this y so the erase blends in. */
    uint8_t r0 = (COL_BG_TOP    >> 16) & 0xFF;
    uint8_t g0 = (COL_BG_TOP    >>  8) & 0xFF;
    uint8_t b0 =  COL_BG_TOP            & 0xFF;
    uint8_t r1 = (COL_BG_BOTTOM >> 16) & 0xFF;
    uint8_t g1 = (COL_BG_BOTTOM >>  8) & 0xFF;
    uint8_t b1 =  COL_BG_BOTTOM         & 0xFF;
    int H = (int)fb_height();
    for (int dy = 0; dy < line_h; dy++) {
        int y = g_step_y + dy;
        int t = (y * 256) / (H ? H : 1);
        uint8_t r = (uint8_t)(((int)r0 * (256 - t) + (int)r1 * t) >> 8);
        uint8_t g = (uint8_t)(((int)g0 * (256 - t) + (int)g1 * t) >> 8);
        uint8_t b = (uint8_t)(((int)b0 * (256 - t) + (int)b1 * t) >> 8);
        uint32_t c = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        fb_hline(0, y, line_w, c);
    }
}

static void draw_bar(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    /* Track */
    fb_rect(g_bar_x, g_bar_y, g_bar_w, g_bar_h, COL_BAR_TRACK);
    /* Fill */
    int fill_w = (g_bar_w * pct) / 100;
    if (fill_w > 0)
        fb_rect(g_bar_x, g_bar_y, fill_w, g_bar_h, COL_BAR_FILL);
}

static void draw_step_and_pct(const char *step, int pct)
{
    /* Step label — left-aligned under the bar's left edge. */
    if (step && step[0]) {
        fb_text(g_step_x, g_step_y, step, COL_STEP);
    }
    /* Percent — right-aligned under the bar's right edge. */
    char pbuf[8];
    int p = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    int n = 0;
    if (p >= 100) { pbuf[n++] = '1'; pbuf[n++] = '0'; pbuf[n++] = '0'; }
    else if (p >= 10) { pbuf[n++] = (char)('0' + p / 10); pbuf[n++] = (char)('0' + p % 10); }
    else { pbuf[n++] = (char)('0' + p); }
    pbuf[n++] = '%';
    pbuf[n] = '\0';
    int pw = n * 8;
    fb_text(g_pct_x - pw, g_step_y, pbuf, COL_STEP);
}

/* ── API ──────────────────────────────────────────────────────────── */

int splash_init(void)
{
    int w = (int)fb_width();
    int h = (int)fb_height();
    if (w <= 0 || h <= 0 || fb_phys_base() == 0) {
        g_active = 0;
        return 0;
    }

    /* Layout */
    g_word_w = WORDMARK_LEN * 8 * WORDMARK_SCALE;     /* 4 * 8 * 6 = 192 */
    g_word_h = 16 * WORDMARK_SCALE;                   /* 96 */
    g_word_x = (w - g_word_w) / 2;
    g_word_y = (h - g_word_h) / 2 - 32;               /* nudged up a bit */

    g_bar_w  = (w * BAR_WIDTH_FRAC_NUM) / BAR_WIDTH_FRAC_DEN;
    g_bar_h  = BAR_HEIGHT;
    g_bar_x  = (w - g_bar_w) / 2;
    g_bar_y  = g_word_y + g_word_h + 48;

    g_step_x = g_bar_x;
    g_step_y = g_bar_y + g_bar_h + 12;
    g_pct_x  = g_bar_x + g_bar_w;

    /* Paint */
    paint_gradient(COL_BG_TOP, COL_BG_BOTTOM, w, h);
    draw_wordmark();
    draw_bar(0);
    draw_step_and_pct("starting", 0);

    g_last_step[0] = '\0';
    s_strcpy_clamp(g_last_step, "starting", STEP_FIELD_CHARS);
    g_last_pct = 0;

    g_active = 1;
    kprint_set_splash_mode(1);

    /* Mirror to serial so the boot log records the splash entry. */
    serial_puts("[splash] active\n");
    return 1;
}

void splash_progress(const char *step, int pct)
{
    if (!g_active) return;

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    /* Always log to serial — keeps the debug stream complete. */
    serial_puts("[splash] ");
    if (step && step[0]) serial_puts(step);
    else                 serial_puts(g_last_step);
    serial_puts(" ");
    {
        char pbuf[8];
        int n = 0, p = pct;
        if (p >= 100) { pbuf[n++] = '1'; pbuf[n++] = '0'; pbuf[n++] = '0'; }
        else if (p >= 10) { pbuf[n++] = (char)('0' + p / 10); pbuf[n++] = (char)('0' + p % 10); }
        else { pbuf[n++] = (char)('0' + p); }
        pbuf[n++] = '%';
        pbuf[n++] = '\n';
        pbuf[n] = '\0';
        serial_puts(pbuf);
    }

    /* If neither value changed, don't repaint. */
    int step_changed = 0;
    if (step && step[0]) {
        const char *a = g_last_step;
        const char *b = step;
        while (*a && *b && *a == *b) { a++; b++; }
        step_changed = (*a != *b);
    }
    int pct_changed = (pct != g_last_pct);
    if (!step_changed && !pct_changed)
        return;

    if (step && step[0]) {
        s_strcpy_clamp(g_last_step, step, STEP_FIELD_CHARS);
    }
    g_last_pct = pct;

    clear_step_line();
    draw_bar(pct);
    draw_step_and_pct(g_last_step, pct);

    /* Keep wordmark crisp — gradient touched only the step line, but
     * if a future change repaints near the wordmark, we'd redraw here. */
    (void)s_strlen;  /* silence unused */
}

void splash_dismiss(void)
{
    if (!g_active) {
        /* Even without a splash, restore kprint normal mode. */
        kprint_set_splash_mode(0);
        return;
    }
    /* Final 100% beat so the bar reads complete in serial logs. */
    splash_progress("ready", 100);

    /* Instant clear to surface — no fade for first scope. */
    fb_clear(COLOR_SURFACE);

    g_active = 0;
    kprint_set_splash_mode(0);
    serial_puts("[splash] dismissed\n");
}

int splash_active(void)
{
    return g_active;
}
