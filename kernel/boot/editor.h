/*
 * Zeos — Text Editor (chain-shaped)
 *
 * Editor is NOT an imperative app. It is a chain that emits typed
 * text_edit signals. Subscriber chains (autosave, history, spell,
 * autocomplete) consume those signals via MDE type-routing. The
 * window is a renderer over the emitter chain.
 *
 * Doctrine notes:
 *   - One global emitter chain (CHAIN_TEXT_EDIT) and one global set
 *     of subscribers. Multi-buffer support is via buffer_id on the
 *     text_edit struct — every emit carries which buffer changed,
 *     and subscribers route work per-buffer through internal tables.
 *     This keeps registry depth bounded and matches notify/clock.
 *   - MDE auto_route wires emitter -> subscribers automatically by
 *     matching output_type=text_edit to input_type=text_edit. No
 *     hand routing.
 *   - Spell + autocomplete are honestly stubbed: their resolve fns
 *     count invocations and return well-formed empty results. They
 *     are not lying about doing real work.
 *   - Autosave debounces 2s of idle then writes via fat32_write.
 *   - History snapshots every 50 emits to VAULT
 *     /editor/history/<buffer_id>/v<n>.
 */

#ifndef ZEOS_EDITOR_H
#define ZEOS_EDITOR_H

#include <stdint.h>

/* ── Limits ─────────────────────────────────────────────────────── */

#define EDITOR_PATH_MAX       256
#define EDITOR_BUFFER_BYTES   (64 * 1024)   /* per buffer text capacity */
#define EDITOR_MAX_BUFFERS    4             /* concurrent open files */
#define EDITOR_LINES_VISIBLE  24            /* visible text rows */
#define EDITOR_FIND_MAX       64
#define EDITOR_WIN_ID         5151          /* dirty registry id */
#define EDITOR_HISTORY_EVERY  50

/* ── Public API ─────────────────────────────────────────────────── */

/* Open editor with a file path (or NULL for empty buffer). Returns 0
 * on success, -1 on failure (no free slot, file unreadable, etc.). */
int  editor_open(const char *path);

/* Close the active editor window. Honors ui_dirty: if the focused
 * buffer is dirty, the standard Save / Discard / Cancel modal opens
 * and the close is deferred until the user picks. */
void editor_close(void);

/* Window currently active? */
int  editor_active(void);

/* Manual save (Ctrl-S). Writes the focused buffer to disk and clears
 * the dirty flag. Returns 0 on success. */
int  editor_save_focused(void);

/* Input dispatch (called from keyboard.c / mouse.c). */
int  editor_key(int ascii);
int  editor_key_arrow(int dir);
int  editor_mouse_down(int x, int y, int button);
int  editor_mouse_move(int x, int y);
int  editor_mouse_up(int x, int y, int button);

/* Render (called from compositor.c). */
void editor_draw(void);

/* Tick — called every frame. Handles spell/autocomplete pump and
 * autosave debounce expiry by re-resolving the relevant chain. */
void editor_tick(void);

/* ── Chain registration ─────────────────────────────────────────── */

/* Registers CHAIN_TEXT_EDIT, CHAIN_TEXT_AUTOSAVE, CHAIN_TEXT_HISTORY,
 * CHAIN_TEXT_SPELL, CHAIN_TEXT_AUTOCOMPLETE under parent_chain_id.
 * Returns total chains registered (1..5), or -1 on failure. */
int editor_chain_register(int parent_chain_id);

/* ── Persistence ────────────────────────────────────────────────── */

/* Save the open-file list + cursor positions to VAULT. */
void editor_persist_save(void);

/* Restore previously open files from VAULT. Idempotent. */
void editor_persist_restore(void);

/* ── Selftest counters ──────────────────────────────────────────── */

uint32_t editor_total_opens(void);
uint32_t editor_total_emits(void);
uint32_t editor_total_autosaves(void);
uint32_t editor_total_snapshots(void);
uint32_t editor_total_spell_checks(void);
uint32_t editor_total_autocompletes(void);

/* Chain ids — set during editor_chain_register, -1 if unregistered. */
extern int CHAIN_TEXT_EDIT;
extern int CHAIN_TEXT_AUTOSAVE;
extern int CHAIN_TEXT_HISTORY;
extern int CHAIN_TEXT_SPELL;
extern int CHAIN_TEXT_AUTOCOMPLETE;

#endif /* ZEOS_EDITOR_H */
