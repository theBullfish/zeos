/*
 * Zeos — chat-zeos E2EE: real per-room group key derivation + AES-XTS
 * body encryption.
 *
 * Group key (32 bytes):
 *   K_room = HMAC-SHA256(salt, sorted(member_ctx[]) || room_id)
 *
 * salt is 32 random bytes generated at e2ee_enable time and persisted to
 * vault under /chat/<room>/e2ee/salt at SOVEREIGN tier. K_room is held
 * in memory only — every member with the same membership set + the
 * persisted salt can re-derive it locally; non-members cannot.
 *
 * Body encryption:
 *   AES-XTS-256 with key = K_room || HMAC-SHA256(K_room, "tweak-key").
 *   Sector size = max body length per call (we operate on a single
 *   "sector" per encrypt). The "sector index" / tweak is the message
 *   sequence number — provided by the caller.
 *
 * On membership change (join/leave), the room owner calls
 * chat_e2ee_rederive(room) to re-derive K_room from the new sorted
 * member list. Old ciphertexts remain decryptable as long as the salt
 * stays put; we keep history readable.
 */

#ifndef ZEOS_CHAT_E2EE_H
#define ZEOS_CHAT_E2EE_H

#include <stdint.h>
#include <stddef.h>

/* Initialize on first use. Idempotent. */
void chat_e2ee_init(void);

/* Enable E2EE for a room. Generates a salt if none exists, persists it
 * to vault, and derives K_room from the current member set. Returns 0
 * on success, -1 if the room is unknown or salt persistence failed. */
int  chat_e2ee_enable(const char *room_id);

/* Disable E2EE for a room. Drops K_room from memory and removes the
 * salt from vault. Returns 0 on success. */
int  chat_e2ee_disable(const char *room_id);

/* True if E2EE is currently active for the room (key resident). */
int  chat_e2ee_is_active(const char *room_id);

/* Re-derive K_room. Call after a member join/leave. */
int  chat_e2ee_rederive(const char *room_id);

/* Encrypt `body_len` bytes of `body` into `out` (which must be at least
 * body_len bytes). Returns the number of bytes written or -1 on error.
 * `seq` is the message sequence number (used as the AES-XTS tweak). */
int  chat_e2ee_encrypt(const char *room_id, uint64_t seq,
                       const uint8_t *body, size_t body_len,
                       uint8_t *out);

/* Decrypt `body_len` bytes of `body` into `out`. Returns bytes written
 * or -1 on error. */
int  chat_e2ee_decrypt(const char *room_id, uint64_t seq,
                       const uint8_t *body, size_t body_len,
                       uint8_t *out);

/* Number of rooms with E2EE currently active (selftest). */
int  chat_e2ee_active_count(void);

#endif /* ZEOS_CHAT_E2EE_H */
