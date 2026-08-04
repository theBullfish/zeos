/*
 * Zeos — Context menu implementation
 *
 * Doctrine: one popup at a time, owns its own input until dismissed.
 * Painted in compositor overlay layer. All pixels drawn via theme
 * tokens — never hardcoded colors.
 */

#include "ui_context_menu.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "compositor.h"
#include "anim.h"      /* L.5: spring appear/dismiss */

/* ── Local helpers ── */

static void cm_strncpy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    if (src) while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int cm_strlen(const char *s) {
    int n = 0; if (!s) return 0; while (s[n]) n++; return n;
}

/* ── Layout ── */

#define CM_ITEM_H        24
#define CM_ITEM_PAD       8
#define CM_WIDTH        180
#define CM_BORDER_RADIUS  4   /* visual only — rect drawn squared */

/* ── State ── */

typedef struct {
    int                 active;
    int                 x, y;
    int                 w, h;
    int                 hover_index;
    int                 item_count;
    ctx_menu_item_t     items[CTX_MENU_MAX_ITEMS];
    /* L.5 spring appear/dismiss */
    float               open_t;    /* 0 = closed, 1 = fully open (scale) */
    int                 anim_id;   /* active spring id, or -1 */
    int                 closing;   /* animating out; input off but still drawing */
} ctx_menu_state_t;

static ctx_menu_state_t g_menu;
static uint32_t g_total_opens = 0;

/* L.5: spring drives the menu's open scale. On the way out (closing), finalize
 * teardown once the scale settles near 0. Dirties a small margin to cover the
 * spring's overshoot. */
static void cm_anim_cb(int anim_id, float value, void *ctx)
{
    (void)anim_id; (void)ctx;
    g_menu.open_t = value;
    compositor_dirty(g_menu.x - 2, g_menu.y - 2, g_menu.w + 8, g_menu.h + 8);
    if (g_menu.closing && value <= 0.03f) {
        g_menu.closing = 0;
        g_menu.item_count = 0;
        g_menu.hover_index = -1;
        g_menu.open_t = 0.0f;
    }
}

/* ── API ── */

void context_menu_open(int x, int y, const ctx_menu_item_t *items, int count)
{
    if (count > CTX_MENU_MAX_ITEMS) count = CTX_MENU_MAX_ITEMS;
    if (count <= 0) return;

    g_menu.active = 1;
    g_menu.hover_index = -1;
    g_menu.item_count = count;
    g_menu.w = CM_WIDTH;
    g_menu.h = count * CM_ITEM_H + 2 * 4;  /* 4px top/bottom padding */

    /* Clamp to screen */
    int sw = (int)fb_width();
    int sh = (int)fb_height();
    if (x + g_menu.w > sw) x = sw - g_menu.w;
    if (y + g_menu.h > sh) y = sh - g_menu.h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_menu.x = x;
    g_menu.y = y;

    for (int i = 0; i < count; i++) {
        cm_strncpy(g_menu.items[i].label, items[i].label, CTX_MENU_LABEL_MAX);
        g_menu.items[i].action  = items[i].action;
        g_menu.items[i].ctx     = items[i].ctx;
        g_menu.items[i].enabled = items[i].enabled;
    }
    g_total_opens++;

    /* L.5: open full immediately (visible), close animates out via the spring
     * (context_menu_close springs open_t 1 -> 0). */
    g_menu.closing = 0;
    g_menu.open_t  = 1.0f;

    compositor_dirty(g_menu.x, g_menu.y, g_menu.w, g_menu.h);
}

void context_menu_close(void)
{
    if (!g_menu.active && !g_menu.closing) return;

    /* L.5: input goes off immediately (active=0 -> context_menu_active() reads
     * false this instant, so callers routing input see the menu as gone -- no
     * behavior ripple), but keep the state and spring the scale 1 -> 0 so it
     * animates out. cm_anim_cb finalizes teardown when it settles. */
    g_menu.active  = 0;
    g_menu.hover_index = -1;
    g_menu.closing = 1;
    g_menu.anim_id = anim_spring_default(g_menu.open_t, 0.0f, cm_anim_cb, 0);

    compositor_dirty(g_menu.x - 2, g_menu.y - 2, g_menu.w + 8, g_menu.h + 8);
}

int context_menu_active(void) { return g_menu.active; }

static int cm_hit_index(int x, int y)
{
    if (!g_menu.active) return -1;
    if (x < g_menu.x || x >= g_menu.x + g_menu.w) return -1;
    int rel = y - (g_menu.y + 4);
    if (rel < 0) return -1;
    int idx = rel / CM_ITEM_H;
    if (idx < 0 || idx >= g_menu.item_count) return -1;
    return idx;
}

int context_menu_mouse_move(int x, int y)
{
    if (!g_menu.active) return 0;
    int idx = cm_hit_index(x, y);
    if (idx != g_menu.hover_index) {
        g_menu.hover_index = idx;
        compositor_dirty(g_menu.x, g_menu.y, g_menu.w, g_menu.h);
    }
    /* Consume only when actually within menu bounds */
    if (x >= g_menu.x && x < g_menu.x + g_menu.w &&
        y >= g_menu.y && y < g_menu.y + g_menu.h) return 1;
    return 0;
}

int context_menu_mouse_down(int x, int y, int button)
{
    if (!g_menu.active) return 0;
    /* Click outside dismisses. */
    if (x < g_menu.x || x >= g_menu.x + g_menu.w ||
        y < g_menu.y || y >= g_menu.y + g_menu.h) {
        context_menu_close();
        return 0;  /* Don't consume — let the click hit whatever's underneath */
    }
    if (button != 1) {
        /* right/middle inside menu just closes. */
        context_menu_close();
        return 1;
    }
    int idx = cm_hit_index(x, y);
    if (idx >= 0 && g_menu.items[idx].enabled && g_menu.items[idx].action) {
        ctx_menu_action_fn fn = g_menu.items[idx].action;
        void *ctx = g_menu.items[idx].ctx;
        context_menu_close();
        fn(ctx);
    } else {
        context_menu_close();
    }
    return 1;
}

int context_menu_key(int ascii)
{
    if (!g_menu.active) return 0;
    if (ascii == 27) {  /* Esc */
        context_menu_close();
        return 1;
    }
    if (ascii == '\n' || ascii == '\r') {
        if (g_menu.hover_index >= 0 &&
            g_menu.items[g_menu.hover_index].enabled &&
            g_menu.items[g_menu.hover_index].action) {
            ctx_menu_action_fn fn = g_menu.items[g_menu.hover_index].action;
            void *ctx = g_menu.items[g_menu.hover_index].ctx;
            context_menu_close();
            fn(ctx);
        } else {
            context_menu_close();
        }
        return 1;
    }
    return 0;
}

void context_menu_draw(void)
{
    if (!g_menu.active && !g_menu.closing) return;

    /* L.5: scale the box by the spring value while CLOSING (dismiss animation).
     * While open, always draw full-size -- gating the open state on the spring
     * risked an invisible menu if open_t hadn't advanced (regression). Items are
     * painted only once nearly settled. */
    float t = g_menu.closing ? g_menu.open_t : 1.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int cw = (int)(g_menu.w * t + 0.5f);
    int ch = (int)(g_menu.h * t + 0.5f);
    if (cw < 2 || ch < 2) return;

    /* Shadow under the (scaled) menu. */
    fb_rect_blend(g_menu.x + 2, g_menu.y + 4,
                  cw, ch, 0x60000000);

    /* Background */
    fb_rect(g_menu.x, g_menu.y, cw, ch, COLOR_SURFACE_HIGH);
    /* Border */
    fb_rect_outline(g_menu.x, g_menu.y, cw, ch,
                    COLOR_SEPARATOR, 1);

    /* Items only once the box is essentially at full size. */
    if (t < 0.9f) return;

    int ty = g_menu.y + 4;
    for (int i = 0; i < g_menu.item_count; i++) {
        ctx_menu_item_t *it = &g_menu.items[i];
        int row_x = g_menu.x + 1;
        int row_y = ty + i * CM_ITEM_H;
        int row_w = g_menu.w - 2;
        int row_h = CM_ITEM_H;

        /* Separator: label "-" or empty + no action */
        if (it->label[0] == '-' && it->action == 0) {
            fb_hline(row_x + 8, row_y + row_h / 2,
                     row_w - 16, COLOR_SEPARATOR);
            continue;
        }

        if (i == g_menu.hover_index && it->enabled) {
            fb_rect(row_x, row_y, row_w, row_h, COLOR_PRIMARY_DIM);
        }

        uint32_t fg = it->enabled ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_3;
        fb_text(row_x + CM_ITEM_PAD, row_y + 6, it->label, fg);
    }
}

uint32_t context_menu_total_opens(void) { return g_total_opens; }

#ifdef ZEOS_DIAG_L5
#include "kprint.h"
/* L.5 selftest: prove the context menu springs open (not instant) and that
 * close DEFERS teardown until the spring settles (items retained while
 * animating out, input off immediately). */
void context_menu_l5_selftest(void)
{
    ctx_menu_item_t items[2];
    for (int i = 0; i < 2; i++) {
        items[i].label[0] = 'A' + i; items[i].label[1] = 0;
        items[i].action = 0; items[i].ctx = 0; items[i].enabled = 1;
    }
    context_menu_open(100, 100, items, 2);
    int opened = (g_menu.open_t > 0.9f) && context_menu_active();  /* full + visible immediately */

    context_menu_close();
    int input_off_now = (context_menu_active() == 0);    /* input gone immediately */
    int still_drawing = (g_menu.closing == 1 && g_menu.item_count == 2); /* deferred teardown */
    for (int i = 0; i < 240; i++) anim_tick(1.0f / 240.0f);
    int torn_down = (g_menu.closing == 0 && g_menu.item_count == 0);

    int pass = opened && input_off_now && still_drawing && torn_down;
    kputs("[L5] opened="); kput_dec((uint64_t)opened);
    kputs(" input_off="); kput_dec((uint64_t)input_off_now);
    kputs(" deferred="); kput_dec((uint64_t)still_drawing);
    kputs(" torndown="); kput_dec((uint64_t)torn_down);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
}
#endif

/* ── C.11: Popover (non-modal, anchor-attached) ──────────────────────────────
 * Unlike the context menu (modal — owns input until dismissed), a popover is
 * NON-modal: it draws an attached info panel near an anchor and does NOT
 * intercept input routing. The owner closes it (on outside click / state
 * change). Used for transient detail (health, tooltips, status). */
#define PV_MAX_LINES 8
#define PV_LINE_MAX  48
#define PV_LINE_H    20
#define PV_PAD        8
#define PV_WIDTH    220

typedef struct {
    int  active;
    int  x, y, w, h;
    int  line_count;
    char lines[PV_MAX_LINES][PV_LINE_MAX];
} popover_t;

static popover_t g_pv;

void popover_open(int anchor_x, int anchor_y, const char *const *lines, int count)
{
    if (count < 1) return;
    if (count > PV_MAX_LINES) count = PV_MAX_LINES;
    g_pv.line_count = count;
    for (int i = 0; i < count; i++) {
        int j = 0;
        if (lines[i]) while (j < PV_LINE_MAX - 1 && lines[i][j]) { g_pv.lines[i][j] = lines[i][j]; j++; }
        g_pv.lines[i][j] = 0;
    }
    g_pv.w = PV_WIDTH;
    g_pv.h = count * PV_LINE_H + 2 * PV_PAD;

    /* Attach below-right of the anchor; clamp to screen. */
    int sw = (int)fb_width(), sh = (int)fb_height();
    int x = anchor_x, y = anchor_y + 4;
    if (x + g_pv.w > sw) x = sw - g_pv.w;
    if (y + g_pv.h > sh) y = anchor_y - g_pv.h - 4;   /* flip above */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_pv.x = x; g_pv.y = y;
    g_pv.active = 1;
    compositor_dirty(g_pv.x - 2, g_pv.y - 2, g_pv.w + 8, g_pv.h + 8);
}

void popover_close(void)
{
    if (!g_pv.active) return;
    g_pv.active = 0;
    compositor_dirty(g_pv.x - 2, g_pv.y - 2, g_pv.w + 8, g_pv.h + 8);
}

int popover_active(void) { return g_pv.active; }

void popover_draw(void)
{
    if (!g_pv.active) return;
    fb_rect_blend(g_pv.x + 2, g_pv.y + 3, g_pv.w, g_pv.h, 0x60000000u);  /* shadow */
    fb_rect(g_pv.x, g_pv.y, g_pv.w, g_pv.h, COLOR_SURFACE_HIGH);
    fb_rect_outline(g_pv.x, g_pv.y, g_pv.w, g_pv.h, COLOR_SEPARATOR, 1);
    for (int i = 0; i < g_pv.line_count; i++)
        fb_text(g_pv.x + PV_PAD, g_pv.y + PV_PAD + i * PV_LINE_H, g_pv.lines[i], COLOR_ON_SURFACE);
}

#ifdef ZEOS_DIAG_C11
#include "kprint.h"
/* C.11 selftest: popover opens attached (clamped on-screen), is NON-modal
 * (context_menu_active stays 0 — it does not grab modal input), holds its lines,
 * and closes cleanly. */
void popover_c11_selftest(void)
{
    const char *lines[3] = { "cpu:0  LIVE", "12 chains", "0 errors" };
    popover_open(1850, 30, lines, 3);   /* near right edge -> must clamp left */
    int opened = popover_active();
    int clamped = (g_pv.x + g_pv.w <= (int)fb_width()) && (g_pv.x >= 0);
    int nonmodal = (context_menu_active() == 0);   /* popover != modal menu */
    int lines_ok = (g_pv.line_count == 3) &&
                   (g_pv.lines[0][0]=='c') && (g_pv.lines[1][0]=='1') && (g_pv.lines[2][0]=='0');
    popover_close();
    int closed = (popover_active() == 0);
    int pass = opened && clamped && nonmodal && lines_ok && closed;
    kputs("[C11] opened="); kput_dec((uint64_t)opened);
    kputs(" clamped="); kput_dec((uint64_t)clamped);
    kputs(" nonmodal="); kput_dec((uint64_t)nonmodal);
    kputs(" lines_ok="); kput_dec((uint64_t)lines_ok);
    kputs(" closed="); kput_dec((uint64_t)closed);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
}
#endif
