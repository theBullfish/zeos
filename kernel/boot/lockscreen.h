/*
 * Zeos -- Lock screen overlay.
 *
 * Idle-driven lock screen. Input timestamps drive state transitions
 * across ACTIVE / DIMMED / LOCKED / BLANKED. PIN-only initially;
 * password / biometrics are future work.
 */

#ifndef ZEOS_LOCKSCREEN_H
#define ZEOS_LOCKSCREEN_H

#include <stdint.h>

void lockscreen_init(void);

/* Becomes the active overlay -- consumes input until PIN matches. */
void lockscreen_show(void);

/* Reset to clean state but stay shown (used by IDLE -> LOCKED on a
 * BLANKED -> input transition). */
void lockscreen_repaint(void);

/* Consume one printable input character. Drives the PIN entry state
 * machine. '\b' deletes one digit, '\n' validates. */
void lockscreen_input(char c);

/* Non-zero while the overlay is the active modal. */
int  lockscreen_active(void);

/* Paint the overlay over the current framebuffer. Called from the
 * compositor's post-overlay hook when active. */
void lockscreen_draw(void);

/* Returns 1 if a PIN is currently configured in VAULT. Used by the
 * selftest summary line. */
int  lockscreen_pin_configured(void);

/* Replace the stored PIN with `new_pin` (digits only, 4..16 chars).
 * Persists to VAULT under /lock/pin. Returns 0 on success. */
int  lockscreen_set_pin(const char *new_pin);

/* Counters for selftest / inspector. */
uint32_t lockscreen_failed_attempts(void);
uint32_t lockscreen_unlock_count(void);

#endif /* ZEOS_LOCKSCREEN_H */
