/*
 * Zeos — Notification System
 *
 * Batched notifications with spring-animated toasts.
 * See notify.h for full specification.
 *
 * History: 256-entry ring persisted to VAULT, replayable across reboots.
 * DND: off / on / scheduled (start_hr..end_hr UTC). Critical bypasses.
 * Per-source mute list. All decisions go through notify_should_silence().
 */

#include <stddef.h>   /* NULL — was arriving by accident via fb.h -> zeos_boot.h -> efi.h */
#include "notify.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "anim.h"
#include "timer.h"
#include "access.h"
#include "chain.h"
#include "chain_registry.h"
#include "compositor.h"
#include "kprint.h"
#include "vault.h"
#include "timeofday.h"
#include "settings_registry.h"

/* ── Local string helpers (bare-metal, no libc) ── */

static void n_strncpy(char *dst, const char *src, int max) {
    int i = 0;
    if (max <= 0) return;
    while (i < max - 1 && src && src[i]) {
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

static void n_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static int n_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Skip ASCII spaces */
static const char *n_skip_ws(const char *s) {
    while (s && *s == ' ') s++;
    return s;
}

/* Parse a non-negative integer, return -1 on no digits */
static int n_parse_int(const char *s, const char **endp) {
    if (!s) { if (endp) *endp = s; return -1; }
    int v = 0; int any = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
        any = 1;
    }
    if (endp) *endp = s;
    return any ? v : -1;
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

/* VAULT keys */
#define NOTIFY_VAULT_DIR        "/notify"
#define NOTIFY_HISTORY_PATH     "/notify/history"
#define NOTIFY_DND_MODE_PATH    "/notify/dnd-mode"
#define NOTIFY_DND_START_PATH   "/notify/dnd-start-hour"
#define NOTIFY_DND_END_PATH     "/notify/dnd-end-hour"
#define NOTIFY_MUTE_LIST_PATH   "/notify/mute-list"

/* History ring on-disk header. The file is laid out as:
 *   [history_header_t][NOTIFY_HISTORY_CAP * notification_record_t]
 * `head` is the index of the next slot to overwrite (oldest entry is at
 * `head` once the ring is full). `count` is how many slots are used. */
#define NOTIFY_HISTORY_MAGIC 0x4E484953u     /* 'NHIS' */
#define NOTIFY_HISTORY_VER   1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t head;     /* next slot to write */
    uint32_t count;    /* 0..NOTIFY_HISTORY_CAP */
} history_header_t;

/* ── Global state ── */

static notify_state_t g_notify;
static int            g_initialized = 0;

/* History ring (newest written at head-1) */
static notification_record_t g_history[NOTIFY_HISTORY_CAP];
static uint32_t              g_history_head;
static uint32_t              g_history_count;

/* DND state */
static dnd_mode_t g_dnd_mode      = DND_OFF;
static int        g_dnd_start_hr  = 22;   /* default 22:00 */
static int        g_dnd_end_hr    =  7;   /* default 07:00 */

/* Per-source mute (whitelist that BYPASSES DND) */
static char g_mute[NOTIFY_MUTE_MAX][NOTIFY_SOURCE_MAX];
static int  g_mute_count;

/* ── Forward decls ── */

static void history_persist(void);
static void history_load(void);
static void dnd_load(void);
static void mute_load(void);
static void mute_persist(void);
static void register_settings(void);

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

static const char *level_label(notify_level_t lvl) {
    switch (lvl) {
    case NOTIFY_INFO:     return "INFO ";
    case NOTIFY_SUCCESS:  return "OK   ";
    case NOTIFY_WARNING:  return "WARN ";
    case NOTIFY_ERROR:    return "ERROR";
    case NOTIFY_CRITICAL: return "CRIT ";
    default:              return "?    ";
    }
}

static int focus_active(void) {
    access_config_t *cfg = access_get();
    return cfg ? cfg->focus_mode : 0;
}

/* M.7: Focus Mode suppresses non-CRITICAL notifications. Extracted so the
 * decision is unit-testable (see notify_m7_selftest). CRITICAL always passes. */
int notify_focus_suppresses(int level) {
    return focus_active() && level != NOTIFY_CRITICAL;
}

static uint64_t tsc_to_sec(uint64_t tsc_delta) {
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) return 0;
    return tsc_delta / freq;
}

/* Lazy init guard so notify_send / notify_tick / shell commands work even
 * if main.c hasn't called notify_init explicitly. */
static void ensure_init(void) {
    if (!g_initialized) notify_init();
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

/* ── History ring ── */

static void history_append(const notification_record_t *rec) {
    /* Append in RAM */
    g_history[g_history_head] = *rec;
    g_history_head = (g_history_head + 1) % NOTIFY_HISTORY_CAP;
    if (g_history_count < NOTIFY_HISTORY_CAP) g_history_count++;

    /* Persist whole ring (size = header + cap * record ~= ~120 KiB max
     * but typical record is 220 bytes => ~56 KiB; vault inode caps at
     * ~48 KiB direct so we tolerate a partial-tail truncation). */
    history_persist();
}

static void history_persist(void) {
    /* Build a flat buffer: header followed by `count` records in
     * chronological order so the on-disk layout is independent of the
     * RAM ring's head pointer. On reload, head=count, ring linear. */
    static uint8_t buf[sizeof(history_header_t) +
                       NOTIFY_HISTORY_CAP * sizeof(notification_record_t)];

    history_header_t hdr;
    hdr.magic   = NOTIFY_HISTORY_MAGIC;
    hdr.version = NOTIFY_HISTORY_VER;
    hdr.head    = g_history_count;   /* linear after compaction */
    hdr.count   = g_history_count;

    n_memcpy(buf, &hdr, sizeof(hdr));

    /* Compact records oldest-first into buf. */
    uint32_t start = (g_history_count == NOTIFY_HISTORY_CAP)
                   ? g_history_head
                   : 0;
    for (uint32_t i = 0; i < g_history_count; i++) {
        uint32_t src_idx = (start + i) % NOTIFY_HISTORY_CAP;
        n_memcpy(buf + sizeof(hdr) + i * sizeof(notification_record_t),
                 &g_history[src_idx],
                 sizeof(notification_record_t));
    }

    uint32_t total = sizeof(hdr) +
                     g_history_count * (uint32_t)sizeof(notification_record_t);

    (void)vault_write(NOTIFY_HISTORY_PATH, buf, total);
}

static void history_load(void) {
    g_history_head = 0;
    g_history_count = 0;
    n_memset(g_history, 0, sizeof(g_history));

    int sz = vault_size(NOTIFY_HISTORY_PATH);
    if (sz < (int)sizeof(history_header_t)) return;

    static uint8_t buf[sizeof(history_header_t) +
                       NOTIFY_HISTORY_CAP * sizeof(notification_record_t)];
    uint32_t to_read = (uint32_t)sz;
    if (to_read > sizeof(buf)) to_read = sizeof(buf);

    int got = vault_read(NOTIFY_HISTORY_PATH, buf, to_read);
    if (got < (int)sizeof(history_header_t)) return;

    history_header_t *hdr = (history_header_t *)buf;
    if (hdr->magic   != NOTIFY_HISTORY_MAGIC)  return;
    if (hdr->version != NOTIFY_HISTORY_VER)    return;

    uint32_t n = hdr->count;
    if (n > NOTIFY_HISTORY_CAP) n = NOTIFY_HISTORY_CAP;

    uint32_t expected = sizeof(history_header_t) +
                        n * (uint32_t)sizeof(notification_record_t);
    if ((uint32_t)got < expected) {
        /* truncated tail; keep what we can */
        n = (got - sizeof(history_header_t)) / sizeof(notification_record_t);
    }

    for (uint32_t i = 0; i < n; i++) {
        n_memcpy(&g_history[i],
                 buf + sizeof(history_header_t) + i * sizeof(notification_record_t),
                 sizeof(notification_record_t));
    }
    g_history_count = n;
    g_history_head  = n % NOTIFY_HISTORY_CAP;
}

/* ── DND state load/save ── */

static void dnd_load_int(const char *path, int *out, int lo, int hi) {
    int sz = vault_size(path);
    if (sz <= 0 || sz > 16) return;
    char buf[17];
    int got = vault_read(path, buf, sizeof(buf) - 1);
    if (got <= 0) return;
    buf[got] = '\0';
    const char *p = buf;
    int v = n_parse_int(p, NULL);
    if (v < lo || v > hi) return;
    *out = v;
}

static void dnd_save_int(const char *path, int v) {
    char buf[16];
    int p = 0;
    if (v == 0) buf[p++] = '0';
    else {
        char tmp[16]; int t = 0;
        if (v < 0) { buf[p++] = '-'; v = -v; }
        while (v > 0 && t < (int)sizeof(tmp)) { tmp[t++] = '0' + (v % 10); v /= 10; }
        while (t > 0) buf[p++] = tmp[--t];
    }
    buf[p] = '\0';
    (void)vault_write(path, buf, (uint32_t)p);
}

static void dnd_load(void) {
    int mode = (int)g_dnd_mode;
    dnd_load_int(NOTIFY_DND_MODE_PATH, &mode, 0, 2);
    g_dnd_mode = (dnd_mode_t)mode;
    dnd_load_int(NOTIFY_DND_START_PATH, &g_dnd_start_hr, 0, 23);
    dnd_load_int(NOTIFY_DND_END_PATH,   &g_dnd_end_hr,   0, 23);
}

/* ── Mute list load/save ── */

static void mute_persist(void) {
    /* Newline-separated source names. */
    char buf[NOTIFY_MUTE_MAX * (NOTIFY_SOURCE_MAX + 1) + 1];
    int p = 0;
    for (int i = 0; i < g_mute_count; i++) {
        for (int j = 0; g_mute[i][j] && j < NOTIFY_SOURCE_MAX; j++)
            buf[p++] = g_mute[i][j];
        buf[p++] = '\n';
    }
    buf[p] = '\0';
    (void)vault_write(NOTIFY_MUTE_LIST_PATH, buf, (uint32_t)p);
}

static void mute_load(void) {
    g_mute_count = 0;
    n_memset(g_mute, 0, sizeof(g_mute));

    int sz = vault_size(NOTIFY_MUTE_LIST_PATH);
    if (sz <= 0) return;

    char buf[NOTIFY_MUTE_MAX * (NOTIFY_SOURCE_MAX + 1) + 1];
    uint32_t to_read = (uint32_t)sz;
    if (to_read > sizeof(buf) - 1) to_read = sizeof(buf) - 1;

    int got = vault_read(NOTIFY_MUTE_LIST_PATH, buf, to_read);
    if (got <= 0) return;
    buf[got] = '\0';

    const char *p = buf;
    while (*p && g_mute_count < NOTIFY_MUTE_MAX) {
        int j = 0;
        while (*p && *p != '\n' && j < NOTIFY_SOURCE_MAX - 1) {
            g_mute[g_mute_count][j++] = *p++;
        }
        g_mute[g_mute_count][j] = '\0';
        if (j > 0) g_mute_count++;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

/* ── DND decision ── */

static int hour_in_window(int now_hr, int start_hr, int end_hr) {
    if (start_hr == end_hr) return 0;        /* zero-length window */
    if (start_hr < end_hr)  return now_hr >= start_hr && now_hr < end_hr;
    /* Wrapped: e.g. 22..7 means 22,23,0,1,...,6 */
    return now_hr >= start_hr || now_hr < end_hr;
}

static int dnd_currently_active(void) {
    switch (g_dnd_mode) {
    case DND_OFF:      return 0;
    case DND_ON:       return 1;
    case DND_SCHEDULE: {
        uint64_t now = tod_now_unix();
        if (now == 0) return 0;   /* no wall clock — fail open */
        int hr = (int)((now / 3600ULL) % 24ULL);
        return hour_in_window(hr, g_dnd_start_hr, g_dnd_end_hr);
    }
    default:           return 0;
    }
}

static int source_is_muted(const char *source) {
    if (!source) return 0;
    for (int i = 0; i < g_mute_count; i++) {
        if (n_streq(g_mute[i], source)) return 1;
    }
    return 0;
}

int notify_should_silence(notify_level_t level, const char *source) {
    if (level == NOTIFY_CRITICAL) return 0;     /* always renders */
    if (!dnd_currently_active())  return 0;
    if (source_is_muted(source))  return 0;     /* whitelisted */
    return 1;
}

/* ── Settings registry getters/setters ── */

static int set_dnd_mode_g(char *out, int max) {
    const char *s = "off";
    if      (g_dnd_mode == DND_ON)       s = "on";
    else if (g_dnd_mode == DND_SCHEDULE) s = "schedule";
    n_strncpy(out, s, max);
    return 0;
}
static int set_dnd_mode_s(const char *v) {
    if      (n_streq(v, "off"))      g_dnd_mode = DND_OFF;
    else if (n_streq(v, "on"))       g_dnd_mode = DND_ON;
    else if (n_streq(v, "schedule")) g_dnd_mode = DND_SCHEDULE;
    else return -1;
    dnd_save_int(NOTIFY_DND_MODE_PATH, (int)g_dnd_mode);
    return 0;
}

static void int_to_str(int v, char *out, int max) {
    int p = 0;
    if (max <= 1) { if (max > 0) out[0] = '\0'; return; }
    if (v < 0) { out[p++] = '-'; v = -v; }
    if (v == 0) { out[p++] = '0'; }
    else {
        char tmp[12]; int t = 0;
        while (v > 0 && t < 11) { tmp[t++] = '0' + (v % 10); v /= 10; }
        while (t > 0 && p < max - 1) out[p++] = tmp[--t];
    }
    out[p] = '\0';
}

static int set_dnd_start_g(char *out, int max) {
    int_to_str(g_dnd_start_hr, out, max); return 0;
}
static int set_dnd_start_s(const char *v) {
    int hr = n_parse_int(v, NULL);
    if (hr < 0 || hr > 23) return -1;
    g_dnd_start_hr = hr;
    dnd_save_int(NOTIFY_DND_START_PATH, hr);
    return 0;
}
static int set_dnd_end_g(char *out, int max) {
    int_to_str(g_dnd_end_hr, out, max); return 0;
}
static int set_dnd_end_s(const char *v) {
    int hr = n_parse_int(v, NULL);
    if (hr < 0 || hr > 23) return -1;
    g_dnd_end_hr = hr;
    dnd_save_int(NOTIFY_DND_END_PATH, hr);
    return 0;
}

static const settings_entry_t s_setting_dnd_mode = {
    .name        = "notify.dnd_mode",
    .kind        = SK_ENUM,
    .flags       = 0,
    .getter      = set_dnd_mode_g,
    .setter      = set_dnd_mode_s,
    .enum_labels = "off|on|schedule",
    .desc        = "Do Not Disturb mode (off / on / schedule)",
};
static const settings_entry_t s_setting_dnd_start = {
    .name        = "notify.dnd_start",
    .kind        = SK_INT,
    .flags       = 0,
    .getter      = set_dnd_start_g,
    .setter      = set_dnd_start_s,
    .enum_labels = NULL,
    .desc        = "DND schedule start hour (0-23 UTC)",
};
static const settings_entry_t s_setting_dnd_end = {
    .name        = "notify.dnd_end",
    .kind        = SK_INT,
    .flags       = 0,
    .getter      = set_dnd_end_g,
    .setter      = set_dnd_end_s,
    .enum_labels = NULL,
    .desc        = "DND schedule end hour (0-23 UTC)",
};

static void register_settings(void) {
    (void)settings_register(&s_setting_dnd_mode);
    (void)settings_register(&s_setting_dnd_start);
    (void)settings_register(&s_setting_dnd_end);
}

/* ── Chain pipeline (CHAIN_NOTIFY) ────────────────────────────────
 *
 * Pipeline: notify_emit_request -> dnd_filter -> history_sink -> toast_render
 *
 * Each notify_send() stages a notification_record_t in the chain's
 * pending slot, then calls chain_resolve(CHAIN_NOTIFY). Every node in
 * the pipeline sees the same struct via the scratch buffers; the final
 * node performs the toast queue update (if was_silenced=0).
 */

typedef struct {
    notification_record_t  rec;
    notify_level_t         level;        /* duplicated from rec for fast paths */
    int                    valid;
    int                    queue_idx;    /* which g_notify.queue slot was used */
} notify_chain_state_t;

static notify_chain_state_t g_notify_chain;
static uint32_t             g_notify_chain_total;
static int                  s_chain_notify = -1;

static void notify_emit_request_resolve(chain_node_t *self,
                                        void *input, void *output)
{
    (void)self; (void)input;
    notification_record_t *out = (notification_record_t *)output;
    if (!g_notify_chain.valid) {
        n_memset(out, 0, sizeof(*out));
        return;
    }
    *out = g_notify_chain.rec;
}

static void dnd_filter_resolve(chain_node_t *self,
                               void *input, void *output)
{
    (void)self;
    notification_record_t *in  = (notification_record_t *)input;
    notification_record_t *out = (notification_record_t *)output;
    *out = *in;
    if (!g_notify_chain.valid) return;
    int silenced = notify_should_silence(g_notify_chain.level, in->source);
    out->was_silenced = (uint8_t)(silenced ? 1 : 0);
    g_notify_chain.rec.was_silenced = out->was_silenced;
}

static void history_sink_resolve(chain_node_t *self,
                                 void *input, void *output)
{
    (void)self;
    notification_record_t *in  = (notification_record_t *)input;
    notification_record_t *out = (notification_record_t *)output;
    *out = *in;
    if (!g_notify_chain.valid) return;

    history_append(in);

    /* State change -- bump CHAIN_NOTIFY's vault_version so every
     * notification is timestamped through MasQ. */
    if (s_chain_notify >= 0) {
        chain_t *c = chain_get(s_chain_notify);
        if (c) c->vault_version++;
    }
}

static void toast_render_resolve(chain_node_t *self,
                                 void *input, void *output)
{
    (void)self;
    notification_record_t *in  = (notification_record_t *)input;
    notification_record_t *out = (notification_record_t *)output;
    *out = *in;
    if (!g_notify_chain.valid) {
        return;
    }

    int idx = g_notify_chain.queue_idx;
    if (idx < 0 || idx >= g_notify.count) {
        g_notify_chain.valid = 0;
        return;
    }
    notification_t *n = &g_notify.queue[idx];

    /* CRITICAL bypasses everything -- show immediately. */
    if (g_notify_chain.level == NOTIFY_CRITICAL) {
        show_notification(idx);
        g_notify_chain_total++;
        g_notify_chain.valid = 0;
        return;
    }

    /* Silenced: keep in history, mark queue entry read so batched
     * surfacing skips it later. */
    if (in->was_silenced) {
        n->read = 1;
        if (g_notify.unread > 0) g_notify.unread--;
        g_notify_chain_total++;
        g_notify_chain.valid = 0;
        return;
    }

    /* Immediate mode: render now (unless Focus Mode swallows). Batch
     * mode defers to notify_tick(). */
    if (!g_notify.batch_mode && !focus_active()) {
        show_notification(idx);
    }
    g_notify_chain_total++;
    g_notify_chain.valid = 0;
}

int notify_chain_register(int parent_chain_id)
{
    if (s_chain_notify >= 0) {
        CHAIN_NOTIFY = s_chain_notify;
        return s_chain_notify;
    }
    int id = chain_create("notify", parent_chain_id, MASQ_INTERNAL);
    if (id < 0) return -1;

    chain_add_node(id, "notify_emit_request",
                   "notify_request", "notification_record",
                   notify_emit_request_resolve);
    chain_add_node(id, "dnd_filter",
                   "notification_record", "notification_record",
                   dnd_filter_resolve);
    chain_add_node(id, "history_sink",
                   "notification_record", "notification_record",
                   history_sink_resolve);
    chain_add_node(id, "toast_render",
                   "notification_record", "notification_event",
                   toast_render_resolve);

    s_chain_notify = id;
    CHAIN_NOTIFY = id;
    return id;
}

uint32_t notify_chain_emitted_total(void) { return g_notify_chain_total; }

/* ── Public API ── */

void notify_init(void) {
    if (g_initialized) return;

    n_memset(&g_notify, 0, sizeof(g_notify));
    g_notify.batch_mode = 1;    /* Batch by default */
    g_notify.last_batch_tsc = timer_read_tsc();
    g_notify.show_index = -1;

    /* Initialize anim_id for all slots */
    for (int i = 0; i < NOTIFY_MAX; i++)
        g_notify.queue[i].anim_id = -1;

    /* History + DND + mute load (idempotent: VAULT may be unmounted at
     * very first call; subsequent notify_send paths don't depend on it). */
    (void)vault_mkdir(NOTIFY_VAULT_DIR, VAULT_TIER_INTERNAL);
    history_load();
    dnd_load();
    mute_load();

    /* Surface DND through the unified settings registry. Idempotent
     * because settings_register dedupes by name. */
    register_settings();

    g_initialized = 1;

    kputs("NOTIFY: initialized (batch mode, ");
    kput_dec(BATCH_INTERVAL_SEC);
    kputs("s interval, history=");
    kput_dec(g_history_count);
    kputs(", DND=");
    if      (g_dnd_mode == DND_OFF) kputs("off");
    else if (g_dnd_mode == DND_ON)  kputs("on");
    else                            kputs("schedule");
    kputs(")\n");
}

void notify_send(const char *text, const char *source, notify_level_t level) {
    ensure_init();

    /* If queue is full, drop oldest read notification */
    if (g_notify.count >= NOTIFY_MAX) {
        for (int i = 1; i < NOTIFY_MAX; i++)
            g_notify.queue[i - 1] = g_notify.queue[i];
        g_notify.count = NOTIFY_MAX - 1;
        g_notify.unread = 0;
        for (int i = 0; i < g_notify.count; i++) {
            if (!g_notify.queue[i].read)
                g_notify.unread++;
        }
    }

    int idx = g_notify.count;
    notification_t *n = &g_notify.queue[idx];
    n_memset(n, 0, sizeof(*n));
    n_strncpy(n->text, text ? text : "", NOTIFY_MAX_TEXT);
    n_strncpy(n->source, source ? source : "system", 32);
    n->level = level;
    n->timestamp = timer_read_tsc();
    n->slide_x = (float)(TOAST_W + TOAST_MARGIN);
    n->anim_id = -1;
    g_notify.count++;
    g_notify.unread++;

    /* Build the chain request. dnd_filter sets was_silenced; we leave
     * it 0 here so the chain owns the decision. */
    n_memset(&g_notify_chain.rec, 0, sizeof(g_notify_chain.rec));
    g_notify_chain.rec.tod_unix = tod_now_unix();
    g_notify_chain.rec.level    = (uint32_t)level;
    n_strncpy(g_notify_chain.rec.source, n->source, NOTIFY_SOURCE_MAX);
    n_strncpy(g_notify_chain.rec.title,  n->source, NOTIFY_TITLE_MAX);
    n_strncpy(g_notify_chain.rec.body,   n->text,   NOTIFY_BODY_MAX);
    g_notify_chain.level     = level;
    g_notify_chain.queue_idx = idx;
    g_notify_chain.valid     = 1;

    if (s_chain_notify >= 0) {
        (void)chain_resolve(s_chain_notify);
        return;
    }

    /* Pre-registration fallback path -- runs the same logic inline so
     * boot-time notifications still work before chain_registry_init has
     * created CHAIN_NOTIFY. After registration, every notify flows
     * through the chain. */
    int silenced = notify_should_silence(level, n->source);
    g_notify_chain.rec.was_silenced = (uint8_t)(silenced ? 1 : 0);
    history_append(&g_notify_chain.rec);
    if (level == NOTIFY_CRITICAL)        { show_notification(idx); }
    else if (silenced)                   { n->read = 1; if (g_notify.unread) g_notify.unread--; }
    else if (!g_notify.batch_mode && !focus_active()) { show_notification(idx); }
    g_notify_chain_total++;
    g_notify_chain.valid = 0;
}

void notify_tick(void) {
    ensure_init();
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
                if (notify_focus_suppresses(g_notify.queue[idx].level)) {
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

/* ── History API ── */

int notify_history_count(void) {
    ensure_init();
    return (int)g_history_count;
}

int notify_history_get(int idx, notification_record_t *out) {
    ensure_init();
    if (!out) return -1;
    if (idx < 0 || (uint32_t)idx >= g_history_count) return -1;

    /* idx 0 = newest. Newest entry is at (head - 1) mod cap. */
    uint32_t newest = (g_history_head + NOTIFY_HISTORY_CAP - 1) % NOTIFY_HISTORY_CAP;
    uint32_t pos    = (newest + NOTIFY_HISTORY_CAP - (uint32_t)idx) % NOTIFY_HISTORY_CAP;
    *out = g_history[pos];
    return 0;
}

void notify_history_clear(void) {
    ensure_init();
    g_history_head  = 0;
    g_history_count = 0;
    n_memset(g_history, 0, sizeof(g_history));
    history_persist();
}

/* ── DND API ── */

dnd_mode_t notify_dnd_mode(void)            { ensure_init(); return g_dnd_mode; }
int        notify_dnd_start_hour(void)      { ensure_init(); return g_dnd_start_hr; }
int        notify_dnd_end_hour(void)        { ensure_init(); return g_dnd_end_hr; }

void notify_dnd_set_mode(dnd_mode_t m) {
    ensure_init();
    if (m != DND_OFF && m != DND_ON && m != DND_SCHEDULE) return;
    g_dnd_mode = m;
    dnd_save_int(NOTIFY_DND_MODE_PATH, (int)m);
}

void notify_dnd_set_window(int start_hour, int end_hour) {
    ensure_init();
    if (start_hour < 0 || start_hour > 23) return;
    if (end_hour   < 0 || end_hour   > 23) return;
    g_dnd_start_hr = start_hour;
    g_dnd_end_hr   = end_hour;
    dnd_save_int(NOTIFY_DND_START_PATH, start_hour);
    dnd_save_int(NOTIFY_DND_END_PATH,   end_hour);
}

int notify_mute_add(const char *source) {
    ensure_init();
    if (!source || !*source) return -1;
    for (int i = 0; i < g_mute_count; i++) {
        if (n_streq(g_mute[i], source)) return -1;   /* dup */
    }
    if (g_mute_count >= NOTIFY_MUTE_MAX) return -1;
    n_strncpy(g_mute[g_mute_count], source, NOTIFY_SOURCE_MAX);
    g_mute_count++;
    mute_persist();
    return 0;
}

int notify_mute_remove(const char *source) {
    ensure_init();
    if (!source) return -1;
    for (int i = 0; i < g_mute_count; i++) {
        if (n_streq(g_mute[i], source)) {
            for (int j = i; j < g_mute_count - 1; j++) {
                n_memcpy(g_mute[j], g_mute[j+1], NOTIFY_SOURCE_MAX);
            }
            n_memset(g_mute[g_mute_count - 1], 0, NOTIFY_SOURCE_MAX);
            g_mute_count--;
            mute_persist();
            return 0;
        }
    }
    return -1;
}

int notify_mute_count(void) { ensure_init(); return g_mute_count; }

const char *notify_mute_get(int idx) {
    ensure_init();
    if (idx < 0 || idx >= g_mute_count) return (const char *)0;
    return g_mute[idx];
}

/* ── Selftest ── */

void notify_print_selftest_line(void) {
    ensure_init();
    kputs("Notify ................ ");
    kput_dec(g_history_count);
    kputs(" history entries, DND=");
    if      (g_dnd_mode == DND_OFF)      kputs("off");
    else if (g_dnd_mode == DND_ON)       kputs("on");
    else                                  kputs("schedule");
    kputs(", chain emits=");
    kput_dec((uint64_t)g_notify_chain_total);
    kputc('\n');
}

/* ── Shell commands ── */

/* Print "[YYYY-MM-DD HH:MM:SS]" using tod_format. */
static void print_history_timestamp(uint64_t epoch) {
    char buf[32];
    if (epoch == 0) {
        kputs("[                   ]");
        return;
    }
    int n = tod_format(epoch, buf, sizeof(buf));
    /* tod_format ends with " UTC"; trim that for the bracketed form. */
    if (n > 4 && buf[n-4] == ' ' && buf[n-3] == 'U') {
        buf[n-4] = '\0';
    }
    kputc('[');
    kputs(buf);
    kputc(']');
}

void notify_cmd_history(const char *args) {
    ensure_init();

    args = n_skip_ws(args);

    if (args && *args && (args[0] == 'c' || args[0] == 'C')) {
        /* "clear" */
        if (n_streq(args, "clear")) {
            notify_history_clear();
            kputs("notify: history cleared\n");
            return;
        }
    }

    int n = 16;
    if (args && *args) {
        const char *end;
        int v = n_parse_int(args, &end);
        if (v > 0) n = v;
    }
    if (n > (int)g_history_count) n = (int)g_history_count;

    if (n == 0) {
        kputs("notify: no history\n");
        return;
    }

    /* Newest first. */
    for (int i = 0; i < n; i++) {
        notification_record_t rec;
        if (notify_history_get(i, &rec) != 0) break;

        print_history_timestamp(rec.tod_unix);
        kputc(' ');
        kputs(level_label((notify_level_t)rec.level));
        kputc(' ');
        kputs(rec.source);
        kputs(": ");
        kputs(rec.body);
        if (rec.was_silenced) kputs("  (silenced)");
        kputc('\n');
    }
}

static void dnd_print_status(void) {
    kputs("dnd: mode=");
    if      (g_dnd_mode == DND_OFF)      kputs("off");
    else if (g_dnd_mode == DND_ON)       kputs("on");
    else                                  kputs("schedule");
    kputs(" window=");
    kput_dec((uint64_t)g_dnd_start_hr);
    kputs(":00-");
    kput_dec((uint64_t)g_dnd_end_hr);
    kputs(":00 UTC active=");
    kputs(dnd_currently_active() ? "yes" : "no");
    kputs(" muted=[");
    for (int i = 0; i < g_mute_count; i++) {
        if (i > 0) kputc(',');
        kputs(g_mute[i]);
    }
    kputs("]\n");
}

void notify_cmd_dnd(const char *args) {
    ensure_init();
    args = n_skip_ws(args);

    if (!args || !*args) {
        dnd_print_status();
        return;
    }

    /* Parse subcommand */
    const char *p = args;
    char sub[16] = {0};
    int sl = 0;
    while (*p && *p != ' ' && sl < (int)sizeof(sub) - 1) sub[sl++] = *p++;
    sub[sl] = '\0';
    p = n_skip_ws(p);

    if (n_streq(sub, "on")) {
        notify_dnd_set_mode(DND_ON);
        kputs("dnd: on\n");
        return;
    }
    if (n_streq(sub, "off")) {
        notify_dnd_set_mode(DND_OFF);
        kputs("dnd: off\n");
        return;
    }
    if (n_streq(sub, "schedule")) {
        /* Optional: "schedule HH-HH" */
        if (*p) {
            const char *end;
            int s_hr = n_parse_int(p, &end);
            const char *q = end;
            if (q && *q == '-') q++;
            int e_hr = (q && *q) ? n_parse_int(q, NULL) : -1;
            if (s_hr < 0 || s_hr > 23 || e_hr < 0 || e_hr > 23) {
                kputs("dnd: usage: dnd schedule HH-HH (UTC, 0-23)\n");
                return;
            }
            notify_dnd_set_window(s_hr, e_hr);
        }
        notify_dnd_set_mode(DND_SCHEDULE);
        dnd_print_status();
        return;
    }
    if (n_streq(sub, "mute")) {
        if (!*p) { kputs("dnd: usage: dnd mute <source>\n"); return; }
        if (notify_mute_add(p) == 0) {
            kputs("dnd: muted ");
            kputs(p);
            kputc('\n');
        } else {
            kputs("dnd: mute failed (duplicate or list full)\n");
        }
        return;
    }
    if (n_streq(sub, "unmute")) {
        if (!*p) { kputs("dnd: usage: dnd unmute <source>\n"); return; }
        if (notify_mute_remove(p) == 0) {
            kputs("dnd: unmuted ");
            kputs(p);
            kputc('\n');
        } else {
            kputs("dnd: source not in mute list\n");
        }
        return;
    }
    if (n_streq(sub, "status")) {
        dnd_print_status();
        return;
    }

    kputs("dnd: usage: dnd [on|off|schedule [start-end]|mute <s>|unmute <s>|status]\n");
}

/* Synthetic-fire helper for testing the toast/silence/history paths.
 * Usage: notify [info|warn|error|critical] <source> <text...>
 *        notify <text>           (defaults: level=info, source=shell) */
void notify_cmd_send(const char *args);
void notify_cmd_send(const char *args) {
    ensure_init();
    args = n_skip_ws(args);
    if (!args || !*args) {
        kputs("notify: usage: notify [info|warn|error|critical] [source] <text>\n");
        return;
    }

    notify_level_t lvl = NOTIFY_INFO;
    const char *p = args;

    /* Level keyword? */
    static const struct { const char *kw; int len; notify_level_t lvl; } kws[] = {
        {"info",     4, NOTIFY_INFO},
        {"warn",     4, NOTIFY_WARNING},
        {"warning",  7, NOTIFY_WARNING},
        {"error",    5, NOTIFY_ERROR},
        {"err",      3, NOTIFY_ERROR},
        {"critical", 8, NOTIFY_CRITICAL},
        {"crit",     4, NOTIFY_CRITICAL},
        {"success",  7, NOTIFY_SUCCESS},
    };
    for (unsigned i = 0; i < sizeof(kws)/sizeof(kws[0]); i++) {
        int match = 1;
        for (int j = 0; j < kws[i].len; j++) {
            if (p[j] != kws[i].kw[j]) { match = 0; break; }
        }
        if (match && (p[kws[i].len] == ' ' || p[kws[i].len] == '\0')) {
            lvl = kws[i].lvl;
            p = n_skip_ws(p + kws[i].len);
            break;
        }
    }

    /* Optional source token: a single short word followed by more text. */
    char source[NOTIFY_SOURCE_MAX] = "shell";
    int tok_len = 0;
    while (p[tok_len] && p[tok_len] != ' ') tok_len++;
    if (tok_len > 0 && tok_len < NOTIFY_SOURCE_MAX - 1 && p[tok_len] == ' ') {
        for (int i = 0; i < tok_len; i++) source[i] = p[i];
        source[tok_len] = '\0';
        p = n_skip_ws(p + tok_len);
    }

    notify_send(p, source, lvl);
    kputs("notify: queued (level=");
    kputs(level_label(lvl));
    kputs(", source=");
    kputs(source);
    kputs(", silenced=");
    kputs(notify_should_silence(lvl, source) ? "yes" : "no");
    kputs(")\n");
}

/* M.7 selftest: Focus Mode suppresses non-CRITICAL notifications and lets
 * CRITICAL through; OFF suppresses nothing. Drives the real access_set_focus_mode
 * and the real suppression predicate used by the notify tick.
 * Un-gated so the production `selftest` shell can run it; PRODUCTION-SAFE: saves
 * the live focus_mode and restores it at the end (net no change). NOTE:
 * access_set_focus_mode calls access_save() -> VAULT write, so this persists 3x
 * (0, 1, restore) with the final write returning focus_mode to its boot value —
 * same net-restored VAULT pattern as G.4's theme_set_scheme. Returns 1 on PASS. */
int notify_m7_selftest(void)
{
    extern void access_set_focus_mode(int);
    access_config_t *cfg = access_get();
    int saved = cfg ? cfg->focus_mode : 0;

    access_set_focus_mode(0);
    int off_info = notify_focus_suppresses(NOTIFY_INFO);      /* 0 */
    int off_crit = notify_focus_suppresses(NOTIFY_CRITICAL);  /* 0 */

    access_set_focus_mode(1);
    int on_info  = notify_focus_suppresses(NOTIFY_INFO);      /* 1 suppressed */
    int on_warn  = notify_focus_suppresses(NOTIFY_WARNING);   /* 1 suppressed */
    int on_err   = notify_focus_suppresses(NOTIFY_ERROR);     /* 1 suppressed */
    int on_crit  = notify_focus_suppresses(NOTIFY_CRITICAL);  /* 0 passes */

    access_set_focus_mode(saved);

    int pass = !off_info && !off_crit && on_info && on_warn && on_err && !on_crit;
    kputs("[M7] off(info/crit)="); kput_dec((uint64_t)off_info); kput_dec((uint64_t)off_crit);
    kputs(" on(info/warn/err/crit)="); kput_dec((uint64_t)on_info); kput_dec((uint64_t)on_warn);
    kput_dec((uint64_t)on_err); kput_dec((uint64_t)on_crit);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
    return pass;
}
