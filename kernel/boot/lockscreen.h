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

/* Forget the stored PIN (deletes /lock/pin). Forces re-enrollment on
 * next boot. Used by `pin reset`. */
int  lockscreen_pin_clear(void);

/* Counters for selftest / inspector. */
uint32_t lockscreen_failed_attempts(void);
uint32_t lockscreen_unlock_count(void);

/* Number of CFA handles currently held by the lockscreen (3 once
 * pin_wrap_handles has run: stored PIN + entry buffer + enroll-first
 * confirm buffer). Selftest reads this to verify the SOVEREIGN-tier
 * wrap actually happened. */
int      lockscreen_cfa_handle_count(void);

/* Copy the stored PIN bytes into `out` (NUL-terminated, max bytes
 * including the terminator). Returns the digit count (>= LOCK_PIN_MIN_LEN
 * on success), or 0 if no PIN is configured / output buffer too small.
 * Used by crypto_disk to derive the SOVEREIGN AES-XTS key at boot. The
 * caller MUST cd_memzero the buffer immediately after KDF. */
int      lockscreen_pin_copy(char *out, int max);

/* ── Cold-boot login ─────────────────────────────────────────────
 *
 * The same PIN overlay runs as a login gate at boot when
 * /lock/cold-boot-required is set. If no PIN is configured yet, the
 * gate runs an enrollment flow ("set a PIN" + confirm) and stores
 * the result before allowing the shell to come up.
 */

/* Read /lock/cold-boot-required from VAULT. Default 1 (require PIN
 * at boot) when the key is missing. */
int  cold_boot_login_required(void);

/* Persist /lock/cold-boot-required. enabled != 0 stores 1, else 0. */
int  cold_boot_login_set(int enabled);

/* Synchronous boot-time gate. Shows the lock overlay (or the
 * enrollment flow on first ever boot), pumps input + composes
 * frames, and only returns once the user has unlocked or completed
 * enrollment. Safe to call before scheduler_run. */
void lockscreen_run_cold_boot_gate(void);

#endif /* ZEOS_LOCKSCREEN_H */
