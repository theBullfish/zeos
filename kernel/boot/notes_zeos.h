/*
 * Zeos — notes-zeos: Notion / Obsidian replacement
 *
 * Subscriber over fs_event for .md files. Parses [[wiki-links]] and
 * maintains:
 *   - backlinks_idx : btree<hash(target), hash(source)>
 *   - search_idx    : btree<hash(token), doc_id>
 *   - docs_idx      : btree<doc_id, hash(path)>
 *
 * No polling. Every .md create / write / unlink / rename re-resolves
 * that file's contribution to the indices in real time.
 *
 * Z+ shape declaration: programs/notes-zeos.zp.
 */

#ifndef ZEOS_NOTES_ZEOS_H
#define ZEOS_NOTES_ZEOS_H

#include <stdint.h>

/* Lazy init: registers fs_event listener + builds initial index by
 * walking /notes recursively. Idempotent. */
void notes_zeos_init(void);

/* Print pages that link TO `target` (i.e. contain [[<basename(target)>]]).
 * Returns count printed. */
int  notes_zeos_print_backlinks(const char *target);

/* Search across all indexed .md files. Returns count printed. */
int  notes_zeos_print_search(const char *query);

/* Print total docs / tokens / backlink edges. */
void notes_zeos_print_stats(void);

/* Force-reindex a single .md path (used by fs_event listener and at
 * boot for the initial walk). pass kind=0 for delete, kind=1 for
 * create/write. Returns the number of wikilinks discovered, or 0. */
int  notes_zeos_index_file(const char *path, int kind);

/* Selftest counters. */
uint32_t notes_zeos_total_docs(void);
uint32_t notes_zeos_total_tokens(void);
uint32_t notes_zeos_total_backlinks(void);

/* Walk every indexed doc. Callback receives the path + per-doc counts.
 * Returns the number of docs visited. (Used by notes_polish's graph.) */
int notes_zeos_walk(void (*cb)(const char *path,
                                uint32_t token_count,
                                uint32_t link_count,
                                void *user),
                    void *user);

#endif /* ZEOS_NOTES_ZEOS_H */
