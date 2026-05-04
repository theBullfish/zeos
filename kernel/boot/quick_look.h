/*
 * Zeos — Quick Look (Spacebar File Preview)
 *
 * Doctrine: previewing a file should not require launching an app.
 * The file list owns the cursor, the user taps space, a centered
 * preview panel appears. Esc dismisses. The panel is read-only and
 * pure presentation — no editing, no chain attached.
 *
 * Type detection:
 *   - PNG (signature 89 50 4E 47): rendered via lodepng
 *   - Plain text (first 4 bytes printable ASCII / NUL-padded): up to
 *     50 lines drawn directly
 *   - Otherwise: metadata card (size, modified time, mime sniff hex)
 */

#ifndef ZEOS_QUICK_LOOK_H
#define ZEOS_QUICK_LOOK_H

#include <stdint.h>

#define QL_PATH_MAX     256

typedef enum {
    QL_KIND_UNKNOWN,
    QL_KIND_TEXT,
    QL_KIND_PNG,
} ql_kind_t;

/* Open Quick Look on a file path. The host loader fills bytes via
 * read_fn — the previewer doesn't depend on FAT32 directly. */
typedef int (*ql_read_fn)(const char *path, uint8_t *out, int max,
                         uint64_t *out_size, uint64_t *out_mtime,
                         void *ctx);

void quick_look_open(const char *path, uint64_t size, uint64_t mtime,
                     ql_read_fn read_fn, void *ctx);

/* Close. */
void quick_look_close(void);

/* Active? */
int  quick_look_active(void);

/* Input. Return 1 if consumed. */
int  quick_look_key(int ascii);
int  quick_look_mouse_down(int x, int y, int button);

/* Draw. */
void quick_look_draw(void);

/* Counter for selftest. */
uint32_t quick_look_total_opens(void);

#endif /* ZEOS_QUICK_LOOK_H */
