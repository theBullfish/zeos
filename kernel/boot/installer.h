/*
 * Zeos -- Installer (Chain Surface)
 *
 * Takes Z-OS from "running live on USB" to "installed on internal drive."
 * Eight-step flow: welcome, select disk, confirm, partition, copy,
 * migrate VAULT, boot order, done.
 *
 * Runs as a blocking chain surface. Renders with fb_rect/fb_text
 * using theme colors. Progress bar fills with persona accent.
 */

#ifndef ZEOS_INSTALLER_H
#define ZEOS_INSTALLER_H

#include <stdint.h>

typedef enum {
    INSTALL_STEP_WELCOME,
    INSTALL_STEP_SELECT_DISK,
    INSTALL_STEP_CONFIRM,
    INSTALL_STEP_PARTITION,
    INSTALL_STEP_COPY,
    INSTALL_STEP_MIGRATE,
    INSTALL_STEP_BOOTORDER,
    INSTALL_STEP_DONE,
} install_step_t;

typedef struct {
    install_step_t step;
    int target_disk;        /* Which NVMe namespace to install to */
    uint64_t disk_size;     /* In blocks */
    uint32_t block_size;
    int confirmed;
    int progress_pct;       /* 0-100 for copy step */
    char status_msg[128];
} installer_state_t;

/* Run the installer (blocks until complete or cancelled). Returns 0 on success. */
int installer_run(void);

/* Draw installer UI (for chain surface integration) */
void installer_draw(int x, int y, int w, int h);

#endif /* ZEOS_INSTALLER_H */
