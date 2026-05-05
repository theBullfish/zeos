/*
 * Image viewer app. PNG via lodepng, scaled to fit, zoom + pan.
 * Foundation for richer formats once decoders land.
 *
 * Design notes:
 *   - One global g_iv state. The viewer is a singleton overlay; opening
 *     a second image reuses (or replaces) the existing decode buffer.
 *   - Input file is pulled into a PMM-contiguous run (pmm_alloc_contiguous)
 *     because PNG inputs routinely exceed 64 KB. Output RGBA is what
 *     lodepng_malloc returns and we keep it alive between frames.
 *   - Decode lifecycle:
 *       open(path) → release prior pixels → read file → decode → cache RGBA
 *       close()    → release pixels, clear state
 *     Multiple opens reuse the prior PNG output buffer ONLY by freeing
 *     and re-allocating; lodepng allocates fresh on every decode, so the
 *     "reuse" happens via the allocator's own free-list (no double-free).
 *   - Drawing: a centered card with title bar (filename + close hint),
 *     image viewport, and footer (dims + zoom + status). Pixels are
 *     blitted with nearest-neighbor sampling using a per-axis index map,
 *     identical pattern to quick_look's blit but parameterised by a
 *     viewport rect, a zoom factor, and a (pan_x, pan_y) origin.
 */

#include "image_viewer.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "compositor.h"
#include "pmm.h"
#include "fat32.h"
#include "kprint.h"
#include "lodepng/lodepng.h"
#include "jpeg.h"

/* lodepng allocator shims (defined in browser.c, also reused by quick_look). */
extern void *lodepng_malloc(uint64_t size);
extern void  lodepng_free(void *ptr);

#define IV_W            900
#define IV_H            640
#define IV_TITLE_H      32
#define IV_FOOTER_H     28
#define IV_PAD          12
#define PAGE_SIZE       4096

/* Hard caps on input/output (lodepng output is ~ w*h*4). */
#define IV_MAX_W            4096
#define IV_MAX_H            4096
#define IV_MAX_FILE_BYTES   (32 * 1024 * 1024)  /* 32 MB on-disk PNG */

/* Format detection. */
typedef enum {
    IV_FMT_NONE = 0,
    IV_FMT_PNG,
    IV_FMT_JPEG,
    IV_FMT_WEBP,
    IV_FMT_HEIC,
    IV_FMT_OTHER,
} iv_fmt_t;

typedef enum {
    IV_STATE_EMPTY = 0,
    IV_STATE_LOADED,
    IV_STATE_FAIL,
    IV_STATE_UNSUPPORTED,
    IV_STATE_TOO_BIG,
} iv_state_t;

typedef struct {
    int        active;

    char       path[IV_PATH_MAX];
    uint64_t   size;
    iv_fmt_t   fmt;
    iv_state_t state;
    char       err[96];

    /* Decoded RGBA. */
    unsigned char *pixels;     /* lodepng_malloc'd */
    unsigned       img_w;
    unsigned       img_h;

    /* View transform. zoom is a Q16 fixed-point factor (65536 == 1.0).
     * pan_x/pan_y are top-left source-pixel offsets, in source pixels
     * Q16 — pan_x/65536 is the source x for the viewport's top-left. */
    int32_t        zoom;       /* Q16. Computed as fit-to-window on load. */
    int32_t        fit_zoom;   /* Q16. Cached fit-to-window factor. */
    int32_t        pan_x;      /* source-pixel Q16 */
    int32_t        pan_y;
    int            fit_mode;   /* 1 if zoom == fit_zoom (auto-recenter). */

    /* Drag tracking. */
    int            dragging;
    int            drag_start_mx, drag_start_my;
    int32_t        drag_start_pan_x, drag_start_pan_y;

    /* Window placement (recomputed on screen-size changes). */
    int            wx, wy;
} iv_t;

static iv_t g_iv;
static uint32_t g_total_opens = 0;
static uint32_t g_total_decodes_ok = 0;
static uint32_t g_total_decodes_fail = 0;

/* ── Tiny string helpers ── */

static void iv_strncpy(char *dst, const char *src, int max) {
    int i = 0; if (!dst || max <= 0) return;
    if (src) while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int iv_strlen(const char *s) {
    int n = 0; if (!s) return 0; while (s[n]) n++; return n;
}

static int iv_streq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == 0 && b[i] == 0;
}

/* Lowercase ASCII compare. */
static int iv_strcaseeq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i]) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static int iv_u64_to_dec(uint64_t v, char *out, int max) {
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v > 0 && n < 24) { tmp[n++] = '0' + (v % 10); v /= 10; }
    int o = 0;
    while (n > 0 && o < max - 1) out[o++] = tmp[--n];
    out[o] = '\0';
    return o;
}

/* ── Path helpers (no allocation) ── */

/* Split path into dir + name. dir gets "/" if path is "/foo.png". */
static void iv_split_path(const char *path, char *dir, int dir_max,
                          char *name, int name_max)
{
    int last_slash = -1;
    int i = 0;
    while (path[i]) { if (path[i] == '/') last_slash = i; i++; }
    if (last_slash < 0) {
        iv_strncpy(dir, "/", dir_max);
        iv_strncpy(name, path, name_max);
        return;
    }
    int dlen = last_slash > 0 ? last_slash : 1;
    int o = 0;
    while (o < dlen && o < dir_max - 1) { dir[o] = path[o]; o++; }
    dir[o] = '\0';
    iv_strncpy(name, path + last_slash + 1, name_max);
}

/* True if filename has a .png suffix (case-insensitive). */
static int iv_has_png_ext(const char *name) {
    int n = iv_strlen(name);
    if (n < 4) return 0;
    const char *e = name + n - 4;
    return iv_strcaseeq(e, ".png");
}

/* True if filename has a known image suffix (PNG or JPEG). */
static int iv_has_image_ext(const char *name) {
    int n = iv_strlen(name);
    if (n < 4) return 0;
    const char *e4 = name + n - 4;
    if (iv_strcaseeq(e4, ".png")) return 1;
    if (iv_strcaseeq(e4, ".jpg")) return 1;
    if (n >= 5 && iv_strcaseeq(name + n - 5, ".jpeg")) return 1;
    return 0;
}

static iv_fmt_t iv_sniff_fmt(const uint8_t *b, int n, const char *path) {
    if (n >= 8 &&
        b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47 &&
        b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A)
        return IV_FMT_PNG;
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return IV_FMT_JPEG;
    if (n >= 12 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' &&
        b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P')
        return IV_FMT_WEBP;
    /* HEIC: ftyp box at offset 4 with brand "heic"/"heix"/"mif1" */
    if (n >= 12 && b[4] == 'f' && b[5] == 't' && b[6] == 'y' && b[7] == 'p') {
        if ((b[8] == 'h' && b[9] == 'e' && b[10] == 'i') ||
            (b[8] == 'm' && b[9] == 'i' && b[10] == 'f' && b[11] == '1'))
            return IV_FMT_HEIC;
    }
    /* Last resort: look at extension. */
    int n2 = iv_strlen(path);
    if (n2 >= 4) {
        const char *e = path + n2 - 4;
        if (iv_strcaseeq(e, ".png"))  return IV_FMT_PNG;
        if (iv_strcaseeq(e, ".jpg"))  return IV_FMT_JPEG;
        if (iv_strcaseeq(e, ".jpeg")) return IV_FMT_JPEG;
    }
    if (n2 >= 5 && iv_strcaseeq(path + n2 - 5, ".jpeg")) return IV_FMT_JPEG;
    return IV_FMT_OTHER;
}

/* ── Decode pipeline ── */

static void iv_release_pixels(void) {
    if (g_iv.pixels) {
        lodepng_free(g_iv.pixels);
        g_iv.pixels = 0;
    }
    g_iv.img_w = 0;
    g_iv.img_h = 0;
}

/* Compute fit-to-window zoom (Q16) for the current image vs viewport. */
static int32_t iv_compute_fit_zoom(int viewport_w, int viewport_h) {
    if (g_iv.img_w == 0 || g_iv.img_h == 0 ||
        viewport_w <= 0 || viewport_h <= 0)
        return 65536;
    /* zoom_x = viewport_w / img_w  (Q16). Pick the smaller axis. */
    int64_t zx = ((int64_t)viewport_w << 16) / (int64_t)g_iv.img_w;
    int64_t zy = ((int64_t)viewport_h << 16) / (int64_t)g_iv.img_h;
    int64_t z = zx < zy ? zx : zy;
    if (z < 1) z = 1;
    if (z > (int64_t)16 << 16) z = (int64_t)16 << 16;
    return (int32_t)z;
}

static void iv_viewport_rect(int *vx, int *vy, int *vw, int *vh) {
    *vx = g_iv.wx + IV_PAD;
    *vy = g_iv.wy + IV_TITLE_H + IV_PAD;
    *vw = IV_W - 2 * IV_PAD;
    *vh = IV_H - IV_TITLE_H - IV_FOOTER_H - 2 * IV_PAD;
    if (*vw < 1) *vw = 1;
    if (*vh < 1) *vh = 1;
}

/* Recenter the image in the viewport at the current zoom. */
static void iv_recenter(void) {
    int vx, vy, vw, vh;
    iv_viewport_rect(&vx, &vy, &vw, &vh);
    if (g_iv.zoom <= 0) g_iv.zoom = 65536;

    /* Display dims at current zoom, in destination pixels. */
    int64_t dw = ((int64_t)g_iv.img_w * (int64_t)g_iv.zoom) >> 16;
    int64_t dh = ((int64_t)g_iv.img_h * (int64_t)g_iv.zoom) >> 16;
    if (dw <= 0) dw = 1;
    if (dh <= 0) dh = 1;

    /* Pan such that source (0,0) maps to dst (vx + (vw-dw)/2, vy + (vh-dh)/2),
     * i.e. pan = -((vw-dw)/2) * (1/zoom). Negative pan moves source left of
     * viewport, which is what we want when image is smaller than viewport. */
    int64_t off_x = ((int64_t)(vw - dw)) / 2;
    int64_t off_y = ((int64_t)(vh - dh)) / 2;
    /* pan_x (source Q16) = -off_x / zoom, in source-pixels Q16.
     * source_x_for_dst0 = -off_x_dst / zoom; in Q16: pan = -off_x * 65536 / zoom * 65536 ... */
    int64_t pan_x = -((int64_t)off_x << 32) / (int64_t)g_iv.zoom;
    int64_t pan_y = -((int64_t)off_y << 32) / (int64_t)g_iv.zoom;
    g_iv.pan_x = (int32_t)pan_x;
    g_iv.pan_y = (int32_t)pan_y;
}

static void iv_set_fit(void) {
    int vx, vy, vw, vh;
    iv_viewport_rect(&vx, &vy, &vw, &vh);
    g_iv.fit_zoom = iv_compute_fit_zoom(vw, vh);
    g_iv.zoom     = g_iv.fit_zoom;
    g_iv.fit_mode = 1;
    iv_recenter();
}

/* Read and decode a PNG file. Returns 1 on success. */
static int iv_decode_png(const char *path, uint64_t size) {
    if (size == 0 || size > IV_MAX_FILE_BYTES) {
        g_iv.state = IV_STATE_TOO_BIG;
        iv_strncpy(g_iv.err, "File exceeds 32 MB cap.", sizeof(g_iv.err));
        return 0;
    }
    if (!fat32_mounted() || path[0] != '/') {
        g_iv.state = IV_STATE_FAIL;
        iv_strncpy(g_iv.err, "FAT32 not mounted or non-absolute path.",
                   sizeof(g_iv.err));
        return 0;
    }

    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        g_iv.state = IV_STATE_FAIL;
        iv_strncpy(g_iv.err, "Out of memory (input buffer).",
                   sizeof(g_iv.err));
        return 0;
    }
    uint8_t *in = (uint8_t *)phys;

    struct fat32_file f;
    if (fat32_open(path, &f) < 0) {
        for (uint64_t i = 0; i < pages; i++) pmm_free(phys + i * PAGE_SIZE);
        g_iv.state = IV_STATE_FAIL;
        iv_strncpy(g_iv.err, "Open failed.", sizeof(g_iv.err));
        return 0;
    }
    int got = fat32_read(&f, in, (uint32_t)size);
    if (got <= 0) {
        for (uint64_t i = 0; i < pages; i++) pmm_free(phys + i * PAGE_SIZE);
        g_iv.state = IV_STATE_FAIL;
        iv_strncpy(g_iv.err, "Read failed.", sizeof(g_iv.err));
        return 0;
    }

    unsigned char *pixels = 0;
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32(&pixels, &w, &h, in, (size_t)got);

    /* Release input buffer immediately. */
    for (uint64_t i = 0; i < pages; i++) pmm_free(phys + i * PAGE_SIZE);

    if (err || !pixels || w == 0 || h == 0) {
        if (pixels) lodepng_free(pixels);
        g_iv.state = IV_STATE_FAIL;
        iv_strncpy(g_iv.err, "PNG decode failed.", sizeof(g_iv.err));
        g_total_decodes_fail++;
        return 0;
    }
    if (w > IV_MAX_W || h > IV_MAX_H) {
        lodepng_free(pixels);
        g_iv.state = IV_STATE_TOO_BIG;
        iv_strncpy(g_iv.err, "Image exceeds 4096x4096 cap.",
                   sizeof(g_iv.err));
        g_total_decodes_fail++;
        return 0;
    }

    /* Replace any prior pixels — lodepng allocates fresh; we drop the
     * old buffer through lodepng_free so its allocator can reuse it on
     * the next decode. That's the "reuse buffer if big enough" path. */
    iv_release_pixels();
    g_iv.pixels = pixels;
    g_iv.img_w  = w;
    g_iv.img_h  = h;
    g_iv.state  = IV_STATE_LOADED;
    g_total_decodes_ok++;
    return 1;
}

/* Read and decode a baseline JPEG file. Returns 1 on success.
 * Mirrors iv_decode_png — same caps, same buffer lifecycle. */
static int iv_decode_jpeg(const char *path, uint64_t size) {
    if (size == 0 || size > IV_MAX_FILE_BYTES) {
        g_iv.state = IV_STATE_TOO_BIG;
        iv_strncpy(g_iv.err, "File exceeds 32 MB cap.", sizeof(g_iv.err));
        return 0;
    }
    if (!fat32_mounted() || path[0] != '/') {
        g_iv.state = IV_STATE_FAIL;
        iv_strncpy(g_iv.err, "FAT32 not mounted or non-absolute path.",
                   sizeof(g_iv.err));
        return 0;
    }
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        g_iv.state = IV_STATE_FAIL;
        iv_strncpy(g_iv.err, "Out of memory (input buffer).", sizeof(g_iv.err));
        return 0;
    }
    uint8_t *in = (uint8_t *)phys;
    struct fat32_file f;
    if (fat32_open(path, &f) < 0) {
        for (uint64_t i = 0; i < pages; i++) pmm_free(phys + i * PAGE_SIZE);
        g_iv.state = IV_STATE_FAIL;
        iv_strncpy(g_iv.err, "Open failed.", sizeof(g_iv.err));
        return 0;
    }
    int got = fat32_read(&f, in, (uint32_t)size);
    if (got <= 0) {
        for (uint64_t i = 0; i < pages; i++) pmm_free(phys + i * PAGE_SIZE);
        g_iv.state = IV_STATE_FAIL;
        iv_strncpy(g_iv.err, "Read failed.", sizeof(g_iv.err));
        return 0;
    }
    unsigned char *pixels = 0;
    unsigned w = 0, h = 0;
    jpeg_status_t st = jpeg_decode_rgba(in, (size_t)got, &pixels, &w, &h);
    for (uint64_t i = 0; i < pages; i++) pmm_free(phys + i * PAGE_SIZE);

    if (st == JPEG_TOO_BIG) {
        if (pixels) lodepng_free(pixels);
        g_iv.state = IV_STATE_TOO_BIG;
        iv_strncpy(g_iv.err, "JPEG exceeds 4096x4096 preview cap.",
                   sizeof(g_iv.err));
        g_total_decodes_fail++;
        return 0;
    }
    if (st == JPEG_UNSUPPORTED) {
        if (pixels) lodepng_free(pixels);
        g_iv.state = IV_STATE_UNSUPPORTED;
        iv_strncpy(g_iv.err,
                   "JPEG variant not supported (baseline only).",
                   sizeof(g_iv.err));
        g_total_decodes_fail++;
        return 0;
    }
    if (st != JPEG_OK || !pixels || w == 0 || h == 0) {
        if (pixels) lodepng_free(pixels);
        g_iv.state = IV_STATE_FAIL;
        iv_strncpy(g_iv.err, "JPEG decode failed.", sizeof(g_iv.err));
        g_total_decodes_fail++;
        return 0;
    }
    iv_release_pixels();
    g_iv.pixels = pixels;
    g_iv.img_w  = w;
    g_iv.img_h  = h;
    g_iv.state  = IV_STATE_LOADED;
    g_total_decodes_ok++;
    return 1;
}

/* ── Public API ── */

static void iv_layout(void) {
    int sw = (int)fb_width(), sh = (int)fb_height();
    g_iv.wx = (sw - IV_W) / 2;
    g_iv.wy = (sh - IV_H) / 2;
    if (g_iv.wx < 0) g_iv.wx = 0;
    if (g_iv.wy < 0) g_iv.wy = 0;
}

int image_viewer_open(const char *path)
{
    if (!path || !*path) return 0;

    g_iv.active = 1;
    iv_strncpy(g_iv.path, path, IV_PATH_MAX);
    g_iv.size = 0;
    g_iv.fmt = IV_FMT_NONE;
    g_iv.state = IV_STATE_EMPTY;
    g_iv.err[0] = '\0';
    g_iv.dragging = 0;
    iv_layout();

    /* Sniff via fat32 header read. */
    uint8_t hdr[16] = {0};
    int hdr_n = 0;
    uint64_t size = 0;
    if (fat32_mounted() && path[0] == '/') {
        struct fat32_file f;
        if (fat32_open(path, &f) >= 0) {
            size = f.size;
            hdr_n = fat32_read(&f, hdr, sizeof(hdr));
        }
    }
    g_iv.size = size;
    g_iv.fmt = iv_sniff_fmt(hdr, hdr_n, path);

    int ok = 0;
    if (g_iv.fmt == IV_FMT_PNG) {
        ok = iv_decode_png(path, size);
        if (ok) iv_set_fit();
    } else if (g_iv.fmt == IV_FMT_JPEG) {
        ok = iv_decode_jpeg(path, size);
        if (ok) iv_set_fit();
    } else if (g_iv.fmt == IV_FMT_WEBP ||
               g_iv.fmt == IV_FMT_HEIC) {
        g_iv.state = IV_STATE_UNSUPPORTED;
        iv_strncpy(g_iv.err,
                   "Format not yet supported (PNG / JPEG only).",
                   sizeof(g_iv.err));
    } else {
        g_iv.state = IV_STATE_UNSUPPORTED;
        iv_strncpy(g_iv.err, "Unrecognized image format.",
                   sizeof(g_iv.err));
    }

    g_total_opens++;
    compositor_dirty(g_iv.wx, g_iv.wy, IV_W, IV_H);
    return ok;
}

void image_viewer_close(void)
{
    if (!g_iv.active) return;
    g_iv.active = 0;
    g_iv.dragging = 0;
    iv_release_pixels();
    g_iv.state = IV_STATE_EMPTY;
    int sw = (int)fb_width(), sh = (int)fb_height();
    /* Dirty the full screen — we draw a dim layer over everything. */
    compositor_dirty(0, 0, sw, sh);
}

int image_viewer_active(void) { return g_iv.active; }

int image_viewer_open_if_supported(const char *path) {
    if (!path) return 0;
    /* Right now we only auto-open PNG. Other formats would be silently
     * accepted but show "unsupported" — better to refuse the open. */
    char dir[IV_PATH_MAX], name[IV_PATH_MAX];
    iv_split_path(path, dir, sizeof(dir), name, sizeof(name));
    if (!iv_has_image_ext(name)) return 0;
    return image_viewer_open(path);
}

/* ── Zoom / pan helpers ── */

static void iv_zoom_step(int dir, int focus_x, int focus_y) {
    /* dir == +1 zoom in, -1 zoom out. Focus is screen px. */
    if (g_iv.state != IV_STATE_LOADED) return;
    int vx, vy, vw, vh;
    iv_viewport_rect(&vx, &vy, &vw, &vh);

    /* Source-pixel under focus, Q16. */
    int64_t fdx = focus_x - vx;
    int64_t fdy = focus_y - vy;
    if (fdx < 0) fdx = vw / 2;
    if (fdy < 0) fdy = vh / 2;
    int64_t src_x = (int64_t)g_iv.pan_x + ((fdx << 32) / (int64_t)g_iv.zoom);
    int64_t src_y = (int64_t)g_iv.pan_y + ((fdy << 32) / (int64_t)g_iv.zoom);

    int64_t old = g_iv.zoom;
    /* 1.25x per step. */
    int64_t z = dir > 0 ? (old * 5) / 4 : (old * 4) / 5;
    if (z < (1 << 12)) z = 1 << 12;            /* min ~0.0625 */
    if (z > ((int64_t)32 << 16)) z = (int64_t)32 << 16;
    g_iv.zoom = (int32_t)z;
    g_iv.fit_mode = 0;

    /* Recompute pan so the same source pixel stays under the focus. */
    int64_t pan_x = src_x - ((fdx << 32) / (int64_t)g_iv.zoom);
    int64_t pan_y = src_y - ((fdy << 32) / (int64_t)g_iv.zoom);
    g_iv.pan_x = (int32_t)pan_x;
    g_iv.pan_y = (int32_t)pan_y;
}

static void iv_pan_step(int dx_px, int dy_px) {
    if (g_iv.state != IV_STATE_LOADED) return;
    /* Pan is in source-pixel Q16; convert dst-pixel deltas via 1/zoom. */
    int64_t dxq = ((int64_t)(-dx_px) << 32) / (int64_t)g_iv.zoom;
    int64_t dyq = ((int64_t)(-dy_px) << 32) / (int64_t)g_iv.zoom;
    g_iv.pan_x += (int32_t)dxq;
    g_iv.pan_y += (int32_t)dyq;
    g_iv.fit_mode = 0;
}

static void iv_set_one_to_one(void) {
    if (g_iv.state != IV_STATE_LOADED) return;
    g_iv.zoom = 65536;
    g_iv.fit_mode = 0;
    iv_recenter();
}

/* ── Directory walk for [/]: find next/prev .png in same dir. ── */

typedef struct {
    char names[64][96];   /* up to 64 PNGs per dir */
    int  count;
} iv_dir_scan_t;

static int iv_dir_cb(const char *name, uint32_t size, uint32_t cluster,
                     uint8_t attr, void *user)
{
    (void)size; (void)cluster;
    iv_dir_scan_t *s = (iv_dir_scan_t *)user;
    if (attr & 0x10) return 0;               /* skip dirs */
    if (!iv_has_png_ext(name)) return 0;
    if (s->count >= 64) return 1;            /* stop */
    int i = 0;
    while (name[i] && i < 95) { s->names[s->count][i] = name[i]; i++; }
    s->names[s->count][i] = '\0';
    s->count++;
    return 0;
}

static void iv_navigate(int dir) {
    if (!g_iv.path[0]) return;
    char d[IV_PATH_MAX], n[IV_PATH_MAX];
    iv_split_path(g_iv.path, d, sizeof(d), n, sizeof(n));
    iv_dir_scan_t scan;
    scan.count = 0;
    if (fat32_readdir(d, iv_dir_cb, &scan) < 0 || scan.count == 0)
        return;

    int cur = -1;
    for (int i = 0; i < scan.count; i++) {
        if (iv_streq(scan.names[i], n)) { cur = i; break; }
    }
    if (cur < 0) cur = 0;
    int nxt = cur + (dir > 0 ? 1 : -1);
    if (nxt < 0) nxt = scan.count - 1;
    if (nxt >= scan.count) nxt = 0;

    /* Build new path: dir + "/" (if dir != "/") + name. */
    char newpath[IV_PATH_MAX];
    int o = 0;
    int dl = iv_strlen(d);
    for (int i = 0; i < dl && o < IV_PATH_MAX - 1; i++) newpath[o++] = d[i];
    if (!(dl == 1 && d[0] == '/') && o < IV_PATH_MAX - 1) newpath[o++] = '/';
    int nl = iv_strlen(scan.names[nxt]);
    for (int i = 0; i < nl && o < IV_PATH_MAX - 1; i++) newpath[o++] = scan.names[nxt][i];
    newpath[o] = '\0';
    image_viewer_open(newpath);
}

/* ── Input ── */

int image_viewer_key(int ascii)
{
    if (!g_iv.active) return 0;
    if (ascii == 27) { image_viewer_close(); return 1; }
    if (ascii == '+' || ascii == '=') { iv_zoom_step(+1, -1, -1); compositor_dirty(g_iv.wx, g_iv.wy, IV_W, IV_H); return 1; }
    if (ascii == '-' || ascii == '_') { iv_zoom_step(-1, -1, -1); compositor_dirty(g_iv.wx, g_iv.wy, IV_W, IV_H); return 1; }
    if (ascii == '1')                 { iv_set_one_to_one();      compositor_dirty(g_iv.wx, g_iv.wy, IV_W, IV_H); return 1; }
    if (ascii == 'f' || ascii == 'F') { iv_set_fit();             compositor_dirty(g_iv.wx, g_iv.wy, IV_W, IV_H); return 1; }
    if (ascii == '[')                 { iv_navigate(-1);          return 1; }
    if (ascii == ']')                 { iv_navigate(+1);          return 1; }
    return 1;  /* swallow other keys while active */
}

int image_viewer_key_arrow(int dir)
{
    if (!g_iv.active) return 0;
    /* Arrow pan step: 64 dst px. */
    int dx = 0, dy = 0;
    if (dir == 0) dx = -64;
    else if (dir == 1) dx = +64;
    else if (dir == 2) dy = -64;
    else if (dir == 3) dy = +64;
    iv_pan_step(dx, dy);
    compositor_dirty(g_iv.wx, g_iv.wy, IV_W, IV_H);
    return 1;
}

static int iv_in_window(int x, int y) {
    return (x >= g_iv.wx && x < g_iv.wx + IV_W &&
            y >= g_iv.wy && y < g_iv.wy + IV_H);
}

static int iv_in_close_btn(int x, int y) {
    int bx = g_iv.wx + IV_W - 30, by = g_iv.wy + 6;
    return (x >= bx && x < bx + 24 && y >= by && y < by + 20);
}

int image_viewer_mouse_down(int x, int y, int button)
{
    if (!g_iv.active) return 0;
    if (!iv_in_window(x, y)) {
        image_viewer_close();
        return 1;
    }
    if (iv_in_close_btn(x, y)) {
        image_viewer_close();
        return 1;
    }
    if (button == 1 && g_iv.state == IV_STATE_LOADED) {
        /* Begin drag-pan. */
        int vx, vy, vw, vh;
        iv_viewport_rect(&vx, &vy, &vw, &vh);
        if (x >= vx && x < vx + vw && y >= vy && y < vy + vh) {
            g_iv.dragging = 1;
            g_iv.drag_start_mx = x;
            g_iv.drag_start_my = y;
            g_iv.drag_start_pan_x = g_iv.pan_x;
            g_iv.drag_start_pan_y = g_iv.pan_y;
        }
    }
    return 1;
}

int image_viewer_mouse_up(int x, int y, int button)
{
    if (!g_iv.active) return 0;
    (void)x; (void)y;
    if (button == 1) g_iv.dragging = 0;
    return 1;
}

int image_viewer_mouse_move(int x, int y)
{
    if (!g_iv.active) return 0;
    if (!g_iv.dragging) return 1;
    int dx = x - g_iv.drag_start_mx;
    int dy = y - g_iv.drag_start_my;
    /* pan := start_pan - drag_dx (in source Q16). */
    int64_t dxq = ((int64_t)(-dx) << 32) / (int64_t)g_iv.zoom;
    int64_t dyq = ((int64_t)(-dy) << 32) / (int64_t)g_iv.zoom;
    g_iv.pan_x = g_iv.drag_start_pan_x + (int32_t)dxq;
    g_iv.pan_y = g_iv.drag_start_pan_y + (int32_t)dyq;
    g_iv.fit_mode = 0;
    compositor_dirty(g_iv.wx, g_iv.wy, IV_W, IV_H);
    return 1;
}

/* ── Drawing ── */

static void iv_blit_image(int vx, int vy, int vw, int vh) {
    if (!g_iv.pixels || g_iv.zoom <= 0) return;
    /* For each viewport pixel (dx, dy):
     *     src_x = pan_x + (dx << 32) / zoom    (in source-pixel Q16)
     *     src_y = pan_y + (dy << 32) / zoom
     * Then sample nearest. Skip if out of source bounds (transparent). */
    int sw = (int)g_iv.img_w;
    int sh = (int)g_iv.img_h;
    int64_t inv = ((int64_t)1 << 32) / (int64_t)g_iv.zoom;  /* dst-px → source Q16 step */

    for (int dy = 0; dy < vh; dy++) {
        int64_t syq = (int64_t)g_iv.pan_y + inv * dy;
        int sy = (int)(syq >> 16);
        if (sy < 0 || sy >= sh) continue;
        const uint8_t *row = g_iv.pixels + (size_t)sy * sw * 4;
        int64_t sxq_base = (int64_t)g_iv.pan_x;
        for (int dx = 0; dx < vw; dx++) {
            int64_t sxq = sxq_base + inv * dx;
            int sx = (int)(sxq >> 16);
            if (sx < 0 || sx >= sw) continue;
            const uint8_t *p = row + sx * 4;
            uint32_t argb = ((uint32_t)p[3] << 24) |
                            ((uint32_t)p[0] << 16) |
                            ((uint32_t)p[1] <<  8) |
                            ((uint32_t)p[2]);
            fb_pixel_blend(vx + dx, vy + dy, argb);
        }
    }
}

void image_viewer_draw(void)
{
    if (!g_iv.active) return;
    iv_layout();

    int sw = (int)fb_width(), sh = (int)fb_height();
    /* Dim background. */
    fb_rect_blend(0, 0, sw, sh, 0xC0000000);

    /* Card. */
    fb_rect(g_iv.wx, g_iv.wy, IV_W, IV_H, COLOR_SURFACE_HIGH);
    fb_rect_outline(g_iv.wx, g_iv.wy, IV_W, IV_H, COLOR_SEPARATOR, 1);

    /* Title bar. */
    fb_rect(g_iv.wx, g_iv.wy, IV_W, IV_TITLE_H, COLOR_SURFACE_TOP);
    /* Filename (truncate by available width). */
    char title[IV_PATH_MAX];
    iv_strncpy(title, g_iv.path, sizeof(title));
    fb_text(g_iv.wx + IV_PAD, g_iv.wy + 9, title, COLOR_ON_SURFACE);
    /* Close button [X]. */
    int bx = g_iv.wx + IV_W - 30, by = g_iv.wy + 6;
    fb_rect(bx, by, 24, 20, COLOR_SURFACE_HIGH);
    fb_rect_outline(bx, by, 24, 20, COLOR_SEPARATOR, 1);
    fb_text(bx + 8, by + 3, "X", COLOR_ON_SURFACE);

    /* Viewport. */
    int vx, vy, vw, vh;
    iv_viewport_rect(&vx, &vy, &vw, &vh);
    fb_rect(vx, vy, vw, vh, COLOR_SURFACE);
    fb_rect_outline(vx, vy, vw, vh, COLOR_SEPARATOR, 1);

    if (g_iv.state == IV_STATE_LOADED) {
        iv_blit_image(vx, vy, vw, vh);
    } else {
        const char *msg = g_iv.err[0] ? g_iv.err : "No image loaded.";
        int tw = iv_strlen(msg) * 8;
        fb_text(vx + (vw - tw) / 2, vy + vh / 2 - 8, msg, COLOR_ON_SURFACE_2);
        if (g_iv.state == IV_STATE_UNSUPPORTED) {
            const char *hint = "WebP / HEIC decoders not yet implemented.";
            int hw = iv_strlen(hint) * 8;
            fb_text(vx + (vw - hw) / 2, vy + vh / 2 + 8, hint,
                    COLOR_ON_SURFACE_3);
        }
    }

    /* Footer. */
    int fy = g_iv.wy + IV_H - IV_FOOTER_H + 6;
    char buf[160];
    int o = 0;
    if (g_iv.state == IV_STATE_LOADED) {
        o += iv_u64_to_dec((uint64_t)g_iv.img_w, buf + o, 159 - o);
        if (o < 158) buf[o++] = 'x';
        o += iv_u64_to_dec((uint64_t)g_iv.img_h, buf + o, 159 - o);
        if (o < 158) buf[o++] = ' ';
        if (o < 158) buf[o++] = ' ';
        const char *zl = "zoom ";
        for (int i = 0; zl[i] && o < 158; i++) buf[o++] = zl[i];
        /* Zoom as integer percent. */
        int zpct = (int)(((int64_t)g_iv.zoom * 100) >> 16);
        o += iv_u64_to_dec((uint64_t)zpct, buf + o, 159 - o);
        if (o < 158) buf[o++] = '%';
        if (g_iv.fit_mode && o < 156) {
            const char *ft = " (fit)";
            for (int i = 0; ft[i] && o < 158; i++) buf[o++] = ft[i];
        }
    } else {
        const char *st = (g_iv.state == IV_STATE_UNSUPPORTED) ?
                         "unsupported format" : "no image";
        for (int i = 0; st[i] && o < 158; i++) buf[o++] = st[i];
    }
    buf[o] = '\0';
    fb_text(g_iv.wx + IV_PAD, fy, buf, COLOR_ON_SURFACE_2);

    /* Keybinds hint, right-aligned-ish. */
    const char *hint = "+/- zoom  1 1:1  f fit  arrows pan  [/] prev/next  Esc close";
    int hw = iv_strlen(hint) * 8;
    fb_text(g_iv.wx + IV_W - IV_PAD - hw, fy, hint, COLOR_ON_SURFACE_3);
}

/* ── Counters ── */

uint32_t image_viewer_total_opens(void)        { return g_total_opens; }
uint32_t image_viewer_total_decodes_ok(void)   { return g_total_decodes_ok; }
uint32_t image_viewer_total_decodes_fail(void) { return g_total_decodes_fail; }
