/*
 * Zeos — List state rendering
 *
 * Doctrine: spinner is TSC-driven so it animates without owning a
 * frame loop. Empty state is a soft message centered in the rect.
 * Error state shows a red dot + reason + optional Retry button.
 */

#include "ui_states.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "timer.h"

static uint32_t g_total_renders = 0;

/* Read TSC; cheap and monotonic. */
static inline uint64_t ls_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int ls_strlen(const char *s) {
    int n = 0; if (!s) return 0; while (s[n]) n++; return n;
}

#define RETRY_W   88
#define RETRY_H   28

static void retry_btn_rect(const list_state_ctx_t *c, int *bx, int *by) {
    *bx = c->x + (c->w - RETRY_W) / 2;
    *by = c->y + c->h / 2 + 28;
}

static void draw_spinner(int cx, int cy, uint32_t color)
{
    /* 8-spoke spinner, one bright spoke that rotates. Spoke index
     * derived from TSC mod 1Hz → 8 phases. */
    uint64_t freq = timer_tsc_freq();
    uint64_t now = ls_rdtsc();
    int phase = 0;
    if (freq > 0) {
        /* one revolution every 800ms — 8 phases at 100ms each. */
        uint64_t step = freq / 10;
        if (step == 0) step = 1;
        phase = (int)((now / step) & 7);
    }

    const int r_inner = 6;
    const int r_outer = 12;
    /* 8 cardinal/intercardinal angles, dx/dy table. */
    const int dx[8] = { 0,  1,  1,  1,  0, -1, -1, -1 };
    const int dy[8] = {-1, -1,  0,  1,  1,  1,  0, -1 };
    for (int i = 0; i < 8; i++) {
        int dim = ((i - phase) & 7);  /* 0 = brightest */
        uint32_t col;
        if (dim == 0) col = color;
        else if (dim == 1) col = COLOR_ON_SURFACE_2;
        else if (dim == 2) col = COLOR_ON_SURFACE_3;
        else               col = COLOR_ON_SURFACE_4;
        int x0 = cx + dx[i] * r_inner;
        int y0 = cy + dy[i] * r_inner;
        int x1 = cx + dx[i] * r_outer;
        int y1 = cy + dy[i] * r_outer;
        fb_line(x0, y0, x1, y1, col);
    }
}

int list_render_state(const list_state_ctx_t *c)
{
    if (!c) return 0;
    g_total_renders++;
    if (c->state == LIST_OK) return 0;

    int cx = c->x + c->w / 2;
    int cy = c->y + c->h / 2;

    if (c->state == LIST_LOADING) {
        draw_spinner(cx, cy - 8, COLOR_PRIMARY);
        const char *msg = c->message ? c->message : "Loading…";
        int tw = ls_strlen(msg) * 8;
        fb_text(cx - tw / 2, cy + 18, msg, COLOR_ON_SURFACE_2);
        return 1;
    }

    if (c->state == LIST_EMPTY) {
        /* Empty circle icon */
        fb_circle(cx, cy - 12, 14, COLOR_ON_SURFACE_3);
        const char *msg = c->message ? c->message : "Nothing here yet.";
        int tw = ls_strlen(msg) * 8;
        fb_text(cx - tw / 2, cy + 12, msg, COLOR_ON_SURFACE_2);
        if (c->cta) {
            int tw2 = ls_strlen(c->cta) * 8;
            fb_text(cx - tw2 / 2, cy + 32, c->cta, COLOR_PRIMARY);
        }
        return 1;
    }

    if (c->state == LIST_ERROR) {
        /* Red dot + message */
        fb_circle_filled(cx, cy - 14, 8, COLOR_DANGER);
        const char *msg = c->message ? c->message : "Could not load.";
        int tw = ls_strlen(msg) * 8;
        fb_text(cx - tw / 2, cy + 4, msg, COLOR_ON_SURFACE_2);
        if (c->retry_cb) {
            int bx, by;
            retry_btn_rect(c, &bx, &by);
            fb_rect(bx, by, RETRY_W, RETRY_H, COLOR_SURFACE_TOP);
            fb_rect_outline(bx, by, RETRY_W, RETRY_H, COLOR_PRIMARY, 1);
            const char *lbl = "Retry";
            int lw = ls_strlen(lbl) * 8;
            fb_text(bx + (RETRY_W - lw) / 2, by + 7, lbl, COLOR_ON_SURFACE);
        }
        return 1;
    }

    return 0;
}

int list_state_mouse_down(const list_state_ctx_t *c, int x, int y)
{
    if (!c || c->state != LIST_ERROR || !c->retry_cb) return 0;
    int bx, by;
    retry_btn_rect(c, &bx, &by);
    if (x >= bx && x < bx + RETRY_W && y >= by && y < by + RETRY_H) {
        c->retry_cb(c->retry_ctx);
        return 1;
    }
    return 0;
}

uint32_t list_state_total_renders(void) { return g_total_renders; }
