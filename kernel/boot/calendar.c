/*
 * Calendar + clock app. Five tabs: month grid, analog/digital clock,
 * alarms, timer, stopwatch. Alarm fire goes through notify.h.
 * CHAIN_CLOCK ticks at ~1Hz, advances state, fires due alarms.
 */

#include "calendar.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "wm.h"
#include "compositor.h"
#include "anim.h"
#include "chain.h"
#include "vault.h"
#include "notify.h"
#include "timeofday.h"
#include "timer.h"
#include "kprint.h"
#include "ui_hover.h"
#include "ui_dirty.h"
#include "mouse.h"

/* ── Public chain id ─────────────────────────────────────────────── */
int CHAIN_CLOCK = -1;

/* ── String helpers (bare-metal) ─────────────────────────────────── */

static int s_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static void s_copy(char *dst, int cap, const char *src) {
    if (!dst || cap <= 0) return;
    int i = 0;
    if (src) {
        while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    }
    dst[i] = 0;
}

static void s_zero(void *p, int n) {
    uint8_t *b = (uint8_t *)p;
    for (int i = 0; i < n; i++) b[i] = 0;
}

static int s_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int s_starts(const char *s, const char *pre) {
    while (*pre) { if (*s++ != *pre++) return 0; }
    return 1;
}

static int parse_uint(const char *s, uint64_t *out) {
    if (!s || !*s) return -1;
    uint64_t v = 0; int any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; any = 1; }
    if (!any) return -1;
    *out = v;
    return 0;
}

static const char *skip_ws(const char *s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

static void fmt_uint2(char *buf, int v) {
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    buf[0] = (char)('0' + (v / 10));
    buf[1] = (char)('0' + (v % 10));
    buf[2] = 0;
}

static void fmt_uint(char *buf, int cap, uint64_t v) {
    if (cap <= 0) return;
    char tmp[24]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } }
    int i = 0;
    while (n-- > 0 && i < cap - 1) buf[i++] = tmp[n];
    buf[i] = 0;
}

/* ── Time math (UTC, no leap-second) ─────────────────────────────── */
/* Civil-from-days algorithm (Howard Hinnant). */

typedef struct {
    int  year, month, day;          /* month 1..12, day 1..31 */
    int  hour, minute, second;
    int  weekday;                   /* 0=Sun .. 6=Sat */
} cal_dt_t;

static void unix_to_dt(uint64_t t, cal_dt_t *out) {
    uint64_t days = t / 86400;
    uint64_t sod  = t % 86400;
    out->hour     = (int)(sod / 3600);
    out->minute   = (int)((sod % 3600) / 60);
    out->second   = (int)(sod % 60);

    /* 1970-01-01 was a Thursday (4). */
    out->weekday = (int)((days + 4) % 7);

    /* Hinnant: convert days since 1970-01-01 to civil date. */
    int64_t z = (int64_t)days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int64_t  y   = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);
    uint32_t mp  = (5*doy + 2)/153;
    uint32_t d   = doy - (153*mp + 2)/5 + 1;
    uint32_t m   = (mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) y += 1;
    out->year  = (int)y;
    out->month = (int)m;
    out->day   = (int)d;
}

/* Returns Unix seconds for the given UTC civil date. */
static uint64_t dt_to_unix(int year, int month, int day,
                           int hour, int minute, int second) {
    int y = (month <= 2) ? year - 1 : year;
    int era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t m = (uint32_t)((month > 2) ? month - 3 : month + 9);
    uint32_t d = (uint32_t)(day - 1);
    uint32_t doy = (153*m + 2)/5 + d;
    uint32_t doe = yoe*365 + yoe/4 - yoe/100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return (uint64_t)(days * 86400 + hour * 3600 + minute * 60 + second);
}

static int days_in_month(int year, int month) {
    static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 30;
    if (month == 2) {
        int leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return dm[month - 1];
}

static const char *month_name(int m) {
    static const char *names[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    if (m < 1 || m > 12) return "?";
    return names[m - 1];
}

/* weekday_short reserved for future per-day labelling. */

/* ── State ───────────────────────────────────────────────────────── */

#define CAL_WIN_ID  4201
#define VAULT_EVENTS  "/calendar/events"
#define VAULT_ALARMS  "/calendar/alarms"
#define VAULT_WCLOCKS "/clock/world_clocks"

#define HOVER_MAX  96

typedef struct {
    /* Window */
    int       surface_id;
    int       open;

    /* Tab */
    cal_tab_t tab;

    /* Calendar view */
    int       view_year;
    int       view_month;
    int       sel_day;            /* 1..days_in_month, 0 = none */
    int       day_detail_open;    /* show day detail panel */

    /* Storage */
    cal_event_t       events[CAL_MAX_EVENTS];
    int               event_count;
    cal_alarm_t       alarms[CAL_MAX_ALARMS];
    int               alarm_count;
    cal_world_clock_t world[CAL_MAX_WORLD_CLOCKS];
    int               world_count;

    /* Timer */
    cal_timer_state_t timer_state;
    uint32_t          timer_total_s;
    uint32_t          timer_remaining_s;
    uint64_t          timer_last_tick_unix;

    /* Stopwatch */
    cal_stopwatch_state_t sw_state;
    uint64_t          sw_start_tsc;
    uint64_t          sw_accum_tsc;        /* accumulated when paused */
    uint32_t          sw_lap_ms[CAL_MAX_LAPS];
    int               sw_lap_count;
    uint32_t          sw_last_lap_total_ms;

    /* Animation: second-hand */
    int               second_anim_id;
    float             second_anim_pos;     /* 0..360 degrees */

    /* Hover registry */
    uint64_t          hover_tokens[HOVER_MAX];
    int               hover_count;

    /* Click edge detection */
    uint8_t           prev_buttons;
    int               last_click_x;
    int               last_click_y;
    int               click_pending;

    /* Dirty flag for vault save coalescing */
    int               events_dirty;
    int               alarms_dirty;
    int               world_dirty;
} cal_state_t;

static cal_state_t S;
static int s_initialized = 0;
static int s_chain_id = -1;

/* ── Forward decls ───────────────────────────────────────────────── */
static void cal_draw(int id, int x, int y, int w, int h);
static void cal_save_events(void);
static void cal_save_alarms(void);
static void cal_save_world(void);
static void cal_load_all(void);
static void cal_seed_defaults(void);
static void cal_run_alarms(uint64_t now_unix);
static void cal_run_timer(uint64_t now_unix);
static void cal_pump_clicks(void);
static void cal_handle_click(int x, int y);

/* ── Hover registry helpers ──────────────────────────────────────── */
static void clear_hovers(void) {
    for (int i = 0; i < S.hover_count; i++) hover_unregister(S.hover_tokens[i]);
    S.hover_count = 0;
}

static void add_hover(int x, int y, int w, int h) {
    if (S.hover_count >= HOVER_MAX) return;
    uint64_t t = hover_register(x, y, w, h, HOVER_CURSOR_POINTER, 0, 0);
    if (t) S.hover_tokens[S.hover_count++] = t;
}

/* ── VAULT (de)serialization ─────────────────────────────────────── */
/* Plain packed structs, fixed sizes — wire-stable for our own use. */

static void cal_save_events(void) {
    if (!s_initialized) return;
    int n = S.event_count;
    uint32_t bytes = (uint32_t)(sizeof(uint32_t) + (uint32_t)n * sizeof(cal_event_t));
    uint8_t  buf[sizeof(uint32_t) + CAL_MAX_EVENTS * sizeof(cal_event_t)];
    uint32_t *cnt = (uint32_t *)buf;
    *cnt = (uint32_t)n;
    cal_event_t *out = (cal_event_t *)(buf + sizeof(uint32_t));
    for (int i = 0; i < n; i++) out[i] = S.events[i];
    (void)vault_write(VAULT_EVENTS, buf, bytes);
    S.events_dirty = 0;
}

static void cal_save_alarms(void) {
    if (!s_initialized) return;
    int n = S.alarm_count;
    uint32_t bytes = (uint32_t)(sizeof(uint32_t) + (uint32_t)n * sizeof(cal_alarm_t));
    uint8_t  buf[sizeof(uint32_t) + CAL_MAX_ALARMS * sizeof(cal_alarm_t)];
    uint32_t *cnt = (uint32_t *)buf;
    *cnt = (uint32_t)n;
    cal_alarm_t *out = (cal_alarm_t *)(buf + sizeof(uint32_t));
    for (int i = 0; i < n; i++) out[i] = S.alarms[i];
    (void)vault_write(VAULT_ALARMS, buf, bytes);
    S.alarms_dirty = 0;
}

static void cal_save_world(void) {
    if (!s_initialized) return;
    int n = S.world_count;
    uint32_t bytes = (uint32_t)(sizeof(uint32_t) + (uint32_t)n * sizeof(cal_world_clock_t));
    uint8_t  buf[sizeof(uint32_t) + CAL_MAX_WORLD_CLOCKS * sizeof(cal_world_clock_t)];
    uint32_t *cnt = (uint32_t *)buf;
    *cnt = (uint32_t)n;
    cal_world_clock_t *out = (cal_world_clock_t *)(buf + sizeof(uint32_t));
    for (int i = 0; i < n; i++) out[i] = S.world[i];
    (void)vault_write(VAULT_WCLOCKS, buf, bytes);
    S.world_dirty = 0;
}

static void cal_load_all(void) {
    /* Events */
    {
        uint8_t buf[sizeof(uint32_t) + CAL_MAX_EVENTS * sizeof(cal_event_t)];
        int got = vault_read(VAULT_EVENTS, buf, sizeof(buf));
        if (got >= (int)sizeof(uint32_t)) {
            uint32_t n = *(uint32_t *)buf;
            if (n > CAL_MAX_EVENTS) n = CAL_MAX_EVENTS;
            int need = (int)(sizeof(uint32_t) + n * sizeof(cal_event_t));
            if (got >= need) {
                cal_event_t *src = (cal_event_t *)(buf + sizeof(uint32_t));
                for (uint32_t i = 0; i < n; i++) S.events[i] = src[i];
                S.event_count = (int)n;
            }
        }
    }
    /* Alarms */
    {
        uint8_t buf[sizeof(uint32_t) + CAL_MAX_ALARMS * sizeof(cal_alarm_t)];
        int got = vault_read(VAULT_ALARMS, buf, sizeof(buf));
        if (got >= (int)sizeof(uint32_t)) {
            uint32_t n = *(uint32_t *)buf;
            if (n > CAL_MAX_ALARMS) n = CAL_MAX_ALARMS;
            int need = (int)(sizeof(uint32_t) + n * sizeof(cal_alarm_t));
            if (got >= need) {
                cal_alarm_t *src = (cal_alarm_t *)(buf + sizeof(uint32_t));
                for (uint32_t i = 0; i < n; i++) S.alarms[i] = src[i];
                S.alarm_count = (int)n;
            }
        }
    }
    /* World clocks */
    {
        uint8_t buf[sizeof(uint32_t) + CAL_MAX_WORLD_CLOCKS * sizeof(cal_world_clock_t)];
        int got = vault_read(VAULT_WCLOCKS, buf, sizeof(buf));
        if (got >= (int)sizeof(uint32_t)) {
            uint32_t n = *(uint32_t *)buf;
            if (n > CAL_MAX_WORLD_CLOCKS) n = CAL_MAX_WORLD_CLOCKS;
            int need = (int)(sizeof(uint32_t) + n * sizeof(cal_world_clock_t));
            if (got >= need) {
                cal_world_clock_t *src = (cal_world_clock_t *)(buf + sizeof(uint32_t));
                for (uint32_t i = 0; i < n; i++) S.world[i] = src[i];
                S.world_count = (int)n;
            }
        }
    }
}

static void cal_seed_defaults(void) {
    if (S.world_count == 0) {
        cal_world_clock_add("UTC",        0);
        cal_world_clock_add("New York",  -300);
        cal_world_clock_add("London",      0);
        cal_world_clock_add("Tokyo",     540);
    }
}

/* ── Public API: events ──────────────────────────────────────────── */

int cal_event_add(uint64_t tod_unix, const char *label) {
    if (!s_initialized) cal_init();
    if (S.event_count >= CAL_MAX_EVENTS) return -1;
    cal_event_t *e = &S.events[S.event_count++];
    e->tod_unix = tod_unix;
    s_copy(e->label, CAL_LABEL_MAX, label ? label : "");
    cal_save_events();
    return S.event_count - 1;
}

int cal_event_remove(int idx) {
    if (idx < 0 || idx >= S.event_count) return -1;
    for (int i = idx; i < S.event_count - 1; i++) S.events[i] = S.events[i + 1];
    S.event_count--;
    cal_save_events();
    return 0;
}

int cal_event_count(void) { return S.event_count; }

const cal_event_t *cal_event_get(int idx) {
    if (idx < 0 || idx >= S.event_count) return 0;
    return &S.events[idx];
}

/* ── Public API: alarms ──────────────────────────────────────────── */

int cal_alarm_add(int hour, int minute, const char *label,
                  uint32_t repeat_mask, int priority) {
    if (!s_initialized) cal_init();
    if (S.alarm_count >= CAL_MAX_ALARMS) return -1;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return -1;
    cal_alarm_t *a = &S.alarms[S.alarm_count++];
    s_zero(a, sizeof(*a));
    a->hour     = (uint8_t)hour;
    a->minute   = (uint8_t)minute;
    a->enabled  = 1;
    a->priority = (uint8_t)(priority ? CAL_ALARM_PRIORITY_CRITICAL
                                     : CAL_ALARM_PRIORITY_NORMAL);
    a->repeat_mask = repeat_mask;
    s_copy(a->label, CAL_ALARM_LABEL_MAX, label ? label : "alarm");
    cal_save_alarms();
    return S.alarm_count - 1;
}

int cal_alarm_remove(int idx) {
    if (idx < 0 || idx >= S.alarm_count) return -1;
    for (int i = idx; i < S.alarm_count - 1; i++) S.alarms[i] = S.alarms[i + 1];
    S.alarm_count--;
    cal_save_alarms();
    return 0;
}

void cal_alarm_set_enabled(int idx, int enabled) {
    if (idx < 0 || idx >= S.alarm_count) return;
    S.alarms[idx].enabled = (uint8_t)(enabled ? 1 : 0);
    cal_save_alarms();
}

int cal_alarm_count(void) { return S.alarm_count; }

const cal_alarm_t *cal_alarm_get(int idx) {
    if (idx < 0 || idx >= S.alarm_count) return 0;
    return &S.alarms[idx];
}

/* ── Public API: world clocks ────────────────────────────────────── */

int cal_world_clock_add(const char *label, int tz_offset_minutes) {
    if (S.world_count >= CAL_MAX_WORLD_CLOCKS) return -1;
    cal_world_clock_t *w = &S.world[S.world_count++];
    w->tz_offset_minutes = tz_offset_minutes;
    s_copy(w->label, CAL_TZ_LABEL_MAX, label ? label : "?");
    cal_save_world();
    return S.world_count - 1;
}

int cal_world_clock_remove(int idx) {
    if (idx < 0 || idx >= S.world_count) return -1;
    for (int i = idx; i < S.world_count - 1; i++) S.world[i] = S.world[i + 1];
    S.world_count--;
    cal_save_world();
    return 0;
}

int cal_world_clock_count(void) { return S.world_count; }

const cal_world_clock_t *cal_world_clock_get(int idx) {
    if (idx < 0 || idx >= S.world_count) return 0;
    return &S.world[idx];
}

/* ── Timer ───────────────────────────────────────────────────────── */

void cal_timer_set_seconds(uint32_t total_seconds) {
    S.timer_total_s     = total_seconds;
    S.timer_remaining_s = total_seconds;
    S.timer_state       = (total_seconds == 0) ? CAL_TIMER_IDLE : CAL_TIMER_PAUSED;
    dirty_register(CAL_WIN_ID);
}

void cal_timer_start(void) {
    if (S.timer_remaining_s == 0) return;
    S.timer_state         = CAL_TIMER_RUNNING;
    S.timer_last_tick_unix = tod_now_unix();
    dirty_register(CAL_WIN_ID);
}

void cal_timer_pause(void) {
    if (S.timer_state == CAL_TIMER_RUNNING) S.timer_state = CAL_TIMER_PAUSED;
    dirty_register(CAL_WIN_ID);
}

void cal_timer_reset(void) {
    S.timer_remaining_s = S.timer_total_s;
    S.timer_state       = (S.timer_total_s == 0) ? CAL_TIMER_IDLE : CAL_TIMER_PAUSED;
    dirty_register(CAL_WIN_ID);
}

uint32_t cal_timer_remaining(void) { return S.timer_remaining_s; }
cal_timer_state_t cal_timer_state(void) { return S.timer_state; }

/* ── Stopwatch ───────────────────────────────────────────────────── */

static uint64_t tsc_to_ms(uint64_t delta_tsc) {
    uint64_t f = timer_tsc_freq();
    if (f == 0) return 0;
    return delta_tsc / (f / 1000 ? f / 1000 : 1);
}

void cal_sw_start(void) {
    if (S.sw_state == CAL_SW_RUNNING) return;
    S.sw_start_tsc = timer_read_tsc();
    S.sw_state     = CAL_SW_RUNNING;
    dirty_register(CAL_WIN_ID);
}

void cal_sw_stop(void) {
    if (S.sw_state == CAL_SW_RUNNING) {
        S.sw_accum_tsc += timer_read_tsc() - S.sw_start_tsc;
        S.sw_start_tsc = 0;
        S.sw_state = CAL_SW_PAUSED;
    }
    dirty_register(CAL_WIN_ID);
}

void cal_sw_lap(void) {
    if (S.sw_lap_count >= CAL_MAX_LAPS) return;
    uint32_t total = (uint32_t)cal_sw_elapsed_ms();
    S.sw_lap_ms[S.sw_lap_count++] = total - S.sw_last_lap_total_ms;
    S.sw_last_lap_total_ms = total;
    dirty_register(CAL_WIN_ID);
}

void cal_sw_reset(void) {
    S.sw_state = CAL_SW_IDLE;
    S.sw_start_tsc = 0;
    S.sw_accum_tsc = 0;
    S.sw_lap_count = 0;
    S.sw_last_lap_total_ms = 0;
    dirty_register(CAL_WIN_ID);
}

uint32_t cal_sw_elapsed_ms(void) {
    uint64_t t = S.sw_accum_tsc;
    if (S.sw_state == CAL_SW_RUNNING) t += timer_read_tsc() - S.sw_start_tsc;
    return (uint32_t)tsc_to_ms(t);
}

int cal_sw_lap_count(void) { return S.sw_lap_count; }

uint32_t cal_sw_lap_ms(int idx) {
    if (idx < 0 || idx >= S.sw_lap_count) return 0;
    return S.sw_lap_ms[idx];
}

cal_stopwatch_state_t cal_sw_state(void) { return S.sw_state; }

/* ── Alarm fire path ─────────────────────────────────────────────── */

static void fire_alarm(cal_alarm_t *a) {
    char body[NOTIFY_MAX_TEXT];
    /* Body packs label and HH:MM into the single notify text slot. */
    char hh[3], mm[3];
    fmt_uint2(hh, a->hour);
    fmt_uint2(mm, a->minute);
    int p = 0;
    /* "label  HH:MM" */
    int ll = s_len(a->label);
    if (ll > 0) {
        for (int i = 0; i < ll && p < (int)sizeof(body) - 8; i++) body[p++] = a->label[i];
        if (p < (int)sizeof(body) - 8) body[p++] = ' ';
        if (p < (int)sizeof(body) - 8) body[p++] = ' ';
    } else {
        const char *def = "alarm  ";
        for (int i = 0; def[i] && p < (int)sizeof(body) - 8; i++) body[p++] = def[i];
    }
    body[p++] = hh[0]; body[p++] = hh[1]; body[p++] = ':';
    body[p++] = mm[0]; body[p++] = mm[1];
    body[p]   = 0;

    notify_level_t lvl = (a->priority == CAL_ALARM_PRIORITY_CRITICAL)
                             ? NOTIFY_CRITICAL : NOTIFY_INFO;
    notify_send(body, "alarm", lvl);
}

static void cal_run_alarms(uint64_t now_unix) {
    if (now_unix == 0) return;
    cal_dt_t now;
    unix_to_dt(now_unix, &now);

    for (int i = 0; i < S.alarm_count; i++) {
        cal_alarm_t *a = &S.alarms[i];
        if (!a->enabled) continue;
        if (a->hour != now.hour || a->minute != now.minute) continue;

        /* Repeat-mask gate (0 = once-only). */
        if (a->repeat_mask != CAL_REPEAT_ONCE) {
            uint32_t bit = 1u << now.weekday;
            if ((a->repeat_mask & bit) == 0) continue;
        }

        /* Dedup: skip if we already fired within the same minute window. */
        uint64_t minute_floor = now_unix - (uint64_t)now.second;
        if (a->last_fired_unix >= minute_floor) continue;

        fire_alarm(a);
        a->last_fired_unix = now_unix;

        if (a->repeat_mask == CAL_REPEAT_ONCE) a->enabled = 0;
        S.alarms_dirty = 1;
    }

    if (S.alarms_dirty) cal_save_alarms();
}

static void cal_run_timer(uint64_t now_unix) {
    if (S.timer_state != CAL_TIMER_RUNNING) return;
    if (now_unix == 0) return;

    if (S.timer_last_tick_unix == 0) {
        S.timer_last_tick_unix = now_unix;
        return;
    }
    uint64_t delta = now_unix - S.timer_last_tick_unix;
    if (delta == 0) return;
    S.timer_last_tick_unix = now_unix;

    if (delta >= S.timer_remaining_s) {
        S.timer_remaining_s = 0;
        S.timer_state       = CAL_TIMER_FIRED;
        notify_send("Timer finished", "timer", NOTIFY_INFO);
    } else {
        S.timer_remaining_s -= (uint32_t)delta;
    }
    dirty_register(CAL_WIN_ID);
}

/* ── Spring callback for second-hand sweep ───────────────────────── */
static void second_anim_update(int anim_id, float pos, void *ctx) {
    (void)anim_id; (void)ctx;
    S.second_anim_pos = pos;
    if (S.open) dirty_register(CAL_WIN_ID);
}

/* ── Chain pipeline ──────────────────────────────────────────────── */

typedef struct {
    uint64_t now_unix;
    int      tick_seq;
} clock_tick_t;

static clock_tick_t g_tick;
static uint32_t     g_tick_counter;

static void node_tick_request(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    clock_tick_t *t = (clock_tick_t *)out;
    g_tick.now_unix = tod_now_unix();
    g_tick.tick_seq = (int)(++g_tick_counter);
    *t = g_tick;
}

static void node_time_advance(chain_node_t *self, void *in, void *out) {
    (void)self;
    clock_tick_t *t = (clock_tick_t *)in;
    *(clock_tick_t *)out = *t;

    /* Spring-retarget the second-hand to the new second's angle. */
    if (t->now_unix) {
        cal_dt_t dt;
        unix_to_dt(t->now_unix, &dt);
        float target = (float)dt.second * 6.0f;
        if (S.second_anim_id < 0) {
            S.second_anim_id = anim_spring_default(S.second_anim_pos, target,
                                                    second_anim_update, 0);
        } else {
            anim_retarget(S.second_anim_id, target);
        }
    }
    if (S.open) dirty_register(CAL_WIN_ID);
}

static void node_alarm_check(chain_node_t *self, void *in, void *out) {
    (void)self;
    clock_tick_t *t = (clock_tick_t *)in;
    *(clock_tick_t *)out = *t;
    cal_run_alarms(t->now_unix);
}

static void node_timer_check(chain_node_t *self, void *in, void *out) {
    (void)self;
    clock_tick_t *t = (clock_tick_t *)in;
    *(clock_tick_t *)out = *t;
    cal_run_timer(t->now_unix);
}

static void node_emit_events(chain_node_t *self, void *in, void *out) {
    (void)self;
    clock_tick_t *t = (clock_tick_t *)in;
    *(clock_tick_t *)out = *t;

    /* Bump CHAIN_CLOCK vault_version so MasQ records the tick. */
    if (s_chain_id >= 0) {
        chain_t *c = chain_get(s_chain_id);
        if (c) c->vault_version++;
    }

    /* Pump click edge detection while we're here -- this gives the
     * app one click sample per chain tick, which is fine for buttons
     * since the chain ticks every ~1s and all our click targets are
     * idempotent (tab switches, day selects, button taps). */
    if (S.open) cal_pump_clicks();
}

int cal_chain_register(int parent_chain_id) {
    if (s_chain_id >= 0) {
        CHAIN_CLOCK = s_chain_id;
        return s_chain_id;
    }
    int id = chain_create("clock", parent_chain_id, MASQ_INTERNAL);
    if (id < 0) return -1;
    chain_add_node(id, "tick_request",  "void",         "clock_tick", node_tick_request);
    chain_add_node(id, "time_advance",  "clock_tick",   "clock_tick", node_time_advance);
    chain_add_node(id, "alarm_check",   "clock_tick",   "clock_tick", node_alarm_check);
    chain_add_node(id, "timer_check",   "clock_tick",   "clock_tick", node_timer_check);
    chain_add_node(id, "emit_events",   "clock_tick",   "clock_tick", node_emit_events);

    chain_t *c = chain_get(id);
    if (c) c->resolve_interval_ticks = 120;     /* ~1 sec at the scheduler's tick rate */

    s_chain_id = id;
    CHAIN_CLOCK = id;
    return id;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

void cal_init(void) {
    if (s_initialized) return;
    s_zero(&S, sizeof(S));
    S.tab               = CAL_TAB_CALENDAR;
    S.surface_id        = -1;
    S.second_anim_id    = -1;
    S.timer_state       = CAL_TIMER_IDLE;
    S.sw_state          = CAL_SW_IDLE;

    /* Preload current month from tod (or 2026-01 fallback). */
    uint64_t now = tod_now_unix();
    if (now) {
        cal_dt_t dt;
        unix_to_dt(now, &dt);
        S.view_year  = dt.year;
        S.view_month = dt.month;
        S.sel_day    = dt.day;
    } else {
        S.view_year = 2026; S.view_month = 1; S.sel_day = 1;
    }

    s_initialized = 1;
    cal_load_all();
    cal_seed_defaults();
}

/* ── Drawing ─────────────────────────────────────────────────────── */

#define TAB_BAR_H   28
#define PADDING     12

static void draw_tab_bar(int x, int y, int w) {
    static const char *names[CAL_TAB_COUNT] = {
        "CALENDAR", "CLOCK", "ALARM", "TIMER", "STOPWATCH"
    };
    int tab_w = w / CAL_TAB_COUNT;
    fb_rect(x, y, w, TAB_BAR_H, COLOR_SURFACE_HIGH);
    for (int i = 0; i < CAL_TAB_COUNT; i++) {
        int tx = x + i * tab_w;
        int active = (S.tab == (cal_tab_t)i);
        if (active) {
            fb_rect(tx, y, tab_w, TAB_BAR_H, COLOR_SURFACE_TOP);
            fb_rect(tx, y + TAB_BAR_H - 2, tab_w, 2, COLOR_PRIMARY);
        }
        int tw = font_measure(names[i], FONT_UI, TYPE_LABEL);
        font_draw(tx + (tab_w - tw) / 2, y + 7, names[i], FONT_UI, TYPE_LABEL,
                  active ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_2);
        add_hover(tx, y, tab_w, TAB_BAR_H);
    }
    fb_rect(x, y + TAB_BAR_H, w, 1, COLOR_SEPARATOR);
}

/* ── CALENDAR tab ────────────────────────────────────────────────── */

static int g_cell_w = 0, g_cell_h = 0, g_grid_x0 = 0, g_grid_y0 = 0;

static void draw_tab_calendar(int x, int y, int w, int h) {
    /* Header: "<month_name> <year>" + < / > nav. */
    char title[64]; int p = 0;
    const char *mn = month_name(S.view_month);
    int ml = s_len(mn);
    for (int i = 0; i < ml && p < 60; i++) title[p++] = mn[i];
    title[p++] = ' ';
    char yb[16]; fmt_uint(yb, sizeof(yb), (uint64_t)S.view_year);
    int yl = s_len(yb);
    for (int i = 0; i < yl && p < 62; i++) title[p++] = yb[i];
    title[p] = 0;

    int hdr_y = y + 8;
    font_draw(x + 12, hdr_y, "<", FONT_UI_BOLD, TYPE_HEADING, COLOR_ON_SURFACE);
    add_hover(x + 8, hdr_y - 4, 24, 24);
    int tw = font_measure(title, FONT_UI_BOLD, TYPE_HEADING);
    font_draw(x + (w - tw) / 2, hdr_y, title, FONT_UI_BOLD, TYPE_HEADING, COLOR_ON_SURFACE);
    font_draw(x + w - 24, hdr_y, ">", FONT_UI_BOLD, TYPE_HEADING, COLOR_ON_SURFACE);
    add_hover(x + w - 32, hdr_y - 4, 24, 24);

    /* Weekday header strip. */
    int strip_y = hdr_y + 36;
    static const char *wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    int grid_x = x + 12;
    int grid_w = w - 24;
    int cell_w = grid_w / 7;
    for (int i = 0; i < 7; i++) {
        font_draw(grid_x + i * cell_w + 6, strip_y, wd[i], FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_2);
    }

    /* Compute first day-of-week and day count. */
    uint64_t first = dt_to_unix(S.view_year, S.view_month, 1, 0, 0, 0);
    cal_dt_t fdt; unix_to_dt(first, &fdt);
    int first_wday = fdt.weekday;
    int dim = days_in_month(S.view_year, S.view_month);

    /* Today highlight. */
    uint64_t now = tod_now_unix();
    cal_dt_t today = (cal_dt_t){0};
    int today_in_view = 0;
    if (now) {
        unix_to_dt(now, &today);
        if (today.year == S.view_year && today.month == S.view_month) today_in_view = 1;
    }

    int grid_y = strip_y + 22;
    int rows   = (first_wday + dim + 6) / 7;
    int cell_h = (y + h - grid_y - 8) / (rows > 0 ? rows : 1);
    if (cell_h < 32) cell_h = 32;
    g_cell_w = cell_w; g_cell_h = cell_h;
    g_grid_x0 = grid_x; g_grid_y0 = grid_y;

    /* Cells */
    int day = 1;
    for (int r = 0; r < rows && day <= dim; r++) {
        for (int c = 0; c < 7 && day <= dim; c++) {
            int cx = grid_x + c * cell_w;
            int cy = grid_y + r * cell_h;
            int cw = cell_w - 2;
            int ch = cell_h - 2;
            if (r == 0 && c < first_wday) continue;

            uint32_t bg = COLOR_SURFACE_HIGH;
            uint32_t fg = COLOR_ON_SURFACE;
            int is_today = today_in_view && day == today.day;
            int is_sel   = day == S.sel_day;
            if (is_today) { bg = COLOR_PRIMARY_DIM; fg = COLOR_ON_SURFACE; }
            else if (is_sel) { bg = COLOR_SURFACE_TOP; }
            fb_rect(cx, cy, cw, ch, bg);
            fb_rect_outline(cx, cy, cw, ch, COLOR_SEPARATOR, 1);

            char db[8]; fmt_uint(db, sizeof(db), (uint64_t)day);
            font_draw(cx + 6, cy + 4, db, FONT_UI, TYPE_BODY, fg);

            /* Event dot if any event lands inside this day (UTC). */
            uint64_t day_start = dt_to_unix(S.view_year, S.view_month, day, 0, 0, 0);
            uint64_t day_end   = day_start + 86400;
            for (int ei = 0; ei < S.event_count; ei++) {
                if (S.events[ei].tod_unix >= day_start && S.events[ei].tod_unix < day_end) {
                    fb_circle_filled(cx + cw - 8, cy + ch - 8, 3, COLOR_SUCCESS);
                    break;
                }
            }

            add_hover(cx, cy, cw, ch);
            day++;
        }
    }

    /* Day-detail panel */
    if (S.day_detail_open && S.sel_day > 0) {
        int dpw = w / 3;
        int dpx = x + w - dpw - 8;
        int dpy = grid_y + 4;
        int dph = y + h - dpy - 8;
        fb_rect(dpx, dpy, dpw, dph, COLOR_SURFACE_TOP);
        fb_rect_outline(dpx, dpy, dpw, dph, COLOR_SEPARATOR, 1);

        char hdr[48]; int hp = 0;
        const char *mn2 = month_name(S.view_month);
        for (int i = 0; mn2[i] && hp < 30; i++) hdr[hp++] = mn2[i];
        hdr[hp++] = ' ';
        char db2[8]; fmt_uint(db2, sizeof(db2), (uint64_t)S.sel_day);
        for (int i = 0; db2[i] && hp < 40; i++) hdr[hp++] = db2[i];
        hdr[hp] = 0;
        font_draw(dpx + 8, dpy + 6, hdr, FONT_UI_BOLD, TYPE_BODY, COLOR_ON_SURFACE);

        int ly = dpy + 30;
        uint64_t day_start = dt_to_unix(S.view_year, S.view_month, S.sel_day, 0, 0, 0);
        uint64_t day_end   = day_start + 86400;
        int shown = 0;
        for (int ei = 0; ei < S.event_count; ei++) {
            if (S.events[ei].tod_unix < day_start || S.events[ei].tod_unix >= day_end) continue;
            cal_dt_t edt; unix_to_dt(S.events[ei].tod_unix, &edt);
            char hh[3], mm[3];
            fmt_uint2(hh, edt.hour); fmt_uint2(mm, edt.minute);
            char line[80]; int lp = 0;
            line[lp++] = hh[0]; line[lp++] = hh[1]; line[lp++] = ':';
            line[lp++] = mm[0]; line[lp++] = mm[1]; line[lp++] = ' ';
            for (int i = 0; S.events[ei].label[i] && lp < 78; i++) line[lp++] = S.events[ei].label[i];
            line[lp] = 0;
            font_draw(dpx + 8, ly, line, FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE);
            ly += 18;
            shown++;
            if (ly > dpy + dph - 40) break;
        }
        if (!shown) {
            font_draw(dpx + 8, ly, "No events", FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_3);
        }

        /* "+ Add event for noon" button (idempotent: always adds 12:00 placeholder). */
        int by = dpy + dph - 30;
        fb_rect(dpx + 8, by, dpw - 16, 24, COLOR_SURFACE_HIGH);
        fb_rect_outline(dpx + 8, by, dpw - 16, 24, COLOR_PRIMARY, 1);
        font_draw(dpx + 16, by + 6, "+ Add 12:00 event", FONT_UI, TYPE_LABEL, COLOR_PRIMARY);
        add_hover(dpx + 8, by, dpw - 16, 24);
    }
}

/* ── CLOCK tab ───────────────────────────────────────────────────── */

/* Tiny cosine/sine via Bhaskara approximation in degrees. Good enough
 * for clock hands at integer pixel resolution. */
static int s_xcos[361];
static int s_xsin[361];
static int s_trig_built = 0;
static void build_trig(void) {
    if (s_trig_built) return;
    /* Hand-rolled: use 16-step lookup combined with Taylor for finer
     * resolution. We only need second-precision visuals. */
    for (int d = 0; d <= 360; d++) {
        /* Use Taylor expansion of sin around 0 with reduction to [-pi, pi].
         * Here we implement cos/sin using the formulas:
         *   sin(x) ~ x - x^3/6 + x^5/120
         *   cos(x) ~ 1 - x^2/2 + x^4/24
         * Inputs are degrees scaled to radians. We multiply by 1024 and store. */
        float deg = (float)d;
        float rad = deg * 3.1415926535f / 180.0f;
        /* Reduce to [-pi/2, pi/2] for accurate Taylor. */
        float sign_sin = 1.0f, sign_cos = 1.0f;
        float r = rad;
        while (r >  3.1415926535f) r -= 6.2831853f;
        while (r < -3.1415926535f) r += 6.2831853f;
        float ra = r;
        if (ra < 0) ra = -ra;
        float c, sn;
        if (ra > 1.5707963f) {
            float r2 = 3.1415926535f - ra;
            sn = r2 - r2*r2*r2/6.0f + r2*r2*r2*r2*r2/120.0f;
            c  = -(1.0f - r2*r2/2.0f + r2*r2*r2*r2/24.0f);
            if (r < 0) sn = -sn;
        } else {
            sn = r - r*r*r/6.0f + r*r*r*r*r/120.0f;
            c  = 1.0f - r*r/2.0f + r*r*r*r/24.0f;
        }
        (void)sign_sin; (void)sign_cos;
        s_xcos[d] = (int)(c * 1024.0f);
        s_xsin[d] = (int)(sn * 1024.0f);
    }
    s_trig_built = 1;
}

static void hand(int cx, int cy, int len, float angle_deg, uint32_t color, int thickness) {
    build_trig();
    /* Normalize angle to [0, 360]. */
    while (angle_deg < 0)    angle_deg += 360.0f;
    while (angle_deg >= 360) angle_deg -= 360.0f;
    int d = (int)angle_deg;
    if (d < 0) d = 0;
    if (d > 360) d = 360;
    /* 0 deg points up: x=cos(angle - 90) etc. Use sin for x, -cos for y. */
    int sn = s_xsin[d];
    int cs = s_xcos[d];
    int x2 = cx + (len * sn) / 1024;
    int y2 = cy - (len * cs) / 1024;
    fb_line(cx, cy, x2, y2, color);
    if (thickness > 1) {
        fb_line(cx + 1, cy, x2 + 1, y2, color);
        fb_line(cx, cy + 1, x2, y2 + 1, color);
    }
}

static void draw_analog_clock(int cx, int cy, int r, uint64_t now_unix) {
    fb_circle_filled(cx, cy, r, COLOR_SURFACE_HIGH);
    fb_circle(cx, cy, r, COLOR_SEPARATOR);
    fb_circle(cx, cy, r - 1, COLOR_SEPARATOR);

    /* Tick marks. */
    build_trig();
    for (int t = 0; t < 12; t++) {
        int d = (t * 30) % 360;
        int sn = s_xsin[d], cs = s_xcos[d];
        int x1 = cx + ((r - 8) * sn) / 1024;
        int y1 = cy - ((r - 8) * cs) / 1024;
        int x2 = cx + ((r - 2) * sn) / 1024;
        int y2 = cy - ((r - 2) * cs) / 1024;
        fb_line(x1, y1, x2, y2, COLOR_ON_SURFACE_2);
    }

    if (now_unix == 0) {
        font_draw(cx - 32, cy - 6, "no clock", FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);
        return;
    }
    cal_dt_t dt; unix_to_dt(now_unix, &dt);

    float h_deg = ((float)(dt.hour % 12) * 30.0f) + ((float)dt.minute * 0.5f);
    float m_deg = (float)dt.minute * 6.0f;
    float s_deg = S.second_anim_pos;        /* spring-driven for smoothness */

    hand(cx, cy, r * 50 / 100, h_deg, COLOR_ON_SURFACE,    3);
    hand(cx, cy, r * 75 / 100, m_deg, COLOR_ON_SURFACE,    2);
    hand(cx, cy, r * 90 / 100, s_deg, COLOR_PRIMARY,       1);
    fb_circle_filled(cx, cy, 4, COLOR_PRIMARY);
}

static void draw_tab_clock(int x, int y, int w, int h) {
    uint64_t now = tod_now_unix();

    /* Analog clock on the left half. */
    int half_w = w / 2;
    int radius = (half_w < (h - 40) ? half_w : (h - 40)) / 2 - 12;
    if (radius < 40) radius = 40;
    int cx = x + half_w / 2;
    int cy = y + 20 + radius;
    draw_analog_clock(cx, cy, radius, now);

    /* Digital readout below the analog. */
    char digital[40] = "—";
    if (now) {
        cal_dt_t dt; unix_to_dt(now, &dt);
        char hh[3], mm[3], ss[3];
        fmt_uint2(hh, dt.hour); fmt_uint2(mm, dt.minute); fmt_uint2(ss, dt.second);
        digital[0] = hh[0]; digital[1] = hh[1]; digital[2] = ':';
        digital[3] = mm[0]; digital[4] = mm[1]; digital[5] = ':';
        digital[6] = ss[0]; digital[7] = ss[1]; digital[8] = 0;
    }
    int dw = font_measure(digital, FONT_CODE_BOLD, TYPE_TITLE);
    font_draw(cx - dw / 2, cy + radius + 12, digital, FONT_CODE_BOLD, TYPE_TITLE, COLOR_ON_SURFACE);

    /* World clocks on the right half. */
    int rx = x + half_w + 8;
    int ry = y + 20;
    int rw = w - half_w - 16;
    fb_rect(rx, ry, rw, h - 40, COLOR_SURFACE_HIGH);
    fb_rect_outline(rx, ry, rw, h - 40, COLOR_SEPARATOR, 1);
    font_draw(rx + 12, ry + 8, "WORLD CLOCKS", FONT_UI_BOLD, TYPE_CAPTION, COLOR_ON_SURFACE_2);

    int ly = ry + 32;
    for (int i = 0; i < S.world_count; i++) {
        const cal_world_clock_t *wc = &S.world[i];
        font_draw(rx + 12, ly, wc->label, FONT_UI, TYPE_BODY, COLOR_ON_SURFACE);

        char readout[16] = "—";
        if (now) {
            uint64_t local = now + (uint64_t)((int64_t)wc->tz_offset_minutes * 60);
            cal_dt_t dt; unix_to_dt(local, &dt);
            char hh[3], mm[3];
            fmt_uint2(hh, dt.hour); fmt_uint2(mm, dt.minute);
            readout[0] = hh[0]; readout[1] = hh[1]; readout[2] = ':';
            readout[3] = mm[0]; readout[4] = mm[1]; readout[5] = 0;
        }
        int rwx = font_measure(readout, FONT_CODE_BOLD, TYPE_HEADING);
        font_draw(rx + rw - rwx - 12, ly - 2, readout, FONT_CODE_BOLD, TYPE_HEADING, COLOR_PRIMARY);
        ly += 36;
        if (ly > ry + h - 60) break;
    }
}

/* ── ALARM tab ───────────────────────────────────────────────────── */

static void draw_tab_alarm(int x, int y, int w, int h) {
    fb_rect(x + 8, y + 8, w - 16, 28, COLOR_SURFACE_HIGH);
    font_draw(x + 16, y + 14, "+ Add 7:00 daily wake", FONT_UI, TYPE_LABEL, COLOR_PRIMARY);
    add_hover(x + 8, y + 8, w - 16, 28);

    int ly = y + 44;
    if (S.alarm_count == 0) {
        font_draw(x + 16, ly, "No alarms set.", FONT_UI, TYPE_BODY, COLOR_ON_SURFACE_3);
        return;
    }

    for (int i = 0; i < S.alarm_count; i++) {
        cal_alarm_t *a = &S.alarms[i];
        fb_rect(x + 8, ly, w - 16, 36, a->enabled ? COLOR_SURFACE_HIGH : COLOR_SURFACE);
        fb_rect_outline(x + 8, ly, w - 16, 36, COLOR_SEPARATOR, 1);

        char tbuf[8];
        char hh[3], mm[3]; fmt_uint2(hh, a->hour); fmt_uint2(mm, a->minute);
        tbuf[0] = hh[0]; tbuf[1] = hh[1]; tbuf[2] = ':'; tbuf[3] = mm[0]; tbuf[4] = mm[1]; tbuf[5] = 0;
        font_draw(x + 18, ly + 6, tbuf, FONT_CODE_BOLD, TYPE_HEADING,
                  a->enabled ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_3);
        font_draw(x + 90, ly + 10, a->label, FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_2);

        const char *rep = "once";
        if (a->repeat_mask == CAL_REPEAT_DAILY) rep = "daily";
        else if (a->repeat_mask != CAL_REPEAT_ONCE) rep = "weekly";
        font_draw(x + w - 120, ly + 12, rep, FONT_UI, TYPE_CAPTION, COLOR_ON_SURFACE_3);

        const char *toggle = a->enabled ? "ON" : "OFF";
        uint32_t tcol = a->enabled ? COLOR_SUCCESS : COLOR_ON_SURFACE_3;
        font_draw(x + w - 36, ly + 10, toggle, FONT_UI_BOLD, TYPE_LABEL, tcol);

        add_hover(x + 8, ly, w - 16, 36);
        ly += 42;
        if (ly > y + h - 12) break;
    }
}

/* ── TIMER tab ───────────────────────────────────────────────────── */

static void draw_tab_timer(int x, int y, int w, int h) {
    uint32_t r = S.timer_remaining_s;
    int hh = (int)(r / 3600);
    int mm = (int)((r % 3600) / 60);
    int ss = (int)(r % 60);

    char buf[16]; int p = 0;
    char hb[3], mb[3], sb[3];
    fmt_uint2(hb, hh); fmt_uint2(mb, mm); fmt_uint2(sb, ss);
    buf[p++] = hb[0]; buf[p++] = hb[1]; buf[p++] = ':';
    buf[p++] = mb[0]; buf[p++] = mb[1]; buf[p++] = ':';
    buf[p++] = sb[0]; buf[p++] = sb[1]; buf[p] = 0;

    int tw = font_measure(buf, FONT_CODE_BOLD, TYPE_DISPLAY);
    int cx = x + w / 2;
    int cy = y + h / 2 - 30;
    uint32_t col = (S.timer_state == CAL_TIMER_FIRED) ? COLOR_DANGER : COLOR_ON_SURFACE;
    font_draw(cx - tw / 2, cy - 24, buf, FONT_CODE_BOLD, TYPE_DISPLAY, col);

    /* Buttons */
    int by = cy + 30;
    int bw = 100, bh = 32, gap = 12;
    int bx = cx - (3 * bw + 2 * gap) / 2;

    const char *labels[3] = {
        S.timer_state == CAL_TIMER_RUNNING ? "Pause" : "Start",
        "Reset",
        "+30s"
    };
    for (int i = 0; i < 3; i++) {
        int x0 = bx + i * (bw + gap);
        fb_rect(x0, by, bw, bh, COLOR_SURFACE_HIGH);
        fb_rect_outline(x0, by, bw, bh, COLOR_PRIMARY, 1);
        int lw = font_measure(labels[i], FONT_UI_BOLD, TYPE_LABEL);
        font_draw(x0 + (bw - lw) / 2, by + 8, labels[i], FONT_UI_BOLD, TYPE_LABEL, COLOR_PRIMARY);
        add_hover(x0, by, bw, bh);
    }

    const char *state_str = "idle";
    switch (S.timer_state) {
    case CAL_TIMER_RUNNING: state_str = "running"; break;
    case CAL_TIMER_PAUSED:  state_str = "paused";  break;
    case CAL_TIMER_FIRED:   state_str = "FIRED";   break;
    default: break;
    }
    font_draw(cx - 24, by + bh + 10, state_str, FONT_UI, TYPE_LABEL, COLOR_ON_SURFACE_2);
}

/* ── STOPWATCH tab ───────────────────────────────────────────────── */

static void draw_tab_stopwatch(int x, int y, int w, int h) {
    uint32_t total_ms = cal_sw_elapsed_ms();
    uint32_t total_s  = total_ms / 1000;
    int hh = (int)(total_s / 3600);
    int mm = (int)((total_s % 3600) / 60);
    int ss = (int)(total_s % 60);
    int cs = (int)((total_ms % 1000) / 10);

    char buf[20]; int p = 0;
    char hb[3], mb[3], sb[3], cb[3];
    fmt_uint2(hb, hh); fmt_uint2(mb, mm); fmt_uint2(sb, ss); fmt_uint2(cb, cs);
    buf[p++] = hb[0]; buf[p++] = hb[1]; buf[p++] = ':';
    buf[p++] = mb[0]; buf[p++] = mb[1]; buf[p++] = ':';
    buf[p++] = sb[0]; buf[p++] = sb[1]; buf[p++] = '.';
    buf[p++] = cb[0]; buf[p++] = cb[1]; buf[p] = 0;

    int tw = font_measure(buf, FONT_CODE_BOLD, TYPE_DISPLAY);
    int cx = x + w / 2;
    int cy = y + 60;
    font_draw(cx - tw / 2, cy, buf, FONT_CODE_BOLD, TYPE_DISPLAY, COLOR_ON_SURFACE);

    /* Buttons */
    int by = cy + 56;
    int bw = 90, bh = 28, gap = 10;
    int bx = cx - (4 * bw + 3 * gap) / 2;
    const char *labels[4] = {
        (S.sw_state == CAL_SW_RUNNING) ? "Stop" : "Start",
        "Lap",
        "Reset",
        "—"
    };
    for (int i = 0; i < 3; i++) {        /* 3 active buttons */
        int x0 = bx + i * (bw + gap);
        fb_rect(x0, by, bw, bh, COLOR_SURFACE_HIGH);
        fb_rect_outline(x0, by, bw, bh, COLOR_PRIMARY, 1);
        int lw = font_measure(labels[i], FONT_UI_BOLD, TYPE_LABEL);
        font_draw(x0 + (bw - lw) / 2, by + 6, labels[i], FONT_UI_BOLD, TYPE_LABEL, COLOR_PRIMARY);
        add_hover(x0, by, bw, bh);
    }

    /* Lap list */
    int ly = by + bh + 16;
    font_draw(x + 24, ly, "LAPS", FONT_UI_BOLD, TYPE_CAPTION, COLOR_ON_SURFACE_2);
    ly += 18;
    for (int i = S.sw_lap_count - 1; i >= 0 && ly < y + h - 16; i--) {
        char lb[40]; int lp = 0;
        char nb[8]; fmt_uint(nb, sizeof(nb), (uint64_t)(i + 1));
        for (int k = 0; nb[k] && lp < 30; k++) lb[lp++] = nb[k];
        lb[lp++] = '.'; lb[lp++] = ' ';
        uint32_t mss = S.sw_lap_ms[i];
        uint32_t s = mss / 1000;
        uint32_t cs2 = (mss % 1000) / 10;
        char hh2[3], mm2[3], ss2[3], cc2[3];
        fmt_uint2(hh2, (int)(s / 3600));
        fmt_uint2(mm2, (int)((s % 3600) / 60));
        fmt_uint2(ss2, (int)(s % 60));
        fmt_uint2(cc2, (int)cs2);
        lb[lp++] = hh2[0]; lb[lp++] = hh2[1]; lb[lp++] = ':';
        lb[lp++] = mm2[0]; lb[lp++] = mm2[1]; lb[lp++] = ':';
        lb[lp++] = ss2[0]; lb[lp++] = ss2[1]; lb[lp++] = '.';
        lb[lp++] = cc2[0]; lb[lp++] = cc2[1]; lb[lp] = 0;
        font_draw(x + 24, ly, lb, FONT_CODE, TYPE_LABEL, COLOR_ON_SURFACE);
        ly += 18;
    }
    (void)labels;
}

/* ── Top-level draw ──────────────────────────────────────────────── */

static void cal_draw(int id, int x, int y, int w, int h) {
    (void)id;
    if (!s_initialized) cal_init();
    clear_hovers();

    fb_rect(x, y, w, h, COLOR_SURFACE);
    draw_tab_bar(x, y, w);

    int cy = y + TAB_BAR_H + 1;
    int ch = h - TAB_BAR_H - 1;
    switch (S.tab) {
    case CAL_TAB_CALENDAR:  draw_tab_calendar (x, cy, w, ch); break;
    case CAL_TAB_CLOCK:     draw_tab_clock    (x, cy, w, ch); break;
    case CAL_TAB_ALARM:     draw_tab_alarm    (x, cy, w, ch); break;
    case CAL_TAB_TIMER:     draw_tab_timer    (x, cy, w, ch); break;
    case CAL_TAB_STOPWATCH: draw_tab_stopwatch(x, cy, w, ch); break;
    default: break;
    }

    dirty_clear(CAL_WIN_ID);
}

/* ── Click dispatch ─────────────────────────────────────────────── */

static int point_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

static void cal_handle_click(int x, int y) {
    chain_surface_t *s = wm_get_surface(S.surface_id);
    if (!s) return;
    int sx = s->x;
    int sy = s->y + WM_TITLEBAR_HEIGHT;
    int sw = s->w;
    int sh = s->h - WM_TITLEBAR_HEIGHT;
    if (!point_in(x, y, sx, sy, sw, sh)) return;

    /* Tab bar */
    int tab_w = sw / CAL_TAB_COUNT;
    if (point_in(x, y, sx, sy, sw, TAB_BAR_H)) {
        int idx = (x - sx) / tab_w;
        if (idx < 0) idx = 0;
        if (idx >= CAL_TAB_COUNT) idx = CAL_TAB_COUNT - 1;
        S.tab = (cal_tab_t)idx;
        dirty_register(CAL_WIN_ID);
        return;
    }

    int ax = sx;
    int ay = sy + TAB_BAR_H + 1;
    int aw = sw;
    int ah = sh - TAB_BAR_H - 1;

    switch (S.tab) {
    case CAL_TAB_CALENDAR: {
        /* < / > arrows. */
        if (point_in(x, y, ax + 8, ay + 4, 24, 24)) {
            S.view_month--;
            if (S.view_month < 1) { S.view_month = 12; S.view_year--; }
            S.day_detail_open = 0;
            dirty_register(CAL_WIN_ID); return;
        }
        if (point_in(x, y, ax + aw - 32, ay + 4, 24, 24)) {
            S.view_month++;
            if (S.view_month > 12) { S.view_month = 1; S.view_year++; }
            S.day_detail_open = 0;
            dirty_register(CAL_WIN_ID); return;
        }
        /* Day detail "+ Add" button. */
        if (S.day_detail_open && S.sel_day > 0) {
            int dpw = aw / 3;
            int dpx = ax + aw - dpw - 8;
            int dpy = g_grid_y0 + 4;
            int dph = ay + ah - dpy - 8;
            int by = dpy + dph - 30;
            if (point_in(x, y, dpx + 8, by, dpw - 16, 24)) {
                uint64_t when = dt_to_unix(S.view_year, S.view_month, S.sel_day, 12, 0, 0);
                cal_event_add(when, "Event");
                dirty_register(CAL_WIN_ID); return;
            }
        }
        /* Grid cell hit-test. */
        if (g_cell_w > 0 && g_cell_h > 0) {
            uint64_t first = dt_to_unix(S.view_year, S.view_month, 1, 0, 0, 0);
            cal_dt_t fdt; unix_to_dt(first, &fdt);
            int first_wday = fdt.weekday;
            int dim = days_in_month(S.view_year, S.view_month);
            int rel_x = x - g_grid_x0;
            int rel_y = y - g_grid_y0;
            if (rel_x >= 0 && rel_y >= 0) {
                int c = rel_x / g_cell_w;
                int r = rel_y / g_cell_h;
                if (c >= 0 && c < 7 && r >= 0) {
                    int day = r * 7 + c - first_wday + 1;
                    if (day >= 1 && day <= dim) {
                        S.sel_day = day;
                        S.day_detail_open = 1;
                        dirty_register(CAL_WIN_ID); return;
                    }
                }
            }
        }
        break;
    }
    case CAL_TAB_ALARM: {
        if (point_in(x, y, ax + 8, ay + 8, aw - 16, 28)) {
            cal_alarm_add(7, 0, "Wake up", CAL_REPEAT_DAILY, CAL_ALARM_PRIORITY_NORMAL);
            dirty_register(CAL_WIN_ID); return;
        }
        int ly = ay + 44;
        for (int i = 0; i < S.alarm_count; i++) {
            if (point_in(x, y, ax + 8, ly, aw - 16, 36)) {
                /* Toggle enabled. */
                cal_alarm_set_enabled(i, !S.alarms[i].enabled);
                dirty_register(CAL_WIN_ID); return;
            }
            ly += 42;
        }
        break;
    }
    case CAL_TAB_TIMER: {
        int cx = ax + aw / 2;
        int cy0 = ay + ah / 2 - 30;
        int by = cy0 + 30;
        int bw = 100, bh = 32, gap = 12;
        int bx = cx - (3 * bw + 2 * gap) / 2;
        for (int i = 0; i < 3; i++) {
            int x0 = bx + i * (bw + gap);
            if (point_in(x, y, x0, by, bw, bh)) {
                if (i == 0) {
                    if (S.timer_state == CAL_TIMER_RUNNING) cal_timer_pause();
                    else { if (S.timer_total_s == 0) cal_timer_set_seconds(60);
                           cal_timer_start(); }
                } else if (i == 1) {
                    cal_timer_reset();
                } else {
                    /* +30s */
                    if (S.timer_state == CAL_TIMER_FIRED) cal_timer_reset();
                    S.timer_remaining_s += 30;
                    S.timer_total_s     += 30;
                    if (S.timer_state == CAL_TIMER_IDLE) S.timer_state = CAL_TIMER_PAUSED;
                }
                dirty_register(CAL_WIN_ID);
                return;
            }
        }
        break;
    }
    case CAL_TAB_STOPWATCH: {
        int cx = ax + aw / 2;
        int by = ay + 60 + 56;
        int bw = 90, bh = 28, gap = 10;
        int bx = cx - (4 * bw + 3 * gap) / 2;
        for (int i = 0; i < 3; i++) {
            int x0 = bx + i * (bw + gap);
            if (point_in(x, y, x0, by, bw, bh)) {
                if (i == 0) { if (S.sw_state == CAL_SW_RUNNING) cal_sw_stop(); else cal_sw_start(); }
                else if (i == 1) cal_sw_lap();
                else cal_sw_reset();
                dirty_register(CAL_WIN_ID);
                return;
            }
        }
        break;
    }
    default: break;
    }
}

static void cal_pump_clicks(void) {
    uint8_t now = mouse_get_buttons();
    /* Rising edge of left button. */
    if ((now & 1) && !(S.prev_buttons & 1)) {
        int mx = mouse_get_x();
        int my = mouse_get_y();
        cal_handle_click(mx, my);
    }
    S.prev_buttons = now;
}

/* ── Open / Close ────────────────────────────────────────────────── */

void cal_open(cal_tab_t tab) {
    if (!s_initialized) cal_init();
    if (S.open) {
        wm_focus_surface(S.surface_id);
        if ((int)tab >= 0 && (int)tab < CAL_TAB_COUNT) S.tab = tab;
        dirty_register(CAL_WIN_ID);
        return;
    }
    if ((int)tab >= 0 && (int)tab < CAL_TAB_COUNT) S.tab = tab;

    int sw = 720, sh = 480;
    int sx = ((int)fb_width()  - sw) / 2;
    int sy = ((int)fb_height() - sh) / 2;
    S.surface_id = wm_create_surface("Calendar + Clock", -1,
                                     sx, sy, sw, sh, cal_draw);
    if (S.surface_id < 0) return;
    S.open = 1;
    wm_focus_surface(S.surface_id);
    dirty_register(CAL_WIN_ID);
}

void cal_close(void) {
    if (!S.open) return;
    if (S.surface_id >= 0) wm_detach_surface(S.surface_id);
    clear_hovers();
    S.surface_id = -1;
    S.open = 0;
}

/* ── Selftest ────────────────────────────────────────────────────── */

void cal_print_selftest_line(void) {
    if (!s_initialized) cal_init();
    int armed = 0;
    for (int i = 0; i < S.alarm_count; i++) if (S.alarms[i].enabled) armed++;
    kputs("Calendar/clock ........ ");
    kput_dec((uint64_t)armed);
    kputs(" alarms armed, ");
    kput_dec((uint64_t)S.event_count);
    kputs(" events, ");
    kput_dec((uint64_t)S.world_count);
    kputs(" world clocks\n");
}

/* ── Shell command handlers ──────────────────────────────────────── */

void cal_cmd_cal(const char *args) {
    (void)args;
    cal_init();
    cal_open(CAL_TAB_CALENDAR);
    kputs("calendar opened\n");
}

void cal_cmd_clock(const char *args) {
    (void)args;
    cal_init();
    cal_open(CAL_TAB_CLOCK);
    kputs("clock opened\n");
}

void cal_cmd_alarm(const char *args) {
    cal_init();
    /* args: "HH:MM <label>"  (label optional) */
    args = skip_ws(args);
    if (!args || !*args) { kputs("usage: alarm HH:MM <label>\n"); return; }
    if (s_len(args) < 5 || args[2] != ':') { kputs("usage: alarm HH:MM <label>\n"); return; }
    uint64_t hh, mm;
    char hb[3] = { args[0], args[1], 0 };
    char mb[3] = { args[3], args[4], 0 };
    if (parse_uint(hb, &hh) < 0 || parse_uint(mb, &mm) < 0) {
        kputs("alarm: bad time\n"); return;
    }
    const char *label = args + 5;
    label = skip_ws(label);
    int idx = cal_alarm_add((int)hh, (int)mm, *label ? label : "alarm",
                            CAL_REPEAT_DAILY, CAL_ALARM_PRIORITY_NORMAL);
    if (idx < 0) { kputs("alarm: full or invalid\n"); return; }
    kputs("alarm armed: ");
    kput_dec(hh); kputs(":"); kput_dec(mm);
    kputs(" \""); kputs(*label ? label : "alarm"); kputs("\"\n");
}

void cal_cmd_alarms(const char *args) {
    (void)args;
    cal_init();
    if (S.alarm_count == 0) { kputs("no alarms\n"); return; }
    for (int i = 0; i < S.alarm_count; i++) {
        cal_alarm_t *a = &S.alarms[i];
        kputs("  ["); kput_dec((uint64_t)i); kputs("] ");
        if (a->hour < 10) kputs("0");
        kput_dec((uint64_t)a->hour); kputs(":");
        if (a->minute < 10) kputs("0");
        kput_dec((uint64_t)a->minute);
        kputs(a->enabled ? "  ON   " : "  off  ");
        kputs(a->repeat_mask == CAL_REPEAT_ONCE ? "once  " :
              a->repeat_mask == CAL_REPEAT_DAILY ? "daily " : "weekly");
        kputs("  \"");
        kputs(a->label);
        kputs("\"\n");
    }
}

void cal_cmd_timer(const char *args) {
    cal_init();
    args = skip_ws(args);
    uint64_t seconds = 60;
    if (args && *args) {
        if (parse_uint(args, &seconds) < 0) {
            kputs("usage: timer <seconds>\n"); return;
        }
    }
    cal_timer_set_seconds((uint32_t)seconds);
    cal_timer_start();
    kputs("timer started for ");
    kput_dec(seconds);
    kputs("s\n");
}

void cal_cmd_stopwatch(const char *args) {
    cal_init();
    args = skip_ws(args);
    if (!args || !*args || s_eq(args, "start")) {
        cal_sw_start(); kputs("stopwatch: started\n"); return;
    }
    if (s_eq(args, "stop")) { cal_sw_stop();  kputs("stopwatch: stopped\n"); return; }
    if (s_eq(args, "lap"))  { cal_sw_lap();
        kputs("stopwatch: lap "); kput_dec((uint64_t)S.sw_lap_count); kputs("\n"); return; }
    if (s_eq(args, "reset")){ cal_sw_reset(); kputs("stopwatch: reset\n"); return; }
    if (s_starts(args, "start")) { cal_sw_start(); kputs("stopwatch: started\n"); return; }
    kputs("usage: stopwatch start|stop|lap|reset\n");
}
