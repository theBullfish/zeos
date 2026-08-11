/*
 * Zeos — chat-zeos polish layer (Slack / Discord killer)
 *
 * Three-pane layout:
 *   [ Rooms list  | Messages center                | Members ]
 *
 * Bubbles with author initials in colored circles, relative timestamps
 * ("2 min ago"), reaction emoji row (placeholder; reactions come in v2),
 * unread badges. Cmd-K palette to switch rooms/search.
 *
 * Security:
 *   - Rooms scoped to CFA identity contexts (already enforced by
 *     chat_zeos_room_visible_to). Lock icon next to E2EE rooms.
 *   - Per-room E2EE: chat_polish_set_e2ee(room) flips a flag; the
 *     polish layer wraps the body in a SOVEREIGN-tier CFA handle and
 *     emits an HMAC-derived per-room key (group key over member CFA
 *     roots). Decrypt happens at render time inside the active ctx.
 *
 * Honest gaps:
 *   - The actual AES-XTS body encryption is wired through the existing
 *     mbedTLS path (commit b618562); the polish module owns the room→key
 *     derivation but the per-message encrypt/decrypt round-trip is
 *     gated behind the same TLS-conf SOVEREIGN handle the disk crypto
 *     uses. Until member-context-shared-secret derivation lands, E2EE
 *     mode displays the lock icon and refuses to send (no fake polish).
 *   - Drag-drop file attach uploads via fs_event but the desktop drop
 *     dispatcher doesn't yet emit the right MIME on this window —
 *     `chat attach <room> <path>` is the manual path until then.
 *   - Markdown in messages: same compact renderer as notes_polish (re-
 *     used; see notes_polish.c).
 *
 * Selftest line:
 *   chat-zeos polish ..... 3-pane UI ready, N rooms visible to active ctx
 */

#ifndef ZEOS_CHAT_POLISH_H
#define ZEOS_CHAT_POLISH_H

#include <stdint.h>

void chat_polish_init(void);
void chat_polish_open(void);
void chat_polish_close(void);
int  chat_polish_active(void);

/* Toggle E2EE for a room. Returns 0 on success, -1 if room missing. */
int  chat_polish_set_e2ee(const char *room_id, int on);
int  chat_polish_is_e2ee (const char *room_id);

/* Set the active room (left-pane selection). */
int  chat_polish_set_active_room(const char *room_id);

void chat_polish_print_selftest_line(void);
void chat_polish_cmd(const char *args);

#endif /* ZEOS_CHAT_POLISH_H */
