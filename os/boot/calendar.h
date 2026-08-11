/*
 * Zeos -- Calendar + Clock App
 *
 * Five tabs: month-grid calendar, analog/digital clock with world
 * clocks, alarms, countdown timer, stopwatch with laps.
 *
 * Persistence:
 *   /calendar/events       -- packed list of {tod_unix, label[64]}
 *   /calendar/alarms       -- packed list of {hour, minute, label[48],
 *                                              repeat_mask, enabled}
 *   /clock/world_clocks    -- packed list of {label[24], tz_offset_min}
 *
 * Pipeline (CHAIN_CLOCK, parent CHAIN_CPU, MASQ_INTERNAL):
 *   tick_request -> time_advance -> alarm_check -> timer_check -> emit_events
 *
 * Alarm fires through notify_send() with NOTIFY_INFO (default) or
 * NOTIFY_CRITICAL (label prefixed with '!'); CRITICAL bypasses DND
 * per the notify history+DND landing.
 */

#ifndef ZEOS_CALENDAR_H
#define ZEOS_CALENDAR_H

#include <stdint.h>

/* ── Limits ─────────────────────────────────────────────────────── */
#define CAL_MAX_EVENTS        64
#define CAL_MAX_ALARMS        16
#define CAL_MAX_WORLD_CLOCKS   8
#define CAL_MAX_LAPS          32

#define CAL_LABEL_MAX         64
#define CAL_ALARM_LABEL_MAX   48
#define CAL_TZ_LABEL_MAX      24

/* Repeat mask bits for alarms.  ONCE = 0 (cleared after fire).        */
#define CAL_REPEAT_ONCE        0u
#define CAL_REPEAT_DAILY       0x7Fu                /* all 7 days */
#define CAL_REPEAT_WEEKDAY(d)  (1u << (d))          /* d in 0..6, 0=Sun */
#define CAL_REPEAT_WEEKLY      CAL_REPEAT_DAILY     /* alias; weekly == daily for now */

/* Priority ranks an alarm as critical (bypasses DND). */
#define CAL_ALARM_PRIORITY_CRITICAL  1
#define CAL_ALARM_PRIORITY_NORMAL    0

/* ── Records (also the on-disk layout) ──────────────────────────── */

typedef struct {
    uint64_t tod_unix;                  /* When the event fires/lives */
    char     label[CAL_LABEL_MAX];
} cal_event_t;

typedef struct {
    uint8_t  hour;                      /* 0..23 */
    uint8_t  minute;                    /* 0..59 */
    uint8_t  enabled;
    uint8_t  priority;                  /* CAL_ALARM_PRIORITY_* */
    uint32_t repeat_mask;               /* bit per weekday (0=Sun..6=Sat) */
    uint64_t last_fired_unix;           /* Dedup so we don't refire same minute */
    char     label[CAL_ALARM_LABEL_MAX];
} cal_alarm_t;

typedef struct {
    int32_t  tz_offset_minutes;
    char     label[CAL_TZ_LABEL_MAX];
} cal_world_clock_t;

typedef enum {
    CAL_TIMER_IDLE,
    CAL_TIMER_RUNNING,
    CAL_TIMER_PAUSED,
    CAL_TIMER_FIRED,
} cal_timer_state_t;

typedef enum {
    CAL_SW_IDLE,
    CAL_SW_RUNNING,
    CAL_SW_PAUSED,
} cal_stopwatch_state_t;

typedef enum {
    CAL_TAB_CALENDAR = 0,
    CAL_TAB_CLOCK    = 1,
    CAL_TAB_ALARM    = 2,
    CAL_TAB_TIMER    = 3,
    CAL_TAB_STOPWATCH= 4,
    CAL_TAB_COUNT    = 5,
} cal_tab_t;

/* ── Lifecycle ──────────────────────────────────────────────────── */

/* Initialize state; loads VAULT keys. Idempotent. */
void cal_init(void);

/* Open the app window (or focus if already open). Optionally select tab. */
void cal_open(cal_tab_t tab);
void cal_close(void);

/* Selftest line:
 *   "Calendar/clock ........ N alarms armed, M events, K world clocks\n" */
void cal_print_selftest_line(void);

/* ── Chain (CHAIN_CLOCK) ─────────────────────────────────────────
 * Registered against parent CHAIN_CPU (typically), MASQ_INTERNAL.
 * resolve_interval_ticks is set to ~1 sec at the scheduler's tick
 * resolution. */
int  cal_chain_register(int parent_chain_id);
extern int CHAIN_CLOCK;

/* ── Storage / VAULT helpers ────────────────────────────────────── */

int  cal_event_add(uint64_t tod_unix, const char *label);
int  cal_event_remove(int idx);
int  cal_event_count(void);
const cal_event_t *cal_event_get(int idx);

int  cal_alarm_add(int hour, int minute, const char *label,
                   uint32_t repeat_mask, int priority);
int  cal_alarm_remove(int idx);
void cal_alarm_set_enabled(int idx, int enabled);
int  cal_alarm_count(void);
const cal_alarm_t *cal_alarm_get(int idx);

int  cal_world_clock_add(const char *label, int tz_offset_minutes);
int  cal_world_clock_remove(int idx);
int  cal_world_clock_count(void);
const cal_world_clock_t *cal_world_clock_get(int idx);

/* ── Timer ─────────────────────────────────────────────────────── */
void              cal_timer_set_seconds(uint32_t total_seconds);
void              cal_timer_start(void);
void              cal_timer_pause(void);
void              cal_timer_reset(void);
uint32_t          cal_timer_remaining(void);
cal_timer_state_t cal_timer_state(void);

/* ── Stopwatch ─────────────────────────────────────────────────── */
void                  cal_sw_start(void);
void                  cal_sw_stop(void);
void                  cal_sw_lap(void);
void                  cal_sw_reset(void);
uint32_t              cal_sw_elapsed_ms(void);
int                   cal_sw_lap_count(void);
uint32_t              cal_sw_lap_ms(int idx);
cal_stopwatch_state_t cal_sw_state(void);

/* ── Shell handlers ─────────────────────────────────────────────── */
void cal_cmd_cal(const char *args);
void cal_cmd_clock(const char *args);
void cal_cmd_alarm(const char *args);
void cal_cmd_alarms(const char *args);
void cal_cmd_timer(const char *args);
void cal_cmd_stopwatch(const char *args);

#endif /* ZEOS_CALENDAR_H */
