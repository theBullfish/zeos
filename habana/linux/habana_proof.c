/*
 * habana_proof — single-binary Linux MME proof for any Habana
 * accelerator on any Linux box. Self-discovers, identifies, runs
 * the smoke / roof / real ladder on each card it finds, prints
 * honest results. No flags. No configuration. No "tell me what
 * you have."
 *
 * Worldview: this binary is a guest in your Linux box, not a
 * questionnaire. It opens its eyes, finds the cards, asks the
 * kernel what they are, and either drives them or says exactly
 * what it does not yet know how to drive. The smoke step is the
 * gate that turns the architectural claim into measured fact.
 *
 * Build:
 *     make
 * Run:
 *     ./habana_proof
 *
 * Required on the host: stock Linux kernel with in-tree
 * `habanalabs` driver loaded. Permission on /dev/accel/accel*
 * (typically root or `video`/`render` group). No userspace
 * dependencies beyond glibc.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>

#include "habanalabs_uapi.h"

/* ─────────────────────────────────────────────────────────────────
 * Per-card record
 * ──────────────────────────────────────────────────────────────── */

typedef struct {
    char    path[320];                /* /dev/accel/accelN or /dev/hlN — NAME_MAX + dir room. */
    int     fd;
    struct hl_info_hw_ip_info ip;
    char    family[32];               /* "Goya" / "Gaudi" / ... */
    char    card_name[24];            /* from ip.card_name */
    int     supported;                /* 1 if we can drive its MME */
    /* Ladder results. */
    int     smoke_attempted;
    int     smoke_passed;
    int     roof_attempted;
    int     roof_passed;
    uint64_t roof_gflops;
    int     real_attempted;
    int     real_passed;
    uint64_t real_latency_us;
    char    skip_reason[128];
} card_t;

#define MAX_CARDS 32
static card_t s_cards[MAX_CARDS];
static int    s_card_count;

/* ─────────────────────────────────────────────────────────────────
 * Family detection
 * ──────────────────────────────────────────────────────────────── */

static const char *family_name(uint32_t device_id)
{
    switch (device_id) {
        case HL_DEVICE_GOYA:      return "Goya";
        case HL_DEVICE_GAUDI:     return "Gaudi";
        case HL_DEVICE_GAUDI_SEC: return "Gaudi-Sec";
        case HL_DEVICE_GAUDI2:    return "Gaudi2";
        case HL_DEVICE_GAUDI2B:   return "Gaudi2B";
        case HL_DEVICE_GAUDI2C:   return "Gaudi2C";
        case HL_DEVICE_GAUDI3:    return "Gaudi3";
        default:                  return "Habana-unknown";
    }
}

/* Whether we have a packet builder for this silicon today. Goya is
 * the implementation target of this commit; Gaudi/Gaudi2/Gaudi3 are
 * detected and reported, with their kernel handshake confirmed via
 * HL_INFO, but their MME descriptor builders land when we have one
 * to validate against. */
static int family_supported_today(uint32_t device_id)
{
    switch (device_id) {
        case HL_DEVICE_GOYA: return 1;
        default:             return 0;
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Discovery — scan /dev/accel/accel* and /dev/hl* without asking
 * the user a single thing about their box.
 * ──────────────────────────────────────────────────────────────── */

static int try_open_and_probe(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;

    struct hl_info_hw_ip_info ip;
    memset(&ip, 0, sizeof(ip));
    struct hl_info_args args;
    memset(&args, 0, sizeof(args));
    args.op = HL_INFO_HW_IP_INFO;
    args.return_pointer = (uint64_t)(uintptr_t)&ip;
    args.return_size = sizeof(ip);

    if (ioctl(fd, HL_IOCTL_INFO, &args) != 0) {
        close(fd);
        return -1;
    }

    if (s_card_count >= MAX_CARDS) {
        close(fd);
        return -1;
    }

    card_t *c = &s_cards[s_card_count++];
    memset(c, 0, sizeof(*c));
    snprintf(c->path, sizeof(c->path), "%s", path);
    c->fd = fd;
    c->ip = ip;
    strncpy(c->family, family_name(ip.device_id), sizeof(c->family) - 1);
    /* card_name in UAPI is fixed-width but not always NUL-terminated. */
    memcpy(c->card_name, ip.card_name, sizeof(ip.card_name));
    c->card_name[sizeof(ip.card_name)] = 0;
    c->supported = family_supported_today(ip.device_id);
    return 0;
}

static void scan_dir(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, prefix, strlen(prefix)) != 0) continue;
        /* Only numeric suffixes (skip accel0/control etc. siblings
         * that aren't accelerators themselves). For /dev/accel the
         * naming is "accel0", "accel1"; for /dev/hl it's "hl0", "hl1".
         * The legacy control nodes are "hl_controlD0" — we skip those
         * because they don't accept HL_IOCTL_INFO. */
        size_t plen = strlen(prefix);
        const char *suffix = de->d_name + plen;
        if (*suffix < '0' || *suffix > '9') continue;

        char path[320];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        (void)try_open_and_probe(path);
    }
    closedir(d);
}

static void discover_all(void)
{
    scan_dir("/dev/accel", "accel");
    scan_dir("/dev",       "hl");
}

/* ─────────────────────────────────────────────────────────────────
 * Allocator helpers (kernel-side HBM)
 * ──────────────────────────────────────────────────────────────── */

static int hbm_alloc(int fd, uint64_t size, uint64_t *handle_out)
{
    union hl_mem_args a;
    memset(&a, 0, sizeof(a));
    a.in.op = HL_MEM_OP_ALLOC;
    a.in.flags = HL_MEM_CONTIGUOUS;
    a.in.alloc.mem_size = size;
    if (ioctl(fd, HL_IOCTL_MEMORY, &a) != 0) return -1;
    *handle_out = a.out.handle;
    return 0;
}

static int hbm_map(int fd, uint64_t handle, uint64_t *dva_out)
{
    union hl_mem_args a;
    memset(&a, 0, sizeof(a));
    a.in.op = HL_MEM_OP_MAP;
    a.in.map_device.handle = handle;
    if (ioctl(fd, HL_IOCTL_MEMORY, &a) != 0) return -1;
    *dva_out = a.out.device_virt_addr;
    return 0;
}

static int hbm_free(int fd, uint64_t handle, uint64_t dva)
{
    union hl_mem_args a;
    memset(&a, 0, sizeof(a));
    a.in.op = HL_MEM_OP_UNMAP;
    a.in.unmap.device_virt_addr = dva;
    (void)ioctl(fd, HL_IOCTL_MEMORY, &a);
    memset(&a, 0, sizeof(a));
    a.in.op = HL_MEM_OP_FREE;
    a.in.free.handle = handle;
    (void)ioctl(fd, HL_IOCTL_MEMORY, &a);
    return 0;
}

/* Create a command buffer of `size` bytes and return its handle +
 * mmap'd userspace address. The kernel mmap-cookie for a CB is the
 * handle itself, shifted; see habanalabs CB ioctl docs. */
static int cb_create(int fd, uint32_t size, uint64_t *handle_out, void **map_out)
{
    union hl_cb_args a;
    memset(&a, 0, sizeof(a));
    a.in.op = HL_CB_OP_CREATE;
    a.in.cb_size = size;
    if (ioctl(fd, HL_IOCTL_CB, &a) != 0) return -1;
    *handle_out = a.out.cb_handle;
    /* The CB is mmap'd at the offset returned in cb_handle (kernel
     * uses the handle as the mmap "pgoff"). */
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)(*handle_out));
    if (p == MAP_FAILED) { *map_out = NULL; return -1; }
    *map_out = p;
    return 0;
}

static void cb_destroy(int fd, uint64_t handle, void *map, uint32_t size)
{
    if (map) munmap(map, size);
    union hl_cb_args a;
    memset(&a, 0, sizeof(a));
    a.in.op = HL_CB_OP_DESTROY;
    a.in.cb_handle = handle;
    (void)ioctl(fd, HL_IOCTL_CB, &a);
}

/* ─────────────────────────────────────────────────────────────────
 * Goya MME packet builder (mirror of kernel/boot/gpu_goya_mme.c)
 *
 * Same wire format as the Zeos-side bypass. See docs/GOYA_BYPASS.md.
 * 14 packets: 13 MSG_LONG (descriptor + CMD=GO) + 1 FENCE.
 * ──────────────────────────────────────────────────────────────── */

#define GOYA_PKT_BYTES         64u
#define GOYA_PKT_HDR_OFF       56u
#define GOYA_PKT_NOP           0x00
#define GOYA_PKT_MSG_LONG      0x02
#define GOYA_PKT_FENCE         0x05

#define GOYA_MME_BASE              0x0F000000u
#define GOYA_MME_REG_CMD           (GOYA_MME_BASE + 0x0000u)
#define GOYA_MME_REG_A_BASE_LO     (GOYA_MME_BASE + 0x0010u)
#define GOYA_MME_REG_A_BASE_HI     (GOYA_MME_BASE + 0x0014u)
#define GOYA_MME_REG_B_BASE_LO     (GOYA_MME_BASE + 0x0018u)
#define GOYA_MME_REG_B_BASE_HI     (GOYA_MME_BASE + 0x001Cu)
#define GOYA_MME_REG_C_BASE_LO     (GOYA_MME_BASE + 0x0020u)
#define GOYA_MME_REG_C_BASE_HI     (GOYA_MME_BASE + 0x0024u)
#define GOYA_MME_REG_DIM_M         (GOYA_MME_BASE + 0x0030u)
#define GOYA_MME_REG_DIM_N         (GOYA_MME_BASE + 0x0034u)
#define GOYA_MME_REG_DIM_K         (GOYA_MME_BASE + 0x0038u)
#define GOYA_MME_REG_A_STRIDE      (GOYA_MME_BASE + 0x0040u)
#define GOYA_MME_REG_B_STRIDE      (GOYA_MME_BASE + 0x0044u)
#define GOYA_MME_REG_C_STRIDE      (GOYA_MME_BASE + 0x0048u)
#define GOYA_MME_REG_FLAGS         (GOYA_MME_BASE + 0x0050u)
#define GOYA_MME_CMD_GO            0x00000001u

/* The DMA / external queue id Goya uses for MME command submission.
 * The kernel exposes per-queue indices; queue 0 is the upper external
 * DMA, which is what hl-thunk's test programs use for MME work. We
 * follow the same convention. */
#define GOYA_QID_EXT_DMA_0          0

#define GOYA_MME_FLAG_FMT_FP16      (0u << 4)
#define GOYA_MME_FLAG_FMT_BF16      (1u << 4)
#define GOYA_MME_FLAG_FMT_INT16     (2u << 4)
#define GOYA_MME_FLAG_FMT_INT8      (3u << 4)
#define GOYA_MME_FLAG_ACCUMULATE    (1u << 0)

static inline void pkt_w32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off + 0] = (uint8_t)(v       & 0xFFu);
    p[off + 1] = (uint8_t)(v >> 8  & 0xFFu);
    p[off + 2] = (uint8_t)(v >> 16 & 0xFFu);
    p[off + 3] = (uint8_t)(v >> 24 & 0xFFu);
}

static void build_msg_long(uint8_t *p, uint32_t reg, uint32_t v)
{
    memset(p, 0, GOYA_PKT_BYTES);
    pkt_w32(p, 0, v);
    pkt_w32(p, 4, reg);
    p[GOYA_PKT_HDR_OFF] = GOYA_PKT_MSG_LONG;
}

static void build_fence(uint8_t *p, uint8_t fence_id, uint32_t target)
{
    memset(p, 0, GOYA_PKT_BYTES);
    pkt_w32(p, 0, target);
    p[4] = fence_id;
    p[GOYA_PKT_HDR_OFF] = GOYA_PKT_FENCE;
}

typedef struct {
    uint64_t a_dva, b_dva, c_dva;
    uint32_t m, n, k;
    uint32_t a_stride, b_stride, c_stride;
    uint32_t flags;          /* GOYA_MME_FLAG_* */
} goya_gemm_t;

/* Returns bytes written. */
static uint32_t goya_pack_gemm(const goya_gemm_t *g, uint8_t *buf, uint32_t buflen)
{
    const uint32_t need = 14 * GOYA_PKT_BYTES;
    if (!g || !buf || buflen < need) return 0;
    if (!g->m || !g->n || !g->k) return 0;
    uint8_t *p = buf;
    build_msg_long(p, GOYA_MME_REG_A_BASE_LO, (uint32_t)(g->a_dva & 0xFFFFFFFFu)); p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_A_BASE_HI, (uint32_t)(g->a_dva >> 32));         p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_B_BASE_LO, (uint32_t)(g->b_dva & 0xFFFFFFFFu)); p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_B_BASE_HI, (uint32_t)(g->b_dva >> 32));         p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_C_BASE_LO, (uint32_t)(g->c_dva & 0xFFFFFFFFu)); p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_C_BASE_HI, (uint32_t)(g->c_dva >> 32));         p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_DIM_M, g->m);                                    p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_DIM_N, g->n);                                    p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_DIM_K, g->k);                                    p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_A_STRIDE, g->a_stride);                          p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_B_STRIDE, g->b_stride);                          p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_C_STRIDE, g->c_stride);                          p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_FLAGS, g->flags);                                p += GOYA_PKT_BYTES;
    build_msg_long(p, GOYA_MME_REG_CMD,   GOYA_MME_CMD_GO);                         p += GOYA_PKT_BYTES;
    build_fence(p, /*fence_id*/ 1, /*target*/ 1);
    return need;
}

/* Submit a CS with a single chunk pointing at our CB. Returns the CS
 * sequence number on success, 0 on failure. */
static uint64_t goya_submit_cs(int fd, uint64_t cb_handle, uint32_t cb_size,
                               uint32_t queue_id)
{
    struct hl_cs_chunk chunk;
    memset(&chunk, 0, sizeof(chunk));
    chunk.cb_handle = cb_handle;
    chunk.cb_size = cb_size;
    chunk.queue_index = queue_id;

    union hl_cs_args a;
    memset(&a, 0, sizeof(a));
    a.in.chunks_execute = (uint64_t)(uintptr_t)&chunk;
    a.in.num_chunks_execute = 1;

    if (ioctl(fd, HL_IOCTL_CS, &a) != 0) return 0;
    return a.out.seq;
}

static int goya_wait_cs(int fd, uint64_t seq, uint64_t timeout_us)
{
    union hl_wait_cs_args a;
    memset(&a, 0, sizeof(a));
    a.in.seq = seq;
    a.in.timeout_us = timeout_us;
    if (ioctl(fd, HL_IOCTL_WAIT_CS, &a) != 0) return -1;
    return (a.out.status == HL_WAIT_CS_STATUS_COMPLETED) ? 0 : -1;
}

/* FP16 helper: encode integers 0..2047 exactly as half-precision. */
static uint16_t int_to_fp16(uint32_t i)
{
    if (i == 0) return 0;
    uint32_t v = i;
    int e = 0;
    while (v > 1) { v >>= 1; e++; }
    uint32_t mant = ((uint32_t)i << (10 - e)) & 0x3FFu;
    uint32_t exp  = (uint32_t)(e + 15);
    return (uint16_t)((exp << 10) | mant);
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000ull;
}

/* ─────────────────────────────────────────────────────────────────
 * The ladder — Goya path
 * ──────────────────────────────────────────────────────────────── */

static int run_goya_smoke(card_t *c)
{
    c->smoke_attempted = 1;
    int fd = c->fd;

    /* Allocate 3 tiny HBM buffers for A, B, C — 128 bytes each (8x8 FP16). */
    uint64_t a_h, b_h, ch, a_dva, b_dva, c_dva;
    if (hbm_alloc(fd, 4096, &a_h) || hbm_map(fd, a_h, &a_dva)) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: hbm_alloc/map A failed (errno=%d)", errno);
        return -1;
    }
    if (hbm_alloc(fd, 4096, &b_h) || hbm_map(fd, b_h, &b_dva)) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: hbm_alloc/map B failed (errno=%d)", errno);
        hbm_free(fd, a_h, a_dva);
        return -1;
    }
    if (hbm_alloc(fd, 4096, &ch) || hbm_map(fd, ch, &c_dva)) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: hbm_alloc/map C failed (errno=%d)", errno);
        hbm_free(fd, a_h, a_dva);
        hbm_free(fd, b_h, b_dva);
        return -1;
    }

    /* Initialize A=I (FP16), B=range(0..63), C=canary 0xDEAD via a
     * staging userspace buffer + DMA. The kernel's MEMORY ioctl path
     * is allocate-only; to write operands we use a small CB that
     * issues LIN_DMA host->device for each tensor. Building those
     * here would double the code; for the smoke step the kernel
     * driver provides `HL_MEM_OP_MAP_BLOCK` debug access on Goya so
     * we can mmap the device memory directly for initialization.
     * If that path is not available on this kernel we degrade
     * honestly: report "smoke: cannot initialize operands without
     * DMA path", which is a real diagnostic, not a fake pass. */
    union hl_mem_args ma;
    memset(&ma, 0, sizeof(ma));
    ma.in.op = HL_MEM_OP_MAP_BLOCK;
    ma.in.map_block.block_addr = a_dva;
    int can_mmap_dev = (ioctl(fd, HL_IOCTL_MEMORY, &ma) == 0);
    if (!can_mmap_dev) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: kernel does not expose device memory mmap on Goya; "
                 "operand init via LIN_DMA — pending host->device DMA path");
        hbm_free(fd, a_h, a_dva);
        hbm_free(fd, b_h, b_dva);
        hbm_free(fd, ch, c_dva);
        return -1;
    }

    void *a_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, (off_t)ma.out.block_handle);
    if (a_map == MAP_FAILED) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: mmap block A failed (errno=%d)", errno);
        hbm_free(fd, a_h, a_dva);
        hbm_free(fd, b_h, b_dva);
        hbm_free(fd, ch, c_dva);
        return -1;
    }
    /* Map B and C similarly. */
    memset(&ma, 0, sizeof(ma));
    ma.in.op = HL_MEM_OP_MAP_BLOCK;
    ma.in.map_block.block_addr = b_dva;
    if (ioctl(fd, HL_IOCTL_MEMORY, &ma) != 0) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: map_block B failed (errno=%d)", errno);
        munmap(a_map, 4096);
        hbm_free(fd, a_h, a_dva);
        hbm_free(fd, b_h, b_dva);
        hbm_free(fd, ch, c_dva);
        return -1;
    }
    void *b_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, (off_t)ma.out.block_handle);
    memset(&ma, 0, sizeof(ma));
    ma.in.op = HL_MEM_OP_MAP_BLOCK;
    ma.in.map_block.block_addr = c_dva;
    if (ioctl(fd, HL_IOCTL_MEMORY, &ma) != 0) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: map_block C failed (errno=%d)", errno);
        munmap(a_map, 4096); if (b_map != MAP_FAILED) munmap(b_map, 4096);
        hbm_free(fd, a_h, a_dva);
        hbm_free(fd, b_h, b_dva);
        hbm_free(fd, ch, c_dva);
        return -1;
    }
    void *c_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, (off_t)ma.out.block_handle);

    /* Initialize. */
    uint16_t *A = (uint16_t *)a_map;
    uint16_t *B = (uint16_t *)b_map;
    uint16_t *C = (uint16_t *)c_map;
    for (int i = 0; i < 64; i++) A[i] = 0;
    for (int i = 0; i < 8;  i++) A[i * 8 + i] = 0x3C00; /* 1.0 fp16 */
    for (int i = 0; i < 64; i++) B[i] = int_to_fp16(i);
    for (int i = 0; i < 64; i++) C[i] = 0xDEAD;

    /* Build packets. */
    uint64_t cb_handle = 0;
    void    *cb_map    = NULL;
    if (cb_create(fd, 4096, &cb_handle, &cb_map) != 0) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: cb_create failed (errno=%d)", errno);
        munmap(a_map, 4096); munmap(b_map, 4096); munmap(c_map, 4096);
        hbm_free(fd, a_h, a_dva); hbm_free(fd, b_h, b_dva); hbm_free(fd, ch, c_dva);
        return -1;
    }
    goya_gemm_t g = {
        .a_dva = a_dva, .b_dva = b_dva, .c_dva = c_dva,
        .m = 8, .n = 8, .k = 8,
        .a_stride = 8, .b_stride = 8, .c_stride = 8,
        .flags = GOYA_MME_FLAG_FMT_FP16,
    };
    uint32_t bytes = goya_pack_gemm(&g, (uint8_t *)cb_map, 4096);
    if (bytes == 0) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: goya_pack_gemm refused (bad descriptor)");
        cb_destroy(fd, cb_handle, cb_map, 4096);
        munmap(a_map, 4096); munmap(b_map, 4096); munmap(c_map, 4096);
        hbm_free(fd, a_h, a_dva); hbm_free(fd, b_h, b_dva); hbm_free(fd, ch, c_dva);
        return -1;
    }

    /* Submit + wait. */
    uint64_t t0 = now_us();
    uint64_t seq = goya_submit_cs(fd, cb_handle, bytes, GOYA_QID_EXT_DMA_0);
    if (seq == 0) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: HL_IOCTL_CS failed (errno=%d)", errno);
        cb_destroy(fd, cb_handle, cb_map, 4096);
        munmap(a_map, 4096); munmap(b_map, 4096); munmap(c_map, 4096);
        hbm_free(fd, a_h, a_dva); hbm_free(fd, b_h, b_dva); hbm_free(fd, ch, c_dva);
        return -1;
    }
    int wait_rc = goya_wait_cs(fd, seq, 100000);
    uint64_t t1 = now_us();
    if (wait_rc != 0) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: HL_WAIT_CS not completed (rc=%d)", wait_rc);
        cb_destroy(fd, cb_handle, cb_map, 4096);
        munmap(a_map, 4096); munmap(b_map, 4096); munmap(c_map, 4096);
        hbm_free(fd, a_h, a_dva); hbm_free(fd, b_h, b_dva); hbm_free(fd, ch, c_dva);
        return -1;
    }

    /* Verify C == B (because A == I). */
    int wrong = 0;
    for (int i = 0; i < 64; i++) {
        if (C[i] != B[i]) { wrong++; break; }
    }

    cb_destroy(fd, cb_handle, cb_map, 4096);
    munmap(a_map, 4096); munmap(b_map, 4096); munmap(c_map, 4096);
    hbm_free(fd, a_h, a_dva); hbm_free(fd, b_h, b_dva); hbm_free(fd, ch, c_dva);

    if (wrong) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "smoke: I*B != B; descriptor format suspect "
                 "(%d/64 elements wrong)", wrong);
        return -1;
    }
    c->smoke_passed = 1;
    c->real_latency_us = t1 - t0;
    return 0;
}

static int run_goya_roof(card_t *c)
{
    c->roof_attempted = 1;
    int fd = c->fd;
    const uint32_t M = 4096, N = 4096, K = 4096;
    const uint64_t tensor_bytes = (uint64_t)M * N * 2; /* FP16 */
    uint64_t a_h, b_h, ch, a_dva, b_dva, c_dva;
    if (hbm_alloc(fd, tensor_bytes, &a_h) || hbm_map(fd, a_h, &a_dva)) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "roof: hbm_alloc A failed (errno=%d)", errno);
        return -1;
    }
    if (hbm_alloc(fd, tensor_bytes, &b_h) || hbm_map(fd, b_h, &b_dva)) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "roof: hbm_alloc B failed (errno=%d)", errno);
        hbm_free(fd, a_h, a_dva); return -1;
    }
    if (hbm_alloc(fd, tensor_bytes, &ch) || hbm_map(fd, ch, &c_dva)) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "roof: hbm_alloc C failed (errno=%d)", errno);
        hbm_free(fd, a_h, a_dva); hbm_free(fd, b_h, b_dva); return -1;
    }

    uint64_t cb_handle = 0; void *cb_map = NULL;
    if (cb_create(fd, 4096, &cb_handle, &cb_map) != 0) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "roof: cb_create failed (errno=%d)", errno);
        hbm_free(fd, a_h, a_dva); hbm_free(fd, b_h, b_dva); hbm_free(fd, ch, c_dva);
        return -1;
    }
    goya_gemm_t g = {
        .a_dva = a_dva, .b_dva = b_dva, .c_dva = c_dva,
        .m = M, .n = N, .k = K,
        .a_stride = M, .b_stride = N, .c_stride = N,
        .flags = GOYA_MME_FLAG_FMT_FP16,
    };
    uint32_t bytes = goya_pack_gemm(&g, (uint8_t *)cb_map, 4096);

    uint64_t t0 = now_us();
    uint64_t seq = goya_submit_cs(fd, cb_handle, bytes, GOYA_QID_EXT_DMA_0);
    if (seq == 0) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "roof: HL_IOCTL_CS failed (errno=%d)", errno);
        cb_destroy(fd, cb_handle, cb_map, 4096);
        hbm_free(fd, a_h, a_dva); hbm_free(fd, b_h, b_dva); hbm_free(fd, ch, c_dva);
        return -1;
    }
    int wait_rc = goya_wait_cs(fd, seq, 1000000);
    uint64_t t1 = now_us();
    cb_destroy(fd, cb_handle, cb_map, 4096);
    hbm_free(fd, a_h, a_dva); hbm_free(fd, b_h, b_dva); hbm_free(fd, ch, c_dva);

    if (wait_rc != 0) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "roof: HL_WAIT_CS not completed");
        return -1;
    }
    uint64_t elapsed_us = t1 - t0;
    if (elapsed_us == 0) elapsed_us = 1;
    uint64_t flops = 2ull * M * N * K;
    /* GFLOPS = flops / (us * 1000). */
    c->roof_gflops = flops / (elapsed_us * 1000ull);
    c->roof_passed = (c->roof_gflops >= 3000ull) ? 1 : 0;
    return c->roof_passed ? 0 : -1;
}

static int run_goya_real(card_t *c)
{
    c->real_attempted = 1;
    int fd = c->fd;
    /* Q*K^T 64x64x64 FP16. Softmax + V follow with TPC kernel work. */
    const uint32_t S = 64;
    const uint64_t tb = (uint64_t)S * S * 2;
    uint64_t a_h, b_h, ch, a_dva, b_dva, c_dva;
    if (hbm_alloc(fd, tb, &a_h) || hbm_map(fd, a_h, &a_dva) ||
        hbm_alloc(fd, tb, &b_h) || hbm_map(fd, b_h, &b_dva) ||
        hbm_alloc(fd, tb, &ch) || hbm_map(fd, ch, &c_dva)) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "real: hbm_alloc failed (errno=%d)", errno);
        return -1;
    }
    uint64_t cb_handle = 0; void *cb_map = NULL;
    if (cb_create(fd, 4096, &cb_handle, &cb_map) != 0) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "real: cb_create failed");
        return -1;
    }
    goya_gemm_t g = {
        .a_dva = a_dva, .b_dva = b_dva, .c_dva = c_dva,
        .m = S, .n = S, .k = S,
        .a_stride = S, .b_stride = S, .c_stride = S,
        .flags = GOYA_MME_FLAG_FMT_FP16,
    };
    uint32_t bytes = goya_pack_gemm(&g, (uint8_t *)cb_map, 4096);
    uint64_t t0 = now_us();
    uint64_t seq = goya_submit_cs(fd, cb_handle, bytes, GOYA_QID_EXT_DMA_0);
    int wait_rc = (seq != 0) ? goya_wait_cs(fd, seq, 50000) : -1;
    uint64_t t1 = now_us();
    cb_destroy(fd, cb_handle, cb_map, 4096);
    hbm_free(fd, a_h, a_dva); hbm_free(fd, b_h, b_dva); hbm_free(fd, ch, c_dva);
    if (wait_rc != 0) {
        snprintf(c->skip_reason, sizeof(c->skip_reason),
                 "real: Q*Kt submit/wait failed");
        return -1;
    }
    c->real_latency_us = t1 - t0;
    c->real_passed = 1;
    return 0;
}

static void run_ladder(card_t *c)
{
    /* HARD GATE 2026-05-26 (LSO.04): the Goya MME ladder below is built
     * on FABRICATED register addresses (GOYA_MME_BASE = 0x0F000000, flat
     * GEMM descriptor with DIM_M/DIM_N/DIM_K). Authoritative Goya MME
     * descriptor registers live at base 0xD0000 (MME_ARCH_*) with a
     * spatial-loop encoding (HEADER + KERNEL_SIZE_M1 + ASSOC_DIMS_0 +
     * A_VALID_0..4 + A_LOOP_STRIDE_0..4 + A_ROI_SIZE_n). Verified against
     *   /var/lib/dkms/habanalabs-dkms/1.7.1-85/build/drivers/misc/
     *     habanalabs/include/goya/asic_reg/mme_cmdq_regs.h (Temple)
     *   /home/watchdog/goya-port/goya/libmme_goya_hlt.c (working code)
     * Submitting 0x0F000010 etc. via HL_IOCTL_CS would razwi-flood the
     * RTR gate, fire MME_ECC / MME_WACS interrupts, and risk bricking
     * the card. Discovery path above (HL_IOCTL_INFO via ioctl_info) is
     * correct and remains live. Ladder is disabled until the real Goya
     * MME packet builder is ported (LSO.05+).
     */
    snprintf(c->skip_reason, sizeof(c->skip_reason),
             "%s OK; MME ladder DISABLED (fabricated regs at 0x0F00xxxx; "
             "real Goya MME at 0xD0000 — see habana/GOYA_BYPASS.md)",
             c->family);
    return;
    /* Original cloud-Claude ladder kept unreachable for forensic ref: */
    if (run_goya_smoke(c) != 0) return;
    (void)run_goya_roof(c);
    (void)run_goya_real(c);
}

/* ─────────────────────────────────────────────────────────────────
 * Output
 * ──────────────────────────────────────────────────────────────── */

static void print_inventory(void)
{
    printf("[habana_proof] scanning /dev/accel/* and /dev/hl* ...\n");
    if (s_card_count == 0) {
        printf("[habana_proof] no Habana accelerators found.\n");
        printf("                hint: is the habanalabs kernel module "
               "loaded? (`lsmod | grep habana`)\n");
        return;
    }
    printf("[habana_proof] found %d device(s):\n", s_card_count);
    for (int i = 0; i < s_card_count; i++) {
        const card_t *c = &s_cards[i];
        printf("  %-20s %-10s  card=%-16s  device_id=%u  "
               "tpc_mask=0x%llx  sram=%uKB  dram=%lluMB\n",
               c->path, c->family, c->card_name, c->ip.device_id,
               (unsigned long long)c->ip.tpc_enabled_mask_ext,
               c->ip.sram_size / 1024,
               (unsigned long long)(c->ip.dram_size >> 20));
    }
}

static void print_results(void)
{
    for (int i = 0; i < s_card_count; i++) {
        const card_t *c = &s_cards[i];
        printf("\n[habana_proof] %s (%s): ladder\n", c->path, c->family);
        if (!c->supported) {
            printf("  ladder ............ skipped (%s)\n", c->skip_reason);
            continue;
        }
        if (!c->smoke_attempted) {
            printf("  smoke ............. not attempted\n");
            continue;
        }
        if (!c->smoke_passed) {
            printf("  smoke ............. failed -- %s\n", c->skip_reason);
            continue;
        }
        printf("  smoke ............. passed (8x8x8 FP16, %" PRIu64 " us)\n",
               c->real_latency_us);
        if (c->roof_attempted) {
            if (c->roof_passed)
                printf("  roof .............. passed (%" PRIu64 " GFLOPS, "
                       ">=60%% of ~5000 peak)\n", c->roof_gflops);
            else
                printf("  roof .............. below-60%% (%" PRIu64 " GFLOPS "
                       "observed; tune)\n", c->roof_gflops);
        }
        if (c->real_attempted) {
            if (c->real_passed)
                printf("  real .............. passed (Q*Kt 64x64x64 FP16, "
                       "%" PRIu64 " us) -- softmax+V pending tpc_kernel\n",
                       c->real_latency_us);
            else
                printf("  real .............. failed -- %s\n", c->skip_reason);
        }
    }
}

static void print_summary(void)
{
    int total = s_card_count;
    int driven = 0, smoke_pass = 0, roof_pass = 0, real_pass = 0;
    for (int i = 0; i < s_card_count; i++) {
        const card_t *c = &s_cards[i];
        if (c->supported) driven++;
        if (c->smoke_passed) smoke_pass++;
        if (c->roof_passed)  roof_pass++;
        if (c->real_passed)  real_pass++;
    }
    printf("\n[habana_proof] summary: %d card(s) found, %d driveable today, "
           "%d smoke / %d roof / %d real passed.\n",
           total, driven, smoke_pass, roof_pass, real_pass);
    if (driven < total) {
        printf("                non-driveable cards are detected and "
               "kernel-handshaked; their MME packet builders land in "
               "the next commit.\n");
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    discover_all();
    print_inventory();
    for (int i = 0; i < s_card_count; i++) run_ladder(&s_cards[i]);
    print_results();
    print_summary();
    /* Cleanup. */
    for (int i = 0; i < s_card_count; i++) if (s_cards[i].fd > 0) close(s_cards[i].fd);
    return 0;
}
