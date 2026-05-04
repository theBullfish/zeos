/*
 * Boot splash. Static wordmark + step + percent. Serial logs continue
 * silently behind the splash. Dismissed once chain_registry_init
 * completes; user sees the scheduler-driven shell from there.
 * Per OS_LITTLE_THINGS.md top-20 #7.
 */

#ifndef ZEOS_SPLASH_H
#define ZEOS_SPLASH_H

#include <stdint.h>

/*
 * Initialize splash. Paints gradient + wordmark to GOP framebuffer
 * and engages kprint splash mode (serial-only text output).
 *
 * Returns 1 if splash is active, 0 if no usable framebuffer
 * (caller should use serial-only fallback in that case).
 */
int splash_init(void);

/*
 * Update progress line beneath the wordmark.
 *   step: short label ("PCI", "ACPI MADT", ...). NULL => keep last.
 *   pct:  0..100. Clamped.
 *
 * No-op when splash is not active.
 */
void splash_progress(const char *step, int pct);

/*
 * Dismiss splash. Clears framebuffer to surface color, releases
 * kprint splash mode (fb+serial output resumes), and resets the
 * fb text cursor to row 0.
 */
void splash_dismiss(void);

/*
 * Query: is the splash currently active?
 */
int splash_active(void);

#endif /* ZEOS_SPLASH_H */
