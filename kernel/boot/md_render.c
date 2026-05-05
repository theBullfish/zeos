/*
 * Zeos — shared markdown renderer.
 *
 * Extracted from notes_polish.c so notes-zeos, chat-zeos, and the
 * editor's preview pane all share one renderer. Inline call (chat) and
 * full call (notes) differ only in whether the background fills and
 * whether multi-line block elements (headings, fences, quotes) are
 * honored.
 */

#include "md_render.h"
#include "fb.h"
#include "font.h"
#include "theme.h"

#define MD_URL_RING_MAX 16

typedef struct {
    int  x, y, w, h;
    char url[160];
} md_url_hit_t;

static md_url_hit_t g_url_ring[MD_URL_RING_MAX];
static int g_url_ring_head = 0;
static int g_url_ring_count = 0;

static int  md_isspace(char c) { return c == ' ' || c == '\t'; }
static int  md_strlen_local(const char *s) { int n = 0; if (!s) return 0; while (s[n]) n++; return n; }

static void url_ring_push(int x, int y, int w, int h, const char *url, int url_len) {
    md_url_hit_t *h_ent = &g_url_ring[g_url_ring_head];
    h_ent->x = x; h_ent->y = y; h_ent->w = w; h_ent->h = h;
    int n = url_len < (int)sizeof(h_ent->url) - 1 ? url_len : (int)sizeof(h_ent->url) - 1;
    for (int i = 0; i < n; i++) h_ent->url[i] = url[i];
    h_ent->url[n] = 0;
    g_url_ring_head = (g_url_ring_head + 1) % MD_URL_RING_MAX;
    if (g_url_ring_count < MD_URL_RING_MAX) g_url_ring_count++;
}

int md_render_url_at(int x, int y, char *out, int out_max) {
    if (!out || out_max <= 0) return 0;
    out[0] = 0;
    for (int i = 0; i < g_url_ring_count; i++) {
        const md_url_hit_t *h = &g_url_ring[i];
        if (x >= h->x && x < h->x + h->w &&
            y >= h->y && y < h->y + h->h) {
            int n = 0;
            while (h->url[n] && n < out_max - 1) { out[n] = h->url[n]; n++; }
            out[n] = 0;
            return 1;
        }
    }
    return 0;
}

/* Detect http:// or https:// URL starting at buf[i]. Returns the URL
 * length (>= 7 for http://x), or 0 if no URL. */
static int detect_url(const char *buf, int n, int i) {
    if (i + 7 > n) return 0;
    int prefix = 0;
    if (buf[i] == 'h' && buf[i+1] == 't' && buf[i+2] == 't' && buf[i+3] == 'p') {
        if (buf[i+4] == ':' && buf[i+5] == '/' && buf[i+6] == '/') prefix = 7;
        else if (buf[i+4] == 's' && i + 8 <= n &&
                 buf[i+5] == ':' && buf[i+6] == '/' && buf[i+7] == '/') prefix = 8;
    }
    if (!prefix) return 0;
    int k = i + prefix;
    while (k < n) {
        char c = buf[k];
        if (c == ' ' || c == '\t' || c == '\n' || c == ')' || c == ']' || c == '"' || c == '<') break;
        k++;
    }
    int len = k - i;
    return len > prefix ? len : 0;
}

/* Render an inline run of body text with bold/italic/code/wikilink/url
 * tinting. Returns the new x position. */
static int draw_inline(int x, int y, const char *buf, int n) {
    int xpos = x;
    int j = 0;
    while (j < n) {
        /* URL? */
        int ulen = detect_url(buf, n, j);
        if (ulen > 0) {
            char tmp[160]; int m = ulen > 159 ? 159 : ulen;
            for (int q = 0; q < m; q++) tmp[q] = buf[j + q]; tmp[m] = 0;
            int new_x = font_draw(xpos, y, tmp, FONT_UI, TYPE_BODY, COLOR_PRIMARY);
            url_ring_push(xpos, y - 2, new_x - xpos, 16, tmp, m);
            xpos = new_x;
            j += ulen;
            continue;
        }
        /* wikilink */
        if (j + 1 < n && buf[j] == '[' && buf[j+1] == '[') {
            int k = j + 2;
            while (k + 1 < n && !(buf[k] == ']' && buf[k+1] == ']')) k++;
            int len = k - (j + 2);
            char tmp[96]; int m = len > 95 ? 95 : len;
            for (int q = 0; q < m; q++) tmp[q] = buf[j + 2 + q]; tmp[m] = 0;
            xpos = font_draw(xpos, y, tmp, FONT_UI, TYPE_BODY, COLOR_PRIMARY);
            j = (k + 2 <= n) ? k + 2 : n;
            continue;
        }
        /* bold (**text**) */
        if (j + 1 < n && buf[j] == '*' && buf[j+1] == '*') {
            int k = j + 2;
            while (k + 1 < n && !(buf[k] == '*' && buf[k+1] == '*')) k++;
            int len = k - (j + 2);
            char tmp[96]; int m = len > 95 ? 95 : len;
            for (int q = 0; q < m; q++) tmp[q] = buf[j + 2 + q]; tmp[m] = 0;
            xpos = font_draw(xpos, y, tmp, FONT_UI_BOLD, TYPE_BODY, COLOR_ON_SURFACE);
            j = (k + 2 <= n) ? k + 2 : n;
            continue;
        }
        /* italic (*text*) — single asterisk, not part of ** */
        if (buf[j] == '*' && (j == 0 || buf[j-1] != '*') &&
            j + 1 < n && buf[j+1] != '*') {
            int k = j + 1;
            while (k < n && buf[k] != '*') k++;
            int len = k - (j + 1);
            char tmp[96]; int m = len > 95 ? 95 : len;
            for (int q = 0; q < m; q++) tmp[q] = buf[j + 1 + q]; tmp[m] = 0;
            /* Italic glyphs not packed; render bold as a near-substitute
             * so the emphasis is visible. */
            xpos = font_draw(xpos, y, tmp, FONT_UI_BOLD, TYPE_BODY, COLOR_ON_SURFACE);
            j = (k + 1 <= n) ? k + 1 : n;
            continue;
        }
        /* inline code */
        if (buf[j] == '`') {
            int k = j + 1;
            while (k < n && buf[k] != '`') k++;
            int len = k - (j + 1);
            char tmp[96]; int m = len > 95 ? 95 : len;
            for (int q = 0; q < m; q++) tmp[q] = buf[j + 1 + q]; tmp[m] = 0;
            xpos = font_draw(xpos, y, tmp, FONT_CODE, TYPE_LABEL, COLOR_WARNING);
            j = (k + 1 <= n) ? k + 1 : n;
            continue;
        }
        /* default: emit one character */
        char ch[2] = { buf[j], 0 };
        xpos = font_draw(xpos, y, ch, FONT_UI, TYPE_BODY, COLOR_ON_SURFACE);
        j++;
    }
    return xpos;
}

void md_render(int x, int y, int w, int h,
               const char *src, int src_len) {
    if (!src || src_len <= 0 || w <= 0 || h <= 0) return;
    fb_rect(x, y, w, h, COLOR_SURFACE);
    int cx = x + 12;
    int cy = y + 12;
    int line_h = 18;
    int i = 0;
    int in_code = 0;
    while (i < src_len && cy < y + h - 16) {
        int line_start = i;
        while (i < src_len && src[i] != '\n') i++;
        int line_end = i;
        if (i < src_len) i++;

        if (line_end - line_start >= 3
            && src[line_start] == '`' && src[line_start + 1] == '`'
            && src[line_start + 2] == '`') {
            in_code = !in_code;
            cy += 6;
            continue;
        }

        char buf[256];
        int n = line_end - line_start;
        if (n > 255) n = 255;
        for (int k = 0; k < n; k++) buf[k] = src[line_start + k];
        buf[n] = 0;

        if (in_code) {
            fb_rect(cx - 4, cy - 2, w - 24, line_h + 2, COLOR_SURFACE_HIGH);
            font_draw(cx, cy, buf, FONT_CODE, TYPE_LABEL, COLOR_PRIMARY);
            cy += line_h;
            continue;
        }

        int p = 0; while (p < n && md_isspace(buf[p])) p++;
        if (p < n && buf[p] == '#') {
            int level = 0;
            while (p < n && buf[p] == '#') { level++; p++; }
            while (p < n && md_isspace(buf[p])) p++;
            int sz = (level == 1) ? TYPE_TITLE : (level == 2) ? TYPE_HEADING : TYPE_BODY;
            font_draw(cx, cy, buf + p, FONT_UI_BOLD, sz, COLOR_ON_SURFACE);
            cy += sz + 8;
            continue;
        }
        if (p < n && buf[p] == '>') {
            fb_rect(cx, cy, 3, line_h, COLOR_PRIMARY);
            (void)draw_inline(cx + 12, cy, buf + p + 1, n - p - 1);
            cy += line_h;
            continue;
        }
        if (p < n && buf[p] == '-' && p + 1 < n && buf[p+1] == ' ') {
            fb_circle_filled(cx + 4, cy + 8, 2, COLOR_PRIMARY);
            (void)draw_inline(cx + 16, cy, buf + p + 2, n - p - 2);
            cy += line_h;
            continue;
        }
        (void)draw_inline(cx, cy, buf, n);
        cy += line_h;
    }
}

int md_render_inline(int x, int y, int w_max,
                     const char *src, int src_len) {
    (void)w_max;
    if (!src || src_len <= 0) return y;
    /* Inline mode: no fill, no headings/lists/quotes; just bold,
     * italic, inline code, wikilinks, URLs. Single line if it fits. */
    /* Strip trailing newline. */
    int n = src_len;
    while (n > 0 && (src[n-1] == '\n' || src[n-1] == '\r')) n--;
    (void)draw_inline(x, y, src, n);
    return y + 16;
}
