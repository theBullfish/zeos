/*
 * Zeos — Image Viewer
 *
 * Standalone image viewer app. Borrows Quick Look's PNG decode path
 * (lodepng + pmm_alloc_contiguous) but exposes it as a real, persistent
 * window the user navigates with: pan, zoom, fit, 1:1, prev/next.
 *
 * Format support:
 *   - PNG: full RGBA decode via lodepng (primary path).
 *   - JPEG / WebP / HEIC: explicitly unsupported — viewer renders a
 *     "format not yet supported" message until decoders land.
 *
 * Lifecycle:
 *   - Decoded RGBA buffer is held in a PMM-contiguous allocation so we
 *     can re-use it across opens when the new image fits.
 *   - Closing releases the PNG output (lodepng_free) and the input
 *     buffer (pmm_free over the page run).
 */

#ifndef ZEOS_IMAGE_VIEWER_H
#define ZEOS_IMAGE_VIEWER_H

#include <stdint.h>

#define IV_PATH_MAX  256

/* Open the viewer on a file path. PNG decode is performed eagerly so
 * the first paint already has pixels. Returns 1 on success, 0 on error
 * (the viewer still opens with an error message). */
int  image_viewer_open(const char *path);

/* Close the viewer (no-op if already closed). */
void image_viewer_close(void);

/* Active? */
int  image_viewer_active(void);

/* Input dispatch — return 1 if event was consumed. */
int  image_viewer_key(int ascii);
int  image_viewer_key_arrow(int dir);                   /* 0=L 1=R 2=U 3=D */
int  image_viewer_mouse_down(int x, int y, int button);
int  image_viewer_mouse_up(int x, int y, int button);
int  image_viewer_mouse_move(int x, int y);

/* Draw — call from compositor overlay layer. */
void image_viewer_draw(void);

/* Counters / selftest */
uint32_t image_viewer_total_opens(void);
uint32_t image_viewer_total_decodes_ok(void);
uint32_t image_viewer_total_decodes_fail(void);

/* Convenience: open viewer if path looks like a supported image. Returns 1 if
 * the viewer opened (or attempted to). Used by list right-click integrations. */
int  image_viewer_open_if_supported(const char *path);

#endif /* ZEOS_IMAGE_VIEWER_H */
