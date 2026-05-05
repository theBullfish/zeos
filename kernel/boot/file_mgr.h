/*
 * Zeos -- File Manager (stream renderer over CHAIN_FS_EVENT)
 *
 * UI is a renderer over the chain. It does NOT poll the filesystem.
 * It registers an fs_event listener and repaints when the listener
 * fires for a path inside the currently displayed directory.
 */

#ifndef ZEOS_FILE_MGR_H
#define ZEOS_FILE_MGR_H

#include <stdint.h>

#define FM_WIN_ID         5152
#define FM_PATH_MAX       256
#define FM_ENTRIES_MAX    128
#define FM_RECENT_MAX     8
#define FM_HISTORY_MAX    32

/* Open the file manager at the given path (or NULL for default). */
int  file_mgr_open(const char *path);
void file_mgr_close(void);
int  file_mgr_active(void);

/* Input dispatch */
int  file_mgr_key(int ascii);
int  file_mgr_key_arrow(int dir);
int  file_mgr_mouse_down(int x, int y, int button);
int  file_mgr_mouse_move(int x, int y);
int  file_mgr_mouse_up(int x, int y, int button);

/* Render */
void file_mgr_draw(void);

/* Tick (lightweight: no polling; just spring/animation if any). */
void file_mgr_tick(void);

/* Persistence */
void file_mgr_persist_save(void);
void file_mgr_persist_restore(void);

/* Selftest */
void file_mgr_print_selftest_line(void);
uint32_t file_mgr_total_listings(void);
uint32_t file_mgr_total_events_seen(void);

/* Shell command entrypoint: `fm [path]` */
void file_mgr_cmd(const char *args);

#endif /* ZEOS_FILE_MGR_H */
