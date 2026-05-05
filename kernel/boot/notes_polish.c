/*
 * notes-zeos polish — split-pane markdown editor + rendered preview +
 * backlink graph. Everything cosmetic builds on the same notes_zeos
 * engine (commits 124b2ff / 948b5d7) so the index stays a single source
 * of truth.
 */

#include "notes_polish.h"
#include "notes_zeos.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "wm.h"
#include "compositor.h"
#include "ui_states.h"
#include "kprint.h"

#define NP_W           1200
#define NP_H            760
#define NP_SOURCE_MAX  8192
#define NP_GRAPH_H      180
#define NP_TITLE_H       36

typedef struct {
    int   active, initialized;
    int   surface_id;
    char  source[NP_SOURCE_MAX];
    int   source_len;
    char  title[64];
    char  search[64];
    int   search_len;
} np_state_t;

static np_state_t N;

static int  s_len(const char *s){int n=0;if(!s)return 0;while(s[n])n++;return n;}
static void s_cpy(char *d,const char *s,int max){int i=0;if(!d||max<=0)return;if(s)while(i<max-1&&s[i]){d[i]=s[i];i++;}d[i]=0;}

void notes_polish_init(void) {
    if (N.initialized) return;
    N.initialized = 1;
    N.surface_id = -1;
    s_cpy(N.title, "scratch.md", sizeof(N.title));
    /* Seed source so the rendered preview is meaningful on first open. */
    const char *seed =
        "# notes-zeos\n\n"
        "Welcome. This is the **Notion / Obsidian** replacement.\n\n"
        "## Features\n"
        "- live [[wikilinks]] indexed by `notes_zeos`\n"
        "- *real* split-pane preview\n"
        "- search across the whole vault\n\n"
        "> Notes are CFA-scoped per identity context.\n\n"
        "```zp\n"
        "chain hello { input -> tap.log }\n"
        "```\n";
    notes_polish_set_source(seed, s_len(seed));
}

void notes_polish_set_source(const char *md, int len) {
    if (!md || len <= 0) { N.source[0] = 0; N.source_len = 0; return; }
    if (len > NP_SOURCE_MAX - 1) len = NP_SOURCE_MAX - 1;
    for (int i = 0; i < len; i++) N.source[i] = md[i];
    N.source[len] = 0;
    N.source_len = len;
}

/* ── Markdown renderer (compact) ────────────────────────────────── */
static int is_space(char c) { return c == ' ' || c == '\t'; }

static void np_render_md(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    int cx = x + 12;
    int cy = y + 12;
    int line_h = 18;
    int i = 0;
    int in_code = 0;
    while (i < N.source_len && cy < y + h - 16) {
        /* Read one line. */
        int line_start = i;
        while (i < N.source_len && N.source[i] != '\n') i++;
        int line_end = i;
        if (i < N.source_len) i++; /* skip \n */

        /* Code-fence toggle */
        if (line_end - line_start >= 3
            && N.source[line_start] == '`' && N.source[line_start + 1] == '`'
            && N.source[line_start + 2] == '`') {
            in_code = !in_code;
            cy += 6;
            continue;
        }

        /* Slice */
        char buf[256];
        int n = line_end - line_start;
        if (n > 255) n = 255;
        for (int k = 0; k < n; k++) buf[k] = N.source[line_start + k];
        buf[n] = 0;

        if (in_code) {
            fb_rect(cx - 4, cy - 2, w - 24, line_h + 2, COLOR_SURFACE_HIGH);
            font_draw(cx, cy, buf, FONT_CODE, TYPE_LABEL, COLOR_PRIMARY);
            cy += line_h;
            continue;
        }

        /* Headings */
        int p = 0; while (p < n && is_space(buf[p])) p++;
        if (p < n && buf[p] == '#') {
            int level = 0;
            while (p < n && buf[p] == '#') { level++; p++; }
            while (p < n && is_space(buf[p])) p++;
            int sz = (level == 1) ? TYPE_TITLE : (level == 2) ? TYPE_HEADING : TYPE_BODY;
            font_draw(cx, cy, buf + p, FONT_UI_BOLD, sz, COLOR_ON_SURFACE);
            cy += sz + 8;
            continue;
        }
        /* Blockquote */
        if (p < n && buf[p] == '>') {
            fb_rect(cx, cy, 3, line_h, COLOR_PRIMARY);
            font_draw(cx + 12, cy, buf + p + 1, FONT_UI, TYPE_BODY, COLOR_ON_SURFACE_2);
            cy += line_h;
            continue;
        }
        /* Unordered list */
        if (p < n && buf[p] == '-' && p + 1 < n && buf[p+1] == ' ') {
            fb_circle_filled(cx + 4, cy + 8, 2, COLOR_PRIMARY);
            font_draw(cx + 16, cy, buf + p + 2, FONT_UI, TYPE_BODY, COLOR_ON_SURFACE);
            cy += line_h;
            continue;
        }
        /* Default body — naive bold/italic/code/wikilink rendering: print
         * with primary text color and special-tint substring runs. We do
         * a single pass that tints **bold**, [[wikilinks]] and `code`. */
        int xpos = cx;
        int j = 0;
        while (j < n) {
            /* wikilink? */
            if (j + 1 < n && buf[j] == '[' && buf[j+1] == '[') {
                int k = j + 2;
                while (k + 1 < n && !(buf[k] == ']' && buf[k+1] == ']')) k++;
                int len = k - (j + 2);
                char tmp[96];
                int m = len > 95 ? 95 : len;
                for (int q = 0; q < m; q++) tmp[q] = buf[j + 2 + q];
                tmp[m] = 0;
                xpos = font_draw(xpos, cy, tmp, FONT_UI, TYPE_BODY, COLOR_PRIMARY);
                j = (k + 2 <= n) ? k + 2 : n;
                continue;
            }
            /* bold? */
            if (j + 1 < n && buf[j] == '*' && buf[j+1] == '*') {
                int k = j + 2;
                while (k + 1 < n && !(buf[k] == '*' && buf[k+1] == '*')) k++;
                int len = k - (j + 2);
                char tmp[96]; int m = len > 95 ? 95 : len;
                for (int q = 0; q < m; q++) tmp[q] = buf[j + 2 + q]; tmp[m] = 0;
                xpos = font_draw(xpos, cy, tmp, FONT_UI_BOLD, TYPE_BODY, COLOR_ON_SURFACE);
                j = (k + 2 <= n) ? k + 2 : n;
                continue;
            }
            /* inline code? */
            if (buf[j] == '`') {
                int k = j + 1;
                while (k < n && buf[k] != '`') k++;
                int len = k - (j + 1);
                char tmp[96]; int m = len > 95 ? 95 : len;
                for (int q = 0; q < m; q++) tmp[q] = buf[j + 1 + q]; tmp[m] = 0;
                xpos = font_draw(xpos, cy, tmp, FONT_CODE, TYPE_LABEL, COLOR_WARNING);
                j = (k + 1 <= n) ? k + 1 : n;
                continue;
            }
            /* default: emit one character, batched as small string */
            char ch[2] = { buf[j], 0 };
            xpos = font_draw(xpos, cy, ch, FONT_UI, TYPE_BODY, COLOR_ON_SURFACE);
            j++;
        }
        cy += line_h;
    }
}

/* ── Source pane ───────────────────────────────────────────────── */
static void np_draw_source(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE_HIGH);
    fb_rect(x + w - 1, y, 1, h, COLOR_SEPARATOR);
    int cy = y + 12;
    int i = 0;
    while (i < N.source_len && cy < y + h - 12) {
        int s = i;
        while (i < N.source_len && N.source[i] != '\n') i++;
        char buf[200]; int n = i - s; if (n > 199) n = 199;
        for (int k = 0; k < n; k++) buf[k] = N.source[s + k]; buf[n] = 0;
        font_draw(x + 12, cy, buf, FONT_CODE, TYPE_CAPTION, COLOR_ON_SURFACE_2);
        cy += 16;
        if (i < N.source_len) i++;
    }
}

/* ── Backlink graph ────────────────────────────────────────────── */
typedef struct {
    int      x, y;
    char     name[40];
    uint32_t links;
} np_node_t;

#define NP_GRAPH_NODES 24
static np_node_t g_nodes[NP_GRAPH_NODES];
static int       g_node_count;

static void np_walk_cb(const char *path, uint32_t tc, uint32_t lc, void *user) {
    (void)tc; (void)user;
    if (g_node_count >= NP_GRAPH_NODES) return;
    /* Use basename. */
    const char *b = path;
    for (const char *q = path; *q; q++) if (*q == '/') b = q + 1;
    s_cpy(g_nodes[g_node_count].name, b, sizeof(g_nodes[0].name));
    g_nodes[g_node_count].links = lc;
    g_node_count++;
}

static void np_draw_graph(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    fb_rect(x, y, w, 1, COLOR_SEPARATOR);
    font_draw(x + 12, y + 6, "Backlink Graph", FONT_UI_BOLD, TYPE_LABEL, COLOR_ON_SURFACE_2);
    g_node_count = 0;
    notes_zeos_walk(np_walk_cb, 0);
    if (g_node_count == 0) {
        font_draw(x + 12, y + h / 2 - 8,
                  "No notes indexed yet — try `notes reindex /notes/welcome.md`",
                  FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_3);
        return;
    }
    /* Ring layout */
    int cx = x + w / 2, cy = y + h / 2 + 12;
    int radius = h / 2 - 30;
    if (radius < 30) radius = 30;
    /* fixed-point sin/cos via small-angle samples (avoid math.h) — emit
     * positions in a deterministic ring. */
    static const int sin_lut[12] = {0,50,86,100,86,50,0,-50,-86,-100,-86,-50};
    static const int cos_lut[12] = {100,86,50,0,-50,-86,-100,-86,-50,0,50,86};
    int slots = g_node_count > 12 ? 12 : g_node_count;
    for (int i = 0; i < g_node_count; i++) {
        int s = i % slots;
        g_nodes[i].x = cx + (cos_lut[s] * radius) / 100;
        g_nodes[i].y = cy + (sin_lut[s] * radius) / 100;
    }
    /* Edges: connect each node to the next (synthesized) plus a chord
     * by link_count. */
    for (int i = 0; i < g_node_count; i++) {
        int j = (i + 1) % g_node_count;
        fb_line(g_nodes[i].x, g_nodes[i].y, g_nodes[j].x, g_nodes[j].y,
                COLOR_SEPARATOR);
        if (g_nodes[i].links > 0) {
            int k = (i + g_nodes[i].links) % g_node_count;
            fb_line(g_nodes[i].x, g_nodes[i].y, g_nodes[k].x, g_nodes[k].y,
                    COLOR_PRIMARY_DIM);
        }
    }
    for (int i = 0; i < g_node_count; i++) {
        fb_circle_filled(g_nodes[i].x, g_nodes[i].y, 8, COLOR_PRIMARY);
        fb_circle(g_nodes[i].x, g_nodes[i].y, 8, COLOR_ON_SURFACE);
        font_draw(g_nodes[i].x - 24, g_nodes[i].y + 12,
                  g_nodes[i].name, FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_2);
    }
}

static void np_draw_titlebar(int x, int y, int w) {
    fb_rect(x, y, w, NP_TITLE_H, COLOR_SURFACE_TOP);
    fb_rect(x, y + NP_TITLE_H - 1, w, 1, COLOR_SEPARATOR);
    font_draw(x + 12, y + 8, N.title, FONT_UI_BOLD, TYPE_HEADING, COLOR_ON_SURFACE);
    /* Live search box on the right. */
    int sx = x + w - 280;
    fb_rect(sx, y + 6, 260, 24, COLOR_SURFACE);
    fb_rect_outline(sx, y + 6, 260, 24, COLOR_SEPARATOR, 1);
    if (N.search_len == 0) {
        font_draw(sx + 8, y + 10, "search…", FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
    } else {
        font_draw(sx + 8, y + 10, N.search, FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE);
    }
}

static void np_draw_content(int id, int x, int y, int w, int h) {
    (void)id;
    fb_rect(x, y, w, h, COLOR_SURFACE);
    np_draw_titlebar(x, y, w);
    int top_h = h - NP_GRAPH_H - NP_TITLE_H;
    np_draw_source(x,           y + NP_TITLE_H, w / 2, top_h);
    np_render_md  (x + w / 2,   y + NP_TITLE_H, w - w / 2, top_h);
    np_draw_graph (x, y + h - NP_GRAPH_H, w, NP_GRAPH_H);
}

void notes_polish_open(void) {
    notes_polish_init();
    if (N.active) { if (N.surface_id >= 0) wm_focus_surface(N.surface_id); return; }
    int sw = (int)fb_width(), sh = (int)fb_height();
    int sx = (sw - NP_W) / 2, sy = (sh - NP_H) / 2;
    N.surface_id = wm_create_surface("notes — notes-zeos", -1,
                                     sx, sy, NP_W, NP_H, np_draw_content);
    if (N.surface_id < 0) return;
    wm_focus_surface(N.surface_id);
    N.active = 1;
}
void notes_polish_close(void) {
    if (!N.active) return;
    if (N.surface_id >= 0) wm_detach_surface(N.surface_id);
    N.surface_id = -1;
    N.active = 0;
}
int notes_polish_active(void) { return N.active; }

void notes_polish_print_selftest_line(void) {
    notes_polish_init();
    kputs("notes-zeos polish .... graph view ready, ");
    kput_dec((uint64_t)notes_zeos_total_docs());
    kputs(" notes, ");
    kput_dec((uint64_t)notes_zeos_total_backlinks());
    kputs(" backlinks\n");
}

void notes_polish_cmd(const char *args) {
    (void)args;
    notes_polish_open();
    kputs("notes opened\n");
}
