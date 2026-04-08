/*
 * Zeos — Panel (Top Bar)
 *
 * Three-zone rendering: persona | chain pills | health + clock.
 * Refreshed each frame. No heap allocations — everything is static.
 */

#include "panel.h"
#include "fb.h"
#include "wm.h"
#include "chain.h"
#include "font.h"
#include "theme.h"
#include "persona.h"
#include "timer.h"
#include "palette.h"
#include "kprint.h"

/* ── Static state ── */
static panel_state_t g_panel;

/* ── Helpers ── */

static void str_copy_n(char *d, const char *s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static uint32_t persona_accent(int persona) {
    switch (persona) {
    case PERSONA_ZEROS: return COLOR_ZEROS_ACCENT;
    case PERSONA_DEREZ: return COLOR_DEREZ_ACCENT;
    default:            return COLOR_FULL_ACCENT;
    }
}

static const char *persona_name(int persona) {
    switch (persona) {
    case PERSONA_ZEROS: return "Zeros";
    case PERSONA_DEREZ: return "DereZ";
    default:            return "Zeos";
    }
}

/* Convert chain_status_t to a pill color */
static uint32_t pill_color(chain_status_t status, uint32_t accent) {
    switch (status) {
    case CHAIN_LIVE:     return accent;
    case CHAIN_PAUSED:   return COLOR_ON_SURFACE_4;   /* dim */
    case CHAIN_ERROR:    return COLOR_DANGER;
    case CHAIN_DETACHED: return COLOR_ON_SURFACE_4;
    }
    return COLOR_ON_SURFACE_4;
}

/* Convert signal_status_t (from WM surface) to chain_status_t */
static chain_status_t signal_to_chain(signal_status_t sig) {
    switch (sig) {
    case SIGNAL_LIVE:     return CHAIN_LIVE;
    case SIGNAL_PAUSED:   return CHAIN_PAUSED;
    case SIGNAL_ERROR:    return CHAIN_ERROR;
    case SIGNAL_DETACHED: return CHAIN_DETACHED;
    }
    return CHAIN_DETACHED;
}

/* ── Rounded rectangle (simple: filled rect + corner masking) ── */

static void draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color) {
    if (w <= 0 || h <= 0) return;

    /* Clamp radius */
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;

    /* Main body (excluding corners) */
    fb_rect(x + r, y, w - 2 * r, h, color);       /* center strip */
    fb_rect(x, y + r, r, h - 2 * r, color);        /* left strip */
    fb_rect(x + w - r, y + r, r, h - 2 * r, color);/* right strip */

    /* Corner circles */
    fb_circle_filled(x + r,     y + r,     r, color);  /* TL */
    fb_circle_filled(x + w - r - 1, y + r,     r, color);  /* TR */
    fb_circle_filled(x + r,     y + h - r - 1, r, color);  /* BL */
    fb_circle_filled(x + w - r - 1, y + h - r - 1, r, color);  /* BR */
}

/* ── Integer formatting ── */

static void format_clock(char *buf, int hours, int minutes) {
    buf[0] = '0' + (hours / 10);
    buf[1] = '0' + (hours % 10);
    buf[2] = ':';
    buf[3] = '0' + (minutes / 10);
    buf[4] = '0' + (minutes % 10);
    buf[5] = 0;
}

static void format_int(char *buf, int val) {
    if (val < 0) val = 0;
    if (val >= 100) {
        buf[0] = '0' + (val / 100);
        buf[1] = '0' + ((val / 10) % 10);
        buf[2] = '0' + (val % 10);
        buf[3] = 0;
    } else if (val >= 10) {
        buf[0] = '0' + (val / 10);
        buf[1] = '0' + (val % 10);
        buf[2] = 0;
    } else {
        buf[0] = '0' + val;
        buf[1] = 0;
    }
}

/* ── API ── */

void panel_init(int persona, int height) {
    g_panel.height = height;
    g_panel.visible = 1;
    g_panel.persona = persona;
    g_panel.persona_color = persona_accent(persona);
    g_panel.pill_count = 0;
    g_panel.chain_total = 0;
    g_panel.chain_live = 0;
    g_panel.chain_error = 0;
    g_panel.notification_count = 0;
    g_panel.screen_w = (int)fb_width();
    str_copy_n(g_panel.clock_str, "00:00", 16);

    kputs("PANEL: initialized persona=");
    kputs(persona_name(persona));
    kputs(" h=");
    kput_dec(height);
    kputs("\n");
}

void panel_update(void) {
    if (!g_panel.visible) return;

    g_panel.screen_w = (int)fb_width();

    /* ── Gather pills from WM surfaces ── */
    int count = wm_surface_count();
    int ws = wm_get_workspace();
    int pill_idx = 0;
    int live = 0, err = 0;

    for (int i = 0; i < count && pill_idx < PANEL_MAX_PILLS; i++) {
        chain_surface_t *s = wm_get_surface_by_index(i);
        if (!s) continue;
        if (s->workspace != ws) continue;
        if (!s->visible && s->state != SURFACE_MINIMIZED) continue;

        panel_pill_t *p = &g_panel.pills[pill_idx];
        p->surface_id = s->id;
        p->chain_id = s->chain_id;
        str_copy_n(p->name, s->title, 32);

        /* Map signal status to chain status for coloring */
        p->status = signal_to_chain(s->signal);
        p->color = pill_color(p->status, g_panel.persona_color);

        if (p->status == CHAIN_LIVE) live++;
        if (p->status == CHAIN_ERROR) err++;

        pill_idx++;
    }

    g_panel.pill_count = pill_idx;
    g_panel.chain_total = count;
    g_panel.chain_live = live;
    g_panel.chain_error = err;

    /* ── Update clock from TSC ── */
    uint64_t tsc = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    if (freq > 0) {
        uint64_t seconds = tsc / freq;
        int hours = (int)((seconds / 3600) % 24);
        int minutes = (int)((seconds / 60) % 60);
        format_clock(g_panel.clock_str, hours, minutes);
    }
}

void panel_draw(void) {
    if (!g_panel.visible) return;

    int pw = g_panel.screen_w;
    int ph = g_panel.height;

    /* ── Panel background ── */
    fb_rect(0, 0, pw, ph, COLOR_SURFACE_HIGH);

    /* ── Bottom separator ── */
    fb_hline(0, ph - 1, pw, COLOR_SEPARATOR);

    /* ═══════════════════════════════════════════════════════════════
     * LEFT ZONE (0 to PANEL_LEFT_W)
     * Persona dot + name. Click opens command palette.
     * ═══════════════════════════════════════════════════════════════ */

    /* Persona dot */
    fb_circle_filled(PANEL_DOT_X, ph / 2, PANEL_DOT_RADIUS, g_panel.persona_color);

    /* Persona name */
    const char *name = persona_name(g_panel.persona);
    font_draw(PANEL_DOT_X + PANEL_DOT_RADIUS + 8, (ph - TYPE_LABEL) / 2,
              name, FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_2);

    /* ═══════════════════════════════════════════════════════════════
     * CENTER ZONE (PANEL_LEFT_W to pw - PANEL_RIGHT_W)
     * One pill per visible WM surface.
     * ═══════════════════════════════════════════════════════════════ */

    int center_start = PANEL_LEFT_W;
    int center_end = pw - PANEL_RIGHT_W;
    int pill_h = ph - PANEL_PILL_VPAD * 2;
    int pill_y = PANEL_PILL_VPAD;
    int pill_r = BORDER_RADIUS_SM;
    int cx = center_start;

    for (int i = 0; i < g_panel.pill_count; i++) {
        panel_pill_t *p = &g_panel.pills[i];

        /* Measure text to size the pill */
        int text_w = font_measure(p->name, FONT_UI, TYPE_CAPTION);
        int pill_w = text_w + PANEL_PILL_HPAD * 2;

        /* Overflow clip: stop if pill would exceed center zone */
        if (cx + pill_w > center_end) break;

        /* Store position for hit testing */
        p->x = cx;
        p->w = pill_w;

        /* Pill background — accent for live, dim for paused, red for error */
        uint32_t bg;
        uint32_t fg;
        if (p->status == CHAIN_LIVE) {
            /* Accent at ~20% opacity over surface */
            bg = (p->color & 0x00FFFFFF) | 0x33000000;
            fg = p->color;
        } else if (p->status == CHAIN_ERROR) {
            bg = (COLOR_DANGER & 0x00FFFFFF) | 0x33000000;
            fg = COLOR_DANGER;
        } else {
            bg = COLOR_SURFACE_TOP;
            fg = COLOR_ON_SURFACE_3;
        }

        /* Draw pill rounded rect */
        draw_rounded_rect(cx, pill_y, pill_w, pill_h, pill_r, bg);

        /* If this surface is focused, draw accent border */
        chain_surface_t *surf = wm_get_surface(p->surface_id);
        if (surf && surf->focused) {
            /* Top accent line on pill */
            fb_hline(cx + pill_r, pill_y, pill_w - 2 * pill_r, g_panel.persona_color);
        }

        /* Pill text */
        font_draw(cx + PANEL_PILL_HPAD,
                  pill_y + (pill_h - TYPE_CAPTION) / 2,
                  p->name, FONT_UI, TYPE_CAPTION, fg);

        cx += pill_w + PANEL_PILL_PAD;
    }

    /* ═══════════════════════════════════════════════════════════════
     * RIGHT ZONE (pw - PANEL_RIGHT_W to pw)
     * System health dot, notification badge, clock.
     * ═══════════════════════════════════════════════════════════════ */

    int rz_x = pw - PANEL_RIGHT_W;
    int ry_center = ph / 2;

    /* System health dot */
    uint32_t health_color;
    if (g_panel.chain_error > 0)
        health_color = COLOR_DANGER;
    else if (g_panel.chain_total > 0 && g_panel.chain_live == 0)
        health_color = COLOR_WARNING;
    else
        health_color = COLOR_SUCCESS;

    fb_circle_filled(rz_x + 16, ry_center, 4, health_color);

    /* Notification count badge (if > 0) */
    if (g_panel.notification_count > 0) {
        char nbuf[4];
        format_int(nbuf, g_panel.notification_count);
        int badge_x = rz_x + 36;

        /* Badge background */
        int badge_tw = font_measure(nbuf, FONT_UI, TYPE_MICRO);
        int badge_w = badge_tw + 8;
        if (badge_w < 16) badge_w = 16;
        draw_rounded_rect(badge_x, ry_center - 7, badge_w, 14, 7, COLOR_DANGER);

        /* Badge text */
        font_draw(badge_x + (badge_w - badge_tw) / 2, ry_center - 5,
                  nbuf, FONT_UI, TYPE_MICRO, COLOR_ON_SURFACE);
    }

    /* Clock — right-aligned */
    int clock_w = font_measure(g_panel.clock_str, FONT_CODE, TYPE_LABEL);
    int clock_x = pw - clock_w - 12;
    font_draw(clock_x, (ph - TYPE_LABEL) / 2,
              g_panel.clock_str, FONT_CODE, TYPE_LABEL, COLOR_ON_SURFACE_2);
}

int panel_click(int x, int y) {
    if (!g_panel.visible) return 0;
    if (y < 0 || y >= g_panel.height) return 0;

    int pw = g_panel.screen_w;

    /* Left zone — command palette */
    if (x < PANEL_LEFT_W) {
        palette_toggle();
        return 1;
    }

    /* Right zone — notifications (placeholder) */
    if (x >= pw - PANEL_RIGHT_W) {
        return 3;
    }

    /* Center zone — pill hit test */
    for (int i = 0; i < g_panel.pill_count; i++) {
        panel_pill_t *p = &g_panel.pills[i];
        if (x >= p->x && x < p->x + p->w) {
            wm_focus_surface(p->surface_id);
            /* Also restore if minimized */
            chain_surface_t *s = wm_get_surface(p->surface_id);
            if (s && s->state == SURFACE_MINIMIZED)
                wm_restore_surface(p->surface_id);
            return 2;
        }
    }

    return 0;
}

void panel_set_persona(int persona) {
    g_panel.persona = persona;
    g_panel.persona_color = persona_accent(persona);
}

int panel_get_height(void) {
    return g_panel.height;
}
