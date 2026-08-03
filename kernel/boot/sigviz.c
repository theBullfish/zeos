/*
 * Zeos -- Signal Chain Visualizer
 *
 * Draws signal chains as box-and-arrow diagrams on the framebuffer.
 * Uses fb drawing primitives (fb_rect, fb_line, fb_text, etc).
 *
 * Layout: tree with roots at top, children below parents.
 * Routes shown as lines between connected nodes.
 * Interactive: click to select, zoom/pan, live state animation.
 */

#include "sigviz.h"
#include "signal.h"
#include "chain.h"
#include "mde.h"
#include "fb.h"
#include "inspector.h"

/* ── Dimensions (base, before zoom) ─────────────────────────────── */

#define NODE_W      140
#define NODE_H      32
#define NODE_PAD_V  48      /* Vertical spacing between depth levels */
#define NODE_PAD_H  24      /* Horizontal spacing between siblings */
#define ARROW_HEAD  6       /* Arrow head size */
#define MARGIN      12      /* Margin from draw area edges */
#define TITLE_H     24      /* Title bar height */

/* Flash duration in frames */
#define FLASH_DURATION 12

/* ── Static state ───────────────────────────────────────────────── */

static sigviz_state_t viz;

/* ── Helpers ────────────────────────────────────────────────────── */

static int sviz_strlen(const char *s)
{
    int len = 0;
    while (s && s[len]) len++;
    return len;
}

/* Bare-metal sine approximation: input 0..255 maps to 0..255 output.
 * Uses a piecewise quadratic that looks close enough for UI pulse. */
static int sine_pulse(uint32_t frame, int period)
{
    int phase = (int)(frame % (uint32_t)period);
    /* Map phase to 0..511 (half period up, half down) */
    int half = period / 2;
    int t;
    if (phase < half)
        t = (phase * 255) / half;
    else
        t = ((period - phase) * 255) / half;
    /* Smooth with cheap quadratic: sin-ish curve */
    int x = t - 128;
    int y = 128 + (x * (256 - (x < 0 ? -x : x))) / 128;
    if (y < 0) y = 0;
    if (y > 255) y = 255;
    return y;
}

/* Apply alpha to a color (set alpha channel byte) */
static uint32_t color_with_alpha(uint32_t color, int alpha)
{
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    return ((uint32_t)alpha << 24) | (color & 0x00FFFFFF);
}

/* Apply zoom to an integer coordinate */
static int zscale(int v)
{
    return (int)((float)v * viz.zoom);
}

/* Transform a layout coordinate to screen coordinate */
static int to_screen_x(int lx)
{
    return viz.area_x + zscale(lx) + viz.pan_x;
}

static int to_screen_y(int ly)
{
    return viz.area_y + TITLE_H + zscale(ly) + viz.pan_y;
}

/* ── Layout computation ─────────────────────────────────────────── */

/* Find layout node by chain_id, returns index or -1 */
static int layout_find(int chain_id)
{
    for (int i = 0; i < viz.layout_count; i++) {
        if (viz.layout[i].chain_id == chain_id)
            return i;
    }
    return -1;
}

/* Count children of a chain in the chain registry */
static int count_children(int parent_id)
{
    int count = 0;
    int total = chain_count();
    for (int i = 0; i < total && i < MAX_CHAINS; i++) {
        chain_t *c = chain_get(i);
        if (c && c->parent_id == parent_id)
            count++;
    }
    return count;
}

/* Recursive: add chain and its children to layout.
 * x_offset: horizontal pixel offset for this subtree's leftmost edge.
 * Returns the total width consumed by this subtree. */
static int layout_subtree(int chain_id, int depth, int x_offset)
{
    if (viz.layout_count >= SIGVIZ_MAX_LAYOUT)
        return NODE_W;

    int idx = viz.layout_count++;
    viz.layout[idx].chain_id = chain_id;
    viz.layout[idx].depth = depth;
    viz.layout[idx].w = NODE_W;
    viz.layout[idx].h = NODE_H;
    viz.layout[idx].y = MARGIN + depth * (NODE_H + NODE_PAD_V);

    /* Gather children */
    int child_ids[MAX_CHAINS];
    int nchildren = 0;
    int total = chain_count();
    for (int i = 0; i < total && i < MAX_CHAINS; i++) {
        chain_t *c = chain_get(i);
        if (c && c->parent_id == chain_id) {
            child_ids[nchildren++] = c->id;
            if (nchildren >= MAX_CHAINS) break;
        }
    }

    viz.layout[idx].child_count = nchildren;
    viz.layout[idx].child_index = 0; /* set by parent */

    if (nchildren == 0) {
        /* Leaf: occupies one node width */
        viz.layout[idx].x = x_offset;
        return NODE_W + NODE_PAD_H;
    }

    /* Layout children, accumulate width */
    int child_x = x_offset;
    for (int c = 0; c < nchildren; c++) {
        /* Set child_index before recursing */
        int before = viz.layout_count;
        int w = layout_subtree(child_ids[c], depth + 1, child_x);
        if (before < viz.layout_count)
            viz.layout[before].child_index = c;
        child_x += w;
    }

    int subtree_width = child_x - x_offset;
    if (subtree_width < NODE_W + NODE_PAD_H)
        subtree_width = NODE_W + NODE_PAD_H;

    /* Center parent over its children */
    viz.layout[idx].x = x_offset + (subtree_width - NODE_W) / 2;

    return subtree_width;
}

/* Build the full layout for all root chains (parent_id == -1),
 * or for a single chain if chain_id >= 0. */
static void layout_build(int root_chain_id)
{
    viz.layout_count = 0;

    if (root_chain_id >= 0) {
        /* Single root */
        layout_subtree(root_chain_id, 0, MARGIN);
        return;
    }

    /* All roots */
    int x_offset = MARGIN;
    int total = chain_count();
    for (int i = 0; i < total && i < MAX_CHAINS; i++) {
        chain_t *c = chain_get(i);
        if (c && c->parent_id == -1) {
            int w = layout_subtree(c->id, 0, x_offset);
            x_offset += w + NODE_PAD_H;
        }
    }
}

/* ── Drawing ────────────────────────────────────────────────────── */

/* Get color for a signal node state */
static uint32_t state_color(enum sig_node_state state)
{
    switch (state) {
    case SIG_IDLE:    return SIGVIZ_IDLE;
    case SIG_READY:   return SIGVIZ_READY;
    case SIG_RUNNING: return SIGVIZ_RUNNING;
    case SIG_DONE:    return SIGVIZ_DONE;
    case SIG_ERROR:   return SIGVIZ_ERROR;
    }
    return SIGVIZ_IDLE;
}

/* Get the chain_status_t for animation purposes */
static chain_status_t get_chain_status(int chain_id)
{
    chain_t *c = chain_get(chain_id);
    if (c) return c->status;
    return CHAIN_DETACHED;
}

/* Draw an arrow from (x0,y0) to (x1,y1) */
static void draw_arrow(int x0, int y0, int x1, int y1, uint32_t color)
{
    fb_line(x0, y0, x1, y1, color);

    /* Arrow head pointing toward (x1,y1) */
    /* Simplified: always draw a small downward chevron at the tip */
    if (y1 > y0) {
        fb_line(x1, y1, x1 - ARROW_HEAD, y1 - ARROW_HEAD, color);
        fb_line(x1, y1, x1 + ARROW_HEAD, y1 - ARROW_HEAD, color);
    } else if (y1 < y0) {
        fb_line(x1, y1, x1 - ARROW_HEAD, y1 + ARROW_HEAD, color);
        fb_line(x1, y1, x1 + ARROW_HEAD, y1 + ARROW_HEAD, color);
    } else {
        /* Horizontal */
        if (x1 > x0) {
            fb_line(x1, y1, x1 - ARROW_HEAD, y1 - ARROW_HEAD, color);
            fb_line(x1, y1, x1 - ARROW_HEAD, y1 + ARROW_HEAD, color);
        } else {
            fb_line(x1, y1, x1 + ARROW_HEAD, y1 - ARROW_HEAD, color);
            fb_line(x1, y1, x1 + ARROW_HEAD, y1 + ARROW_HEAD, color);
        }
    }
}

/* Draw a single node box */
static void draw_node(sigviz_layout_node_t *ln)
{
    chain_t *c = chain_get(ln->chain_id);
    if (!c) return;

    int sx = to_screen_x(ln->x);
    int sy = to_screen_y(ln->y);
    int sw = zscale(ln->w);
    int sh = zscale(ln->h);

    /* Clip: skip if entirely outside the draw area */
    if (sx + sw < viz.area_x || sx > viz.area_x + viz.area_w) return;
    if (sy + sh < viz.area_y || sy > viz.area_y + viz.area_h) return;

    /* Determine opacity based on chain status */
    chain_status_t status = c->status;
    int alpha = 255;

    if (status == CHAIN_PAUSED) {
        /* Paused: 60% opacity */
        alpha = 153;
    } else if (status == CHAIN_LIVE) {
        /* Running: gentle pulse 180..255 */
        int pulse = sine_pulse(viz.frame, 60); /* ~1 second at 60fps */
        alpha = 180 + (pulse * 75) / 255;
    }

    /* Node background with alpha */
    uint32_t bg = color_with_alpha(SIGVIZ_NODE_BG, alpha);
    fb_rect_blend(sx, sy, sw, sh, bg);

    /* State indicator (left bar, 4px scaled) */
    int bar_w = zscale(4);
    if (bar_w < 1) bar_w = 1;

    /* Determine state color from first signal node (if signal chain exists) */
    struct sig_chain *sc = sig_get_chain(ln->chain_id);
    uint32_t scolor = SIGVIZ_IDLE;
    if (sc && sc->node_count > 0) {
        /* Use worst state among nodes */
        enum sig_node_state worst = SIG_IDLE;
        for (int n = 0; n < sc->node_count; n++) {
            if (sc->nodes[n].state > worst)
                worst = sc->nodes[n].state;
        }
        scolor = state_color(worst);
    } else {
        /* Derive from chain status */
        if (status == CHAIN_LIVE)     scolor = SIGVIZ_RUNNING;
        else if (status == CHAIN_ERROR) scolor = SIGVIZ_ERROR;
        else if (status == CHAIN_PAUSED) scolor = SIGVIZ_READY;
    }
    fb_rect(sx, sy, bar_w, sh, scolor);

    /* Error: border flashes danger */
    int is_error = (status == CHAIN_ERROR);
    int error_flash = is_error && ((viz.frame / 8) & 1);

    /* Resolve flash */
    int is_flashing = (viz.flash_id == ln->chain_id && viz.flash_frames > 0);

    /* Border */
    int is_selected = (viz.selected_id == ln->chain_id);
    int border_thick = is_selected ? 3 : 1;
    uint32_t border_color;

    if (is_flashing) {
        border_color = theme_accent();
        border_thick = 3;
    } else if (error_flash) {
        border_color = SIGVIZ_ERROR;
        border_thick = 2;
    } else if (is_selected) {
        border_color = theme_accent();
    } else {
        border_color = theme_accent_dim();
    }
    fb_rect_outline(sx, sy, sw, sh, border_color, border_thick);

    /* Node name */
    int text_x = sx + zscale(8);
    int text_y = sy + zscale(4);
    fb_text(text_x, text_y, c->name, color_with_alpha(SIGVIZ_TEXT, alpha));

    /* Selected: show type info below name if there's room */
    if (is_selected && c->node_count > 0) {
        int type_y = sy + zscale(18);
        /* Show first node's input/output types */
        const char *itype = c->nodes[0].input_type;
        const char *otype = c->nodes[0].output_type;
        if (sviz_strlen(itype) > 0 || sviz_strlen(otype) > 0) {
            /* Build a tiny type string: "in->out" */
            /* Just show output type to keep it short */
            if (sviz_strlen(otype) > 0) {
                fb_text(text_x, type_y, otype, COLOR_ON_SURFACE_3);
            }
        }
    }

    /* Timing info (if chain has resolved) */
    if (c->last_resolve_ms > 0.0f && !is_selected) {
        /* Draw a small timing bar at the bottom of the node */
        int bar_max = sw - zscale(16);
        if (bar_max < 8) bar_max = 8;
        float max_ms = 10.0f;  /* Scale reference */
        int bw = (int)(c->last_resolve_ms * (float)bar_max / max_ms);
        if (bw > bar_max) bw = bar_max;
        if (bw < 2) bw = 2;
        int bx = sx + sw - bw - zscale(4);
        int by = sy + sh - zscale(5);
        fb_rect(bx, by, bw, zscale(3), scolor);
    }
}

/* Draw MDE routes between chain nodes */
static void draw_routes(void)
{
    int rcount = mde_route_count();
    if (rcount <= 0) return;

    /* Iterate all routes by checking pairs in layout */
    for (int i = 0; i < viz.layout_count; i++) {
        int from_id = viz.layout[i].chain_id;
        mde_route_t routes[16];
        int n = mde_find_routes_from(from_id, routes, 16);

        for (int r = 0; r < n; r++) {
            int to_id = routes[r].to_chain;
            int j = layout_find(to_id);
            if (j < 0) continue;

            /* Draw line from bottom-center of source to top-center of dest */
            int x0 = to_screen_x(viz.layout[i].x + viz.layout[i].w / 2);
            int y0 = to_screen_y(viz.layout[i].y + viz.layout[i].h);
            int x1 = to_screen_x(viz.layout[j].x + viz.layout[j].w / 2);
            int y1 = to_screen_y(viz.layout[j].y);

            /* Route color: dim for inactive, brighter for active */
            uint32_t rcol = routes[r].active ? SIGVIZ_EDGE : COLOR_ON_SURFACE_4;

            draw_arrow(x0, y0, x1, y1, rcol);
        }
    }
}

/* Draw parent->child tree edges (structural, not MDE routes) */
static void draw_tree_edges(void)
{
    for (int i = 0; i < viz.layout_count; i++) {
        int cid = viz.layout[i].chain_id;
        chain_t *c = chain_get(cid);
        if (!c || c->parent_id < 0) continue;

        int pidx = layout_find(c->parent_id);
        if (pidx < 0) continue;

        /* Line from parent bottom-center to child top-center */
        int x0 = to_screen_x(viz.layout[pidx].x + viz.layout[pidx].w / 2);
        int y0 = to_screen_y(viz.layout[pidx].y + viz.layout[pidx].h);
        int x1 = to_screen_x(viz.layout[i].x + viz.layout[i].w / 2);
        int y1 = to_screen_y(viz.layout[i].y);

        /* Structural edge: subtle, dashed feel via dimmer color */
        fb_line(x0, y0, x1, y1, COLOR_ON_SURFACE_4);
    }
}

/* ── Public API ─────────────────────────────────────────────────── */

void sigviz_init(void)
{
    viz.zoom = 1.0f;
    viz.pan_x = 0;
    viz.pan_y = 0;
    viz.selected_id = -1;
    viz.frame = 0;
    viz.flash_id = -1;
    viz.flash_frames = 0;
    viz.layout_count = 0;
    viz.area_x = 0;
    viz.area_y = 0;
    viz.area_w = 0;
    viz.area_h = 0;
}

void sigviz_draw(int chain_id, int x, int y, int w, int h)
{
    /* Store draw area */
    viz.area_x = x;
    viz.area_y = y;
    viz.area_w = w;
    viz.area_h = h;

    /* Background */
    fb_rect(x, y, w, h, SIGVIZ_BG);

    /* Title bar */
    fb_rect(x, y, w, TITLE_H, COLOR_SURFACE_HIGH);

    if (chain_id >= 0) {
        chain_t *root = chain_get(chain_id);
        if (root)
            fb_text(x + 4, y + 4, root->name, SIGVIZ_TEXT);
        else
            fb_text(x + 4, y + 4, "Signal Graph", SIGVIZ_TEXT);
    } else {
        fb_text(x + 4, y + 4, "Signal Graph", SIGVIZ_TEXT);
    }

    /* Zoom indicator in title bar */
    {
        char zbuf[16];
        int zi = 0;
        int zpct = (int)(viz.zoom * 100.0f);
        if (zpct >= 100) {
            zbuf[zi++] = '0' + (zpct / 100);
            zpct %= 100;
            zbuf[zi++] = '0' + (zpct / 10);
            zbuf[zi++] = '0' + (zpct % 10);
        } else {
            zbuf[zi++] = '0' + (zpct / 10);
            zbuf[zi++] = '0' + (zpct % 10);
        }
        zbuf[zi++] = '%';
        zbuf[zi] = '\0';
        fb_text(x + w - 40, y + 4, zbuf, COLOR_ON_SURFACE_3);
    }

    /* Rebuild layout */
    layout_build(chain_id);

    if (viz.layout_count == 0) {
        fb_text(x + MARGIN, y + TITLE_H + MARGIN, "No chains", COLOR_ON_SURFACE_3);
        return;
    }

    /* Draw tree edges first (behind everything) */
    draw_tree_edges();

    /* Draw MDE routes (type-matched connections) */
    draw_routes();

    /* Draw all nodes */
    for (int i = 0; i < viz.layout_count; i++) {
        draw_node(&viz.layout[i]);
    }

    /* Decrement flash counter */
    if (viz.flash_frames > 0) {
        viz.flash_frames--;
        if (viz.flash_frames == 0)
            viz.flash_id = -1;
    }
}

int sigviz_click(int x, int y)
{
    /* Hit-test against all layout nodes (in screen coordinates) */
    for (int i = 0; i < viz.layout_count; i++) {
        sigviz_layout_node_t *ln = &viz.layout[i];

        int sx = to_screen_x(ln->x);
        int sy = to_screen_y(ln->y);
        int sw = zscale(ln->w);
        int sh = zscale(ln->h);

        if (x >= sx && x < sx + sw && y >= sy && y < sy + sh) {
            int cid = ln->chain_id;
            if (viz.selected_id == cid) {
                /* Already selected: open inspector */
                inspector_show(cid);
            }
            viz.selected_id = cid;
            return cid;
        }
    }

    /* Clicked empty space: deselect */
    viz.selected_id = -1;
    return -1;
}

int sigviz_get_selected(void)
{
    return viz.selected_id;
}

void sigviz_deselect(void)
{
    viz.selected_id = -1;
}

void sigviz_zoom_in(void)
{
    viz.zoom += SIGVIZ_ZOOM_STEP;
    if (viz.zoom > SIGVIZ_ZOOM_MAX)
        viz.zoom = SIGVIZ_ZOOM_MAX;
}

void sigviz_zoom_out(void)
{
    viz.zoom -= SIGVIZ_ZOOM_STEP;
    if (viz.zoom < SIGVIZ_ZOOM_MIN)
        viz.zoom = SIGVIZ_ZOOM_MIN;
}

void sigviz_zoom_set(float level)
{
    viz.zoom = level;
    if (viz.zoom < SIGVIZ_ZOOM_MIN) viz.zoom = SIGVIZ_ZOOM_MIN;
    if (viz.zoom > SIGVIZ_ZOOM_MAX) viz.zoom = SIGVIZ_ZOOM_MAX;
}

void sigviz_pan(int dx, int dy)
{
    viz.pan_x += dx;
    viz.pan_y += dy;
}

void sigviz_pan_reset(void)
{
    viz.pan_x = 0;
    viz.pan_y = 0;
}

void sigviz_notify_resolved(int chain_id)
{
    viz.flash_id = chain_id;
    viz.flash_frames = FLASH_DURATION;
}

void sigviz_tick(void)
{
    viz.frame++;
}

const sigviz_state_t *sigviz_get_state(void)
{
    return &viz;
}

/* ── K.1: chain-graph overlay (wires ACTION_CHAIN_GRAPH, previously a stub) ──
 * A full-screen dimmed panel that renders the live chain graph via sigviz_draw
 * over all chains (-1). Toggled by Super+G; Esc/Super+G closes. sigviz_init on
 * open so zoom/pan are sane (it was never called at boot). */
static int g_sigviz_overlay = 0;

void sigviz_overlay_toggle(void)
{
    g_sigviz_overlay = !g_sigviz_overlay;
    if (g_sigviz_overlay) sigviz_init();
}

int sigviz_overlay_visible(void) { return g_sigviz_overlay; }

void sigviz_overlay_close(void) { g_sigviz_overlay = 0; }

void sigviz_overlay_draw(void)
{
    if (!g_sigviz_overlay) return;
    int sw = (int)fb_width();
    int sh = (int)fb_height();
    int m  = 80;
    fb_rect_blend(0, 0, sw, sh, 0xC00D1117u);          /* dim scrim */
    sigviz_draw(-1, m, m, sw - 2 * m, sh - 2 * m);     /* all chains */
}
