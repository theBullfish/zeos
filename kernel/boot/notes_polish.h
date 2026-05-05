/*
 * Zeos — notes-zeos polish layer (Notion / Obsidian killer)
 *
 * Three regions:
 *   - Top: tag cloud (deferred to v2 — header notes only).
 *   - Left: source markdown (editable text buffer, reuses ui_undo).
 *   - Right: rendered preview (headings, bold, italic, code, lists,
 *     blockquotes, syntax-highlighted Z+ code).
 *   - Bottom strip: backlink graph nodes (one per indexed doc, edges
 *     synthesized from per-doc link counts).
 *
 * Live-filter search is wired in the title bar; daily-note auto-create
 * is hooked from CHAIN_CLOCK in main.c initialization.
 *
 * Security:
 *   - Notes are CFA-namespaced via notes_zeos's existing per-context
 *     index (commits 78002aa / b618562); SOVEREIGN-tagged notes are
 *     stored under the disk-encryption SOVEREIGN handle.
 *   - The "share" toggle promotes a note to REFERENCE tier; default
 *     is INTERNAL.
 *
 * Honest gaps:
 *   - Markdown renderer here handles headings (#, ##, ###), bold
 *     **text**, italic *text*, fenced code blocks ```lang...```,
 *     unordered lists -, blockquotes >, and [[wikilinks]]. Tables,
 *     embedded images, and footnotes are roadmap.
 *   - Tag cloud is computed but not yet drawn; emits placeholder.
 *   - Spring-animated backlink graph layout uses a fixed deterministic
 *     ring — force-directed coming when anim.c exposes the node-based
 *     spring API.
 *
 * Selftest line:
 *   notes-zeos polish .... graph view ready, N notes, M backlinks
 */

#ifndef ZEOS_NOTES_POLISH_H
#define ZEOS_NOTES_POLISH_H

#include <stdint.h>

void notes_polish_init(void);
void notes_polish_open(void);
void notes_polish_close(void);
int  notes_polish_active(void);

/* Set the source-pane buffer (e.g. when opening a .md file). */
void notes_polish_set_source(const char *md, int len);

void notes_polish_print_selftest_line(void);
void notes_polish_cmd(const char *args);

#endif /* ZEOS_NOTES_POLISH_H */
