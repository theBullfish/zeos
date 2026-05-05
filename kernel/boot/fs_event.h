/*
 * Zeos -- Filesystem event chain
 *
 * Every FS mutation in the system emits a typed `fs_event` signal.
 * Consumers subscribe via MDE type-routing:
 *   - File manager UI (refreshes directory listing on relevant events)
 *   - Trash GC (CHAIN_FS_TRASH_REACT)
 *   - Search index (CHAIN_FS_INDEX, stub: counts events per directory)
 *   - Universal undo (CHAIN_FS_UNDO, 32-entry reverse op ring)
 *   - Notify (CHAIN_FS_NOTIFY, large-delete + low-disk warnings)
 *
 * The "manager" is just one of many consumers. masq_journal already
 * records BLOCK-level writes; CHAIN_FS_EVENT is the higher-level
 * file-shaped record that named chains operate against.
 *
 * Pipeline (CHAIN_FS_EVENT):
 *   fs_request -> validate -> execute -> emit_event   (output: fs_event)
 *
 * Public emit entrypoint: fs_event_emit(kind, path, size, prior_kind).
 * Every fat32_* mutation calls this on success-return; subscribers
 * see the same event regardless of who initiated the mutation.
 */

#ifndef ZEOS_FS_EVENT_H
#define ZEOS_FS_EVENT_H

#include <stdint.h>

#define FS_EVENT_PATH_MAX 256

typedef enum {
    FS_EV_NONE = 0,
    FS_EV_CREATE,        /* file created (zero size) */
    FS_EV_WRITE,         /* file content changed; size = new size */
    FS_EV_TRUNCATE,
    FS_EV_UNLINK,        /* hard delete */
    FS_EV_MKDIR,
    FS_EV_RMDIR,         /* dir removed (via unlink on dir) */
    FS_EV_TRASH,         /* soft-delete; path = original, new_path = trash id */
    FS_EV_RESTORE,
    FS_EV_RENAME,
} fs_event_kind_t;

typedef struct {
    int             kind;                   /* fs_event_kind_t */
    char            path[FS_EVENT_PATH_MAX];
    char            new_path[FS_EVENT_PATH_MAX]; /* rename / trash id */
    uint32_t        size;
    uint64_t        timestamp;              /* tod_now_unix; 0 if tod not ready */
    uint64_t        tsc;                    /* TSC at emit */
    int             prior_kind;             /* For undo: what op this reverses */
    int             seq;                    /* monotonic */
    int             valid;
} fs_event_t;

typedef struct {
    int             kind;
    const char     *path;
    const char     *new_path;
    const void     *buf;
    uint32_t        size;
    uint32_t        offset;
} fs_request_t;

/* ── Chain ids (set during fs_event_chain_register) ──────────────── */
extern int CHAIN_FS_EVENT;
extern int CHAIN_FS_TRASH_REACT;
extern int CHAIN_FS_INDEX;
extern int CHAIN_FS_UNDO;
extern int CHAIN_FS_NOTIFY;

/* Register all chains under parent_chain_id. Returns total subscriber
 * chains registered (0..4) plus 1 for the emitter, or -1 on failure. */
int fs_event_chain_register(int parent_chain_id);

/* Emit an fs_event. Called from inside fat32_* functions on success-
 * return. Builds fs_request, runs validate -> execute (no-op here
 * since the underlying mutation already completed; the chain shape is
 * the record) -> emit_event. Synchronously fans out to subscribers. */
void fs_event_emit(int kind, const char *path, uint32_t size,
                   int prior_kind);

void fs_event_emit_rename(const char *old_path, const char *new_path,
                          uint32_t size);

/* Trash: path = original, id = trash subdir name (used by undo to
 * restore). Records as FS_EV_TRASH. */
void fs_event_emit_trash(const char *path, const char *id, uint32_t size);

/* Restore: emits FS_EV_RESTORE with path=original-path (where the
 * file is now), id used as a hint (may be empty). */
void fs_event_emit_restore(const char *path, const char *id, uint32_t size);

/* ── UI listener (non-chain, for file manager live-refresh) ──────── */
typedef void (*fs_event_listener_fn)(const fs_event_t *e, void *ctx);
int  fs_event_register_listener(fs_event_listener_fn fn, void *ctx);

/* ── Universal undo ──────────────────────────────────────────────── */
/* Reverse the most-recently-recorded op. Returns 0 on success,
 * -1 if the ring is empty or the op isn't reversible. */
int  fs_event_undo_pop(void);
int  fs_event_undo_count(void);

/* ── Selftest counters ───────────────────────────────────────────── */
uint32_t fs_event_total_emits(void);
uint32_t fs_event_total_trash_reacts(void);
uint32_t fs_event_total_index_updates(void);
uint32_t fs_event_total_undo_records(void);
uint32_t fs_event_total_notify_fires(void);

#endif /* ZEOS_FS_EVENT_H */
