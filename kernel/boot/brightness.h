/*
 * Zeos — display backlight control via ACPI _BCL/_BCM/_BQC.
 *
 * Pattern-scans DSDT/SSDT for a `Name(_BCL, Package(...))` declaration
 * on the display device, exposes the supported level set, and provides
 * a settable API that writes to `_BCM` when the firmware inlined a
 * targetable Method/Name. Many BIOSes route _BCM through SMI which we
 * cannot drive from kernel mode without an AML interpreter; in that
 * case we report queryable levels but `settable=0` and persist the
 * desired level for future use.
 *
 * Native GPU PWM (Intel BLC_PWM_CTL @ 0x48254/0x61254, AMD
 * DC_BL_PWM_CTL) is per-vendor and blocked on real-vendor GPU drivers.
 */

#ifndef ZEOS_BRIGHTNESS_H
#define ZEOS_BRIGHTNESS_H

#include <stdint.h>

#define BRIGHTNESS_MAX_LEVELS 32

void brightness_init(void);

/* 1 if a backlight surface was discovered (_BCL package found). */
int brightness_present(void);

/* Returns count of supported levels written to `out` (capped at `max`).
 * Levels are the raw values from the _BCL Package, sorted ascending,
 * with the first two entries (AC max / battery max policy hints) skipped
 * per ACPI spec. Returns 0 when no backlight present. */
int brightness_levels(int *out, int max);

/* Current level, normalized 0..100 over the discovered range. -1 if
 * unknown / no backlight. */
int brightness_get(void);

/* Set the requested level (0..100). Snaps to the nearest supported
 * raw level, persists the desired value to /display/brightness, and
 * attempts to drive _BCM. Returns 0 on hardware change, 1 if persisted
 * only (no settable path), -1 on no backlight. */
int brightness_set(int level);

/* Step relative (clamped to 0..100). */
int brightness_step(int delta);

/* Persist current level to VAULT (/display/brightness). */
int brightness_save_to_vault(void);

/* Restore from VAULT. Returns 0 on success, -1 on miss/mismatch. */
int brightness_restore_from_vault(void);

#endif /* ZEOS_BRIGHTNESS_H */
