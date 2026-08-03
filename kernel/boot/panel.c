/*
 * Zeos — Panel (Top Bar)
 *
 * Three-zone rendering: persona | chain pills | health + clock.
 * Refreshed each frame. No heap allocations — everything is static.
 */

#include "panel.h"
#include "fb.h"
#include "compositor.h"
#include "wm.h"
#include "workspaces.h"
#include "chain.h"
#include "font.h"
#include "theme.h"
#include "persona.h"
#include "access.h"
#include "timer.h"
#include "palette.h"
#include "kprint.h"
#include "persona_filter.h"
#include "ui_hover.h"
#include "ui_context_menu.h"

/* ── UI primitive wiring ── */
#define PANEL_MAX_HOVERS  (PANEL_MAX_PILLS + 4)
static uint64_t s_panel_tokens[PANEL_MAX_HOVERS];
static int      s_panel_token_count = 0;

/* Panel state (declared early: panel_right_click hit-tests pills). */
static panel_state_t g_panel;

static void rc_settings(void *ctx) { (void)ctx; kputs("PANEL: open settings\n"); }
static void rc_about(void *ctx)    { (void)ctx; kputs("PANEL: about Zeos\n"); }
static void rc_activity(void *ctx) {
    (void)ctx;
    extern int activity_open(void);
    (void)activity_open();
}

/* D.6: per-pill right-click target + actions (act on the pill's window). */
static int s_rc_pill_surface = -1;
static void rc_pill_focus(void *ctx)    { (void)ctx; if (s_rc_pill_surface >= 0) { wm_focus_surface(s_rc_pill_surface); wm_restore_surface(s_rc_pill_surface); } }
static void rc_pill_minimize(void *ctx) { (void)ctx; if (s_rc_pill_surface >= 0) wm_minimize_surface(s_rc_pill_surface); }
static void rc_pill_close(void *ctx)    { (void)ctx; if (s_rc_pill_surface >= 0) wm_detach_surface(s_rc_pill_surface); }

int panel_right_click(int x, int y) {
    extern int panel_get_height(void);
    if (y < 0 || y >= panel_get_height()) return 0;

    /* D.6: right-click on a center-zone pill -> per-window menu. */
    for (int i = 0; i < g_panel.pill_count; i++) {
        panel_pill_t *p = &g_panel.pills[i];
        if (p->w > 0 && x >= p->x && x < p->x + p->w) {
            s_rc_pill_surface = p->surface_id;
            static const ctx_menu_item_t pill_items[3] = {
                { "Focus",    rc_pill_focus,    0, 1 },
                { "Minimize", rc_pill_minimize, 0, 1 },
                { "Close",    rc_pill_close,    0, 1 },
            };
            context_menu_open(x, y, pill_items, 3);
            return 1;
        }
    }

    static const ctx_menu_item_t items[3] = {
        { "Activity Monitor", rc_activity, 0, 1 },
        { "Settings",         rc_settings, 0, 1 },
        { "About Zeos",       rc_about,    0, 1 },
    };
    context_menu_open(x, y, items, 3);
    return 1;
}

/* ── Static state ── */
static int s_ws_indicator_x = 0;   /* set by panel_draw, used by panel_click */
static int s_ws_indicator_y = 0;
static int s_ws_indicator_w = 0;

/* ── Helpers ── */

static void str_copy_n(char *d, const char *s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static uint32_t persona_accent(int persona) {
    uint32_t c;
    switch (persona) {
    case PERSONA_ZEROS: c = COLOR_ZEROS_ACCENT; break;
    case PERSONA_DEREZ: c = COLOR_DEREZ_ACCENT; break;
    default:            c = COLOR_FULL_ACCENT;  break;
    }
    /* M.4: mute (LOW_STIMULI) / boost (HIGH_CONTRAST) per active sensory mode. */
    return access_apply_sensory(c);
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

void panel_set_height(int h) { if (h < 16) h = 16; g_panel.height = h; }

/* D.6 auto-hide reveal zone: pointer within this many px of the top edge
 * reveals the panel; moving below the panel hides it again. */
#define PANEL_REVEAL_ZONE 2

void panel_set_auto_hide(int on) {
    g_panel.auto_hide = on ? 1 : 0;
    /* Enabling hides immediately (until the pointer reaches the top edge);
     * disabling always shows. */
    g_panel.visible = on ? 0 : 1;
}

int panel_get_auto_hide(void) { return g_panel.auto_hide; }

int panel_pointer_y(int y) {
    if (!g_panel.auto_hide) return 0;
    int want = g_panel.visible;
    if (y <= PANEL_REVEAL_ZONE)          want = 1;   /* at top edge -> reveal */
    else if (y > g_panel.height)         want = 0;   /* left the bar -> hide */
    if (want != g_panel.visible) { g_panel.visible = want; return 1; }
    return 0;
}

void panel_init(int persona, int height) {
    g_panel.height = height;
    g_panel.visible = 1;
    g_panel.auto_hide = 0;
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

        /* MasQ persona filter: skip chains beyond perception */
        if (s->chain_id >= 0 && !persona_can_see(s->chain_id)) continue;

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

    /* ── Update clock: REAL wall-clock (tod / CMOS RTC via tod_init) when
     *    available, else fall back to TSC uptime so the field is never dead. ── */
    {
        extern uint64_t tod_now_unix(void);   /* Unix epoch secs; 0 if tod not ready */
        int hours, minutes;
        uint64_t epoch = tod_now_unix();
        if (epoch > 0) {
            uint64_t sod = epoch % 86400ULL;          /* seconds into the UTC day */
            hours   = (int)(sod / 3600);
            minutes = (int)((sod / 60) % 60);
        } else {
            uint64_t freq = timer_tsc_freq();
            uint64_t seconds = freq ? (timer_read_tsc() / freq) : 0;
            hours   = (int)((seconds / 3600) % 24);
            minutes = (int)((seconds / 60) % 60);
        }
        char prev[16];
        str_copy_n(prev, g_panel.clock_str, sizeof(prev));
        format_clock(g_panel.clock_str, hours, minutes);
        /* Request a composite only when the displayed time changes, so the
         * clock stays live under the producer peek-gate without recompositing
         * every tick. */
        int changed = 0;
        for (int i = 0; i < (int)sizeof(prev); i++) {
            if (prev[i] != g_panel.clock_str[i]) { changed = 1; break; }
            if (prev[i] == 0) break;
        }
        if (changed) {
            extern void compositor_dirty(int, int, int, int);
            compositor_dirty(0, 0, g_panel.screen_w,
                             compositor_get_state()->panel_h);
        }
    }
}

/* Left-click on the top panel bar: consume it (so it doesn't fall through to a
 * window behind the bar). Status-item handlers get wired here later. */
int panel_left_click(int x, int y) {
    (void)x;
    compositor_t *comp = compositor_get_state();
    if (!comp->panel_visible) return 0;
    if (y >= comp->panel_h)   return 0;
    return 1;
}

void panel_draw(void) {
    if (!g_panel.visible) return;

    int pw = g_panel.screen_w;
    int ph = g_panel.height;

    /* ── Panel background (D.6 vibrancy) ── */
    /* Translucent fill so the wallpaper/windows beneath bleed through subtly
     * (frosted panel), instead of a flat opaque bar. Panel paints after the
     * desktop + windows in the composite order, so the blend picks up whatever
     * is under it. ~90% surface over ~10% content. */
    fb_rect_blend(0, 0, pw, ph, 0xE6161B22u);

    /* ── Bottom separator ── */
    fb_hline(0, ph - 1, pw, COLOR_SEPARATOR);

    /* Refresh hover zones every draw. */
    for (int i = 0; i < s_panel_token_count; i++) hover_unregister(s_panel_tokens[i]);
    s_panel_token_count = 0;
    {
        uint64_t tok = hover_register(0, 0, PANEL_LEFT_W, ph,
                                      HOVER_CURSOR_POINTER, 0, 0);
        if (tok && s_panel_token_count < PANEL_MAX_HOVERS)
            s_panel_tokens[s_panel_token_count++] = tok;
        tok = hover_register(pw - PANEL_RIGHT_W, 0, PANEL_RIGHT_W, ph,
                             HOVER_CURSOR_POINTER, 0, 0);
        if (tok && s_panel_token_count < PANEL_MAX_HOVERS)
            s_panel_tokens[s_panel_token_count++] = tok;
    }

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

        /* Measure text to size the pill. Width tracks the content: left pad +
         * status dot + gap + text + right pad. (The +14 is the dot+gap the
         * text is shifted by; without it the label runs to the pill edge.) */
        int text_w = font_measure(p->name, FONT_UI, TYPE_CAPTION);
        int pill_w = text_w + PANEL_PILL_HPAD * 2 + 14;

        /* Overflow clip: stop if pill would exceed center zone */
        if (cx + pill_w > center_end) break;

        /* Store position for hit testing */
        p->x = cx;
        p->w = pill_w;

        /* Hover zone for the pill (cursor → hand). */
        if (s_panel_token_count < PANEL_MAX_HOVERS) {
            uint64_t ptok = hover_register(cx, pill_y, pill_w, pill_h,
                                           HOVER_CURSOR_POINTER, 0, 0);
            if (ptok) s_panel_tokens[s_panel_token_count++] = ptok;
        }

        /* Pill background is a dim elevated surface so the NAME stays readable.
         * Status is shown by a colored status dot + (when focused) an accent
         * underline -- not by flooding the pill with the accent color, which
         * made the same-color text invisible. */
        chain_surface_t *surf = wm_get_surface(p->surface_id);
        uint32_t bg = (surf && surf->focused) ? COLOR_SURFACE_TOP : COLOR_SURFACE_HIGH;
        uint32_t fg = (p->status == CHAIN_ERROR) ? COLOR_DANGER : COLOR_ON_SURFACE;
        uint32_t dot = (p->status == CHAIN_LIVE)  ? COLOR_SUCCESS
                     : (p->status == CHAIN_ERROR) ? COLOR_DANGER
                     : COLOR_ON_SURFACE_4;

        draw_rounded_rect(cx, pill_y, pill_w, pill_h, pill_r, bg);

        /* Status dot at the left of the pill */
        fb_circle_filled(cx + PANEL_PILL_HPAD + 3,
                         pill_y + pill_h / 2, 3, dot);

        /* Focused: accent underline */
        if (surf && surf->focused) {
            fb_hline(cx + pill_r, pill_y + pill_h - 2,
                     pill_w - 2 * pill_r, g_panel.persona_color);
        }

        /* Pill text (shifted right to clear the status dot) */
        font_draw(cx + PANEL_PILL_HPAD + 12,
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

    /* Workspace indicator dots — positioned just right of the health
     * dot. Click switches workspaces. */
    s_ws_indicator_x = rz_x + 28;
    s_ws_indicator_y = ry_center;
    s_ws_indicator_w = workspaces_draw_indicator(s_ws_indicator_x, ry_center);

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

    /* Privacy overlay: camera-in-use red dot. Painted last so it sits
     * on top of any right-zone glyph that might overlap. */
    extern void panel_overlay_draw_camera_indicator(void);
    panel_overlay_draw_camera_indicator();
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
        /* Workspace indicator hit-test wins over generic right-zone. */
        int ws_hit = workspaces_indicator_hit(x, y, s_ws_indicator_x,
                                               s_ws_indicator_y);
        if (ws_hit >= 0) {
            workspace_switch(ws_hit);
            return 4;
        }
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

/* ── Privacy: camera-in-use indicator ──────────────────────────
 * A red dot drawn next to the system-health dot whenever any UVC
 * stream is active. Cleared when streaming stops. The flag is set
 * by usb_uvc_start / usb_uvc_stop. */
static int s_camera_active = 0;

void panel_indicator_camera(int active)
{
    s_camera_active = active ? 1 : 0;
}

/* Called from panel_draw via post-draw hook: panel_draw renders the
 * three zones and then this overlay paints a red dot if a camera is
 * live. The hook is invoked at the tail of panel_draw via the
 * panel_overlay_draw_camera_indicator call we add into the right-zone
 * pass. To avoid touching the existing draw flow, we expose this as
 * a stand-alone painter that callers (compositor / panel render
 * subscriber) invoke after panel_draw. */
void panel_overlay_draw_camera_indicator(void)
{
    if (!s_camera_active || !g_panel.visible) return;
    int pw = g_panel.screen_w;
    int ph = g_panel.height;
    int rz_x = pw - PANEL_RIGHT_W;
    int ry_center = ph / 2;
    /* Draw a 4-px red dot just to the left of the health dot. */
    fb_circle_filled(rz_x + 6, ry_center, 4, COLOR_DANGER);
}

#ifdef ZEOS_DIAG_D6
/* D.6 selftest: panel auto-hide. Enable -> panel hides; pointer at the top edge
 * (y<=REVEAL) reveals it; pointer below the bar hides it again; disable -> always
 * visible. (Vibrancy = translucent bg, observed via screendump; per-pill right-
 * click already verified id=416.) */
void panel_d6_selftest(void)
{
    int saved_ah = g_panel.auto_hide, saved_vis = g_panel.visible;

    panel_set_auto_hide(1);
    int hid_on_enable = (g_panel.visible == 0);
    int revealed = 0, rehid = 0;
    (void)panel_pointer_y(0);                 int r1 = g_panel.visible;   /* top edge -> reveal */
    revealed = (r1 == 1);
    (void)panel_pointer_y(g_panel.height + 50); int r2 = g_panel.visible; /* below bar -> hide */
    rehid = (r2 == 0);
    panel_set_auto_hide(0);
    int shown_on_disable = (g_panel.visible == 1);

    /* restore */
    g_panel.auto_hide = saved_ah; g_panel.visible = saved_vis;

    int pass = hid_on_enable && revealed && rehid && shown_on_disable;
    kputs("[D6] hide_on_enable="); kput_dec((uint64_t)hid_on_enable);
    kputs(" reveal_top="); kput_dec((uint64_t)revealed);
    kputs(" hide_below="); kput_dec((uint64_t)rehid);
    kputs(" show_on_disable="); kput_dec((uint64_t)shown_on_disable);
    kputs(pass ? " -> PASS (vibrancy: screendump; per-pill: id=416)\n" : " -> FAIL\n");
}
#endif
