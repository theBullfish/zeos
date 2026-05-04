/*
 * Zeos — Notification System
 *
 * Batched by default (spec: 15-30 min production, 30s alpha).
 * Never interrupts fullscreen. Focus Mode suppresses non-critical.
 * CRITICAL level bypasses Focus Mode.
 *
 * Toast: top-right, 300px wide, 64px tall, spring slide in/out.
 * Level-colored left border (accent/green/amber/red).
 *
 * Wire notify_tick() and notify_draw() into compositor_frame()
 * as an overlay layer.
 *
 * History: 256-entry ring persisted to VAULT, replayable across reboots.
 * DND: off / on / scheduled (start_hr..end_hr UTC). Critical bypasses.
 * Per-source mute list. All decisions go through notify_should_silence().
 */

#ifndef ZEOS_NOTIFY_H
#define ZEOS_NOTIFY_H

#include <stdint.h>

#define NOTIFY_MAX            16
#define NOTIFY_MAX_TEXT      128
#define NOTIFY_HISTORY_CAP   256
#define NOTIFY_MUTE_MAX        8
#define NOTIFY_SOURCE_MAX     32
#define NOTIFY_TITLE_MAX      48
#define NOTIFY_BODY_MAX      128

typedef enum {
    NOTIFY_INFO,
    NOTIFY_SUCCESS,
    NOTIFY_WARNING,
    NOTIFY_ERROR,
    NOTIFY_CRITICAL,    /* Bypasses Focus Mode AND DND */
} notify_level_t;

typedef enum {
    DND_OFF      = 0,
    DND_ON       = 1,
    DND_SCHEDULE = 2,
} dnd_mode_t;

typedef struct {
    char            text[NOTIFY_MAX_TEXT];
    char            source[32];         /* Which chain sent this */
    notify_level_t  level;
    uint64_t        timestamp;
    int             read;               /* User has seen it */
    int             visible;            /* Currently showing on screen */
    float           slide_x;            /* Spring animation position */
    int             anim_id;
} notification_t;

/* Persistent history record. Stored in a 256-entry ring at /notify/history. */
typedef struct {
    uint64_t        tod_unix;       /* Wall-clock seconds (0 if tod not ready) */
    uint32_t        level;          /* notify_level_t */
    char            source[NOTIFY_SOURCE_MAX];
    char            title[NOTIFY_TITLE_MAX];
    char            body[NOTIFY_BODY_MAX];
    uint8_t         dismissed;
    uint8_t         was_silenced;   /* DND suppressed the toast */
    uint8_t         _pad[2];
} notification_record_t;

typedef struct {
    notification_t  queue[NOTIFY_MAX];
    int             count;
    int             unread;
    int             batch_mode;         /* 1 = batch (default), 0 = immediate */
    uint64_t        last_batch_tsc;     /* When we last showed batched notifs */
    int             showing;            /* Currently displaying a notification */
    int             show_index;         /* Which notification is visible */
    int             panel_open;         /* Notification list panel visible */
    uint64_t        show_start_tsc;     /* When current toast appeared */
} notify_state_t;

/* Initialize the notification system */
void notify_init(void);

/* Queue a notification */
void notify_send(const char *text, const char *source, notify_level_t level);

/* Called per frame -- handles timing and animation */
void notify_tick(void);

/* Render visible notifications (overlay layer) */
void notify_draw(void);

/* Dismiss current notification */
void notify_dismiss(void);

/* Toggle notification list panel (top-right corner action) */
void notify_show_all(void);

/* Number of unread notifications */
int notify_unread_count(void);

/* ── History API ───────────────────────────────────────────────────── */

/* Number of entries currently in the persistent history ring (0..256). */
int  notify_history_count(void);

/* Fetch a history entry. idx=0 is newest. Returns 0 on success, -1 OOB. */
int  notify_history_get(int idx, notification_record_t *out);

/* Wipe the history ring (both RAM and VAULT). */
void notify_history_clear(void);

/* ── DND API ───────────────────────────────────────────────────────── */

/* Returns 1 if a notification of `level` from `source` should be silenced
 * (toast suppressed). Critical never silenced. Per-source whitelist bypass. */
int  notify_should_silence(notify_level_t level, const char *source);

dnd_mode_t notify_dnd_mode(void);
void       notify_dnd_set_mode(dnd_mode_t m);
int        notify_dnd_start_hour(void);
int        notify_dnd_end_hour(void);
void       notify_dnd_set_window(int start_hour, int end_hour);

/* Per-source mute list. Sources here BYPASS DND (always render).
 * Returns 0 on success, -1 if list full or duplicate. */
int  notify_mute_add(const char *source);
int  notify_mute_remove(const char *source);
int  notify_mute_count(void);
const char *notify_mute_get(int idx);  /* NULL on OOB */

/* Selftest line printer. Format:
 *   "Notify ................ N history entries, DND=off\n" */
void notify_print_selftest_line(void);

/* Register CHAIN_NOTIFY (parent CHAIN_CPU, MASQ_INTERNAL) with the
 * 4-node pipeline:
 *   notify_emit_request -> dnd_filter -> history_sink -> toast_render
 * Returns the new chain id, or -1 on failure. Idempotent -- subsequent
 * calls return the existing id. */
int  notify_chain_register(int parent_chain_id);

/* Lifetime count of notifications that flowed through the chain
 * (whether silenced or rendered). Selftest reads this to prove the
 * pipeline is alive. */
uint32_t notify_chain_emitted_total(void);

/* ── Shell command implementations ──────────────────────────────────
 * Wired internally; need entries in shell.c's command table. */
void notify_cmd_history(const char *args);
void notify_cmd_dnd(const char *args);
void notify_cmd_send(const char *args);

#endif /* ZEOS_NOTIFY_H */
