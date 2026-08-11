/*
 * Zeos -- Command Palette
 *
 * Searchable overlay that makes every OS action reachable.
 * Top-left sacred corner. Super+Space to toggle.
 *
 * Visual: centered 500px overlay, semi-transparent dark bg,
 * search box at top, filtered results below, max 8 visible.
 *
 * Filter: case-insensitive substring match.
 * No malloc. Static arrays only.
 */

#include "palette.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "keybinds.h"
#include "chain.h"
#include "wm.h"
#include "persona_filter.h"
#include "ui_undo.h"
#include "ui_hover.h"
#include "settings_registry.h"   /* J.3: enumerate settings into the palette */
#include "settings.h"            /* settings_open() */
#include "ui_context_menu.h"
#include "ui_states.h"
#include "kprint.h"

/* ── UI primitive wiring ── */
static undo_buffer_t s_pal_undo;
static int           s_pal_undo_inited = 0;
#define PAL_MAX_ROW_HOVERS 8
static uint64_t s_row_tokens[PAL_MAX_ROW_HOVERS];
static int      s_row_token_count = 0;
static int      s_rc_filtered_idx = -1;

/* ── String helpers (freestanding, no libc) ─────────────────────── */

static int pal_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void pal_strncpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static char pal_tolower(char c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

/*
 * Case-insensitive substring search.
 * Returns 1 if needle is found in haystack, 0 otherwise.
 */
static int pal_strcasestr(const char *haystack, const char *needle)
{
    int hlen = pal_strlen(haystack);
    int nlen = pal_strlen(needle);
    int i, j;

    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;

    for (i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (j = 0; j < nlen; j++) {
            if (pal_tolower(haystack[i + j]) != pal_tolower(needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

/* ── Global state ───────────────────────────────────────────────── */

static palette_state_t pal;

/* ── Visual constants ───────────────────────────────────────────── */

#define PAL_WIDTH          500
#define PAL_MAX_HEIGHT     400
#define PAL_SEARCH_HEIGHT   40
#define PAL_ITEM_HEIGHT     32
#define PAL_VISIBLE_ITEMS    8
#define PAL_PADDING         16
#define PAL_CORNER_INSET     8

/* Semi-transparent surface: COLOR_SURFACE with ~85% alpha (0xD9) */
#define PAL_BG_COLOR       ((COLOR_SURFACE & 0x00FFFFFF) | 0xD9000000)
/* Search box background — surface_high */
#define PAL_SEARCH_BG      COLOR_SURFACE_HIGH
/* Search box border — separator */
#define PAL_SEARCH_BORDER  COLOR_SEPARATOR
/* Selected item highlight — surface_top */
#define PAL_SELECT_BG      COLOR_SURFACE_TOP

/* ── Default action callbacks ───────────────────────────────────── */

static void action_open_terminal(void *ctx)
{
    (void)ctx;
    /* Shell chain will be opened by the shell subsystem */
}

static void action_toggle_tiling(void *ctx)
{
    (void)ctx;
    wm_toggle_tiling();
}

static void action_show_desktop(void *ctx)
{
    (void)ctx;
    /* Spring-animated show/restore desktop toggle */
    if (wm_is_desktop_shown())
        wm_restore_desktop();
    else
        wm_show_desktop();
}

static void action_switch_ws1(void *ctx) { (void)ctx; wm_switch_workspace(0); }
static void action_switch_ws2(void *ctx) { (void)ctx; wm_switch_workspace(1); }
static void action_switch_ws3(void *ctx) { (void)ctx; wm_switch_workspace(2); }
static void action_switch_ws4(void *ctx) { (void)ctx; wm_switch_workspace(3); }

static void action_inspect_chain(void *ctx)
{
    (void)ctx;
    extern int activity_open(void);   /* system/chain state inspector */
    (void)activity_open();
}

static void action_settings(void *ctx)
{
    (void)ctx;
    extern void settings_open(void);
    settings_open();
}

static void action_focus_chain(void *ctx)
{
    /* ctx holds chain id as pointer-sized int */
    int chain_id = (int)(uint64_t)ctx;
    /* Find a surface bound to this chain and focus it */
    for (int i = 0; i < WM_MAX_SURFACES; i++) {
        chain_surface_t *s = wm_get_surface(i);
        if (s && s->chain_id == chain_id) {
            wm_focus_surface(s->id);
            wm_restore_surface(s->id);
            break;
        }
    }
}

/* ── Filter logic ───────────────────────────────────────────────── */

static void palette_filter(void)
{
    pal.filtered_count = 0;
    pal.selected = 0;
    pal.scroll_offset = 0;

    for (int i = 0; i < pal.item_count; i++) {
        if (pal.query_len == 0 ||
            pal_strcasestr(pal.items[i].label, pal.query) ||
            pal_strcasestr(pal.items[i].category, pal.query)) {
            if (pal.filtered_count < PALETTE_MAX_ITEMS)
                pal.filtered[pal.filtered_count++] = pal.items[i];
        }
    }
}

/* ── Refresh dynamic items (chains) ─────────────────────────────── */

static void palette_refresh_chains(void)
{
    /*
     * Remove old chain entries, then re-add active chains.
     * Chain items have category "chain".
     */
    int write = 0;
    for (int i = 0; i < pal.item_count; i++) {
        int is_chain = 1;
        const char *cat = pal.items[i].category;
        if (cat[0] == 'c' && cat[1] == 'h' && cat[2] == 'a' &&
            cat[3] == 'i' && cat[4] == 'n' && cat[5] == '\0')
            is_chain = 1;
        else
            is_chain = 0;

        if (!is_chain) {
            if (write != i)
                pal.items[write] = pal.items[i];
            write++;
        }
    }
    pal.item_count = write;

    /* Add all live chains visible to the current persona */
    int count = chain_count();
    for (int i = 0; i < count && i < MAX_CHAINS; i++) {
        chain_t *c = chain_get(i);
        if (!c || c->status == CHAIN_DETACHED) continue;
        if (!persona_can_see(i)) continue;
        if (pal.item_count >= PALETTE_MAX_ITEMS) break;

        palette_item_t *item = &pal.items[pal.item_count];

        /* Build label: "Focus: <chain_name>" */
        char *dst = item->label;
        const char *prefix = "Focus: ";
        int pi = 0;
        while (prefix[pi] && pi < 7) { dst[pi] = prefix[pi]; pi++; }
        int ci = 0;
        while (c->name[ci] && pi + ci < 63) {
            dst[pi + ci] = c->name[ci];
            ci++;
        }
        dst[pi + ci] = '\0';

        pal_strncpy(item->category, "chain", 32);
        item->action_id = 100 + i;
        item->execute = action_focus_chain;
        item->ctx = (void *)(uint64_t)c->id;

        pal.item_count++;
    }
}

/* ── Undo apply: replace [pos..pos+old_len) of the query with new ── */
static void palette_undo_apply(void *ctx, int pos,
                               const char *old_text, int old_len,
                               const char *new_text, int new_len)
{
    (void)ctx; (void)old_text;
    int qlen = pal.query_len;
    if (pos < 0) pos = 0;
    if (pos > qlen) pos = qlen;
    int tail_len = qlen - (pos + old_len);
    if (tail_len < 0) tail_len = 0;
    char tail[PALETTE_MAX_QUERY];
    for (int i = 0; i < tail_len && i < PALETTE_MAX_QUERY - 1; i++)
        tail[i] = pal.query[pos + old_len + i];
    int wp = pos;
    for (int i = 0; i < new_len && wp < PALETTE_MAX_QUERY - 1; i++)
        pal.query[wp++] = new_text[i];
    for (int i = 0; i < tail_len && wp < PALETTE_MAX_QUERY - 1; i++)
        pal.query[wp++] = tail[i];
    pal.query[wp] = 0;
    pal.query_len = wp;
    palette_filter();
}
static void palette_ensure_undo(void) {
    if (s_pal_undo_inited) return;
    undo_init(&s_pal_undo, palette_undo_apply, 0);
    s_pal_undo_inited = 1;
}

/* ── Right-click actions ── */
static void rc_run(void *ctx) {
    (void)ctx;
    if (s_rc_filtered_idx < 0 || s_rc_filtered_idx >= pal.filtered_count) return;
    palette_item_t *it = &pal.filtered[s_rc_filtered_idx];
    if (it->execute) it->execute(it->ctx);
    palette_hide();
}
static void rc_show_source(void *ctx) {
    (void)ctx;
    if (s_rc_filtered_idx < 0 || s_rc_filtered_idx >= pal.filtered_count) return;
    kputs("PAL: source: "); kputs(pal.filtered[s_rc_filtered_idx].label); kputs("\n");
}
static void rc_copy(void *ctx) {
    (void)ctx;
    if (s_rc_filtered_idx < 0 || s_rc_filtered_idx >= pal.filtered_count) return;
    kputs("PAL: copy: "); kputs(pal.filtered[s_rc_filtered_idx].label); kputs("\n");
}

int palette_right_click(int x, int y) {
    if (!pal.visible) return 0;
    int scr_w = (int)fb_width();
    int scr_h = (int)fb_height();
    int pal_w = PAL_WIDTH;
    int visible = pal.filtered_count;
    if (visible > PAL_VISIBLE_ITEMS) visible = PAL_VISIBLE_ITEMS;
    int results_h = visible * PAL_ITEM_HEIGHT;
    int pal_h = PAL_SEARCH_HEIGHT + PAL_PADDING * 2 + results_h;
    if (pal_h > PAL_MAX_HEIGHT) pal_h = PAL_MAX_HEIGHT;
    if (pal_h < PAL_SEARCH_HEIGHT + PAL_PADDING * 2)
        pal_h = PAL_SEARCH_HEIGHT + PAL_PADDING * 2;
    int px = (scr_w - pal_w) / 2;
    int py = (scr_h - pal_h) / 3;
    if (py < Z16) py = Z16;
    int sx = px + PAL_PADDING;
    int sy = py + PAL_PADDING;
    int sw = pal_w - PAL_PADDING * 2;
    int ry = sy + PAL_SEARCH_HEIGHT;
    if (x < sx || x >= sx + sw) return 0;
    for (int i = 0; i < PAL_VISIBLE_ITEMS; i++) {
        int idx = pal.scroll_offset + i;
        if (idx >= pal.filtered_count) break;
        int item_y = ry + i * PAL_ITEM_HEIGHT;
        if (y >= item_y && y < item_y + PAL_ITEM_HEIGHT) {
            s_rc_filtered_idx = idx;
            static const ctx_menu_item_t items[3] = {
                { "Run",         rc_run,         0, 1 },
                { "Show source", rc_show_source, 0, 1 },
                { "Copy",        rc_copy,        0, 1 },
            };
            context_menu_open(x, y, items, 3);
            return 1;
        }
    }
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────── */

int palette_add_item(const char *label, const char *category,
                     void (*execute_fn)(void *ctx), void *ctx)
{
    if (pal.item_count >= PALETTE_MAX_ITEMS)
        return -1;

    palette_item_t *item = &pal.items[pal.item_count];
    pal_strncpy(item->label, label, 64);
    pal_strncpy(item->category, category, 32);
    item->action_id = ACTION_NONE;
    item->execute = execute_fn;
    item->ctx = ctx;
    pal.item_count++;
    return 0;
}

void palette_init(void)
{
    /* Zero entire state */
    for (int i = 0; i < (int)sizeof(palette_state_t); i++)
        ((char *)&pal)[i] = 0;

    /* Register default items */
    palette_add_item("Open Terminal",      "app",     action_open_terminal,  0);
    pal.items[0].action_id = ACTION_OPEN_TERMINAL;

    palette_add_item("Toggle Tiling",      "window",  action_toggle_tiling,  0);
    pal.items[1].action_id = ACTION_TOGGLE_TILING;

    palette_add_item("Show Desktop",       "window",  action_show_desktop,   0);
    pal.items[2].action_id = ACTION_SHOW_DESKTOP;

    palette_add_item("Switch Workspace 1", "window",  action_switch_ws1,     0);
    pal.items[3].action_id = ACTION_WORKSPACE_1;

    palette_add_item("Switch Workspace 2", "window",  action_switch_ws2,     0);
    pal.items[4].action_id = ACTION_WORKSPACE_2;

    palette_add_item("Switch Workspace 3", "window",  action_switch_ws3,     0);
    palette_add_item("Switch Workspace 4", "window",  action_switch_ws4,     0);

    palette_add_item("Inspect Chain",      "signal",  action_inspect_chain,  0);
    pal.items[7].action_id = ACTION_INSPECT_CHAIN;

    palette_add_item("Settings",           "setting", action_settings,       0);

    palette_filter();
}

/* J.3: activate an enumerated setting. Bool -> toggle inline (the common quick
 * action); anything else -> open the settings app focused on it. ctx is the
 * statically-allocated settings_entry_t* the registry holds by reference. */
static void action_setting_entry(void *ctx)
{
    const settings_entry_t *e = (const settings_entry_t *)ctx;
    if (!e) { settings_open(); return; }
    if (e->kind == SK_BOOL) {
        char cur[16];
        if (settings_get(e->name, cur, sizeof(cur)) == 0) {
            int on = (cur[0] == '1' || cur[0] == 'o' || cur[0] == 't' ||
                      cur[0] == 'y' || cur[0] == 'O' || cur[0] == 'T');
            settings_set(e->name, on ? "0" : "1");
            return;
        }
    }
    settings_open();
}

/* J.3: strip stale "setting" items and re-add one per registered setting, so
 * the palette search covers every setting. Mirrors palette_refresh_chains. */
static int palette_add_setting_cb(const settings_entry_t *e, void *user)
{
    (void)user;
    if (pal.item_count >= PALETTE_MAX_ITEMS) return 1;  /* stop: palette full */
    const char *label = (e->desc && e->desc[0]) ? e->desc : e->name;
    palette_add_item(label, "setting", action_setting_entry, (void *)e);
    return 0;
}

static void palette_refresh_settings(void)
{
    /* Remove existing "setting" items (compact in place). */
    int write = 0;
    for (int i = 0; i < pal.item_count; i++) {
        const char *cat = pal.items[i].category;
        int is_setting = (cat[0] == 's' && cat[1] == 'e' && cat[2] == 't' &&
                          cat[3] == 't' && cat[4] == 'i' && cat[5] == 'n' &&
                          cat[6] == 'g' && cat[7] == '\0');
        if (!is_setting) {
            if (write != i) pal.items[write] = pal.items[i];
            write++;
        }
    }
    pal.item_count = write;

    int before = pal.item_count;
    settings_enumerate(palette_add_setting_cb, 0);
    int added = pal.item_count - before;
    if (added < settings_count())
        kputs("[palette] NOTE: settings truncated (palette full)\n");
}

/* J.3 selftest: the command palette enumerates EVERY registry setting (search-
 * first settings). Refreshes the palette's settings items and asserts the count
 * matches settings_count() (or the palette hit its cap).
 * Un-gated so the production `selftest` shell can run it; PRODUCTION-SAFE:
 * palette_refresh_settings only mutates the palette's item list, which is never
 * rendered while the palette is hidden (pal.visible=0 at the shell) and is fully
 * rebuilt on the next palette_show — a benign internal recompute (F.4 class). It
 * does NOT set pal.visible or dirty the compositor. Safety rests on the
 * render-gate (nothing reads pal.items[] while hidden) + the full rebuild on the
 * next palette_show; we also restore pal.item_count so a hidden enumeration
 * leaves no observable change. (Not byte-identical: items[] content beyond the
 * restored count is stale, but unreferenced and overwritten on next show.)
 * Returns 1 on PASS. */
int palette_j3_selftest(void)
{
    int saved_item_count = pal.item_count;
    palette_refresh_settings();
    int settings_items = 0;
    for (int i = 0; i < pal.item_count; i++) {
        const char *c = pal.items[i].category;
        if (c[0]=='s'&&c[1]=='e'&&c[2]=='t'&&c[3]=='t'&&c[4]=='i'&&c[5]=='n'&&c[6]=='g'&&c[7]==0)
            settings_items++;
    }
    int reg = settings_count();
    int pass = (reg > 0) && (settings_items == reg || pal.item_count >= PALETTE_MAX_ITEMS);
    pal.item_count = saved_item_count;   /* restore: leave the hidden palette list untouched */
    kputs("[J3] palette settings items="); kput_dec((uint64_t)settings_items);
    kputs(" registry="); kput_dec((uint64_t)reg);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
    return pass;
}

void palette_show(void)
{
    pal.visible = 1;
    pal.query[0] = '\0';
    pal.query_len = 0;
    pal.selected = 0;
    pal.scroll_offset = 0;

    palette_refresh_chains();
    palette_refresh_settings();   /* J.3: settings searchable in the palette */
    palette_filter();

    palette_ensure_undo();
    undo_set_focus(&s_pal_undo);
    /* Full recomposite so the palette overlay + backdrop paints (D.4). */
    { extern void compositor_dirty_all(void); compositor_dirty_all(); }
}

void palette_hide(void)
{
    pal.visible = 0;
    for (int i = 0; i < s_row_token_count; i++) hover_unregister(s_row_tokens[i]);
    s_row_token_count = 0;
    if (undo_get_focus() == &s_pal_undo) undo_set_focus(0);
    /* Full recomposite so the full-screen backdrop is cleared (D.4). */
    { extern void compositor_dirty_all(void); compositor_dirty_all(); }
}

void palette_toggle(void)
{
    if (pal.visible)
        palette_hide();
    else
        palette_show();
#ifdef ZEOS_DIAG_KEYTRACE
    { extern void kputs(const char*); extern void kput_hex(uint64_t);
      kputs("[KT] palette_toggle -> visible="); kput_hex((uint64_t)pal.visible);
      kputs("\n"); }
#endif
}

/* Repaint after any state change. The result list grows/shrinks with the query,
 * so the region a shrinking list vacates must be recomposited too -- dirty the
 * whole screen (matches palette_show/hide). Without this the overlay shows a
 * stale frame after typing even though the query/filter updated. */
static void palette_repaint(void)
{
    extern void compositor_dirty_all(void);
    compositor_dirty_all();
}

void palette_type(char c)
{
    if (!pal.visible) return;
    if (pal.query_len >= PALETTE_MAX_QUERY - 1) return;

    palette_ensure_undo();
    char ins[2] = { c, 0 };
    undo_record(&s_pal_undo, pal.query_len, "", ins);

    pal.query[pal.query_len++] = c;
    pal.query[pal.query_len] = '\0';
    palette_filter();
    palette_repaint();
}

void palette_backspace(void)
{
    if (!pal.visible) return;
    if (pal.query_len <= 0) return;

    palette_ensure_undo();
    char del[2] = { pal.query[pal.query_len - 1], 0 };
    undo_record(&s_pal_undo, pal.query_len - 1, del, "");

    pal.query_len--;
    pal.query[pal.query_len] = '\0';
    palette_filter();
    palette_repaint();
}

void palette_up(void)
{
    if (!pal.visible) return;
    if (pal.selected > 0) {
        pal.selected--;
        if (pal.selected < pal.scroll_offset)
            pal.scroll_offset = pal.selected;
    }
    palette_repaint();
}

void palette_down(void)
{
    if (!pal.visible) return;
    if (pal.selected < pal.filtered_count - 1) {
        pal.selected++;
        if (pal.selected >= pal.scroll_offset + PAL_VISIBLE_ITEMS)
            pal.scroll_offset = pal.selected - PAL_VISIBLE_ITEMS + 1;
    }
    palette_repaint();
}

void palette_execute(void)
{
    if (!pal.visible) return;
    if (pal.selected < 0 || pal.selected >= pal.filtered_count) return;

    palette_item_t *item = &pal.filtered[pal.selected];
    palette_hide();

    if (item->execute)
        item->execute(item->ctx);
}

int palette_is_visible(void)
{
    return pal.visible;
}

/* ── Drawing ────────────────────────────────────────────────────── */

void palette_draw(void)
{
    if (!pal.visible) return;

    int scr_w = (int)fb_width();
    int scr_h = (int)fb_height();

    /* Calculate overlay geometry — centered */
    int pal_w = PAL_WIDTH;
    int visible = pal.filtered_count;
    if (visible > PAL_VISIBLE_ITEMS) visible = PAL_VISIBLE_ITEMS;

    int results_h = visible * PAL_ITEM_HEIGHT;
    int pal_h = PAL_SEARCH_HEIGHT + PAL_PADDING * 2 + results_h;
    if (pal_h > PAL_MAX_HEIGHT) pal_h = PAL_MAX_HEIGHT;

    /* Minimum height: search box + padding */
    if (pal_h < PAL_SEARCH_HEIGHT + PAL_PADDING * 2)
        pal_h = PAL_SEARCH_HEIGHT + PAL_PADDING * 2;

    int px = (scr_w - pal_w) / 2;
    int py = (scr_h - pal_h) / 3;  /* Slightly above center */
    if (py < Z16) py = Z16;

    /* ── Dimmed backdrop over entire screen ── */
    fb_rect_blend(0, 0, scr_w, scr_h, 0x60000000);

    /* ── Palette background ── */
    fb_rect_blend(px, py, pal_w, pal_h, PAL_BG_COLOR);

    /* ── Border ── */
    fb_rect_outline(px, py, pal_w, pal_h, PAL_SEARCH_BORDER, 1);

    /* ── Search box ── */
    int sx = px + PAL_PADDING;
    int sy = py + PAL_PADDING;
    int sw = pal_w - PAL_PADDING * 2;
    int sh = PAL_SEARCH_HEIGHT - PAL_CORNER_INSET;

    fb_rect(sx, sy, sw, sh, PAL_SEARCH_BG);
    fb_rect_outline(sx, sy, sw, sh, COLOR_PRIMARY, 1);

    /* Search text */
    int text_x = sx + Z2;
    int text_y = sy + (sh - TYPE_BODY) / 2;

    if (pal.query_len > 0) {
        font_draw(text_x, text_y, pal.query, FONT_UI, TYPE_BODY,
                  (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_PRIMARY << 24));
    } else {
        /* Placeholder */
        font_draw(text_x, text_y, "Type to search...", FONT_UI, TYPE_BODY,
                  (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_TERTIARY << 24));
    }

    /* Cursor */
    int cursor_x = text_x;
    if (pal.query_len > 0)
        cursor_x += font_measure(pal.query, FONT_UI, TYPE_BODY);
    fb_rect(cursor_x, sy + 6, 2, sh - 12, COLOR_PRIMARY);

    /* ── Results list ── */
    int ry = sy + PAL_SEARCH_HEIGHT;

    /* Drop previous row hover zones; we re-issue below. */
    for (int i = 0; i < s_row_token_count; i++) hover_unregister(s_row_tokens[i]);
    s_row_token_count = 0;

    /* Empty state — caller hasn't typed or query has no matches. */
    if (pal.filtered_count == 0) {
        list_state_ctx_t lc = {
            sx, ry, sw, PAL_ITEM_HEIGHT * 3, LIST_EMPTY,
            pal.query_len > 0 ? "No matches — try a different query."
                              : "Type to search actions, chains, settings.",
            0, 0, 0
        };
        list_render_state(&lc);
    }

    for (int i = 0; i < PAL_VISIBLE_ITEMS; i++) {
        int idx = pal.scroll_offset + i;
        if (idx >= pal.filtered_count) break;

        palette_item_t *item = &pal.filtered[idx];
        int item_y = ry + i * PAL_ITEM_HEIGHT;

        /* Register hover zone for this row (cursor → hand). */
        if (s_row_token_count < PAL_MAX_ROW_HOVERS) {
            uint64_t tok = hover_register(sx, item_y, sw, PAL_ITEM_HEIGHT,
                                          HOVER_CURSOR_POINTER, 0, 0);
            if (tok) s_row_tokens[s_row_token_count++] = tok;
        }

        /* Highlight selected item */
        if (idx == pal.selected) {
            fb_rect(sx, item_y, sw, PAL_ITEM_HEIGHT, PAL_SELECT_BG);
            /* Accent bar on left edge */
            fb_rect(sx, item_y, 3, PAL_ITEM_HEIGHT, COLOR_PRIMARY);
        }

        /* Item label */
        int label_y = item_y + (PAL_ITEM_HEIGHT - TYPE_BODY) / 2;
        uint32_t label_color;
        if (idx == pal.selected)
            label_color = (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_PRIMARY << 24);
        else
            label_color = (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_SECONDARY << 24);

        font_draw(sx + Z3, label_y, item->label, FONT_UI, TYPE_BODY, label_color);

        /* Category label — right-aligned, tertiary color */
        uint32_t cat_color = (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_TERTIARY << 24);
        int cat_w = font_measure(item->category, FONT_UI, TYPE_CAPTION);
        int cat_x = sx + sw - cat_w - Z2;
        int cat_y = item_y + (PAL_ITEM_HEIGHT - TYPE_CAPTION) / 2;
        font_draw(cat_x, cat_y, item->category, FONT_UI, TYPE_CAPTION, cat_color);
    }

    /* ── Scroll indicator (if more items than visible) ── */
    if (pal.filtered_count > PAL_VISIBLE_ITEMS) {
        int indicator_x = px + pal_w - Z2 - 3;
        int track_y = ry;
        int track_h = PAL_VISIBLE_ITEMS * PAL_ITEM_HEIGHT;

        /* Thumb position and size */
        int thumb_h = (track_h * PAL_VISIBLE_ITEMS) / pal.filtered_count;
        if (thumb_h < Z4) thumb_h = Z4;
        int max_scroll = pal.filtered_count - PAL_VISIBLE_ITEMS;
        int thumb_y = track_y;
        if (max_scroll > 0)
            thumb_y = track_y + (pal.scroll_offset * (track_h - thumb_h)) / max_scroll;

        fb_rect(indicator_x, thumb_y, 3, thumb_h, COLOR_ON_SURFACE_3);
    }
}
