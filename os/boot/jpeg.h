/*
 * Zeos — baseline JPEG decoder
 *
 * Real, no stubs. Decodes baseline (SOF0) JPEG files only:
 *   - Markers parsed: SOI, APPn (skipped), DQT, SOF0, DHT, SOS, EOI.
 *   - Huffman decode → dequantize → 8x8 inverse DCT (AAN-style) →
 *     YCbCr→RGBA color-space conversion. 4:4:4, 4:2:2, 4:2:0 chroma
 *     subsampling supported (1x1, 2x1, 2x2 luma sampling factors with
 *     1x1 chroma). Other sampling factors rejected.
 *
 * Hard caps:
 *   - 4096x4096 image dimensions. Larger images return JPEG_TOO_BIG.
 *   - 32 MB on-disk input.
 *
 * Out of scope (documented honest follow-ups):
 *   - Progressive JPEG (SOF2)
 *   - Arithmetic-coded JPEG (SOF1, SOF9, SOF10)
 *   - 12-bit samples
 *   - EXIF orientation
 *   - JFIF thumbnail extraction
 *
 * Output buffer layout matches lodepng_decode32: ABGR-as-RGBA byte
 * order in memory (R, G, B, A per pixel, row-major top-down). Pixels
 * are allocated via lodepng_malloc so callers can free with
 * lodepng_free and the same blit paths used by PNG just work.
 *
 * Codex Labs LLC — 2026
 */

#ifndef ZEOS_JPEG_H
#define ZEOS_JPEG_H

#include <stdint.h>
#include <stddef.h>

#define JPEG_MAX_W   4096
#define JPEG_MAX_H   4096

typedef enum {
    JPEG_OK = 0,
    JPEG_BAD_INPUT,
    JPEG_UNSUPPORTED,   /* progressive / arithmetic / 12-bit / weird sampling */
    JPEG_TOO_BIG,
    JPEG_OOM,
    JPEG_TRUNCATED,
} jpeg_status_t;

/* Decode a baseline JPEG into a freshly-allocated RGBA buffer.
 *
 *   in       — pointer to JPEG bytes
 *   in_size  — length of `in`
 *   out      — receives a pointer to lodepng_malloc'd RGBA buffer (caller
 *              must lodepng_free it). Set to 0 on failure.
 *   out_w    — receives image width on success
 *   out_h    — receives image height on success
 *
 * Returns JPEG_OK or one of the JPEG_* error codes. */
jpeg_status_t jpeg_decode_rgba(const uint8_t *in, size_t in_size,
                               unsigned char **out,
                               unsigned *out_w, unsigned *out_h);

#endif /* ZEOS_JPEG_H */
