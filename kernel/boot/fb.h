/*
 * Zeos framebuffer console — bare metal text output.
 * No UEFI, no firmware. Just pixels.
 */

#ifndef ZEOS_FB_H
#define ZEOS_FB_H

#include "zeos_boot.h"

/* Initialize framebuffer console from boot info */
void fb_init(struct zeos_framebuffer *fb);

/* Clear screen to a 32-bit ARGB color */
void fb_clear(uint32_t color);

/* B.5/B.9 composite clip: restrict all subsequent drawing to [x,x+w) x [y,y+h).
 * The compositor sets this to the delta region it is correcting so a full-layer
 * redraw only touches the changed pixels. fb_reset_clip() restores whole-screen
 * drawing. Non-composite code never calls these, so it is unaffected. */
void fb_set_clip(int x, int y, int w, int h);
void fb_reset_clip(void);

/* B.9 selfcheck: FNV-1a hash of the scanned-out front buffer. */
uint64_t fb_frame_checksum(void);

/* B.9 selfcheck: FNV-1a hash of the back buffer (cursor-free composed scene). */
uint64_t fb_backbuf_checksum(void);

/* B.5/B.9 step 2: flip only the delta region to the front (partial present). */
void fb_present_end_rect(int x, int y, int w, int h);

/* Write a single character at the current cursor position */
void fb_putc(char c);

/* Write a null-terminated string */
void fb_puts(const char *s);

/* Print a 64-bit value as hex */
void fb_put_hex(uint64_t val);

/* Print a 64-bit value as decimal */
void fb_put_dec(uint64_t val);

/* ── Pixel-level drawing primitives ─────────── */

/* Get framebuffer dimensions */
uint32_t fb_width(void);
uint32_t fb_height(void);

/* Physical base address of the framebuffer (identity-mapped on UEFI). */
uint64_t fb_phys_base(void);
/* Pitch in pixels (>= width). */
uint32_t fb_pitch_pixels(void);

/* B.6 double buffer / atomic present: composite into a WB back buffer, flip the
 * whole scene to the WC front in one bulk copy. */
int  fb_backbuf_init(void);
int  fb_backbuf_ready(void);
void fb_present_begin(void);
void fb_present_end(void);

/* Set a single pixel (bounds-checked) */
void fb_pixel(int x, int y, uint32_t color);

/* Filled rectangle */
void fb_rect(int x, int y, int w, int h, uint32_t color);

/* Rectangle outline (border only) */
void fb_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);

/* Horizontal line */
void fb_hline(int x, int y, int w, uint32_t color);

/* Vertical line */
void fb_vline(int x, int y, int h, uint32_t color);

/* Arbitrary line (Bresenham) */
void fb_line(int x0, int y0, int x1, int y1, uint32_t color);

/* Circle outline (Midpoint algorithm) */
void fb_circle(int cx, int cy, int r, uint32_t color);

/* Filled circle */
void fb_circle_filled(int cx, int cy, int r, uint32_t color);

/* Draw text at pixel position (not character grid) */
void fb_text(int x, int y, const char *s, uint32_t color);
/* Boot-font text scaled by an integer factor (8*scale x 16*scale per glyph). */
void fb_text_scaled(int x, int y, const char *s, uint32_t color, int scale);

/* Draw text with background color */
void fb_text_bg(int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* Blit: copy a pixel buffer to framebuffer (for images/video frames) */
void fb_blit(int x, int y, int w, int h, const uint32_t *pixels);
/* Read a w*h rect of the framebuffer into out (row-major). Symmetric w/ fb_blit. */
void fb_read_rect(int x, int y, int w, int h, uint32_t *out);

/* Alpha blend a color onto the framebuffer */
void fb_pixel_blend(int x, int y, uint32_t color);

/* Filled rectangle with alpha blending */
void fb_rect_blend(int x, int y, int w, int h, uint32_t color);

/* Get current text cursor position (character grid) */
void fb_cursor_pos(uint32_t *col, uint32_t *row);

/* Set text cursor position (character grid) */
void fb_set_cursor(uint32_t col, uint32_t row);

#endif /* ZEOS_FB_H */
