/*
 * Zeos -- OTA Updater
 *
 * The entire OS is one file: BOOTZ.EFI.
 * Updates = download new binary, write to ESP, reboot.
 *
 * No package manager. No dependencies. One file.
 * Update server provides version.txt and BOOTZ.EFI.
 */

#ifndef ZEOS_UPDATER_H
#define ZEOS_UPDATER_H

#include <stdint.h>

typedef enum {
    UPDATE_IDLE,
    UPDATE_CHECKING,
    UPDATE_AVAILABLE,
    UPDATE_DOWNLOADING,
    UPDATE_WRITING,
    UPDATE_DONE,
    UPDATE_ERROR,
} update_state_t;

typedef struct {
    update_state_t state;
    char current_version[32];
    char available_version[32];
    char update_url[256];
    int  progress_pct;
    char error_msg[128];
} updater_state_t;

/* Initialize updater with the running kernel's version string (e.g. "0.1.0") */
void updater_init(const char *current_version);

/* Check update server for a new version.
 * GETs <update_server>/version.txt, compares against current.
 * Returns 0 if check succeeded (even if no update), -1 on error. */
int updater_check(const char *update_server);

/* Download the new BOOTZ.EFI from the update URL.
 * Stores in heap buffer. Tracks progress.
 * Returns 0 on success, -1 on error. */
int updater_download(void);

/* Write downloaded binary to ESP (overwrite /EFI/BOOT/BOOTX64.EFI).
 * Finds ESP via GPT, locates file in FAT32, overwrites, verifies.
 * Returns 0 on success, -1 on error. */
int updater_apply(void);

/* Render update UI panel at (x,y) with dimensions (w,h). */
void updater_draw(int x, int y, int w, int h);

/* Get pointer to updater state (read-only by convention). */
updater_state_t *updater_get_state(void);

#endif /* ZEOS_UPDATER_H */
