/*
 * Zeos — Hot Corners
 *
 * Detects cursor dwell in screen corners and triggers OS actions.
 * Purely static state, no allocations. Called once per compositor frame.
 *
 * Corner zone layout (zone_size = 8px):
 *
 *   [TL]━━━━━━━━━━━━━━━━━━━━━━━━━━━[TR]
 *    ┃                                ┃
 *    ┃          (screen)              ┃
 *    ┃                                ┃
 *   [BL]━━━━━━━━━━━━━━━━━━━━━━━━━━━[BR]
 */

#include "hotcorners.h"
#include "timer.h"
#include "kprint.h"
#include "palette.h"
#include "wm.h"

/* ── Static state ── */
static hotcorners_state_t g_hc;

/* ── Defaults ── */
#define HC_DEFAULT_ZONE    8
#define HC_DEFAULT_DWELL 150

/* ── Corner detection ── */

/*
 * Determine which corner (if any) the cursor occupies.
 * Returns CORNER_COUNT if the cursor is not in any corner zone.
 */
static corner_id_t detect_corner(int mx, int my)
{
    int z = g_hc.zone_size;
    int w = g_hc.screen_w;
    int h = g_hc.screen_h;

    int at_left  = (mx < z);
    int at_right = (mx >= w - z);
    int at_top   = (my < z);
    int at_bot   = (my >= h - z);

    if (at_top && at_left)   return CORNER_TL;
    if (at_top && at_right)  return CORNER_TR;
    if (at_bot && at_left)   return CORNER_BL;
    if (at_bot && at_right)  return CORNER_BR;

    return CORNER_COUNT;  /* Not in any corner */
}

/* ── Corner actions ── */

static void action_tl(void)
{
    /* Top-left: Command Palette */
    kputs("[HC] TL fire -> palette_toggle\n");
    palette_toggle();
}

static void action_tr(void)
{
    /* Top-right: toggle the Notifications / Signal Status panel. */
    kputs("[HC] TR fire -> notify_show_all\n");
    extern void notify_show_all(void);
    notify_show_all();
}

static void action_bl(void)
{
    /* Bottom-left: Cycle to next workspace */
    int current = wm_get_workspace();
    int next = (current + 1) % WM_MAX_WORKSPACES;
    kputs("[HC] BL fire -> workspace "); kput_dec((uint64_t)current);
    kputs(" -> "); kput_dec((uint64_t)next); kputs("\n");
    wm_switch_workspace(next);
}

static void action_br(void)
{
    /* Bottom-right: Show Desktop — minimize all visible surfaces */
    kputs("[HC] BR fire -> show desktop\n");
    int count = wm_surface_count();
    for (int i = 0; i < count; i++) {
        chain_surface_t *s = wm_get_surface_by_index(i);
        if (s && s->visible && s->state != SURFACE_MINIMIZED)
            wm_minimize_surface(s->id);
    }
}

/* Dispatch table indexed by corner_id_t */
static void (*corner_actions[CORNER_COUNT])(void) = {
    [CORNER_TL] = action_tl,
    [CORNER_TR] = action_tr,
    [CORNER_BL] = action_bl,
    [CORNER_BR] = action_br,
};

/* ── Public API ── */

void hotcorners_init(int screen_w, int screen_h)
{
    g_hc.screen_w = screen_w;
    g_hc.screen_h = screen_h;
    g_hc.zone_size = HC_DEFAULT_ZONE;
    g_hc.dwell_ms = HC_DEFAULT_DWELL;
    g_hc.enabled = 1;
    g_hc.active_corner = -1;
    g_hc.enter_tsc = 0;

    for (int i = 0; i < CORNER_COUNT; i++)
        g_hc.triggered[i] = 0;

    kputs("HC: hot corners active (zone=");
    kput_dec((uint64_t)g_hc.zone_size);
    kputs("px, dwell=");
    kput_dec((uint64_t)g_hc.dwell_ms);
    kputs("ms)\n");
}

void hotcorners_tick(int mouse_x, int mouse_y)
{
    if (!g_hc.enabled)
        return;

    corner_id_t corner = detect_corner(mouse_x, mouse_y);

    if (corner == CORNER_COUNT) {
        /*
         * Cursor is outside all corner zones.
         * Reset active corner and clear all triggered flags
         * so corners can fire again on next entry.
         */
        if (g_hc.active_corner >= 0) {
            g_hc.triggered[g_hc.active_corner] = 0;
            g_hc.active_corner = -1;
            g_hc.enter_tsc = 0;
        }
        return;
    }

    /* Cursor is in a corner zone */
    int c = (int)corner;

    if (g_hc.active_corner != c) {
        /* Entered a new corner zone — start dwell timer */
        if (g_hc.active_corner >= 0)
            g_hc.triggered[g_hc.active_corner] = 0;

        g_hc.active_corner = c;
        g_hc.enter_tsc = timer_read_tsc();
        return;
    }

    /* Still in the same corner — check dwell time */
    if (g_hc.triggered[c])
        return;  /* Already fired, waiting for cursor to leave */

    uint64_t now = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    if (freq == 0)
        return;

    uint64_t elapsed_ms = ((now - g_hc.enter_tsc) * 1000) / freq;

    if (elapsed_ms >= (uint64_t)g_hc.dwell_ms) {
        /* Dwell threshold reached — fire the action */
        g_hc.triggered[c] = 1;

        if (corner_actions[c])
            corner_actions[c]();
    }
}

void hotcorners_enable(int enabled)
{
    g_hc.enabled = enabled;

    /* Reset state when toggling */
    g_hc.active_corner = -1;
    g_hc.enter_tsc = 0;
    for (int i = 0; i < CORNER_COUNT; i++)
        g_hc.triggered[i] = 0;
}
