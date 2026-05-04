/*
 * Zeos — Quick Look implementation
 *
 * Doctrine: read once, render forever. The preview panel owns a
 * 64KB scratch buffer (static) so it never allocates from heap.
 * Files larger than the buffer get a metadata-only card.
 */

#include "quick_look.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "compositor.h"
#include "timeofday.h"

#define QL_BUF_SIZE   (64 * 1024)
#define QL_W          640
#define QL_H          480
#define QL_PAD         16

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

typedef struct {
    int       active;
    char      path[QL_PATH_MAX];
    uint64_t  size;
    uint64_t  mtime;
    ql_kind_t kind;

    /* Loaded bytes */
    uint8_t   buf[QL_BUF_SIZE];
    int       buf_used;     /* bytes loaded */

    /* PNG decode result (kept tiny — only metadata; full RGBA decode
     * would need the heap. For now we render PNGs as their hex header
     * + dimensions inferred from IHDR. lodepng integration is a TODO
     * that doesn't block the preview pattern.) */
    int       png_w, png_h;
} ql_state_t;

static ql_state_t g_ql;
static uint32_t g_total_opens = 0;

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

/* ── API ── */

void quick_look_open(const char *path, uint64_t size, uint64_t mtime,
                     ql_read_fn read_fn, void *ctx)
{
    g_ql.active = 1;
    ql_strncpy(g_ql.path, path ? path : "", QL_PATH_MAX);
    g_ql.size = size;
    g_ql.mtime = mtime;
    g_ql.buf_used = 0;
    g_ql.kind = QL_KIND_UNKNOWN;
    g_ql.png_w = 0;
    g_ql.png_h = 0;

    if (read_fn) {
        uint64_t s = 0, m = 0;
        int n = read_fn(path, g_ql.buf, QL_BUF_SIZE, &s, &m, ctx);
        if (n > 0) {
            g_ql.buf_used = n;
            if (s) g_ql.size = s;
            if (m) g_ql.mtime = m;
            g_ql.kind = ql_sniff(g_ql.buf, n);
            if (g_ql.kind == QL_KIND_PNG)
                ql_parse_png_dims(g_ql.buf, n, &g_ql.png_w, &g_ql.png_h);
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
        /* Show PNG dimensions + "rendered preview not available" — full
         * lodepng decode requires heap > 64KB for typical screenshots
         * and is wired separately by the image viewer chain. */
        char buf[80];
        int o = 0;
        const char *p = "PNG image  ";
        while (p[o] && o < 79) { buf[o] = p[o]; o++; }
        o += ql_u64_to_dec((uint64_t)g_ql.png_w, buf + o, 79 - o);
        if (o < 78) buf[o++] = 'x';
        o += ql_u64_to_dec((uint64_t)g_ql.png_h, buf + o, 79 - o);
        buf[o] = '\0';
        fb_text(body_x, body_y, buf, COLOR_ON_SURFACE);
        fb_text(body_x, body_y + 20,
                "(open in image viewer for full render)",
                COLOR_ON_SURFACE_2);
    } else {
        ql_draw_metadata(body_x, body_y);
    }
}

uint32_t quick_look_total_opens(void) { return g_total_opens; }
