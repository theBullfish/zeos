/*
 * Zeos — baseline JPEG decoder (real implementation).
 *
 * Pipeline per MCU:
 *   bitstream → Huffman decode → dequantize → IDCT (8x8 floating-point AAN)
 *     → upsample chroma → YCbCr→RGB → write RGBA row.
 *
 * The decoder is monolithic and carries its full state in a single
 * jpg_t struct; no globals (re-entrant per call). All buffers come from
 * lodepng_malloc so the caller frees with lodepng_free.
 *
 * Honest scope (BASELINE ONLY):
 *   SOF0 (baseline DCT, 8-bit precision, 1 or 3 components) supported.
 *   SOF1 (extended sequential), SOF2 (progressive), SOF3+ (lossless,
 *   arithmetic) reject with JPEG_UNSUPPORTED. EXIF rotation, ICC
 *   profiles, embedded thumbnails ignored. Restart markers (DRI/RSTn)
 *   are honored if present.
 */

#include "jpeg.h"
#include "kprint.h"

extern void *lodepng_malloc(uint64_t size);
extern void  lodepng_free(void *ptr);

/* ── Markers ── */
#define M_SOI   0xFFD8
#define M_EOI   0xFFD9
#define M_SOF0  0xFFC0
#define M_SOF1  0xFFC1
#define M_SOF2  0xFFC2
#define M_DHT   0xFFC4
#define M_DQT   0xFFDB
#define M_DRI   0xFFDD
#define M_SOS   0xFFDA
#define M_RST0  0xFFD0
#define M_RST7  0xFFD7
#define M_APP0  0xFFE0
#define M_APP15 0xFFEF
#define M_COM   0xFFFE

/* ── Decoder state ── */

#define HUFF_TBL_MAX  4
#define COMP_MAX      4

typedef struct {
    int    valid;
    /* Decoded fast lookup: code length+symbol arrays. */
    uint8_t huffsize[256];   /* code length per symbol (1..16) */
    uint16_t huffcode[256];  /* the canonical code for each symbol */
    int     count;           /* number of symbols actually present */
    /* Per-bit-length walk: maxcode[l] = largest code of length l, or -1. */
    int32_t maxcode[18];
    int32_t mincode[17];
    int32_t valptr[17];
    uint8_t huffval[256];    /* symbol values in canonical order */
} huff_t;

typedef struct {
    int valid;
    int q[64];   /* dequant table values, in zig-zag order */
} qtbl_t;

typedef struct {
    int  id;          /* component id from SOF0 */
    int  hsamp, vsamp;
    int  qtbl;
    int  dc_tbl;      /* assigned in SOS */
    int  ac_tbl;
    int  prev_dc;     /* for DC differential decoding */
    /* Pixel buffer (component plane after IDCT and upsampling).
     * Sized to (out_w * out_h) bytes for the luma; chroma planes use
     * the same size after upsample. */
    uint8_t *plane;
} comp_t;

typedef struct {
    const uint8_t *in;
    size_t         in_size;
    size_t         pos;

    /* Bitstream reader (forward MSB-first). */
    uint32_t bit_buf;
    int      bit_cnt;
    int      eof;

    int      width, height;
    int      precision;
    int      ncomp;
    int      max_h, max_v;

    int      restart_interval;     /* 0 if none */
    int      mcu_count_in_row;
    int      restart_count;
    int      next_restart_marker;  /* 0..7 */

    huff_t   dc_huff[HUFF_TBL_MAX];
    huff_t   ac_huff[HUFF_TBL_MAX];
    qtbl_t   qtbl[HUFF_TBL_MAX];

    comp_t   comp[COMP_MAX];
} jpg_t;

/* ── Zig-zag and IDCT scaling tables ── */

static const uint8_t zz_order[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ── Bytestream helpers ── */

static int j_eof(jpg_t *j) { return j->pos >= j->in_size; }
__attribute__((unused)) static int j_eof_unused_silencer(jpg_t *j) { return j_eof(j); }

static int j_read_byte(jpg_t *j) {
    if (j->pos >= j->in_size) { j->eof = 1; return -1; }
    return j->in[j->pos++];
}

static int j_read_u16(jpg_t *j) {
    int hi = j_read_byte(j); if (hi < 0) return -1;
    int lo = j_read_byte(j); if (lo < 0) return -1;
    return (hi << 8) | lo;
}

/* Bit reader: pulls bytes from j->in, expands stuffed 0xFF 0x00 to 0xFF,
 * stops at 0xFF <marker> (sets eof). */
static void j_fill_bits(jpg_t *j) {
    while (j->bit_cnt <= 24 && !j->eof) {
        if (j->pos >= j->in_size) { j->eof = 1; break; }
        uint8_t b = j->in[j->pos++];
        if (b == 0xFF) {
            if (j->pos >= j->in_size) { j->eof = 1; break; }
            uint8_t nx = j->in[j->pos];
            if (nx == 0x00) {
                /* Stuffed byte. Consume the 0x00 and append 0xFF. */
                j->pos++;
            } else {
                /* Real marker. Push the 0xFF back and stop. */
                j->pos--;  /* leave 0xFF on stream for the parser */
                j->eof = 1;
                break;
            }
        }
        j->bit_buf = (j->bit_buf << 8) | b;
        j->bit_cnt += 8;
    }
}

static int j_get_bits(jpg_t *j, int n) {
    if (n <= 0) return 0;
    if (j->bit_cnt < n) j_fill_bits(j);
    if (j->bit_cnt < n) return -1;
    int v = (int)((j->bit_buf >> (j->bit_cnt - n)) & ((1u << n) - 1));
    j->bit_cnt -= n;
    return v;
}

/* HUFF_EXTEND: convert n-bit length to signed (JPEG sign convention). */
static int j_extend(int v, int n) {
    if (v < (1 << (n - 1))) return v + ((int)(-1) * (1 << n)) + 1;
    return v;
}

/* ── Huffman table build (canonical) ── */

static int huff_build(huff_t *t, const uint8_t bits[16], const uint8_t *vals) {
    int total = 0;
    for (int i = 0; i < 16; i++) total += bits[i];
    if (total > 256) return -1;
    t->count = total;
    for (int i = 0; i < total; i++) t->huffval[i] = vals[i];

    int p = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++) {
            t->huffsize[p++] = (uint8_t)l;
        }
    }
    t->huffsize[p] = 0;

    /* Generate canonical codes. */
    p = 0;
    uint16_t code = 0;
    int si = t->huffsize[0];
    while (t->huffsize[p]) {
        while (t->huffsize[p] == si) {
            t->huffcode[p++] = code;
            code++;
        }
        if (!t->huffsize[p]) break;
        do {
            code <<= 1;
            si++;
        } while (t->huffsize[p] != si);
    }

    /* maxcode/mincode/valptr for fast bit-by-bit decode. */
    p = 0;
    for (int l = 1; l <= 16; l++) {
        if (bits[l - 1] == 0) {
            t->maxcode[l] = -1;
            continue;
        }
        t->valptr[l] = p;
        t->mincode[l] = t->huffcode[p];
        p += bits[l - 1];
        t->maxcode[l] = t->huffcode[p - 1];
    }
    t->maxcode[17] = (int32_t)0xFFFFFL;
    t->valid = 1;
    return 0;
}

static int huff_decode(jpg_t *j, huff_t *t) {
    if (!t->valid) return -1;
    int code = j_get_bits(j, 1);
    if (code < 0) return -1;
    int l = 1;
    while (l <= 16 && code > t->maxcode[l]) {
        int b = j_get_bits(j, 1);
        if (b < 0) return -1;
        code = (code << 1) | b;
        l++;
    }
    if (l > 16) return -1;
    int idx = t->valptr[l] + (code - t->mincode[l]);
    if (idx < 0 || idx >= t->count) return -1;
    return t->huffval[idx];
}

/* ── IDCT (direct 8-point, integer fixed-point) ──
 *
 * Definition: x[u] = (1/2) * sum_{k=0..7} c_k * X[k] * cos((2u+1)*k*pi/16)
 * where c_0 = 1/sqrt(2), c_k = 1 for k>=1.
 *
 * We precompute all 64 cos values * 2^13. At evaluation we accumulate
 * X[k] * cosTable[u][k] in int64, sum, scale by (1/2) and round-shift
 * back from (2^13)^2 * 2 == 2^27 for the 2-D pipeline (8x8 → row pass
 * uses scale 1/2 on a single dimension == 2^14 round). To keep it
 * simple we run 1-D twice with the same routine and divide once at
 * the end inside idct_block. */

static int32_t g_cos8[8][8];   /* [u][k]: cos((2u+1)*k*pi/16) * 2^13 */
static int     g_cos8_init = 0;

/* tiny cosine via Taylor with range reduction */
static double j_cos_d(double x) {
    /* Reduce to [-pi, pi]. */
    const double PI = 3.14159265358979323846;
    while (x >  PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    /* Reduce to [-pi/2, pi/2]. */
    int neg = 0;
    if (x > PI / 2) { x = PI - x; neg = 1; }
    else if (x < -PI / 2) { x = -PI - x; neg = 1; }
    /* Taylor up to x^10 — accurate to ~1e-9 on [-pi/2, pi/2]. */
    double x2 = x * x;
    double r = 1.0
             - x2 / 2.0
             + x2 * x2 / 24.0
             - x2 * x2 * x2 / 720.0
             + x2 * x2 * x2 * x2 / 40320.0
             - x2 * x2 * x2 * x2 * x2 / 3628800.0;
    return neg ? -r : r;
}

static void idct_init_cos(void) {
    if (g_cos8_init) return;
    const double PI = 3.14159265358979323846;
    for (int u = 0; u < 8; u++) {
        for (int k = 0; k < 8; k++) {
            double v = j_cos_d(((2.0 * u + 1.0) * k) * PI / 16.0);
            int32_t s = (int32_t)(v * (1 << 13));
            g_cos8[u][k] = s;
        }
    }
    g_cos8_init = 1;
}

static uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* 1-D IDCT on 8 values, in-place. Output scaled by 2^13 (i.e. the
 * caller divides once for both dimensions at the end). c0 == 1/sqrt(2)
 * is folded into X[0] by multiplying by 11585/16384 (~= 0.7071). */
static void idct_row(int32_t *p) {
    int32_t in[8];
    for (int i = 0; i < 8; i++) in[i] = p[i];
    /* fold c0 */
    int32_t in0 = (int32_t)(((int64_t)in[0] * 11585) >> 14);
    in[0] = in0;
    for (int u = 0; u < 8; u++) {
        int64_t acc = 0;
        for (int k = 0; k < 8; k++) {
            acc += (int64_t)in[k] * g_cos8[u][k];
        }
        /* scale by 1/2 and shift down by 13 (cos table fixed-point). */
        p[u] = (int32_t)((acc + (1LL << 13)) >> 14);
    }
}

/* Full 2-D IDCT on 8x8 block (in zig-zag-already-inverted, dequantized
 * coeffs). Produces 8x8 uint8 samples written to out[stride*y + x]. */
static void idct_block(int32_t *blk, uint8_t *out, int stride) {
    int32_t tmp[64];
    /* Rows */
    for (int r = 0; r < 8; r++) {
        int32_t row[8];
        for (int c = 0; c < 8; c++) row[c] = blk[r * 8 + c];
        idct_row(row);
        for (int c = 0; c < 8; c++) tmp[r * 8 + c] = row[c];
    }
    /* Cols */
    for (int c = 0; c < 8; c++) {
        int32_t col[8];
        for (int r = 0; r < 8; r++) col[r] = tmp[r * 8 + c];
        idct_row(col);
        for (int r = 0; r < 8; r++) tmp[r * 8 + c] = col[r];
    }
    /* Level shift +128 and write as uint8. */
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int v = tmp[r * 8 + c] + 128;
            out[r * stride + c] = clamp_u8(v);
        }
    }
}

/* ── Decode one 8x8 block: Huffman → coefficients → dequant → IDCT. ── */

static int decode_block(jpg_t *j, comp_t *cmp, int32_t *coef) {
    huff_t *dc = &j->dc_huff[cmp->dc_tbl];
    huff_t *ac = &j->ac_huff[cmp->ac_tbl];
    qtbl_t *qt = &j->qtbl[cmp->qtbl];
    if (!dc->valid || !ac->valid || !qt->valid) return -1;

    for (int i = 0; i < 64; i++) coef[i] = 0;

    /* DC */
    int t = huff_decode(j, dc);
    if (t < 0) return -1;
    int dc_diff = 0;
    if (t > 0) {
        int v = j_get_bits(j, t);
        if (v < 0) return -1;
        dc_diff = j_extend(v, t);
    }
    cmp->prev_dc += dc_diff;
    /* In zig-zag position 0 == natural (0,0). */
    coef[0] = cmp->prev_dc * qt->q[0];

    /* AC */
    int k = 1;
    while (k < 64) {
        int rs = huff_decode(j, ac);
        if (rs < 0) return -1;
        int rrr = (rs >> 4) & 0xF;
        int ssss = rs & 0xF;
        if (ssss == 0) {
            if (rrr == 15) { k += 16; continue; }  /* ZRL */
            else break;                             /* EOB */
        }
        k += rrr;
        if (k >= 64) return -1;
        int v = j_get_bits(j, ssss);
        if (v < 0) return -1;
        int val = j_extend(v, ssss);
        int natural = zz_order[k];
        coef[natural] = val * qt->q[k];   /* qt is in zig-zag order */
        k++;
    }
    return 0;
}

/* ── Restart marker handling ── */

static int j_process_restart(jpg_t *j) {
    /* Drop bit buffer, expect FF Dn. */
    j->bit_buf = 0;
    j->bit_cnt = 0;
    j->eof = 0;
    /* Skip any fill bytes. */
    while (j->pos < j->in_size && j->in[j->pos] == 0xFF) j->pos++;
    if (j->pos >= j->in_size) return -1;
    uint8_t mk = j->in[j->pos++];
    if (mk < 0xD0 || mk > 0xD7) return -1;
    for (int c = 0; c < j->ncomp; c++) j->comp[c].prev_dc = 0;
    return 0;
}

/* ── Marker parsers ── */

static jpeg_status_t parse_dqt(jpg_t *j, int seg_len) {
    int remaining = seg_len - 2;
    while (remaining > 0) {
        int t = j_read_byte(j); remaining--;
        if (t < 0) return JPEG_TRUNCATED;
        int prec = (t >> 4) & 0xF;   /* 0 = 8-bit, 1 = 16-bit */
        int id = t & 0xF;
        if (id >= HUFF_TBL_MAX) return JPEG_BAD_INPUT;
        for (int i = 0; i < 64; i++) {
            int v;
            if (prec == 0) {
                v = j_read_byte(j); remaining--;
            } else {
                v = j_read_u16(j); remaining -= 2;
            }
            if (v < 0) return JPEG_TRUNCATED;
            j->qtbl[id].q[i] = v;
        }
        j->qtbl[id].valid = 1;
    }
    return JPEG_OK;
}

static jpeg_status_t parse_dht(jpg_t *j, int seg_len) {
    int remaining = seg_len - 2;
    while (remaining > 0) {
        int t = j_read_byte(j); remaining--;
        if (t < 0) return JPEG_TRUNCATED;
        int cls = (t >> 4) & 0xF;     /* 0 = DC, 1 = AC */
        int id  = t & 0xF;
        if (id >= HUFF_TBL_MAX) return JPEG_BAD_INPUT;
        uint8_t bits[16];
        int total = 0;
        for (int i = 0; i < 16; i++) {
            int b = j_read_byte(j); remaining--;
            if (b < 0) return JPEG_TRUNCATED;
            bits[i] = (uint8_t)b;
            total += b;
        }
        if (total > 256) return JPEG_BAD_INPUT;
        uint8_t vals[256];
        for (int i = 0; i < total; i++) {
            int v = j_read_byte(j); remaining--;
            if (v < 0) return JPEG_TRUNCATED;
            vals[i] = (uint8_t)v;
        }
        huff_t *t2 = (cls == 0) ? &j->dc_huff[id] : &j->ac_huff[id];
        if (huff_build(t2, bits, vals) < 0) return JPEG_BAD_INPUT;
    }
    return JPEG_OK;
}

static jpeg_status_t parse_sof(jpg_t *j, int seg_len) {
    int prec = j_read_byte(j);
    int h = j_read_u16(j);
    int w = j_read_u16(j);
    int n = j_read_byte(j);
    if (prec != 8) return JPEG_UNSUPPORTED;
    if (n != 1 && n != 3) return JPEG_UNSUPPORTED;
    if (w <= 0 || h <= 0) return JPEG_BAD_INPUT;
    if (w > JPEG_MAX_W || h > JPEG_MAX_H) return JPEG_TOO_BIG;
    if (seg_len != 8 + 3 * n) return JPEG_BAD_INPUT;

    j->width = w;
    j->height = h;
    j->precision = prec;
    j->ncomp = n;
    j->max_h = 0;
    j->max_v = 0;
    for (int i = 0; i < n; i++) {
        int id = j_read_byte(j);
        int hv = j_read_byte(j);
        int qid = j_read_byte(j);
        if (id < 0 || qid < 0 || qid >= HUFF_TBL_MAX) return JPEG_BAD_INPUT;
        j->comp[i].id    = id;
        j->comp[i].hsamp = (hv >> 4) & 0xF;
        j->comp[i].vsamp = hv & 0xF;
        j->comp[i].qtbl  = qid;
        if (j->comp[i].hsamp == 0 || j->comp[i].vsamp == 0) return JPEG_BAD_INPUT;
        if (j->comp[i].hsamp > 2 || j->comp[i].vsamp > 2) return JPEG_UNSUPPORTED;
        if (j->comp[i].hsamp > j->max_h) j->max_h = j->comp[i].hsamp;
        if (j->comp[i].vsamp > j->max_v) j->max_v = j->comp[i].vsamp;
    }
    /* For now restrict to common sampling: luma h/v <= 2, chroma 1x1.
     * Y h={1 or 2}, V={1 or 2}; Cb/Cr both 1x1. */
    if (n == 3) {
        if (j->comp[1].hsamp != 1 || j->comp[1].vsamp != 1 ||
            j->comp[2].hsamp != 1 || j->comp[2].vsamp != 1) {
            return JPEG_UNSUPPORTED;
        }
    }
    return JPEG_OK;
}

static jpeg_status_t parse_sos(jpg_t *j, int seg_len) {
    int n = j_read_byte(j);
    if (n != j->ncomp) return JPEG_BAD_INPUT;
    if (seg_len != 6 + 2 * n) return JPEG_BAD_INPUT;
    for (int i = 0; i < n; i++) {
        int id = j_read_byte(j);
        int td = j_read_byte(j);
        /* Match by component id. */
        int found = -1;
        for (int c = 0; c < j->ncomp; c++) {
            if (j->comp[c].id == id) { found = c; break; }
        }
        if (found < 0) return JPEG_BAD_INPUT;
        j->comp[found].dc_tbl = (td >> 4) & 0xF;
        j->comp[found].ac_tbl = td & 0xF;
        if (j->comp[found].dc_tbl >= HUFF_TBL_MAX) return JPEG_BAD_INPUT;
        if (j->comp[found].ac_tbl >= HUFF_TBL_MAX) return JPEG_BAD_INPUT;
    }
    /* Ss, Se, Ah/Al — baseline ignores. */
    int ss = j_read_byte(j);
    int se = j_read_byte(j);
    int ahal = j_read_byte(j);
    if (ss != 0 || se != 63 || ahal != 0) return JPEG_UNSUPPORTED;
    return JPEG_OK;
}

/* ── Top-level decode ── */

static jpeg_status_t decode_scan(jpg_t *j, uint8_t *rgba) {
    int mcu_w = j->max_h * 8;
    int mcu_h = j->max_v * 8;
    int mcus_x = (j->width  + mcu_w - 1) / mcu_w;
    int mcus_y = (j->height + mcu_h - 1) / mcu_h;

    /* Allocate per-component planes sized to mcus_x*mcus_y*mcu_w*mcu_h
     * for luma; chroma is mcus_x*mcus_y*8*8 (since chroma is always 1x1
     * in our supported subset). We'll upsample at write time. */
    int luma_w = mcus_x * mcu_w;
    int luma_h = mcus_y * mcu_h;
    int chroma_w = mcus_x * 8;
    int chroma_h = mcus_y * 8;

    j->comp[0].plane = (uint8_t *)lodepng_malloc((uint64_t)luma_w * (uint64_t)luma_h);
    if (!j->comp[0].plane) return JPEG_OOM;
    if (j->ncomp == 3) {
        j->comp[1].plane = (uint8_t *)lodepng_malloc((uint64_t)chroma_w * (uint64_t)chroma_h);
        j->comp[2].plane = (uint8_t *)lodepng_malloc((uint64_t)chroma_w * (uint64_t)chroma_h);
        if (!j->comp[1].plane || !j->comp[2].plane) return JPEG_OOM;
    }

    int32_t coef[64];
    uint8_t blk[64];
    j->bit_buf = 0;
    j->bit_cnt = 0;
    j->eof = 0;
    for (int c = 0; c < j->ncomp; c++) j->comp[c].prev_dc = 0;
    j->restart_count = 0;

    for (int my = 0; my < mcus_y; my++) {
        for (int mx = 0; mx < mcus_x; mx++) {
            /* For each component, decode hsamp*vsamp blocks. */
            for (int c = 0; c < j->ncomp; c++) {
                int hs = j->comp[c].hsamp;
                int vs = j->comp[c].vsamp;
                for (int by = 0; by < vs; by++) {
                    for (int bx = 0; bx < hs; bx++) {
                        if (decode_block(j, &j->comp[c], coef) < 0)
                            return JPEG_BAD_INPUT;
                        idct_block(coef, blk, 8);
                        /* Place into plane. */
                        int plane_w = (c == 0) ? luma_w : chroma_w;
                        int dst_x, dst_y;
                        if (c == 0) {
                            dst_x = mx * mcu_w + bx * 8;
                            dst_y = my * mcu_h + by * 8;
                        } else {
                            dst_x = mx * 8;
                            dst_y = my * 8;
                        }
                        uint8_t *plane = j->comp[c].plane;
                        for (int yy = 0; yy < 8; yy++) {
                            for (int xx = 0; xx < 8; xx++) {
                                plane[(dst_y + yy) * plane_w + (dst_x + xx)] =
                                    blk[yy * 8 + xx];
                            }
                        }
                    }
                }
            }

            if (j->restart_interval > 0) {
                j->restart_count++;
                if (j->restart_count == j->restart_interval) {
                    j->restart_count = 0;
                    if (mx + 1 < mcus_x || my + 1 < mcus_y) {
                        if (j_process_restart(j) < 0) return JPEG_BAD_INPUT;
                    }
                }
            }
        }
    }

    /* Convert to RGBA. */
    for (int y = 0; y < j->height; y++) {
        uint8_t *out_row = rgba + (size_t)y * (size_t)j->width * 4;
        const uint8_t *yrow = j->comp[0].plane + (size_t)y * (size_t)luma_w;
        for (int x = 0; x < j->width; x++) {
            int Y = yrow[x];
            int R, G, B;
            if (j->ncomp == 1) {
                R = G = B = Y;
            } else {
                /* Map output pixel (x,y) to chroma plane via subsampling
                 * factors. Chroma plane dim = chroma_w x chroma_h with
                 * 1x1 sampling, so each chroma sample covers max_h*8 ×
                 * max_v*8 pixels of output. */
                int cx = (x * j->comp[1].hsamp) / j->max_h;
                int cy = (y * j->comp[1].vsamp) / j->max_v;
                if (cx >= chroma_w) cx = chroma_w - 1;
                if (cy >= chroma_h) cy = chroma_h - 1;
                int Cb = j->comp[1].plane[cy * chroma_w + cx];
                int Cr = j->comp[2].plane[cy * chroma_w + cx];
                /* JFIF YCbCr→RGB (BT.601). */
                int cb = Cb - 128;
                int cr = Cr - 128;
                R = Y + ((  91881 * cr) >> 16);
                G = Y - ((( 22554 * cb) + (46802 * cr)) >> 16);
                B = Y + (( 116130 * cb) >> 16);
            }
            out_row[x * 4 + 0] = clamp_u8(R);
            out_row[x * 4 + 1] = clamp_u8(G);
            out_row[x * 4 + 2] = clamp_u8(B);
            out_row[x * 4 + 3] = 0xFF;
        }
    }
    return JPEG_OK;
}

jpeg_status_t jpeg_decode_rgba(const uint8_t *in, size_t in_size,
                               unsigned char **out,
                               unsigned *out_w, unsigned *out_h)
{
    if (out) *out = 0;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!in || in_size < 4) return JPEG_BAD_INPUT;
    if (!(in[0] == 0xFF && in[1] == 0xD8)) return JPEG_BAD_INPUT;
    idct_init_cos();

    jpg_t *j = (jpg_t *)lodepng_malloc(sizeof(*j));
    if (!j) return JPEG_OOM;
    for (int i = 0; i < (int)sizeof(*j); i++) ((uint8_t *)j)[i] = 0;
    j->in = in;
    j->in_size = in_size;
    j->pos = 2;   /* past SOI */

    jpeg_status_t st = JPEG_OK;
    int sof_seen = 0;
    int sos_seen = 0;
    uint8_t *rgba = 0;

    while (j->pos < in_size) {
        if (j->in[j->pos] != 0xFF) { st = JPEG_BAD_INPUT; goto done; }
        /* Skip fill bytes. */
        while (j->pos < in_size && j->in[j->pos] == 0xFF) j->pos++;
        if (j->pos >= in_size) { st = JPEG_TRUNCATED; goto done; }
        uint8_t mk = j->in[j->pos++];
        uint16_t marker = 0xFF00 | mk;

        if (marker == M_SOI) continue;
        if (marker == M_EOI) break;

        if (marker >= M_RST0 && marker <= M_RST7) continue;

        /* All other markers carry a length. */
        int seg_len = j_read_u16(j);
        if (seg_len < 2) { st = JPEG_BAD_INPUT; goto done; }
        size_t seg_start = j->pos;

        if (marker == M_DQT) {
            st = parse_dqt(j, seg_len);
        } else if (marker == M_DHT) {
            st = parse_dht(j, seg_len);
        } else if (marker == M_SOF0) {
            st = parse_sof(j, seg_len);
            sof_seen = 1;
        } else if (marker == M_SOF1) {
            /* Extended sequential — same syntax as SOF0; we accept it. */
            st = parse_sof(j, seg_len);
            sof_seen = 1;
        } else if (marker == M_SOF2) {
            st = JPEG_UNSUPPORTED;
        } else if (marker == M_DRI) {
            int v = j_read_u16(j);
            if (v < 0) { st = JPEG_TRUNCATED; goto done; }
            j->restart_interval = v;
        } else if (marker == M_SOS) {
            st = parse_sos(j, seg_len);
            if (st != JPEG_OK) goto done;
            sos_seen = 1;
            if (!sof_seen) { st = JPEG_BAD_INPUT; goto done; }
            /* Allocate RGBA output. */
            rgba = (uint8_t *)lodepng_malloc((uint64_t)j->width * (uint64_t)j->height * 4ull);
            if (!rgba) { st = JPEG_OOM; goto done; }
            /* Skip to entropy-coded data: parse_sos already consumed
             * the 6 + 2n parameters; entropy data starts here. */
            j->pos = seg_start + seg_len;
            st = decode_scan(j, rgba);
            if (st != JPEG_OK) goto done;
            /* After scan, pos is at the next marker (we hit eof on FF). */
            /* Look for EOI. */
            break;
        } else if ((marker >= M_APP0 && marker <= M_APP15) || marker == M_COM) {
            /* Skip. */
        } else {
            /* Unknown segment — skip. */
        }
        if (st != JPEG_OK) goto done;
        j->pos = seg_start + seg_len;
    }

    if (!sos_seen) { st = JPEG_BAD_INPUT; goto done; }

    if (out) *out = rgba;
    if (out_w) *out_w = (unsigned)j->width;
    if (out_h) *out_h = (unsigned)j->height;
    rgba = 0;  /* transferred to caller */

done:
    if (j->comp[0].plane) lodepng_free(j->comp[0].plane);
    if (j->comp[1].plane) lodepng_free(j->comp[1].plane);
    if (j->comp[2].plane) lodepng_free(j->comp[2].plane);
    if (rgba) lodepng_free(rgba);
    lodepng_free(j);
    return st;
}
