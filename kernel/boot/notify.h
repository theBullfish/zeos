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
 */

#ifndef ZEOS_NOTIFY_H
#define ZEOS_NOTIFY_H

#include <stdint.h>

#define NOTIFY_MAX       16
#define NOTIFY_MAX_TEXT  128

typedef enum {
    NOTIFY_INFO,
    NOTIFY_SUCCESS,
    NOTIFY_WARNING,
    NOTIFY_ERROR,
    NOTIFY_CRITICAL,    /* Bypasses Focus Mode */
} notify_level_t;

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

#endif /* ZEOS_NOTIFY_H */
