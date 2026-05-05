/*
 * Zeos — WM per-app per-CFA-context persistence.
 *
 * Apps register a (get_state, apply_state) pair under a stable name.
 * The registry walks all apps and persists their state into VAULT
 * under "/wm/<ctx_id>/<app_name>" as a versioned, CRC-protected blob.
 *
 * Triggers (the WM owns them; apps don't poll):
 *   - On window close (wm_detach_surface) → save_for(app)
 *   - 60s tick from wm_persist_tick() (called from wm_draw_all loop)
 *   - On window open → restore(app)
 *
 * Each app stores a flat blob: { x, y, w, h, sort_col, sort_dir,
 * filter_str[64], theme_idx, scroll_pos }. Apps that don't use a
 * particular field zero it.
 */

#ifndef ZEOS_WM_PERSIST_H
#define ZEOS_WM_PERSIST_H

#include <stdint.h>

#define WM_PERSIST_NAME_MAX     32
#define WM_PERSIST_FILTER_MAX   64
#define WM_PERSIST_MAX_APPS     16
#define WM_PERSIST_BLOB_VERSION 1u

/* Wire-stable on-disk record. */
typedef struct {
    uint32_t version;       /* WM_PERSIST_BLOB_VERSION */
    uint32_t crc32;         /* over the rest of the struct (= 0 when computing) */
    int32_t  x, y, w, h;
    int32_t  sort_col;
    int32_t  sort_dir;
    int32_t  theme_idx;
    int32_t  scroll_pos;
    char     filter_str[WM_PERSIST_FILTER_MAX];
    uint32_t reserved[4];
} wm_persist_blob_t;

/* App-side getter / setter callbacks. The blob is zeroed before
 * `get` is called; `apply` is invoked with a validated blob. */
typedef void (*wm_persist_get_fn)(wm_persist_blob_t *out);
typedef void (*wm_persist_apply_fn)(const wm_persist_blob_t *in);

/* Register an app. Idempotent (re-registration replaces the entry
 * for the same name). Returns 0 on success, -1 on table full. */
int  wm_persist_register(const char *app_name,
                         wm_persist_get_fn get,
                         wm_persist_apply_fn apply);

/* Save state for one app right now. Returns 0 on success. */
int  wm_persist_save(const char *app_name);

/* Save state for every registered app. */
void wm_persist_save_all(void);

/* Load state for one app and call its apply callback. Returns 0
 * if a valid blob was found and applied, -1 otherwise. */
int  wm_persist_restore(const char *app_name);

/* Periodic tick. Cheap; checks if 60s have elapsed since the last
 * full save and runs save_all if so. Safe to call every frame. */
void wm_persist_tick(void);

/* Selftest: count of registered apps. */
uint32_t wm_persist_app_count(void);
uint32_t wm_persist_total_saves(void);
uint32_t wm_persist_total_restores(void);

#endif /* ZEOS_WM_PERSIST_H */
