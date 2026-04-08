/*
 * Zeos — Notification System
 *
 * Batched notifications with spring-animated toasts.
 * See notify.h for full specification.
 */

#include "notify.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "anim.h"
#include "timer.h"
#include "access.h"
#include "compositor.h"
#include "kprint.h"

/* ── Local string helpers (bare-metal, no libc) ── */

static void n_strncpy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void n_memset(void *dst, uint8_t val, uint32_t n) {
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++)
        p[i] = val;
}

/* ── Constants ── */

#define TOAST_W          300
#define TOAST_H           64
#define TOAST_MARGIN      12    /* From screen edge */
#define TOAST_BORDER_W     4    /* Colored left border */
#define TOAST_PADDING     12    /* Internal padding */
#define TOAST_Y_OFFSET    56    /* Below panel bar */

/* Batch interval: 30 seconds for alpha testing.
   Production: 15-30 minutes (900-1800 seconds).
   Change BATCH_INTERVAL_SEC when shipping. */
#define BATCH_INTERVAL_SEC  30

/* Auto-dismiss after 5 seconds */
#define DISMISS_SEC          5

/* Panel list constants */
#define PANEL_W           320
#define PANEL_ITEM_H       52
#define PANEL_HEADER_H     36
#define PANEL_MAX_VISIBLE   8

/* ── Global state ── */

static notify_state_t g_notify;

/* ── Helpers ── */

static uint32_t level_color(notify_level_t level) {
    switch (level) {
    case NOTIFY_INFO:     return COLOR_PRIMARY;     /* Accent */
    case NOTIFY_SUCCESS:  return COLOR_SUCCESS;     /* Green */
    case NOTIFY_WARNING:  return COLOR_WARNING;     /* Amber */
    case NOTIFY_ERROR:    return COLOR_DANGER;      /* Red */
    case NOTIFY_CRITICAL: return COLOR_DANGER;      /* Red */
    default:              return COLOR_PRIMARY;
    }
}

static int focus_active(void) {
    access_config_t *cfg = access_get();
    return cfg ? cfg->focus_mode : 0;
}

static uint64_t tsc_to_sec(uint64_t tsc_delta) {
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) return 0;
    return tsc_delta / freq;
}

/* Spring callback: update slide_x on the notification */
static void slide_cb(int anim_id, float position, void *ctx) {
    (void)anim_id;
    notification_t *n = (notification_t *)ctx;
    n->slide_x = position;
}

static void show_notification(int idx) {
    if (idx < 0 || idx >= g_notify.count) return;
    notification_t *n = &g_notify.queue[idx];

    /* Already visible */
    if (n->visible) return;

    /* Cancel any existing animation */
    if (n->anim_id >= 0 && anim_is_active(n->anim_id))
        anim_cancel(n->anim_id);

    /* Spring slide in from right: start off-screen, animate to 0 */
    n->slide_x = (float)(TOAST_W + TOAST_MARGIN);
    n->anim_id = anim_spring(
        (float)(TOAST_W + TOAST_MARGIN), 0.0f,
        SPRING_SMOOTH_S, SPRING_SMOOTH_D,
        slide_cb, n
    );

    n->visible = 1;
    n->read = 1;
    g_notify.showing = 1;
    g_notify.show_index = idx;
    g_notify.show_start_tsc = timer_read_tsc();

    if (g_notify.unread > 0)
        g_notify.unread--;

    compositor_dirty_all();
}

static void hide_notification(int idx) {
    if (idx < 0 || idx >= g_notify.count) return;
    notification_t *n = &g_notify.queue[idx];

    if (!n->visible) return;

    /* Cancel any existing animation */
    if (n->anim_id >= 0 && anim_is_active(n->anim_id))
        anim_cancel(n->anim_id);

    /* Spring slide out to right */
    n->anim_id = anim_spring(
        n->slide_x, (float)(TOAST_W + TOAST_MARGIN),
        SPRING_SMOOTH_S, SPRING_SMOOTH_D,
        slide_cb, n
    );

    n->visible = 0;
    g_notify.showing = 0;

    compositor_dirty_all();
}

/* Find next unread notification index, or -1 */
static int next_unread(void) {
    for (int i = 0; i < g_notify.count; i++) {
        if (!g_notify.queue[i].read)
            return i;
    }
    return -1;
}

/* ── Public API ── */

void notify_init(void) {
    n_memset(&g_notify, 0, sizeof(g_notify));
    g_notify.batch_mode = 1;    /* Batch by default */
    g_notify.last_batch_tsc = timer_read_tsc();
    g_notify.show_index = -1;

    /* Initialize anim_id for all slots */
    for (int i = 0; i < NOTIFY_MAX; i++)
        g_notify.queue[i].anim_id = -1;

    kputs("NOTIFY: initialized (batch mode, ");
    kput_dec(BATCH_INTERVAL_SEC);
    kputs("s interval)\n");
}

void notify_send(const char *text, const char *source, notify_level_t level) {
    /* If queue is full, drop oldest read notification */
    if (g_notify.count >= NOTIFY_MAX) {
        /* Shift everything down, dropping index 0 */
        for (int i = 1; i < NOTIFY_MAX; i++)
            g_notify.queue[i - 1] = g_notify.queue[i];
        g_notify.count = NOTIFY_MAX - 1;
        /* Recalculate unread */
        g_notify.unread = 0;
        for (int i = 0; i < g_notify.count; i++) {
            if (!g_notify.queue[i].read)
                g_notify.unread++;
        }
    }

    int idx = g_notify.count;
    notification_t *n = &g_notify.queue[idx];
    n_memset(n, 0, sizeof(*n));

    n_strncpy(n->text, text, NOTIFY_MAX_TEXT);
    n_strncpy(n->source, source ? source : "system", 32);
    n->level = level;
    n->timestamp = timer_read_tsc();
    n->read = 0;
    n->visible = 0;
    n->slide_x = (float)(TOAST_W + TOAST_MARGIN);
    n->anim_id = -1;

    g_notify.count++;
    g_notify.unread++;

    /* CRITICAL bypasses everything -- show immediately */
    if (level == NOTIFY_CRITICAL) {
        show_notification(idx);
        return;
    }

    /* Immediate mode: show right away (if not in Focus Mode) */
    if (!g_notify.batch_mode) {
        if (!focus_active())
            show_notification(idx);
    }

    /* Batch mode: will be shown by notify_tick() when interval elapses */
}

void notify_tick(void) {
    uint64_t now = timer_read_tsc();

    /* ── Auto-dismiss current toast after DISMISS_SEC ── */
    if (g_notify.showing && g_notify.show_index >= 0) {
        uint64_t elapsed = tsc_to_sec(now - g_notify.show_start_tsc);
        if (elapsed >= DISMISS_SEC) {
            hide_notification(g_notify.show_index);
        }
    }

    /* ── Batch mode: show next unread when interval elapses ── */
    if (g_notify.batch_mode && g_notify.unread > 0 && !g_notify.showing) {
        uint64_t batch_elapsed = tsc_to_sec(now - g_notify.last_batch_tsc);
        if (batch_elapsed >= BATCH_INTERVAL_SEC) {
            g_notify.last_batch_tsc = now;

            /* Focus Mode: suppress non-critical */
            int idx = next_unread();
            if (idx >= 0) {
                if (focus_active() && g_notify.queue[idx].level != NOTIFY_CRITICAL) {
                    /* Suppressed -- skip, try again next interval */
                    return;
                }
                show_notification(idx);
            }
        }
    }
}

void notify_draw(void) {
    int screen_w = (int)fb_width();

    /* ── Draw active toast ── */
    if (g_notify.show_index >= 0 && g_notify.show_index < g_notify.count) {
        notification_t *n = &g_notify.queue[g_notify.show_index];

        /* Only draw if animation is running or notification is visible */
        int anim_active = (n->anim_id >= 0 && anim_is_active(n->anim_id));
        if (!n->visible && !anim_active)
            goto draw_panel;

        int base_x = screen_w - TOAST_W - TOAST_MARGIN;
        int toast_x = base_x + (int)n->slide_x;
        int toast_y = TOAST_Y_OFFSET;

        /* Background */
        fb_rect_blend(toast_x, toast_y, TOAST_W, TOAST_H, COLOR_SURFACE_HIGH);

        /* Level-colored left border */
        uint32_t border_color = level_color(n->level);
        fb_rect(toast_x, toast_y, TOAST_BORDER_W, TOAST_H, border_color);

        /* Source name (tertiary text, small) */
        int text_x = toast_x + TOAST_BORDER_W + TOAST_PADDING;
        int text_y = toast_y + TOAST_PADDING;
        uint32_t src_color = (COLOR_ON_SURFACE_3 & 0x00FFFFFF) | ((uint32_t)TEXT_TERTIARY << 24);
        font_draw(text_x, text_y, n->source, FONT_UI, TYPE_CAPTION, src_color);

        /* Message text (primary text) */
        int msg_y = text_y + TYPE_CAPTION + 6;
        int max_text_w = TOAST_W - TOAST_BORDER_W - TOAST_PADDING * 2;
        (void)max_text_w; /* TODO: text truncation */
        uint32_t msg_color = (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_PRIMARY << 24);
        font_draw(text_x, msg_y, n->text, FONT_UI, TYPE_LABEL, msg_color);
    }

draw_panel:
    /* ── Draw notification list panel (if open) ── */
    if (!g_notify.panel_open) return;

    int panel_x = screen_w - PANEL_W - TOAST_MARGIN;
    int panel_y = TOAST_Y_OFFSET;
    int item_count = g_notify.count;
    if (item_count > PANEL_MAX_VISIBLE)
        item_count = PANEL_MAX_VISIBLE;
    int panel_h = PANEL_HEADER_H + item_count * PANEL_ITEM_H;

    /* Panel background */
    fb_rect_blend(panel_x, panel_y, PANEL_W, panel_h, COLOR_SURFACE_HIGH);

    /* Header */
    uint32_t hdr_color = (COLOR_ON_SURFACE & 0x00FFFFFF) | ((uint32_t)TEXT_PRIMARY << 24);
    font_draw(panel_x + TOAST_PADDING, panel_y + 8,
              "Notifications", FONT_UI_BOLD, TYPE_LABEL, hdr_color);

    /* Separator */
    fb_hline(panel_x, panel_y + PANEL_HEADER_H - 1, PANEL_W, COLOR_SEPARATOR);

    /* Notification items (newest first) */
    int y = panel_y + PANEL_HEADER_H;
    int drawn = 0;
    for (int i = g_notify.count - 1; i >= 0 && drawn < PANEL_MAX_VISIBLE; i--, drawn++) {
        notification_t *n = &g_notify.queue[i];

        /* Unread indicator: level-colored left border */
        if (!n->read) {
            fb_rect(panel_x, y, TOAST_BORDER_W, PANEL_ITEM_H, level_color(n->level));
        }

        /* Source */
        uint32_t src_color = (COLOR_ON_SURFACE_3 & 0x00FFFFFF) | ((uint32_t)TEXT_TERTIARY << 24);
        font_draw(panel_x + TOAST_BORDER_W + TOAST_PADDING, y + 6,
                  n->source, FONT_UI, TYPE_MICRO, src_color);

        /* Message */
        uint32_t msg_color = (COLOR_ON_SURFACE & 0x00FFFFFF) |
                             ((uint32_t)(n->read ? TEXT_SECONDARY : TEXT_PRIMARY) << 24);
        font_draw(panel_x + TOAST_BORDER_W + TOAST_PADDING, y + 6 + TYPE_MICRO + 4,
                  n->text, FONT_UI, TYPE_CAPTION, msg_color);

        /* Separator */
        fb_hline(panel_x, y + PANEL_ITEM_H - 1, PANEL_W, COLOR_SEPARATOR);

        y += PANEL_ITEM_H;
    }
}

void notify_dismiss(void) {
    if (g_notify.showing && g_notify.show_index >= 0) {
        hide_notification(g_notify.show_index);
    }
}

void notify_show_all(void) {
    g_notify.panel_open = !g_notify.panel_open;

    /* Mark all as read when panel is opened */
    if (g_notify.panel_open) {
        for (int i = 0; i < g_notify.count; i++)
            g_notify.queue[i].read = 1;
        g_notify.unread = 0;
    }

    compositor_dirty_all();
}

int notify_unread_count(void) {
    return g_notify.unread;
}
