/*
 * Zeos — ACPI S3 suspend/resume.
 *
 * suspend_to_ram() orchestrates the full path: quiesce chains, save
 * device state, save CPU state, write SLP_TYPx + SLP_EN to PM1a/PM1b
 * control, wake from RTC alarm or other configured wake source,
 * restore device state, unquiesce chains, return.
 *
 * Per-driver hooks register at init via suspend_register_driver().
 * Each driver provides a save_fn (snapshot register state and any
 * controller config) and a restore_fn (re-init the controller from
 * the snapshot). Hooks run in registration order on save and reverse
 * order on restore.
 */

#ifndef ZEOS_SUSPEND_H
#define ZEOS_SUSPEND_H

#include <stdint.h>

#define SUSPEND_MAX_DRIVERS 16

typedef int  (*suspend_save_fn)(void *cookie);
typedef int  (*suspend_restore_fn)(void *cookie);

/* Register a per-driver save/restore pair. Cookie is passed through
 * to both callbacks. Returns 0 on success, -1 if the table is full. */
int suspend_register_driver(const char *name,
                            suspend_save_fn save,
                            suspend_restore_fn restore,
                            void *cookie);

/* Drop into ACPI S3. If wake_seconds > 0, programs the RTC alarm to
 * wake the system that many seconds in the future. Returns 0 if the
 * full suspend/resume cycle completed, -err if any stage failed.
 *
 * On firmware that doesn't honour our wake vector, this function may
 * never return — the caller should treat that as "the OS image was
 * lost; user will reboot". */
int suspend_to_ram(uint32_t wake_seconds);

/* Entry point chained from the wake stub. Restores CPU state and
 * jumps back into the C-side resume path. Implemented in assembly
 * inside suspend.c. */
void suspend_resume_entry(void);

/* Diagnostics — counts incremented on every full cycle / failure. */
uint32_t suspend_cycle_count(void);
uint32_t suspend_failure_count(void);

#endif /* ZEOS_SUSPEND_H */
