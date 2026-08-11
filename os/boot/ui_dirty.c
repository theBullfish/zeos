/*
 * Zeos — Dirty / Save-Changes modal
 *
 * Doctrine: the modal is a single-purpose construct. It owns input
 * exclusively while open, fires the callback exactly once, then
 * disappears. Callers never see "modal already open" — opening a new
 * one cancels the previous and fires CANCEL on it.
 */

#include "ui_dirty.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "compositor.h"
#include "timeofday.h"

static void dd_strncpy(char *dst, const char *src, int max) {
    int i = 0; if (!dst || max <= 0) return;
    if (src) while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ── Dirty registry ── */

typedef struct {
    int      used;
    int      window_id;
    int      dirty;
    uint64_t modified_at;
} dirty_slot_t;

static dirty_slot_t g_dirty[UI_DIRTY_MAX];

static dirty_slot_t *dd_find(int wid) {
    for (int i = 0; i < UI_DIRTY_MAX; i++)
        if (g_dirty[i].used && g_dirty[i].window_id == wid)
            return &g_dirty[i];
    return 0;
}

static dirty_slot_t *dd_alloc(int wid) {
    dirty_slot_t *s = dd_find(wid);
    if (s) return s;
    for (int i = 0; i < UI_DIRTY_MAX; i++) {
        if (!g_dirty[i].used) {
            g_dirty[i].used = 1;
            g_dirty[i].window_id = wid;
            g_dirty[i].dirty = 0;
            g_dirty[i].modified_at = 0;
            return &g_dirty[i];
        }
    }
    return 0;
}

void dirty_register(int wid)
{
    dirty_slot_t *s = dd_alloc(wid);
    if (!s) return;
    s->dirty = 1;
    s->modified_at = tod_now_unix();
}

void dirty_clear(int wid)
{
    dirty_slot_t *s = dd_find(wid);
    if (!s) return;
    s->dirty = 0;
}

void dirty_touch(int wid)
{
    dirty_slot_t *s = dd_alloc(wid);
    if (!s) return;
    s->modified_at = tod_now_unix();
}

int dirty_get(int wid)
{
    dirty_slot_t *s = dd_find(wid);
    return s ? s->dirty : 0;
}

uint64_t dirty_modified_at(int wid)
{
    dirty_slot_t *s = dd_find(wid);
    return s ? s->modified_at : 0;
}

int dirty_count(void)
{
    int n = 0;
    for (int i = 0; i < UI_DIRTY_MAX; i++)
        if (g_dirty[i].used && g_dirty[i].dirty) n++;
    return n;
}

/* ── Modal ── */

#define DM_W   400
#define DM_H   170
#define DM_BTN_W 96
#define DM_BTN_H 32

typedef struct {
    int               active;
    int               x, y;
    int               window_id;
    char              title[64];
    dirty_modal_cb_fn cb;
    void             *ctx;
    int               hover_btn;   /* 0=Save 1=Discard 2=Cancel, -1=none */
} dirty_modal_t;

static dirty_modal_t g_modal;

static void dm_layout(int *bx, int *by) {
    int sw = (int)fb_width();
    int sh = (int)fb_height();
    g_modal.x = (sw - DM_W) / 2;
    g_modal.y = (sh - DM_H) / 2;
    *bx = g_modal.x + (DM_W - (3 * DM_BTN_W + 2 * 12)) / 2;
    *by = g_modal.y + DM_H - DM_BTN_H - 16;
}

static void dm_btn_rect(int idx, int *out_x, int *out_y) {
    int bx, by;
    dm_layout(&bx, &by);
    *out_x = bx + idx * (DM_BTN_W + 12);
    *out_y = by;
}

static int dm_btn_hit(int x, int y) {
    for (int i = 0; i < 3; i++) {
        int bx, by;
        dm_btn_rect(i, &bx, &by);
        if (x >= bx && x < bx + DM_BTN_W &&
            y >= by && y < by + DM_BTN_H) return i;
    }
    return -1;
}

static void dm_close_with(dirty_response_t r) {
    if (!g_modal.active) return;
    int wid = g_modal.window_id;
    dirty_modal_cb_fn cb = g_modal.cb;
    void *ctx = g_modal.ctx;
    g_modal.active = 0;
    compositor_dirty(g_modal.x, g_modal.y, DM_W, DM_H);
    if (cb) cb(wid, r, ctx);
}

void dirty_open_close_modal(int window_id, const char *title,
                            dirty_modal_cb_fn cb, void *ctx)
{
    if (g_modal.active) {
        /* Cancel the in-flight one first. */
        dm_close_with(DIRTY_CANCEL);
    }
    g_modal.active = 1;
    g_modal.window_id = window_id;
    dd_strncpy(g_modal.title, title ? title : "Unsaved changes", 64);
    g_modal.cb = cb;
    g_modal.ctx = ctx;
    g_modal.hover_btn = -1;
    int bx, by;
    dm_layout(&bx, &by);
    compositor_dirty(g_modal.x, g_modal.y, DM_W, DM_H);
}

int dirty_modal_active(void) { return g_modal.active; }

int dirty_modal_mouse_move(int x, int y)
{
    if (!g_modal.active) return 0;
    int idx = dm_btn_hit(x, y);
    if (idx != g_modal.hover_btn) {
        g_modal.hover_btn = idx;
        compositor_dirty(g_modal.x, g_modal.y, DM_W, DM_H);
    }
    return 1;  /* always consume while modal is up */
}

int dirty_modal_mouse_down(int x, int y, int button)
{
    if (!g_modal.active) return 0;
    (void)button;
    int idx = dm_btn_hit(x, y);
    if (idx == 0)      dm_close_with(DIRTY_SAVE);
    else if (idx == 1) dm_close_with(DIRTY_DISCARD);
    else if (idx == 2) dm_close_with(DIRTY_CANCEL);
    return 1;
}

int dirty_modal_key(int ascii)
{
    if (!g_modal.active) return 0;
    if (ascii == 27) {                  /* Esc -> Cancel */
        dm_close_with(DIRTY_CANCEL);
        return 1;
    }
    if (ascii == '\n' || ascii == '\r') { /* Enter -> Save */
        dm_close_with(DIRTY_SAVE);
        return 1;
    }
    if (ascii == 's' || ascii == 'S') { dm_close_with(DIRTY_SAVE);    return 1; }
    if (ascii == 'd' || ascii == 'D') { dm_close_with(DIRTY_DISCARD); return 1; }
    return 1;  /* swallow everything while modal is up */
}

void dirty_modal_draw(void)
{
    if (!g_modal.active) return;

    /* Dim the rest of the screen. */
    fb_rect_blend(0, 0, (int)fb_width(), (int)fb_height(), 0x80000000);

    /* Card */
    fb_rect(g_modal.x, g_modal.y, DM_W, DM_H, COLOR_SURFACE_HIGH);
    fb_rect_outline(g_modal.x, g_modal.y, DM_W, DM_H, COLOR_SEPARATOR, 1);

    /* Title */
    fb_text(g_modal.x + 20, g_modal.y + 18, g_modal.title, COLOR_ON_SURFACE);
    fb_text(g_modal.x + 20, g_modal.y + 44,
            "This document has unsaved changes.",
            COLOR_ON_SURFACE_2);
    fb_text(g_modal.x + 20, g_modal.y + 64,
            "Save before closing?",
            COLOR_ON_SURFACE_2);

    static const char *labels[3] = { "Save", "Discard", "Cancel" };
    for (int i = 0; i < 3; i++) {
        int bx, by;
        dm_btn_rect(i, &bx, &by);
        uint32_t bg = (i == 0) ? COLOR_PRIMARY : COLOR_SURFACE_TOP;
        if (i == g_modal.hover_btn) bg = COLOR_PRIMARY_DIM;
        fb_rect(bx, by, DM_BTN_W, DM_BTN_H, bg);
        fb_rect_outline(bx, by, DM_BTN_W, DM_BTN_H, COLOR_SEPARATOR, 1);
        /* Center text crudely (assume ~7px/char for default font). */
        int label_len = 0; while (labels[i][label_len]) label_len++;
        int tx = bx + (DM_BTN_W - label_len * 8) / 2;
        if (tx < bx + 6) tx = bx + 6;
        fb_text(tx, by + 9, labels[i], COLOR_ON_SURFACE);
    }
}
