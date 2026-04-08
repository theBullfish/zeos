/*
 * Zeos -- Procedural Icon Rendering
 *
 * Each icon is drawn from framebuffer primitives (fb_rect, fb_line,
 * fb_circle, fb_hline, fb_vline) scaled to the requested pixel size.
 * Coordinates are expressed as fractions of `size` so they stay
 * proportional at ICON_SM (16), ICON_MD (24), ICON_LG (32).
 *
 * Alpha 0.1: procedural shapes. Future: build-time SVG rasterization.
 */

#include "icon_render.h"
#include "fb.h"
#include "theme.h"

/* Persona accent — defined in shell.c, declared in sigviz.h */
extern uint32_t theme_accent(void);

/* ── Helpers ── */

/* Scale a value by size/base. Base=16 is the reference grid. */
static inline int sc(int val, int size)
{
    return (val * size + 8) / 16;
}

/* Minimum thickness: 1px at 16, scales up with size */
static inline int th(int size)
{
    int t = size / 16;
    return t < 1 ? 1 : t;
}

/* ── Individual icon draw functions ── */

/* FILE: rectangle with folded corner (dog-ear) */
static void draw_file(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    int w = sc(10, s);
    int h = sc(14, s);
    int ox = sc(3, s);
    int oy = sc(1, s);
    int fold = sc(3, s);

    /* Body outline */
    fb_rect_outline(x + ox, y + oy, w, h, c, t);
    /* Fold: triangle in top-right */
    fb_line(x + ox + w - fold, y + oy, x + ox + w - fold, y + oy + fold, c);
    fb_line(x + ox + w - fold, y + oy + fold, x + ox + w, y + oy + fold, c);
    /* Text lines inside */
    fb_hline(x + ox + sc(2, s), y + oy + sc(5, s), w - sc(4, s), c);
    fb_hline(x + ox + sc(2, s), y + oy + sc(7, s), w - sc(4, s), c);
    fb_hline(x + ox + sc(2, s), y + oy + sc(9, s), w - sc(5, s), c);
}

/* FOLDER: rectangle with raised tab on top-left */
static void draw_folder(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    int w = sc(14, s);
    int h = sc(10, s);
    int ox = sc(1, s);
    int oy = sc(3, s);
    int tab_w = sc(5, s);
    int tab_h = sc(2, s);

    /* Tab */
    fb_rect_outline(x + ox, y + oy - tab_h, tab_w, tab_h + t, c, t);
    /* Body */
    fb_rect_outline(x + ox, y + oy, w, h, c, t);
}

/* TERMINAL: rectangle with "> _" prompt */
static void draw_terminal(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    int w = sc(14, s);
    int h = sc(12, s);
    int ox = sc(1, s);
    int oy = sc(2, s);

    /* Box */
    fb_rect_outline(x + ox, y + oy, w, h, c, t);
    /* Chevron ">" */
    int cx = x + ox + sc(3, s);
    int cy = y + oy + sc(4, s);
    int ch = sc(3, s);
    fb_line(cx, cy, cx + ch, cy + ch, c);
    fb_line(cx + ch, cy + ch, cx, cy + ch + ch, c);
    /* Underscore cursor */
    fb_hline(cx + ch + sc(2, s), cy + ch + ch, sc(3, s), c);
}

/* BROWSER: circle (globe) with crosshair lines */
static void draw_browser(int x, int y, int s, uint32_t c)
{
    int r = sc(6, s);
    int cx = x + s / 2;
    int cy = y + s / 2;

    fb_circle(cx, cy, r, c);
    /* Horizontal equator */
    fb_hline(cx - r, cy, r * 2, c);
    /* Vertical meridian */
    fb_vline(cx, cy - r, r * 2, c);
}

/* SETTINGS / GEAR: octagon approximation (circle with notches) */
static void draw_settings(int x, int y, int s, uint32_t c)
{
    int cx = x + s / 2;
    int cy = y + s / 2;
    int ro = sc(7, s);  /* outer radius */
    int ri = sc(4, s);  /* inner radius */
    int t = th(s);
    int tooth = sc(2, s);

    /* Inner circle */
    fb_circle(cx, cy, ri, c);
    /* Teeth: 8 short lines radiating outward at 45 degree increments */
    /* top */
    fb_rect(cx - t, cy - ro, t * 2, tooth, c);
    /* bottom */
    fb_rect(cx - t, cy + ro - tooth, t * 2, tooth, c);
    /* left */
    fb_rect(cx - ro, cy - t, tooth, t * 2, c);
    /* right */
    fb_rect(cx + ro - tooth, cy - t, tooth, t * 2, c);
    /* top-left */
    fb_line(cx - ri + 1, cy - ri + 1, cx - ro + 1, cy - ro + 1, c);
    /* top-right */
    fb_line(cx + ri - 1, cy - ri + 1, cx + ro - 1, cy - ro + 1, c);
    /* bottom-left */
    fb_line(cx - ri + 1, cy + ri - 1, cx - ro + 1, cy + ro - 1, c);
    /* bottom-right */
    fb_line(cx + ri - 1, cy + ri - 1, cx + ro - 1, cy + ro - 1, c);
}

/* CHAIN: two interlocking circles */
static void draw_chain(int x, int y, int s, uint32_t c)
{
    int r = sc(4, s);
    int cy = y + s / 2;
    int cx1 = x + s / 2 - sc(2, s);
    int cx2 = x + s / 2 + sc(2, s);

    fb_circle(cx1, cy, r, c);
    fb_circle(cx2, cy, r, c);
}

/* SIGNAL: three bars of increasing height */
static void draw_signal(int x, int y, int s, uint32_t c)
{
    int bw = sc(3, s);
    int gap = sc(1, s);
    int ox = sc(2, s);
    int base = y + sc(14, s);

    /* Short bar */
    int h1 = sc(4, s);
    fb_rect(x + ox, base - h1, bw, h1, c);
    /* Medium bar */
    int h2 = sc(8, s);
    fb_rect(x + ox + bw + gap, base - h2, bw, h2, c);
    /* Tall bar */
    int h3 = sc(12, s);
    fb_rect(x + ox + (bw + gap) * 2, base - h3, bw, h3, c);
}

/* LOCK: rectangle body with half-circle shackle */
static void draw_lock(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    int bw = sc(10, s);
    int bh = sc(7, s);
    int bx = x + sc(3, s);
    int by = y + sc(8, s);

    /* Body */
    fb_rect_outline(bx, by, bw, bh, c, t);
    /* Shackle (arc approximation: three lines) */
    int sr = sc(3, s);
    int scx = x + s / 2;
    int scy = by;
    fb_vline(scx - sr, scy - sr, sr, c);
    fb_hline(scx - sr, scy - sr, sr * 2, c);
    fb_vline(scx + sr, scy - sr, sr, c);
    /* Keyhole dot */
    fb_rect(x + s / 2 - t, by + bh / 3, t * 2, t * 2, c);
}

/* UNLOCK: same as lock but shackle lifted on one side */
static void draw_unlock(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    int bw = sc(10, s);
    int bh = sc(7, s);
    int bx = x + sc(3, s);
    int by = y + sc(8, s);

    /* Body */
    fb_rect_outline(bx, by, bw, bh, c, t);
    /* Shackle open: left side lifted */
    int sr = sc(3, s);
    int scx = x + s / 2;
    int scy = by;
    fb_vline(scx - sr, scy - sr - sc(2, s), sr + sc(2, s), c);
    fb_hline(scx - sr, scy - sr - sc(2, s), sr * 2, c);
    fb_vline(scx + sr, scy - sr, sr, c);
    /* Keyhole dot */
    fb_rect(x + s / 2 - t, by + bh / 3, t * 2, t * 2, c);
}

/* NETWORK: three dots in a triangle, connected by lines */
static void draw_network(int x, int y, int s, uint32_t c)
{
    int cx = x + s / 2;
    int r = th(s) + 1;

    /* Top node */
    int tx = cx, ty = y + sc(2, s);
    /* Bottom-left */
    int lx = x + sc(3, s), ly = y + sc(13, s);
    /* Bottom-right */
    int rx = x + sc(13, s), ry = y + sc(13, s);

    fb_line(tx, ty, lx, ly, c);
    fb_line(tx, ty, rx, ry, c);
    fb_line(lx, ly, rx, ry, c);

    fb_circle_filled(tx, ty, r, c);
    fb_circle_filled(lx, ly, r, c);
    fb_circle_filled(rx, ry, r, c);
}

/* AUDIO: speaker cone with sound waves */
static void draw_audio(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    /* Speaker body */
    int bx = x + sc(2, s);
    int by = y + sc(5, s);
    int bw = sc(3, s);
    int bh = sc(6, s);
    fb_rect(bx, by, bw, bh, c);
    /* Cone (triangle via lines) */
    fb_line(bx + bw, by, bx + bw + sc(3, s), by - sc(2, s), c);
    fb_line(bx + bw, by + bh, bx + bw + sc(3, s), by + bh + sc(2, s), c);
    fb_vline(bx + bw + sc(3, s), by - sc(2, s), bh + sc(4, s), c);
    /* Sound arcs (small) */
    int ax = bx + bw + sc(5, s);
    int acy = y + s / 2;
    fb_line(ax, acy - sc(2, s), ax + t, acy, c);
    fb_line(ax + t, acy, ax, acy + sc(2, s), c);
    /* Outer arc */
    ax += sc(2, s);
    fb_line(ax, acy - sc(3, s), ax + t, acy, c);
    fb_line(ax + t, acy, ax, acy + sc(3, s), c);
}

/* DISPLAY: monitor rectangle on a stand */
static void draw_display(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    int mw = sc(14, s);
    int mh = sc(9, s);
    int mx = x + sc(1, s);
    int my = y + sc(1, s);

    /* Screen */
    fb_rect_outline(mx, my, mw, mh, c, t);
    /* Stand neck */
    int neck_x = x + s / 2;
    fb_vline(neck_x, my + mh, sc(2, s), c);
    /* Stand base */
    fb_hline(neck_x - sc(3, s), my + mh + sc(2, s), sc(6, s), c);
}

/* STORAGE: stacked drive rectangles */
static void draw_storage(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    int w = sc(12, s);
    int h = sc(3, s);
    int ox = sc(2, s);
    int gap = sc(1, s);

    /* Three stacked drives */
    for (int i = 0; i < 3; i++) {
        int dy = y + sc(2, s) + i * (h + gap);
        fb_rect_outline(x + ox, dy, w, h, c, t);
        /* Activity LED dot */
        fb_rect(x + ox + w - sc(2, s), dy + h / 2, t, t, c);
    }
}

/* USB: trident symbol */
static void draw_usb(int x, int y, int s, uint32_t c)
{
    int cx = x + s / 2;
    int t = th(s);

    /* Main vertical stem */
    fb_vline(cx, y + sc(2, s), sc(12, s), c);
    /* Arrow tip at top */
    fb_line(cx, y + sc(2, s), cx - sc(2, s), y + sc(5, s), c);
    fb_line(cx, y + sc(2, s), cx + sc(2, s), y + sc(5, s), c);
    /* Left branch (circle terminal) */
    fb_line(cx, y + sc(7, s), cx - sc(4, s), y + sc(5, s), c);
    fb_circle(cx - sc(4, s), y + sc(5, s), t + 1, c);
    /* Right branch (square terminal) */
    fb_line(cx, y + sc(9, s), cx + sc(4, s), y + sc(7, s), c);
    fb_rect_outline(cx + sc(3, s), y + sc(6, s), sc(2, s), sc(2, s), c, t);
    /* Base dot */
    fb_circle_filled(cx, y + sc(14, s), t + 1, c);
}

/* SEARCH: circle with diagonal line (magnifying glass) */
static void draw_search(int x, int y, int s, uint32_t c)
{
    int r = sc(5, s);
    int cx = x + sc(6, s);
    int cy = y + sc(6, s);

    fb_circle(cx, cy, r, c);
    /* Handle */
    fb_line(cx + r - 1, cy + r - 1,
            cx + r + sc(3, s), cy + r + sc(3, s), c);
    /* Thicken handle */
    if (s >= ICON_MD) {
        fb_line(cx + r, cy + r - 2,
                cx + r + sc(3, s) + 1, cy + r + sc(3, s) - 1, c);
    }
}

/* HOME: triangle roof on rectangle base */
static void draw_home(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    int cx = x + s / 2;

    /* Roof (triangle) */
    int ry = y + sc(2, s);
    int rw = sc(12, s);
    fb_line(cx, ry, cx - rw / 2, ry + sc(5, s), c);
    fb_line(cx, ry, cx + rw / 2, ry + sc(5, s), c);
    fb_hline(cx - rw / 2, ry + sc(5, s), rw, c);

    /* Base (rectangle) */
    int bw = sc(8, s);
    int bh = sc(6, s);
    fb_rect_outline(cx - bw / 2, y + sc(7, s), bw, bh, c, t);
    /* Door */
    int dw = sc(3, s);
    int dh = sc(4, s);
    fb_rect_outline(cx - dw / 2, y + sc(9, s), dw, dh, c, t);
}

/* BACK: left-pointing arrow */
static void draw_back(int x, int y, int s, uint32_t c)
{
    int cy = y + s / 2;
    int ax = x + sc(4, s);
    int aw = sc(8, s);
    int ah = sc(4, s);

    /* Shaft */
    fb_hline(ax, cy, aw, c);
    /* Arrowhead */
    fb_line(ax, cy, ax + ah, cy - ah, c);
    fb_line(ax, cy, ax + ah, cy + ah, c);
}

/* FORWARD: right-pointing arrow */
static void draw_forward(int x, int y, int s, uint32_t c)
{
    int cy = y + s / 2;
    int tip = x + sc(12, s);
    int aw = sc(8, s);
    int ah = sc(4, s);

    /* Shaft */
    fb_hline(tip - aw, cy, aw, c);
    /* Arrowhead */
    fb_line(tip, cy, tip - ah, cy - ah, c);
    fb_line(tip, cy, tip - ah, cy + ah, c);
}

/* REFRESH: circular arrow (270-degree arc with arrowhead) */
static void draw_refresh(int x, int y, int s, uint32_t c)
{
    int cx = x + s / 2;
    int cy = y + s / 2;
    int r = sc(5, s);

    /* Draw the circle */
    fb_circle(cx, cy, r, c);
    /* Break at top to suggest gap, place arrowhead */
    /* Erase a bit at top-right — approximate by overwriting with nothing visible */
    /* Arrow tip pointing clockwise at top */
    int ax = cx + sc(1, s);
    int ay = cy - r;
    fb_line(ax, ay, ax + sc(2, s), ay + sc(2, s), c);
    fb_line(ax, ay, ax - sc(2, s), ay + sc(2, s), c);
}

/* CLOSE: X shape */
static void draw_close(int x, int y, int s, uint32_t c)
{
    int m = sc(4, s);
    int e = s - m;

    fb_line(x + m, y + m, x + e, y + e, c);
    fb_line(x + e, y + m, x + m, y + e, c);
    /* Thicken for larger sizes */
    if (s >= ICON_MD) {
        fb_line(x + m + 1, y + m, x + e + 1, y + e, c);
        fb_line(x + e - 1, y + m, x + m - 1, y + e, c);
    }
}

/* MINIMIZE: horizontal line in the middle-bottom */
static void draw_minimize(int x, int y, int s, uint32_t c)
{
    int m = sc(4, s);
    int w = s - m * 2;
    int t = th(s);

    fb_rect(x + m, y + s - m - t, w, t, c);
}

/* MAXIMIZE: square outline */
static void draw_maximize(int x, int y, int s, uint32_t c)
{
    int m = sc(3, s);
    int t = th(s);

    fb_rect_outline(x + m, y + m, s - m * 2, s - m * 2, c, t);
}

/* BOOKMARK: flag/pennant shape */
static void draw_bookmark(int x, int y, int s, uint32_t c)
{
    int bw = sc(8, s);
    int bh = sc(12, s);
    int ox = x + sc(4, s);
    int oy = y + sc(2, s);

    /* Left edge */
    fb_vline(ox, oy, bh, c);
    /* Right edge */
    fb_vline(ox + bw, oy, bh, c);
    /* Top edge */
    fb_hline(ox, oy, bw, c);
    /* V-notch at bottom */
    fb_line(ox, oy + bh, ox + bw / 2, oy + bh - sc(3, s), c);
    fb_line(ox + bw, oy + bh, ox + bw / 2, oy + bh - sc(3, s), c);
}

/* ALERT: triangle with exclamation mark */
static void draw_alert(int x, int y, int s, uint32_t c)
{
    int cx = x + s / 2;
    int top = y + sc(1, s);
    int bot = y + sc(14, s);
    int half = sc(6, s);

    /* Triangle */
    fb_line(cx, top, cx - half, bot, c);
    fb_line(cx, top, cx + half, bot, c);
    fb_hline(cx - half, bot, half * 2, c);
    /* Exclamation stem */
    fb_vline(cx, top + sc(4, s), sc(4, s), c);
    /* Exclamation dot */
    int t = th(s);
    fb_rect(cx - t / 2, bot - sc(2, s), t + 1, t + 1, c);
}

/* CHECK: checkmark / tick */
static void draw_check(int x, int y, int s, uint32_t c)
{
    int sx = x + sc(3, s);
    int sy = y + sc(8, s);
    int mx = x + sc(6, s);
    int my = y + sc(11, s);
    int ex = x + sc(13, s);
    int ey = y + sc(4, s);

    fb_line(sx, sy, mx, my, c);
    fb_line(mx, my, ex, ey, c);
    /* Thicken */
    if (s >= ICON_MD) {
        fb_line(sx, sy + 1, mx, my + 1, c);
        fb_line(mx, my + 1, ex, ey + 1, c);
    }
}

/* PLUS: + shape */
static void draw_plus(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    int m = sc(3, s);
    int len = s - m * 2;
    int cx = x + s / 2;
    int cy = y + s / 2;

    fb_rect(cx - t, y + m, t * 2, len, c);       /* vertical */
    fb_rect(x + m, cy - t, len, t * 2, c);       /* horizontal */
}

/* MINUS: - shape */
static void draw_minus(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    int m = sc(3, s);
    int len = s - m * 2;
    int cy = y + s / 2;

    fb_rect(x + m, cy - t, len, t * 2, c);
}

/* GEAR: circle with protruding teeth (8 teeth via tick marks) */
static void draw_gear(int x, int y, int s, uint32_t c)
{
    int cx = x + s / 2;
    int cy = y + s / 2;
    int ro = sc(6, s);
    int ri = sc(3, s);
    int t = th(s);

    /* Inner ring */
    fb_circle(cx, cy, ri, c);
    /* Outer ring */
    fb_circle(cx, cy, ro, c);
    /* 4 axis teeth (lines from inner to outer) */
    fb_rect(cx - t, cy - ro, t * 2, ro - ri, c);      /* top */
    fb_rect(cx - t, cy + ri, t * 2, ro - ri, c);      /* bottom */
    fb_rect(cx - ro, cy - t, ro - ri, t * 2, c);      /* left */
    fb_rect(cx + ri, cy - t, ro - ri, t * 2, c);      /* right */
}

/* USER: circle head on top of arc shoulders */
static void draw_user(int x, int y, int s, uint32_t c)
{
    int cx = x + s / 2;
    /* Head */
    int hr = sc(3, s);
    int hy = y + sc(4, s);
    fb_circle(cx, hy, hr, c);
    /* Shoulders (arc from bottom-left to bottom-right) */
    int sw = sc(10, s);
    int sy = y + sc(10, s);
    fb_line(cx - sw / 2, y + sc(14, s), cx - sw / 2 + sc(1, s), sy, c);
    fb_line(cx + sw / 2, y + sc(14, s), cx + sw / 2 - sc(1, s), sy, c);
    fb_hline(cx - sw / 2 + sc(1, s), sy, sw - sc(2, s), c);
    /* Base */
    fb_hline(cx - sw / 2, y + sc(14, s), sw, c);
}

/* PENCIL: diagonal pencil shape */
static void draw_pencil(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    /* Pencil body: diagonal line from top-right to bottom-left */
    int x0 = x + sc(11, s);
    int y0 = y + sc(2, s);
    int x1 = x + sc(3, s);
    int y1 = y + sc(13, s);

    fb_line(x0, y0, x1, y1, c);
    /* Parallel line for body width */
    fb_line(x0 + t, y0 + t, x1 + t, y1 + t, c);
    fb_line(x0 - t, y0 + t, x1 - t, y1 + t, c);
    /* Pencil tip (small triangle at bottom-left) */
    fb_line(x1 - t, y1 + t, x1, y1 + sc(2, s), c);
    fb_line(x1 + t, y1 + t, x1, y1 + sc(2, s), c);
    /* Eraser line at top */
    fb_line(x0 - sc(1, s), y0 + sc(1, s),
            x0 + sc(1, s) + t, y0 + sc(1, s), c);
}

/* TRASH: can with lid and vertical lines */
static void draw_trash(int x, int y, int s, uint32_t c)
{
    int t = th(s);
    int bw = sc(10, s);
    int bh = sc(9, s);
    int bx = x + sc(3, s);
    int by = y + sc(5, s);

    /* Lid */
    fb_hline(bx - sc(1, s), by - sc(1, s), bw + sc(2, s), c);
    /* Handle on lid */
    fb_hline(x + s / 2 - sc(2, s), by - sc(2, s), sc(4, s), c);
    /* Body */
    fb_rect_outline(bx, by, bw, bh, c, t);
    /* Vertical lines inside (slots) */
    int slots = 3;
    int sw = bw / (slots + 1);
    for (int i = 1; i <= slots; i++) {
        fb_vline(bx + sw * i, by + sc(2, s), bh - sc(3, s), c);
    }
}

/* ── Dispatch table ── */

typedef void (*icon_fn_t)(int x, int y, int s, uint32_t c);

static const icon_fn_t icon_table[ICON_COUNT] = {
    [ICON_FILE]      = draw_file,
    [ICON_FOLDER]    = draw_folder,
    [ICON_TERMINAL]  = draw_terminal,
    [ICON_BROWSER]   = draw_browser,
    [ICON_SETTINGS]  = draw_settings,
    [ICON_CHAIN]     = draw_chain,
    [ICON_SIGNAL]    = draw_signal,
    [ICON_LOCK]      = draw_lock,
    [ICON_UNLOCK]    = draw_unlock,
    [ICON_NETWORK]   = draw_network,
    [ICON_AUDIO]     = draw_audio,
    [ICON_DISPLAY]   = draw_display,
    [ICON_STORAGE]   = draw_storage,
    [ICON_USB]       = draw_usb,
    [ICON_SEARCH]    = draw_search,
    [ICON_HOME]      = draw_home,
    [ICON_BACK]      = draw_back,
    [ICON_FORWARD]   = draw_forward,
    [ICON_REFRESH]   = draw_refresh,
    [ICON_CLOSE]     = draw_close,
    [ICON_MINIMIZE]  = draw_minimize,
    [ICON_MAXIMIZE]  = draw_maximize,
    [ICON_BOOKMARK]  = draw_bookmark,
    [ICON_ALERT]     = draw_alert,
    [ICON_CHECK]     = draw_check,
    [ICON_PLUS]      = draw_plus,
    [ICON_MINUS]     = draw_minus,
    [ICON_GEAR]      = draw_gear,
    [ICON_USER]      = draw_user,
    [ICON_PENCIL]    = draw_pencil,
    [ICON_TRASH]     = draw_trash,
};

/* ── Public API ── */

void icon_draw(icon_id_t icon, int x, int y, int size, uint32_t color)
{
    if (icon < 0 || icon >= ICON_COUNT)
        return;
    if (!icon_table[icon])
        return;
    icon_table[icon](x, y, size, color);
}

void icon_draw_accent(icon_id_t icon, int x, int y, int size)
{
    icon_draw(icon, x, y, size, theme_accent());
}
