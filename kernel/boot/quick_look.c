/*
 * Zeos — Quick Look implementation
 *
 * Doctrine: read once, render forever. The preview panel owns a
 * small static sniff buffer so type detection never allocates.
 * For PNGs we go further: full RGBA decode via lodepng, with the
 * input file bytes pulled into a PMM-contiguous buffer (heap is
 * the wrong tool for >64 KB allocations). Decoded RGBA pixels are
 * kept until the panel closes, then freed.
 *
 * Quick Look: full PNG decode via lodepng + pmm_alloc_contiguous.
 * Capped at 1920x1080 (8 MB buffer). Larger → metadata only.
 * JPEG / WebP / HEIC are future work.
 *
 * Async-friendly: the first draw after open shows a spinner and
 * arms a "needs decode" flag. The next draw performs the decode
 * and paints. This keeps quick_look_open() — which runs on the
 * keyboard ISR's caller path — out of the heavy decode work.
 */

#include "quick_look.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "compositor.h"
#include "timeofday.h"
#include "pmm.h"
#include "fat32.h"
#include "ui_states.h"
#include "lodepng/lodepng.h"

/* lodepng allocator shims live in browser.c — reuse them here. */
extern void *lodepng_malloc(uint64_t size);
extern void  lodepng_free(void *ptr);

#define QL_SNIFF_SIZE  (4 * 1024)   /* enough for magic + IHDR + text head */
#define QL_W           640
#define QL_H           480
#define QL_PAD          16
#define PAGE_SIZE      4096

/* Hard caps on PNG dimensions. 1920x1080 RGBA = ~8 MB output buffer. */
#define MAX_PREVIEW_W       1920
#define MAX_PREVIEW_H       1080
#define MAX_PNG_FILE_BYTES  (8 * 1024 * 1024)   /* 8 MB on-disk PNG */

static void ql_strncpy(char *dst, const char *src, int max) {
    int i = 0; if (!dst || max <= 0) return;
    if (src) while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int ql_strlen(const char *s) {
    int n = 0; if (!s) return 0; while (s[n]) n++; return n;
}

/* Tiny u64 -> decimal printer into a buffer. Returns chars written. */
static int ql_u64_to_dec(uint64_t v, char *out, int max) {
    char tmp[24];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else while (v > 0 && n < 24) { tmp[n++] = '0' + (v % 10); v /= 10; }
    int o = 0;
    while (n > 0 && o < max - 1) out[o++] = tmp[--n];
    out[o] = '\0';
    return o;
}

static int ql_u64_to_hex2(uint64_t v, char *out, int max) {
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;
    if (max < 3) { if (max > 0) out[0] = 0; return 0; }
    out[o++] = hex[(v >> 4) & 0xF];
    out[o++] = hex[v & 0xF];
    out[o] = '\0';
    return o;
}

/* ── State ── */

typedef enum {
    QL_PNG_NONE = 0,    /* not a PNG */
    QL_PNG_PENDING,     /* PNG detected, decode not yet attempted */
    QL_PNG_OK,          /* decoded RGBA in png_pixels */
    QL_PNG_TOO_BIG,     /* dimensions exceed cap */
    QL_PNG_FAIL,        /* decode error / OOM / unreadable */
} ql_png_state_t;

typedef struct {
    int       active;
    char      path[QL_PATH_MAX];
    uint64_t  size;
    uint64_t  mtime;
    ql_kind_t kind;

    /* Sniff buffer — always populated by read_fn. */
    uint8_t   buf[QL_SNIFF_SIZE];
    int       buf_used;

    /* PNG decode result. */
    int             png_w, png_h;       /* native dims from IHDR */
    ql_png_state_t  png_state;
    unsigned char  *png_pixels;         /* RGBA, lodepng_malloc'd */
    unsigned        png_decoded_w;      /* dims confirmed by decoder */
    unsigned        png_decoded_h;
} ql_state_t;

static ql_state_t g_ql;
static uint32_t g_total_opens = 0;
static uint32_t g_total_decodes_ok = 0;
static uint32_t g_total_decodes_fail = 0;

/* ── Type sniff ── */

static ql_kind_t ql_sniff(const uint8_t *b, int n) {
    if (n >= 4 && b[0] == 0x89 && b[1] == 0x50 &&
        b[2] == 0x4E && b[3] == 0x47) return QL_KIND_PNG;

    /* Plain text heuristic: all bytes in first 64 either printable
     * ASCII (0x20..0x7E) or whitespace (\t \n \r). */
    int sample = n < 64 ? n : 64;
    if (sample == 0) return QL_KIND_UNKNOWN;
    for (int i = 0; i < sample; i++) {
        uint8_t c = b[i];
        int ok = (c >= 0x20 && c <= 0x7E) ||
                 c == '\t' || c == '\n' || c == '\r';
        if (!ok) return QL_KIND_UNKNOWN;
    }
    return QL_KIND_TEXT;
}

/* PNG width/height live at bytes [16..23] in big-endian. */
static void ql_parse_png_dims(const uint8_t *b, int n, int *w, int *h) {
    *w = 0; *h = 0;
    if (n < 24) return;
    *w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    *h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
}

/* ── PNG decode ── */

static void ql_release_png(void) {
    if (g_ql.png_pixels) {
        lodepng_free(g_ql.png_pixels);
        g_ql.png_pixels = 0;
    }
    g_ql.png_decoded_w = 0;
    g_ql.png_decoded_h = 0;
}

/* Pull the entire PNG into a contiguous PMM buffer, decode, then
 * release the input buffer. Output RGBA stays alive in g_ql.png_pixels
 * (lodepng_malloc-backed) until quick_look_close.
 *
 * Failure modes set png_state and bail; the draw path falls back to
 * the metadata card. */
static void ql_decode_png(void) {
    /* Pre-flight: size cap and dimension cap from IHDR. */
    if (g_ql.size == 0 || g_ql.size > MAX_PNG_FILE_BYTES) {
        g_ql.png_state = QL_PNG_TOO_BIG;
        return;
    }
    if (g_ql.png_w <= 0 || g_ql.png_h <= 0 ||
        g_ql.png_w > MAX_PREVIEW_W || g_ql.png_h > MAX_PREVIEW_H) {
        g_ql.png_state = QL_PNG_TOO_BIG;
        return;
    }
    if (!fat32_mounted() || g_ql.path[0] != '/') {
        g_ql.png_state = QL_PNG_FAIL;
        return;
    }

    /* Allocate input buffer from PMM (page-aligned, contiguous). */
    uint64_t pages = (g_ql.size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        g_ql.png_state = QL_PNG_FAIL;
        return;
    }
    uint8_t *in_buf = (uint8_t *)phys;

    /* Read full file. fat32_read is a single-shot read against an
     * fat32_file cursor; we open fresh to read from offset 0. */
    struct fat32_file f;
    if (fat32_open(g_ql.path, &f) < 0) {
        for (uint64_t i = 0; i < pages; i++)
            pmm_free(phys + i * PAGE_SIZE);
        g_ql.png_state = QL_PNG_FAIL;
        return;
    }
    int got = fat32_read(&f, in_buf, (uint32_t)g_ql.size);
    if (got <= 0 || (uint64_t)got < g_ql.size) {
        /* Short read — try to decode anyway with what we got, but
         * if that also fails we'll surface the error. */
        if (got <= 0) {
            for (uint64_t i = 0; i < pages; i++)
                pmm_free(phys + i * PAGE_SIZE);
            g_ql.png_state = QL_PNG_FAIL;
            return;
        }
    }

    unsigned char *pixels = 0;
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32(&pixels, &w, &h, in_buf, (size_t)got);

    /* Release input buffer immediately — decoder no longer needs it. */
    for (uint64_t i = 0; i < pages; i++)
        pmm_free(phys + i * PAGE_SIZE);

    if (err || !pixels || w == 0 || h == 0 ||
        w > MAX_PREVIEW_W || h > MAX_PREVIEW_H) {
        if (pixels) lodepng_free(pixels);
        g_ql.png_state = QL_PNG_FAIL;
        g_total_decodes_fail++;
        return;
    }

    g_ql.png_pixels    = pixels;
    g_ql.png_decoded_w = w;
    g_ql.png_decoded_h = h;
    g_ql.png_state     = QL_PNG_OK;
    g_total_decodes_ok++;
}

/* ── API ── */

void quick_look_open(const char *path, uint64_t size, uint64_t mtime,
                     ql_read_fn read_fn, void *ctx)
{
    /* Drop any prior decode. */
    ql_release_png();

    g_ql.active = 1;
    ql_strncpy(g_ql.path, path ? path : "", QL_PATH_MAX);
    g_ql.size = size;
    g_ql.mtime = mtime;
    g_ql.buf_used = 0;
    g_ql.kind = QL_KIND_UNKNOWN;
    g_ql.png_w = 0;
    g_ql.png_h = 0;
    g_ql.png_state = QL_PNG_NONE;

    if (read_fn) {
        uint64_t s = 0, m = 0;
        int n = read_fn(path, g_ql.buf, QL_SNIFF_SIZE, &s, &m, ctx);
        if (n > 0) {
            g_ql.buf_used = n;
            if (s) g_ql.size = s;
            if (m) g_ql.mtime = m;
            g_ql.kind = ql_sniff(g_ql.buf, n);
            if (g_ql.kind == QL_KIND_PNG) {
                ql_parse_png_dims(g_ql.buf, n, &g_ql.png_w, &g_ql.png_h);
                /* Defer decode to first draw — gives the compositor a
                 * frame to paint the spinner before we burn cycles in
                 * inflate(). */
                g_ql.png_state = QL_PNG_PENDING;
            }
        }
    }

    g_total_opens++;
    int sw = (int)fb_width(), sh = (int)fb_height();
    compositor_dirty((sw - QL_W) / 2, (sh - QL_H) / 2, QL_W, QL_H);
}

void quick_look_close(void)
{
    if (!g_ql.active) return;
    g_ql.active = 0;
    ql_release_png();
    int sw = (int)fb_width(), sh = (int)fb_height();
    compositor_dirty((sw - QL_W) / 2, (sh - QL_H) / 2, QL_W, QL_H);
}

int quick_look_active(void) { return g_ql.active; }

int quick_look_key(int ascii)
{
    if (!g_ql.active) return 0;
    if (ascii == 27 || ascii == ' ') {  /* Esc or Space toggles off */
        quick_look_close();
        return 1;
    }
    return 1;  /* swallow other keys while active */
}

int quick_look_mouse_down(int x, int y, int button)
{
    if (!g_ql.active) return 0;
    (void)button;
    int sw = (int)fb_width(), sh = (int)fb_height();
    int qx = (sw - QL_W) / 2, qy = (sh - QL_H) / 2;
    if (x < qx || x >= qx + QL_W || y < qy || y >= qy + QL_H) {
        quick_look_close();
        return 1;
    }
    return 1;
}

/* ── Drawing helpers ── */

static void ql_draw_text_body(int x, int y, int w, int h)
{
    /* Render up to 50 lines from buf. */
    const int line_h = 14;
    const int max_lines = h / line_h;
    int line_target = max_lines < 50 ? max_lines : 50;

    char line[96];
    int li = 0;
    int lines = 0;
    int cy = y;
    for (int i = 0; i < g_ql.buf_used && lines < line_target; i++) {
        char c = (char)g_ql.buf[i];
        if (c == '\n' || li >= 95) {
            line[li] = '\0';
            fb_text(x, cy, line, COLOR_ON_SURFACE);
            cy += line_h;
            lines++;
            li = 0;
            if (c != '\n') { line[li++] = c; }
        } else if (c == '\r' || c == '\t') {
            if (c == '\t' && li < 92) {
                line[li++] = ' '; line[li++] = ' ';
            }
        } else if (c >= 0x20 && c <= 0x7E) {
            line[li++] = c;
        }
    }
    if (li > 0 && lines < line_target) {
        line[li] = '\0';
        fb_text(x, cy, line, COLOR_ON_SURFACE);
    }
    (void)w;
}

static void ql_draw_metadata(int x, int y)
{
    char buf[80];
    fb_text(x, y, "File preview", COLOR_ON_SURFACE);

    fb_text(x, y + 24, g_ql.path, COLOR_ON_SURFACE_2);

    /* size */
    int o = 0;
    const char *prefix = "Size: ";
    while (prefix[o] && o < 79) { buf[o] = prefix[o]; o++; }
    o += ql_u64_to_dec(g_ql.size, buf + o, 79 - o);
    if (o < 78) { buf[o++] = ' '; buf[o++] = 'B'; }
    buf[o] = '\0';
    fb_text(x, y + 48, buf, COLOR_ON_SURFACE_2);

    /* mtime as epoch decimal (full tod_format requires more state) */
    o = 0;
    const char *p2 = "Modified epoch: ";
    while (p2[o] && o < 79) { buf[o] = p2[o]; o++; }
    o += ql_u64_to_dec(g_ql.mtime, buf + o, 79 - o);
    buf[o] = '\0';
    fb_text(x, y + 68, buf, COLOR_ON_SURFACE_2);

    /* mime sniff: first 4 bytes hex */
    o = 0;
    const char *p3 = "First 4 bytes: ";
    while (p3[o] && o < 79) { buf[o] = p3[o]; o++; }
    int n = g_ql.buf_used < 4 ? g_ql.buf_used : 4;
    for (int i = 0; i < n; i++) {
        ql_u64_to_hex2(g_ql.buf[i], buf + o, 79 - o);
        o += 2;
        if (i < n - 1 && o < 78) buf[o++] = ' ';
    }
    buf[o] = '\0';
    fb_text(x, y + 88, buf, COLOR_ON_SURFACE_2);
}

/* Centered nearest-neighbor blit of decoded RGBA into the body rect.
 * Down- and up-scale handled the same way (skip / replicate by the
 * (dst*src)/dst index map). Aspect is preserved. */
static void ql_draw_png_pixels(int bx, int by, int bw, int bh)
{
    int sw = (int)g_ql.png_decoded_w;
    int sh = (int)g_ql.png_decoded_h;
    if (sw <= 0 || sh <= 0 || !g_ql.png_pixels) return;

    /* Fit into body rect, preserve aspect. */
    int dw = bw;
    int dh = (sh * dw) / sw;
    if (dh > bh) {
        dh = bh;
        dw = (sw * dh) / sh;
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    int dx0 = bx + (bw - dw) / 2;
    int dy0 = by + (bh - dh) / 2;

    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        if (sy < 0) sy = 0;
        if (sy >= sh) sy = sh - 1;
        const uint8_t *row = g_ql.png_pixels + (size_t)sy * sw * 4;
        for (int x = 0; x < dw; x++) {
            int sx = (x * sw) / dw;
            if (sx < 0) sx = 0;
            if (sx >= sw) sx = sw - 1;
            const uint8_t *p = row + sx * 4;
            uint32_t argb = ((uint32_t)p[3] << 24) |
                            ((uint32_t)p[0] << 16) |
                            ((uint32_t)p[1] <<  8) |
                            ((uint32_t)p[2]);
            fb_pixel_blend(dx0 + x, dy0 + y, argb);
        }
    }
}

void quick_look_draw(void)
{
    if (!g_ql.active) return;

    int sw = (int)fb_width(), sh = (int)fb_height();
    int qx = (sw - QL_W) / 2, qy = (sh - QL_H) / 2;

    /* Dim background */
    fb_rect_blend(0, 0, sw, sh, 0x80000000);

    /* Card */
    fb_rect(qx, qy, QL_W, QL_H, COLOR_SURFACE_HIGH);
    fb_rect_outline(qx, qy, QL_W, QL_H, COLOR_SEPARATOR, 1);

    /* Header bar */
    fb_rect(qx, qy, QL_W, 32, COLOR_SURFACE_TOP);
    fb_text(qx + QL_PAD, qy + 9, g_ql.path, COLOR_ON_SURFACE);
    fb_text(qx + QL_W - 72, qy + 9, "[Esc]", COLOR_ON_SURFACE_3);

    int body_x = qx + QL_PAD;
    int body_y = qy + 32 + QL_PAD;
    int body_w = QL_W - 2 * QL_PAD;
    int body_h = QL_H - 32 - 2 * QL_PAD;

    if (g_ql.kind == QL_KIND_TEXT && g_ql.buf_used > 0) {
        ql_draw_text_body(body_x, body_y, body_w, body_h);
    } else if (g_ql.kind == QL_KIND_PNG) {
        if (g_ql.png_state == QL_PNG_PENDING) {
            /* Spinner via list_render_state — first frame after open. */
            list_state_ctx_t lc = {
                .x = body_x, .y = body_y, .w = body_w, .h = body_h,
                .state = LIST_LOADING,
                .message = "Decoding PNG…",
                .cta = 0, .retry_cb = 0, .retry_ctx = 0,
            };
            list_render_state(&lc);
            /* Trigger decode and request another paint. */
            ql_decode_png();
            compositor_dirty(qx, qy, QL_W, QL_H);
            return;
        }
        if (g_ql.png_state == QL_PNG_OK && g_ql.png_pixels) {
            ql_draw_png_pixels(body_x, body_y, body_w, body_h);
            /* Caption: dimensions */
            char buf[80];
            int o = 0;
            o += ql_u64_to_dec((uint64_t)g_ql.png_decoded_w, buf + o, 79 - o);
            if (o < 78) buf[o++] = 'x';
            o += ql_u64_to_dec((uint64_t)g_ql.png_decoded_h, buf + o, 79 - o);
            buf[o] = '\0';
            int cap_w = ql_strlen(buf) * 8;
            fb_text(body_x + (body_w - cap_w) / 2,
                    body_y + body_h - 14, buf, COLOR_ON_SURFACE_3);
        } else if (g_ql.png_state == QL_PNG_TOO_BIG) {
            char buf[80];
            int o = 0;
            const char *p = "PNG ";
            while (p[o] && o < 79) { buf[o] = p[o]; o++; }
            o += ql_u64_to_dec((uint64_t)g_ql.png_w, buf + o, 79 - o);
            if (o < 78) buf[o++] = 'x';
            o += ql_u64_to_dec((uint64_t)g_ql.png_h, buf + o, 79 - o);
            buf[o] = '\0';
            fb_text(body_x, body_y, buf, COLOR_ON_SURFACE);
            fb_text(body_x, body_y + 20,
                    "Too large to preview (cap 1920x1080).",
                    COLOR_ON_SURFACE_2);
        } else {
            /* QL_PNG_FAIL — fall back to metadata card. */
            ql_draw_metadata(body_x, body_y);
            fb_text(body_x, body_y + 112,
                    "(PNG decode failed)", COLOR_ON_SURFACE_3);
        }
    } else {
        ql_draw_metadata(body_x, body_y);
    }
}

uint32_t quick_look_total_opens(void) { return g_total_opens; }
uint32_t quick_look_total_decodes_ok(void)   { return g_total_decodes_ok; }
uint32_t quick_look_total_decodes_fail(void) { return g_total_decodes_fail; }
