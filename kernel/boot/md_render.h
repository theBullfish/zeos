/*
 * Zeos — shared markdown renderer.
 *
 * Single renderer used by notes-zeos, chat-zeos, and the editor's
 * preview pane so the same input renders identically everywhere.
 *
 * Supported syntax:
 *   # / ## / ### headings
 *   > blockquote
 *   - <text>     unordered list
 *   **bold**
 *   *italic*    (rendered as bold where italic glyphs aren't packed)
 *   `inline code`
 *   ```fenced``` code blocks (per-block background)
 *   [[wikilinks]]    rendered in primary accent
 *   http(s)://... auto-link, primary accent, click-to-copy via
 *                 md_render_url_at(x, y) hit-test
 *
 * Honest scope:
 *   No tables, no nested lists, no images. The output is a presentation
 *   layer; for full Markdown semantics, the source still lives in
 *   notes_zeos's index. URL hit-testing is opt-in: callers register
 *   the layout buffer they care about; default is no hit table.
 */

#ifndef ZEOS_MD_RENDER_H
#define ZEOS_MD_RENDER_H

#include <stdint.h>

/* One render call. Draws into the rect (x,y,w,h) on the framebuffer.
 * `src` is a NUL-terminated UTF-8 markdown string; `src_len` is the
 * usable byte count (callers can pass strlen). The rect is filled with
 * COLOR_SURFACE before drawing. */
void md_render(int x, int y, int w, int h,
               const char *src, int src_len);

/* Compact / inline variant for chat message rows: renders one or two
 * paragraphs of markdown without filling the background, in the
 * default body color. Returns the bottom-Y of the rendered region. */
int md_render_inline(int x, int y, int w_max,
                     const char *src, int src_len);

/* Hit-test: was an URL clicked? The renderer maintains a small ring
 * of the most recent URL runs drawn (full screen-y bounding box +
 * URL string). Returns 1 if (x,y) lands on a known URL run and writes
 * up to `out_max` bytes (NUL-terminated) of the URL into `out`. */
int md_render_url_at(int x, int y, char *out, int out_max);

#endif /* ZEOS_MD_RENDER_H */
