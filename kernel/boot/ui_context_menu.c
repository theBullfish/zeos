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

#include "kprint.h"
/* L.5 selftest: prove the context menu opens full+visible immediately, and that
 * CLOSE springs out with DEFERRED teardown until the spring settles (items
 * retained while animating out, input off immediately). Un-gated so the production `selftest`
 * shell can run it; opens+closes a menu then tears it down (synchronous, no
 * frame rendered, so invisible). Returns 1 on PASS. */
int context_menu_l5_selftest(void)
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
    return pass;
}

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

/* C.11 selftest: popover opens attached (clamped on-screen), is NON-modal
 * (context_menu_active stays 0 — it does not grab modal input), holds its lines,
 * and closes cleanly.
 * Un-gated so the production `selftest` shell can run it; PRODUCTION-SAFE: the
 * popover is a g_pv global (NOT a wm surface), opened and closed synchronously
 * inside one cmd_selftest call (never composited while active -> no on-screen
 * flash), and popover_close leaves g_pv.active=0. Returns 1 on PASS. */
int popover_c11_selftest(void)
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
    return pass;
}

/* ── C.10: Sheet (modal, slides down from a window titlebar) ─────────────────
 * A sheet is attached to a parent surface and is MODAL to that surface only
 * (its window's content is blocked while the sheet is up; the rest of the
 * desktop stays live). It springs down from just under the parent titlebar. */
#include "anim.h"
#include "wm.h"

typedef struct {
    int   active;
    int   parent_id;
    int   x, y, w, h;
    float slide;      /* 0 = tucked at titlebar, 1 = fully down */
    int   anim_id;
    char  title[48];
} sheet_t;

static sheet_t g_sheet;

static void sheet_slide_cb(int id, float v, void *ctx) { (void)id; (void)ctx; g_sheet.slide = v;
    compositor_dirty(g_sheet.x - 2, g_sheet.y - 2, g_sheet.w + 8, g_sheet.h + 8); }

int sheet_open(int parent_id, const char *title, int height)
{
    chain_surface_t *p = wm_get_surface(parent_id);
    if (!p) return 0;
    g_sheet.parent_id = parent_id;
    g_sheet.w = p->w - 40; if (g_sheet.w < 120) g_sheet.w = p->w;
    g_sheet.x = p->x + (p->w - g_sheet.w) / 2;
    g_sheet.y = p->y + 40;                 /* just below the 40px titlebar */
    g_sheet.h = height > 0 ? height : 140;
    int j = 0; if (title) while (j < 47 && title[j]) { g_sheet.title[j] = title[j]; j++; } g_sheet.title[j] = 0;
    g_sheet.slide = 0.0f;
    g_sheet.active = 1;
    g_sheet.anim_id = anim_spring(0.0f, 1.0f, SPRING_SMOOTH_S, SPRING_SMOOTH_D, sheet_slide_cb, 0);
    return 1;
}

void sheet_close(void)
{
    if (!g_sheet.active) return;
    g_sheet.anim_id = anim_spring(g_sheet.slide, 0.0f, SPRING_SMOOTH_S, SPRING_SMOOTH_D, sheet_slide_cb, 0);
    g_sheet.active = 0;   /* input un-blocks immediately; slide animates out */
}

int  sheet_active(void) { return g_sheet.active; }
/* C.10: is a sheet modal over this surface? (parent's content should ignore input) */
int  sheet_modal_for(int surface_id) { return g_sheet.active && g_sheet.parent_id == surface_id; }

void sheet_draw(void)
{
    if (!g_sheet.active && g_sheet.slide <= 0.01f) return;
    int drawn_h = (int)(g_sheet.h * g_sheet.slide + 0.5f);
    if (drawn_h < 2) return;
    /* dim the parent window behind the sheet (modal scrim over the parent only) */
    chain_surface_t *p = wm_get_surface(g_sheet.parent_id);
    if (p) fb_rect_blend(p->x, p->y + 40, p->w, p->h - 40, 0x50000000u);
    fb_rect(g_sheet.x, g_sheet.y, g_sheet.w, drawn_h, COLOR_SURFACE_TOP);
    fb_rect_outline(g_sheet.x, g_sheet.y, g_sheet.w, drawn_h, COLOR_SEPARATOR, 1);
    if (drawn_h > 24)
        fb_text(g_sheet.x + 12, g_sheet.y + 8, g_sheet.title, COLOR_ON_SURFACE);
}

/* C.10 selftest: sheet attaches to a parent surface, springs down (gradual),
 * is modal to that parent only, and closes.
 * Un-gated so the production `selftest` shell can run it; PRODUCTION-SAFE: the 2
 * probe surfaces have persist_name=0 (no VAULT write on detach) and are created,
 * asserted, and torn down (detach + tick to settle so surface_scale_cb removes
 * them) synchronously inside one cmd_selftest call; after sheet_close the slide
 * spring is ticked to full settle (slide->0) so sheet_draw early-returns — no
 * ghost sheet or ghost surface left on the live desktop. Returns 1 on PASS. */
int sheet_c10_selftest(void)
{
    extern int  wm_create_surface(const char*, int, int, int, int, int, void(*)(int,int,int,int,int));
    extern void wm_force_visible(int);
    extern void anim_tick(float);
    extern void wm_detach_surface(int);
    extern chain_surface_t *wm_get_surface(int);
    int p = wm_create_surface("C10parent", -1, 300, 200, 500, 400, 0);
    wm_force_visible(p);
    int other = wm_create_surface("C10other", -1, 50, 50, 200, 150, 0);
    wm_force_visible(other);

    int opened = sheet_open(p, "Save changes?", 160);
    int modal_parent = sheet_modal_for(p);
    int not_modal_other = !sheet_modal_for(other);
    for (int i = 0; i < 4; i++) anim_tick(1.0f/240.0f);
    int sliding = (g_sheet.slide > 0.0f && g_sheet.slide < 1.0f);
    for (int i = 0; i < 600; i++) anim_tick(1.0f/240.0f);
    int down = (g_sheet.slide > 0.9f);
    sheet_close();
    int closed = (sheet_active() == 0);

    /* teardown: tick the sheet slide-out to settle (slide->0 so sheet_draw
     * early-returns) AND detach both probe surfaces so surface_scale_cb removes
     * them — no ghost sheet/surface left on the live desktop. */
    wm_detach_surface(p); wm_detach_surface(other);
    for (int i = 0; i < 600; i++) anim_tick(1.0f/240.0f);
    int sheet_tucked = (g_sheet.slide <= 0.01f);
    chain_surface_t *pp = wm_get_surface(p), *po = wm_get_surface(other);
    int torndown = pp && po &&
                   pp->visible == 0 && pp->signal == SIGNAL_DETACHED &&
                   po->visible == 0 && po->signal == SIGNAL_DETACHED &&
                   sheet_tucked;

    int pass = opened && modal_parent && not_modal_other && sliding && down && closed && torndown;
    kputs("[C10] opened="); kput_dec((uint64_t)opened);
    kputs(" modal_parent="); kput_dec((uint64_t)modal_parent);
    kputs(" not_modal_other="); kput_dec((uint64_t)not_modal_other);
    kputs(" sliding="); kput_dec((uint64_t)sliding);
    kputs(" down="); kput_dec((uint64_t)down);
    kputs(" closed="); kput_dec((uint64_t)closed);
    kputs(" torndown="); kput_dec((uint64_t)torndown);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
    return pass;
}
