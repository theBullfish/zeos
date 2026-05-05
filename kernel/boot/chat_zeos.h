/*
 * Zeos — chat-zeos: Slack / Discord chat-server core in Z+.
 *
 * First-principles primitive (REPLACEMENTS.md item 5):
 *   ordered append-only log + visibility scope + delivery to subscribers.
 *
 * State:
 *   message_log    : btree<pack(room_idx, seq), msg_idx>     (append-only)
 *   presence_table : btree<hash(ctx), status_int>            (online/away/offline)
 *   search_idx     : btree<hash(token), msg_idx>             (full-text)
 *   rooms_idx      : btree<hash(room_id), room_idx>
 *
 * Persistence: every chat_message is also appended to vault key
 *   /chat/<room_id>/log/<seq>
 * so messages survive reboot via the existing vault persistence layer.
 *
 * Identity-context isolation: INTERNAL-tier private rooms; REFERENCE
 * tier public rooms. Cross-context perceive on private rooms denied.
 *
 * The .zp file (programs/chat-zeos.zp) declares the chain shape.
 *
 * Codex Labs LLC — 2026
 */

#ifndef ZEOS_CHAT_ZEOS_H
#define ZEOS_CHAT_ZEOS_H

#include <stdint.h>

/* Lazy init: allocate btrees, set "self" ctx if unset. Idempotent. */
void chat_zeos_init(void);

/* Identity context — defaults to "self" but can be set per-session.
 * In a fully wired CFA setup this comes from the active identity
 * context root. For the single-host case the default is sufficient. */
void        chat_zeos_set_ctx(const char *ctx);
const char *chat_zeos_current_ctx(void);

/* Room management. visibility = 0 private (INTERNAL), 1 public
 * (REFERENCE), 2 channel (REFERENCE+broadcast). Returns 0 on success. */
int  chat_zeos_create_room(const char *room_id, const char *name, int visibility);
int  chat_zeos_join_room  (const char *room_id, const char *ctx);
int  chat_zeos_leave_room (const char *room_id, const char *ctx);
int  chat_zeos_room_visible_to(const char *room_id, const char *ctx);

/* List visible rooms for `ctx`. Prints to console. Returns count. */
int  chat_zeos_print_rooms(const char *ctx);

/* Append a message. ctx must be a member (or room must be public).
 * Returns the new seq on success, < 0 on denial. */
int  chat_zeos_send(const char *room_id, const char *ctx, const char *body);

/* Print the last `n` messages in `room_id` (or all if n <= 0). */
int  chat_zeos_print_tail(const char *room_id, const char *ctx, int n);

/* Search across rooms visible to `ctx`. Returns count printed. */
int  chat_zeos_print_search(const char *ctx, const char *query);

/* Heartbeat / presence. status: "online" / "away" / "offline". */
int  chat_zeos_presence(const char *ctx, const char *status);
int  chat_zeos_print_presence(const char *ctx);

/* Bench: send N synthetic messages to room `bench` and report ms. */
void chat_zeos_bench(int n);

/* Voice/federation stub — single-host only today; emits a clear notice. */
void chat_zeos_voice_stub(void);

/* Selftest counters. */
uint32_t chat_zeos_total_rooms(void);
uint32_t chat_zeos_total_messages(void);
uint32_t chat_zeos_total_members(void);

/* Walk every room visible to ctx (or all rooms if ctx == NULL).
 * Callback receives id, name, visibility, member_count, msg_count. */
int chat_zeos_walk_rooms(const char *ctx,
                         void (*cb)(const char *id, const char *name,
                                    int visibility, int member_count,
                                    int msg_count, void *user),
                         void *user);

/* Walk last N messages in a room (most-recent-first). */
int chat_zeos_walk_tail(const char *room_id, const char *ctx, int n,
                        void (*cb)(uint64_t ts, const char *author,
                                   const char *body, void *user),
                        void *user);

#endif /* ZEOS_CHAT_ZEOS_H */
