/*
 * Zeos — Hover registry implementation
 *
 * Doctrine: a static array is enough. We don't have thousands of
 * clickables on screen at once — UI_HOVER_MAX of 128 is plenty for
 * dock items, panel buttons, list rows, and window controls combined.
 */

#include "ui_hover.h"
#include "cursor.h"

static hover_zone_t g_zones[UI_HOVER_MAX];
static uint64_t g_next_token = 1;
static uint32_t g_total_enters = 0;

static int hover_count_used(void) {
    int n = 0;
    for (int i = 0; i < UI_HOVER_MAX; i++) if (g_zones[i].used) n++;
    return n;
}

uint64_t hover_register(int x, int y, int w, int h,
                        hover_cursor_hint_t hint,
                        hover_cb_fn cb, void *ctx)
{
    for (int i = 0; i < UI_HOVER_MAX; i++) {
        if (!g_zones[i].used) {
            g_zones[i].used = 1;
            g_zones[i].x = x; g_zones[i].y = y;
            g_zones[i].w = w; g_zones[i].h = h;
            g_zones[i].cursor_hint = hint;
            g_zones[i].cb = cb;
            g_zones[i].ctx = ctx;
            g_zones[i].active = 0;
            g_zones[i].token = g_next_token++;
            return g_zones[i].token;
        }
    }
    return 0;
}

static int hover_find(uint64_t token) {
    if (!token) return -1;
    for (int i = 0; i < UI_HOVER_MAX; i++)
        if (g_zones[i].used && g_zones[i].token == token) return i;
    return -1;
}

void hover_update_rect(uint64_t token, int x, int y, int w, int h)
{
    int i = hover_find(token);
    if (i < 0) return;
    g_zones[i].x = x; g_zones[i].y = y;
    g_zones[i].w = w; g_zones[i].h = h;
}

void hover_unregister(uint64_t token)
{
    int i = hover_find(token);
    if (i < 0) return;
    if (g_zones[i].active && g_zones[i].cb)
        g_zones[i].cb(g_zones[i].ctx, 0);
    g_zones[i].used = 0;
    g_zones[i].active = 0;
    g_zones[i].cb = 0;
    g_zones[i].ctx = 0;
    g_zones[i].token = 0;
}

void hover_clear_all(void)
{
    for (int i = 0; i < UI_HOVER_MAX; i++) {
        g_zones[i].used = 0;
        g_zones[i].active = 0;
        g_zones[i].cb = 0;
        g_zones[i].ctx = 0;
        g_zones[i].token = 0;
    }
}

void hover_dispatch(int x, int y)
{
    /* Walk all zones; track topmost (highest index = most recently
     * registered) hit for cursor hint selection. Fire enter/leave on
     * every zone whose state flipped. */
    int top_hit = -1;
    for (int i = 0; i < UI_HOVER_MAX; i++) {
        if (!g_zones[i].used) continue;
        int inside = (x >= g_zones[i].x && x < g_zones[i].x + g_zones[i].w &&
                      y >= g_zones[i].y && y < g_zones[i].y + g_zones[i].h);
        if (inside) top_hit = i;
        if (inside != g_zones[i].active) {
            g_zones[i].active = inside;
            if (inside) g_total_enters++;
            if (g_zones[i].cb)
                g_zones[i].cb(g_zones[i].ctx, inside);
        }
    }

    /* Cursor hint follows the topmost active zone. */
    if (top_hit >= 0) {
        switch (g_zones[top_hit].cursor_hint) {
            case HOVER_CURSOR_POINTER: cursor_set(CURSOR_POINTER); break;
            case HOVER_CURSOR_TEXT:    cursor_set(CURSOR_TEXT);    break;
            case HOVER_CURSOR_RESIZE:  cursor_set(CURSOR_RESIZE_DIAG); break;
            default:                   cursor_set(CURSOR_DEFAULT); break;
        }
    } else {
        cursor_set(CURSOR_DEFAULT);
    }
}

int hover_any_active(void)
{
    for (int i = 0; i < UI_HOVER_MAX; i++)
        if (g_zones[i].used && g_zones[i].active) return 1;
    return 0;
}

uint32_t hover_total_zones(void)  { return (uint32_t)hover_count_used(); }
uint32_t hover_total_enters(void) { return g_total_enters; }
