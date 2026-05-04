/*
 * Zeos — Undo / Redo Ring Buffer
 *
 * Per-text-field undo/redo. Each undo_buffer_t owns a 64-entry ring
 * of edit records (position, deleted_text, inserted_text, timestamp).
 * Apps with editable text register a buffer, call undo_record() on
 * every edit, and undo_perform() / undo_redo() apply edits in reverse
 * by mutating the host text via callback.
 *
 * Doctrine: undo lives next to the text it edits. There is no global
 * undo stack — every editable widget has its own ring, and Ctrl-Z
 * routes to whichever buffer has focus.
 */

#ifndef ZEOS_UI_UNDO_H
#define ZEOS_UI_UNDO_H

#include <stdint.h>

#define UI_UNDO_RING_SIZE   64
#define UI_UNDO_TEXT_MAX    64

typedef struct {
    int      position;                  /* Where the edit happened (char index) */
    char     deleted[UI_UNDO_TEXT_MAX]; /* What was removed (NUL-terminated) */
    char     inserted[UI_UNDO_TEXT_MAX];/* What was inserted (NUL-terminated) */
    uint64_t timestamp;
    int      used;
} undo_record_t;

/* Apply callback: replaces text from [pos..pos+old_len) with new_text.
 * Host implements this — it knows the actual storage. */
typedef void (*undo_apply_fn)(void *ctx,
                              int pos,
                              const char *old_text, int old_len,
                              const char *new_text, int new_len);

typedef struct {
    undo_record_t  ring[UI_UNDO_RING_SIZE];
    int            head;        /* Next write slot */
    int            count;       /* Records held (0..RING_SIZE) */
    int            cursor;      /* Where redo continues from */
    undo_apply_fn  apply;       /* Host callback */
    void          *ctx;         /* Host context for apply */
    int            in_transaction; /* Suppress recording during apply */
} undo_buffer_t;

/* Initialize a buffer. apply may be NULL — undo_perform becomes a no-op
 * but the ring still records (useful for testing). */
void undo_init(undo_buffer_t *buf, undo_apply_fn apply, void *ctx);

/* Begin a logical edit boundary (clears redo tail). */
void undo_begin(undo_buffer_t *buf);

/* Record an edit: at position `pos`, the string `deleted` (may be empty)
 * was replaced with `inserted` (may be empty). Both NUL-terminated. */
void undo_record(undo_buffer_t *buf, int pos,
                 const char *deleted, const char *inserted);

/* Apply most recent edit in reverse. Returns 1 if performed, 0 if empty. */
int  undo_perform(undo_buffer_t *buf);

/* Re-apply the next edit forward. Returns 1 if performed, 0 if empty. */
int  undo_redo(undo_buffer_t *buf);

/* Currently-focused buffer for global Ctrl-Z routing. NULL = none. */
void           undo_set_focus(undo_buffer_t *buf);
undo_buffer_t *undo_get_focus(void);

/* Process a Ctrl-Z / Ctrl-Shift-Z / Ctrl-Y key press from the keyboard
 * layer. If a focused buffer exists, swallows the event and returns 1. */
int  undo_handle_key(int ascii, int shift_held);

/* Counters for selftest. */
uint32_t undo_total_records(void);
uint32_t undo_total_undos(void);
uint32_t undo_total_redos(void);

#endif /* ZEOS_UI_UNDO_H */
