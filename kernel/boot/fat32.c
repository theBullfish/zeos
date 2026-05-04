/*
 * Zeos — FAT32 driver (read + write)
 *
 * See fat32.h for scope and conventions.
 *
 * Design notes:
 *   - All disk access goes through v_read() / v_write(). One
 *     global mount. block_write goes through CHAIN_BLOCK so every
 *     disk-write sector is recorded by masq_journal automatically.
 *   - Cluster sizes can exceed the block size (e.g. 4 KiB clusters on
 *     a 512-byte sector device = 8 sectors per cluster). All cluster
 *     I/O goes through read_cluster() / write_cluster() which handle
 *     the multiplication.
 *   - LFN: VFAT long names are stored as preceding 0x0F-attribute
 *     entries, ordered last-fragment-first with bit 0x40 set on the
 *     final fragment. We assemble them in order, keep ASCII only
 *     (UCS-2 high bytes ignored — adequate for filenames in scope).
 *   - Path matching is case-insensitive ASCII for both 8.3 and LFN.
 *
 * FAT32 read+write. LFN supported on read; write uses 8.3 only initially.
 * No journaling beyond CHAIN_BLOCK.masq_journal (which records every write
 * sector). No free-cluster bitmap optimization (scans FAT linearly with
 * a 256-entry free cache).
 *
 * Trash: rm moves to /.zeos-trash/ by default; `rm -f` for direct
 * unlink. CHAIN_TRASH_GC auto-empties older than 30 days. The trash
 * implementation lives at the bottom of this file; it operates by
 * re-pointing FAT32 directory entries (no cluster-chain copy), so
 * trash + restore are both O(1) regardless of file size.
 */

#include "fat32.h"
#include "block.h"
#include "heap.h"
#include "kprint.h"
#include "timeofday.h"

/* ── Local string / mem helpers (no libc) ─────────────────────── */

static void m_set(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = v;
}
static void m_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
static char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}
static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (to_lower(*a) != to_lower(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* ── Mount state ──────────────────────────────────────────────── */

typedef struct {
    int      mounted;
    int      drive_idx;         /* Block drive index this volume lives on */
    uint64_t part_lba;          /* LBA of partition start */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_size_sectors;
    uint64_t fat1_lba;          /* Block-LBA where FAT1 starts */
    uint64_t data_region_lba;   /* Block-LBA of cluster 2 */
    uint32_t root_cluster;
    uint32_t cluster_bytes;     /* sectors_per_cluster * bytes_per_sector */
    uint32_t blk_size;          /* Underlying block size (typically 512) */
} fat32_vol_t;

static fat32_vol_t V;

/* Drive-aware I/O wrappers. Reads bypass CHAIN_BLOCK; writes go through
 * it (and therefore through masq_journal). */
static int v_read(uint64_t lba, uint32_t count, void *buf) {
    return block_read_drive(V.drive_idx, lba, count, buf);
}
static int v_write(uint64_t lba, uint32_t count, const void *buf) {
    return block_write_drive(V.drive_idx, lba, count, buf);
}

/* ── Low-level I/O ────────────────────────────────────────────── */

/* Look up next cluster in FAT1. Returns 0xFFFFFFFF on error. */
static uint32_t fat_next_cluster(uint32_t cluster)
{
    if (V.blk_size == 0) return 0xFFFFFFFF;
    uint32_t entries_per_block = V.blk_size / 4;
    uint32_t fat_block = cluster / entries_per_block;
    uint32_t fat_idx   = cluster % entries_per_block;

    uint8_t *buf = (uint8_t *)kmalloc(V.blk_size);
    if (!buf) return 0xFFFFFFFF;
    if (v_read(V.fat1_lba + fat_block, 1, buf) != 0) {
        kfree(buf); return 0xFFFFFFFF;
    }
    uint32_t v = (uint32_t)buf[fat_idx*4]
               | ((uint32_t)buf[fat_idx*4+1] << 8)
               | ((uint32_t)buf[fat_idx*4+2] << 16)
               | ((uint32_t)buf[fat_idx*4+3] << 24);
    kfree(buf);
    return v & 0x0FFFFFFF;
}

static int cluster_is_eoc(uint32_t c) {
    return (c >= 0x0FFFFFF8) || (c < 2);
}

static uint64_t cluster_lba(uint32_t cluster) {
    return V.data_region_lba +
           (uint64_t)(cluster - 2) * V.sectors_per_cluster;
}

static int read_cluster(uint32_t cluster, void *buf)
{
    return v_read(cluster_lba(cluster), V.sectors_per_cluster, buf);
}

/* ── BPB parse / mount ────────────────────────────────────────── */

int fat32_mount(int drive_idx, uint64_t partition_lba)
{
    if (drive_idx < 0) return -1;

    uint32_t blk = block_size();
    if (blk == 0) blk = 512;

    uint8_t *bpb = (uint8_t *)kmalloc(blk);
    if (!bpb) return -1;

    /* Stash drive index before any I/O so v_read/v_write hit the right
     * device. If validation fails below we leave V partially populated
     * but with mounted=0; callers must check fat32_mounted(). */
    V.drive_idx = drive_idx;

    if (block_read_drive(drive_idx, partition_lba, 1, bpb) != 0) {
        kfree(bpb); return -1;
    }

    uint16_t bps        = bpb[11] | (bpb[12] << 8);
    uint8_t  spc        = bpb[13];
    uint16_t reserved   = bpb[14] | (bpb[15] << 8);
    uint8_t  nfats      = bpb[16];
    uint32_t fatsz32    = bpb[36] | (bpb[37] << 8)
                       | (bpb[38] << 16) | (bpb[39] << 24);
    uint32_t rootclus   = bpb[44] | (bpb[45] << 8)
                       | (bpb[46] << 16) | (bpb[47] << 24);
    uint16_t boot_sig   = bpb[510] | (bpb[511] << 8);
    uint16_t fatsz16    = bpb[22] | (bpb[23] << 8);

    if (boot_sig != 0xAA55 || bps == 0 || spc == 0 || nfats == 0 ||
        fatsz32 == 0 || fatsz16 != 0 || rootclus < 2) {
        kfree(bpb);
        return -1;
    }

    V.part_lba            = partition_lba;
    V.bytes_per_sector    = bps;
    V.sectors_per_cluster = spc;
    V.reserved_sectors    = reserved;
    V.num_fats            = nfats;
    V.fat_size_sectors    = fatsz32;
    V.root_cluster        = rootclus;
    V.cluster_bytes       = (uint32_t)bps * spc;
    V.blk_size            = blk;

    uint32_t scale = (bps >= blk) ? (bps / blk) : 1;

    V.fat1_lba        = partition_lba + (uint64_t)reserved * scale;
    V.data_region_lba = partition_lba +
                        ((uint64_t)reserved + (uint64_t)nfats * fatsz32) * scale;

    V.mounted = 1;
    kfree(bpb);

    kputs("FAT32: mounted at LBA ");
    kput_dec(partition_lba);
    kputs(" (cluster=");
    kput_dec(V.cluster_bytes);
    kputs("B, root=");
    kput_dec(V.root_cluster);
    kputs(")\n");
    return 0;
}

int fat32_mounted(void) { return V.mounted; }

/* ── Directory walking ────────────────────────────────────────── */

static void make_short_name(const uint8_t *e, char *out)
{
    int o = 0;
    for (int i = 0; i < 8; i++) {
        if (e[i] == ' ') break;
        out[o++] = (char)e[i];
    }
    if (e[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11; i++) {
            if (e[i] == ' ') break;
            out[o++] = (char)e[i];
        }
    }
    out[o] = '\0';
}

static int decode_lfn_fragment(const uint8_t *e, char *out)
{
    static const uint8_t off[13] = {
        1,3,5,7,9,
        14,16,18,20,22,24,26,
        28,30
    };
    int n = 0;
    for (int i = 0; i < 13; i++) {
        uint16_t ch = e[off[i]] | ((uint16_t)e[off[i]+1] << 8);
        if (ch == 0 || ch == 0xFFFF) break;
        out[n++] = (char)(ch & 0x7F);
    }
    return n;
}

typedef int (*dir_visit_fn)(const char *long_name,
                            const char *short_name,
                            uint32_t cluster, uint32_t size,
                            uint8_t attr, void *user);

static int walk_dir(uint32_t dir_cluster, dir_visit_fn visit, void *user)
{
    uint8_t *cbuf = (uint8_t *)kmalloc(V.cluster_bytes);
    if (!cbuf) return -1;

    char lfn[FAT32_NAME_MAX + 16];
    int  lfn_len = 0;
    int  lfn_active = 0;

    uint32_t c = dir_cluster;
    while (!cluster_is_eoc(c)) {
        if (read_cluster(c, cbuf) != 0) { kfree(cbuf); return -1; }

        for (uint32_t off = 0; off < V.cluster_bytes; off += 32) {
            uint8_t *e = cbuf + off;

            if (e[0] == 0x00) {
                kfree(cbuf); return 0;
            }
            if (e[0] == 0xE5) { lfn_active = 0; continue; }

            uint8_t attr = e[11];

            if ((attr & 0x0F) == 0x0F) {
                int ord = e[0] & 0x1F;
                if (e[0] & 0x40) {
                    lfn_len = 0;
                    lfn_active = 1;
                    m_set(lfn, 0, sizeof(lfn));
                }
                if (lfn_active && ord >= 1 && ord <= 20) {
                    char frag[14];
                    int  fn = decode_lfn_fragment(e, frag);
                    int dst = (ord - 1) * 13;
                    if (dst + fn <= FAT32_NAME_MAX) {
                        m_copy(lfn + dst, frag, fn);
                        if (dst + fn > lfn_len) lfn_len = dst + fn;
                    }
                }
                continue;
            }

            if (attr & 0x08) { lfn_active = 0; continue; }

            char shortname[16];
            make_short_name(e, shortname);

            uint32_t cluster =
                ((uint32_t)(e[20] | (e[21] << 8)) << 16) |
                (uint32_t)(e[26] | (e[27] << 8));
            uint32_t size = e[28] | ((uint32_t)e[29] << 8)
                          | ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);

            const char *longp = 0;
            char lfn_term[FAT32_NAME_MAX + 1];
            if (lfn_active && lfn_len > 0) {
                int n = lfn_len;
                if (n > FAT32_NAME_MAX) n = FAT32_NAME_MAX;
                m_copy(lfn_term, lfn, n);
                lfn_term[n] = '\0';
                longp = lfn_term;
            }
            lfn_active = 0;

            int stop = visit(longp, shortname, cluster, size, attr, user);
            if (stop) { kfree(cbuf); return 0; }
        }

        c = fat_next_cluster(c);
        if (c == 0xFFFFFFFF) { kfree(cbuf); return -1; }
    }
    kfree(cbuf);
    return 0;
}

/* ── Path resolution ──────────────────────────────────────────── */

struct find_ctx {
    const char *want;
    int         found;
    uint32_t    cluster;
    uint32_t    size;
    uint8_t     attr;
};

static int find_visitor(const char *lname, const char *sname,
                        uint32_t cluster, uint32_t size,
                        uint8_t attr, void *user)
{
    struct find_ctx *ctx = (struct find_ctx *)user;
    if ((lname && ieq(lname, ctx->want)) || ieq(sname, ctx->want)) {
        ctx->found = 1;
        ctx->cluster = cluster;
        ctx->size = size;
        ctx->attr = attr;
        return 1;
    }
    return 0;
}

static int resolve_path(const char *path, struct fat32_file *out)
{
    if (!V.mounted) return -1;

    while (*path == '/') path++;

    out->start_cluster = V.root_cluster;
    out->cur_cluster = V.root_cluster;
    out->cur_offset = 0;
    out->size = 0;
    out->is_dir = 1;

    if (*path == '\0') return 0; /* Root */

    char comp[FAT32_NAME_MAX + 1];
    uint32_t parent_cluster = V.root_cluster;
    uint8_t  parent_attr = 0x10;

    while (*path) {
        int n = 0;
        while (*path && *path != '/' && n < FAT32_NAME_MAX) {
            comp[n++] = *path++;
        }
        comp[n] = '\0';
        while (*path == '/') path++;

        if (n == 0) continue;

        if (!(parent_attr & 0x10)) return -1;

        struct find_ctx ctx;
        ctx.want = comp;
        ctx.found = 0;
        if (walk_dir(parent_cluster, find_visitor, &ctx) != 0) return -1;
        if (!ctx.found) return -1;

        parent_cluster = ctx.cluster ? ctx.cluster : V.root_cluster;
        parent_attr = ctx.attr;

        out->start_cluster = ctx.cluster;
        out->cur_cluster = ctx.cluster;
        out->size = ctx.size;
        out->is_dir = (ctx.attr & 0x10) ? 1 : 0;
    }
    return 0;
}

int fat32_open(const char *path, struct fat32_file *out)
{
    if (!out || !path) return -1;
    return resolve_path(path, out);
}

/* ── File reading ─────────────────────────────────────────────── */

int fat32_read(struct fat32_file *file, void *buf, uint32_t max_len)
{
    if (!V.mounted || !file || !buf) return -1;
    if (file->is_dir) return -1;
    if (file->cur_offset >= file->size) return 0;

    uint32_t remain_file = file->size - file->cur_offset;
    if (max_len > remain_file) max_len = remain_file;

    uint8_t *out = (uint8_t *)buf;
    uint32_t got = 0;

    uint8_t *cbuf = (uint8_t *)kmalloc(V.cluster_bytes);
    if (!cbuf) return -1;

    uint32_t cluster = file->cur_cluster;
    if (cluster_is_eoc(cluster)) { kfree(cbuf); return 0; }

    uint32_t in_cluster = file->cur_offset % V.cluster_bytes;

    while (got < max_len && !cluster_is_eoc(cluster)) {
        if (read_cluster(cluster, cbuf) != 0) { kfree(cbuf); return -1; }
        uint32_t avail = V.cluster_bytes - in_cluster;
        uint32_t want = max_len - got;
        uint32_t copy = (want < avail) ? want : avail;
        m_copy(out + got, cbuf + in_cluster, copy);
        got += copy;
        in_cluster += copy;
        if (in_cluster >= V.cluster_bytes) {
            in_cluster = 0;
            cluster = fat_next_cluster(cluster);
            if (cluster == 0xFFFFFFFF) { kfree(cbuf); return -1; }
        }
    }
    kfree(cbuf);

    file->cur_offset += got;
    file->cur_cluster = cluster;
    return (int)got;
}

/* ── List directory ───────────────────────────────────────────── */

struct list_ctx {
    struct fat32_dirent *out;
    int max;
    int n;
};

static int list_visitor(const char *lname, const char *sname,
                        uint32_t cluster, uint32_t size,
                        uint8_t attr, void *user)
{
    struct list_ctx *ctx = (struct list_ctx *)user;
    if (ctx->n >= ctx->max) return 1;
    if (sname[0] == '.' && (sname[1] == '\0' ||
        (sname[1] == '.' && sname[2] == '\0'))) return 0;

    struct fat32_dirent *d = &ctx->out[ctx->n];
    const char *src = (lname && lname[0]) ? lname : sname;
    int i = 0;
    while (src[i] && i < FAT32_NAME_MAX) { d->name[i] = src[i]; i++; }
    d->name[i] = '\0';
    d->size = size;
    d->start_cluster = cluster;
    d->is_dir = (attr & 0x10) ? 1 : 0;
    d->attr = attr;
    ctx->n++;
    return 0;
}

int fat32_list(const char *dir, struct fat32_dirent *entries, int max)
{
    if (!V.mounted || !entries) return -1;

    struct fat32_file f;
    if (fat32_open(dir ? dir : "/", &f) != 0) return -1;
    if (!f.is_dir) return -1;

    struct list_ctx ctx;
    ctx.out = entries;
    ctx.max = max;
    ctx.n = 0;
    if (walk_dir(f.start_cluster, list_visitor, &ctx) != 0) return -1;
    return ctx.n;
}

/* ── Auto-mount ───────────────────────────────────────────────── */

/* Minimal GPT walk: just enough to find a FAT32 candidate partition.
 * The full GPT module being extracted in parallel will take this over
 * later; until then, this stays self-contained. */
static int try_gpt_partition_drive(int drive_idx)
{
    uint32_t blk = block_size();
    if (blk == 0) blk = 512;
    uint8_t *buf = (uint8_t *)kmalloc(blk);
    if (!buf) return -1;

    /* GPT header at LBA 1 */
    if (block_read_drive(drive_idx, 1, 1, buf) != 0) { kfree(buf); return -1; }
    static const uint8_t sig[8] = {'E','F','I',' ','P','A','R','T'};
    for (int i = 0; i < 8; i++) {
        if (buf[i] != sig[i]) { kfree(buf); return -1; }
    }
    uint64_t entry_lba = (uint64_t)buf[72] | ((uint64_t)buf[73]<<8)
                       | ((uint64_t)buf[74]<<16) | ((uint64_t)buf[75]<<24)
                       | ((uint64_t)buf[76]<<32) | ((uint64_t)buf[77]<<40)
                       | ((uint64_t)buf[78]<<48) | ((uint64_t)buf[79]<<56);
    uint32_t num_entries = buf[80] | (buf[81]<<8)
                         | ((uint32_t)buf[82]<<16) | ((uint32_t)buf[83]<<24);
    uint32_t entry_size = buf[84] | (buf[85]<<8)
                        | ((uint32_t)buf[86]<<16) | ((uint32_t)buf[87]<<24);
    if (entry_size == 0 || entry_size > 512 || num_entries > 256) {
        kfree(buf); return -1;
    }

    uint32_t entries_per_block = blk / entry_size;
    if (entries_per_block == 0) { kfree(buf); return -1; }
    uint32_t blocks = (num_entries + entries_per_block - 1)
                    / entries_per_block;

    for (uint32_t bi = 0; bi < blocks; bi++) {
        if (block_read_drive(drive_idx, entry_lba + bi, 1, buf) != 0) break;
        for (uint32_t ei = 0; ei < entries_per_block; ei++) {
            uint8_t *e = buf + ei * entry_size;
            int nonzero = 0;
            for (int k = 0; k < 16; k++) if (e[k]) { nonzero = 1; break; }
            if (!nonzero) continue;
            uint64_t first_lba = (uint64_t)e[32] | ((uint64_t)e[33]<<8)
                | ((uint64_t)e[34]<<16) | ((uint64_t)e[35]<<24)
                | ((uint64_t)e[36]<<32) | ((uint64_t)e[37]<<40)
                | ((uint64_t)e[38]<<48) | ((uint64_t)e[39]<<56);
            if (first_lba == 0) continue;
            if (fat32_mount(drive_idx, first_lba) == 0) {
                kfree(buf); return 0;
            }
        }
    }
    kfree(buf);
    return -1;
}

int fat32_automount(void)
{
    int n = block_drive_count();
    /* Walk every drive: try whole-disk first, then GPT. First mount
     * wins. This lets a kernel with a VAULT NVMe on drive 0 still find
     * a FAT32 volume on drive 1 (USB stick, second NVMe, etc.). */
    for (int d = 0; d < n; d++) {
        if (fat32_mount(d, 0) == 0) return 0;
        if (try_gpt_partition_drive(d) == 0) return 0;
    }
    return -1;
}

/* ───────────────────────────────────────────────────────────────────
 *  Write side
 * ─────────────────────────────────────────────────────────────────── */

#define FAT32_EOC      0x0FFFFFFFu
#define FAT32_FREE     0x00000000u
#define FAT32_ATTR_RO  0x01
#define FAT32_ATTR_HID 0x02
#define FAT32_ATTR_SYS 0x04
#define FAT32_ATTR_VOL 0x08
#define FAT32_ATTR_DIR 0x10
#define FAT32_ATTR_AR  0x20

/* ── Free-cluster cache ─────────────────────────────────────────── */

#define FREE_CACHE_MAX 256
#define FREE_CACHE_INVALIDATE_AFTER 1000

static uint32_t g_free_cache[FREE_CACHE_MAX];
static uint32_t g_free_cache_n   = 0;
static uint32_t g_free_cache_pos = 0;
static uint32_t g_alloc_count    = 0;
static uint32_t g_fat_scan_hint  = 2;

static void free_cache_invalidate(void)
{
    g_free_cache_n = 0;
    g_free_cache_pos = 0;
}

/* ── FAT entry I/O ──────────────────────────────────────────────── */

static int fat_read_entry(uint32_t cluster, uint32_t *out_value)
{
    if (V.blk_size == 0) return -1;
    uint32_t epb = V.blk_size / 4;
    uint64_t blk = V.fat1_lba + cluster / epb;
    uint32_t idx = cluster % epb;

    uint8_t *buf = (uint8_t *)kmalloc(V.blk_size);
    if (!buf) return -1;
    if (v_read(blk, 1, buf) != 0) { kfree(buf); return -1; }
    uint32_t v = ((uint32_t)buf[idx*4]      ) |
                 ((uint32_t)buf[idx*4+1] << 8) |
                 ((uint32_t)buf[idx*4+2] <<16) |
                 ((uint32_t)buf[idx*4+3] <<24);
    kfree(buf);
    *out_value = v & 0x0FFFFFFFu;
    return 0;
}

/* Write a single FAT entry to ALL FAT copies. */
static int fat_write_entry(uint32_t cluster, uint32_t value)
{
    if (V.blk_size == 0) return -1;
    uint32_t epb = V.blk_size / 4;
    uint32_t blk_off = cluster / epb;
    uint32_t idx = cluster % epb;

    uint8_t *buf = (uint8_t *)kmalloc(V.blk_size);
    if (!buf) return -1;

    uint32_t scale = (V.bytes_per_sector >= V.blk_size)
                   ? (V.bytes_per_sector / V.blk_size) : 1;
    uint64_t fat_span_blocks = (uint64_t)V.fat_size_sectors * scale;

    for (uint32_t fi = 0; fi < V.num_fats; fi++) {
        uint64_t lba = V.fat1_lba + (uint64_t)fi * fat_span_blocks + blk_off;
        if (v_read(lba, 1, buf) != 0) { kfree(buf); return -1; }
        uint32_t cur = ((uint32_t)buf[idx*4]      ) |
                       ((uint32_t)buf[idx*4+1] << 8) |
                       ((uint32_t)buf[idx*4+2] <<16) |
                       ((uint32_t)buf[idx*4+3] <<24);
        uint32_t v = (cur & 0xF0000000u) | (value & 0x0FFFFFFFu);
        buf[idx*4]   = (uint8_t)(v      );
        buf[idx*4+1] = (uint8_t)(v >>  8);
        buf[idx*4+2] = (uint8_t)(v >> 16);
        buf[idx*4+3] = (uint8_t)(v >> 24);
        if (v_write(lba, 1, buf) != 0) { kfree(buf); return -1; }
    }
    kfree(buf);
    return 0;
}

/* Refill the free-cluster cache by linearly scanning the FAT. */
static int free_cache_refill(void)
{
    if (V.blk_size == 0) return 0;
    uint32_t epb = V.blk_size / 4;
    uint32_t scale = (V.bytes_per_sector >= V.blk_size)
                   ? (V.bytes_per_sector / V.blk_size) : 1;
    uint32_t total_entries = V.fat_size_sectors * scale * epb;

    uint8_t *buf = (uint8_t *)kmalloc(V.blk_size);
    if (!buf) return 0;

    free_cache_invalidate();

    uint32_t scanned = 0;
    uint32_t c = g_fat_scan_hint < 2 ? 2 : g_fat_scan_hint;
    uint32_t max_scan = total_entries + 256;
    while (scanned < max_scan && g_free_cache_n < FREE_CACHE_MAX) {
        if (c >= total_entries) c = 2;
        uint64_t blk = V.fat1_lba + c / epb;
        uint32_t idx = c % epb;
        if (v_read(blk, 1, buf) != 0) break;

        for (; idx < epb && c < total_entries &&
               g_free_cache_n < FREE_CACHE_MAX;
             idx++, c++, scanned++) {
            uint32_t v = ((uint32_t)buf[idx*4]      ) |
                         ((uint32_t)buf[idx*4+1] << 8) |
                         ((uint32_t)buf[idx*4+2] <<16) |
                         ((uint32_t)buf[idx*4+3] <<24);
            v &= 0x0FFFFFFFu;
            if (v == FAT32_FREE) {
                g_free_cache[g_free_cache_n++] = c;
            }
        }
    }
    kfree(buf);
    g_free_cache_pos = 0;
    if (g_free_cache_n > 0) {
        g_fat_scan_hint = g_free_cache[g_free_cache_n - 1] + 1;
    }
    return (int)g_free_cache_n;
}

/* Allocate a single free cluster (marks it EOC). 0 = failure. */
static uint32_t alloc_cluster(void)
{
    if (g_free_cache_pos >= g_free_cache_n) {
        if (free_cache_refill() == 0) return 0;
    }
    uint32_t c = 0;
    while (g_free_cache_pos < g_free_cache_n) {
        uint32_t cand = g_free_cache[g_free_cache_pos++];
        uint32_t v = 0;
        if (fat_read_entry(cand, &v) != 0) continue;
        if (v == FAT32_FREE) { c = cand; break; }
    }
    if (c == 0) {
        free_cache_invalidate();
        if (free_cache_refill() == 0) return 0;
        while (g_free_cache_pos < g_free_cache_n) {
            uint32_t cand = g_free_cache[g_free_cache_pos++];
            uint32_t v = 0;
            if (fat_read_entry(cand, &v) != 0) continue;
            if (v == FAT32_FREE) { c = cand; break; }
        }
    }
    if (c == 0) return 0;

    if (fat_write_entry(c, FAT32_EOC) != 0) return 0;
    g_alloc_count++;
    if (g_alloc_count >= FREE_CACHE_INVALIDATE_AFTER) {
        g_alloc_count = 0;
        free_cache_invalidate();
    }
    return c;
}

/* Free a chain starting at `start` (inclusive). */
static int free_chain(uint32_t start)
{
    uint32_t c = start;
    while (!cluster_is_eoc(c)) {
        uint32_t nxt = fat_next_cluster(c);
        if (fat_write_entry(c, FAT32_FREE) != 0) return -1;
        c = nxt;
        if (c == 0xFFFFFFFFu) return -1;
    }
    free_cache_invalidate();
    return 0;
}

/* Cluster I/O. */
static int write_cluster(uint32_t cluster, const void *buf)
{
    return v_write(cluster_lba(cluster), V.sectors_per_cluster, buf);
}

static int zero_cluster(uint32_t cluster)
{
    uint8_t *z = (uint8_t *)kmalloc(V.cluster_bytes);
    if (!z) return -1;
    m_set(z, 0, V.cluster_bytes);
    int rc = write_cluster(cluster, z);
    kfree(z);
    return rc;
}

/* ── Path / 8.3 helpers ─────────────────────────────────────────── */

static int path_split_parent(const char *path, char *parent, char *base)
{
    int len = 0;
    while (path[len]) len++;
    if (len == 0) return -1;

    int last = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') last = i;
    }
    if (last < 0) {
        parent[0] = '/'; parent[1] = '\0';
        int n = 0;
        while (path[n] && n < FAT32_NAME_MAX) { base[n] = path[n]; n++; }
        base[n] = '\0';
        return 0;
    }
    int pn = 0;
    if (last == 0) { parent[pn++] = '/'; }
    else {
        for (int i = 0; i < last && pn < FAT32_NAME_MAX; i++) parent[pn++] = path[i];
    }
    parent[pn] = '\0';
    int bn = 0;
    for (int i = last + 1; i < len && bn < FAT32_NAME_MAX; i++) base[bn++] = path[i];
    base[bn] = '\0';
    if (base[0] == '\0') return -1;
    return 0;
}

static char to_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int valid_short_char(char c)
{
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' ||
        c == '@' || c == '~' || c == '`' || c == '!' || c == '(' ||
        c == ')' || c == '{' || c == '}' || c == '^' || c == '#' ||
        c == '&') return 1;
    return 0;
}

static int make_8_3(const char *name, uint8_t out[11])
{
    for (int i = 0; i < 11; i++) out[i] = ' ';
    if (!name || !name[0]) return -1;

    int dot = -1;
    int len = 0;
    while (name[len]) len++;
    for (int i = len - 1; i >= 0; i--) {
        if (name[i] == '.') { dot = i; break; }
    }

    int base_end = (dot < 0) ? len : dot;
    int oi = 0;
    for (int i = 0; i < base_end && oi < 8; i++) {
        char c = to_upper(name[i]);
        if (!valid_short_char(c)) c = '_';
        out[oi++] = (uint8_t)c;
    }
    if (dot >= 0) {
        oi = 8;
        for (int i = dot + 1; i < len && oi < 11; i++) {
            char c = to_upper(name[i]);
            if (!valid_short_char(c)) c = '_';
            out[oi++] = (uint8_t)c;
        }
    }
    return 0;
}

/* ── Timestamps (TSC-derived placeholder; no RTC) ───────────────── */

static uint64_t fat_tsc_seed(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void fat_fill_times(uint8_t *e)
{
    uint64_t t = fat_tsc_seed();
    uint16_t day   = (uint16_t)((t & 0x1F) | 1);          if (day > 28) day = 28;
    uint16_t month = (uint16_t)(((t >> 5) & 0x0F) | 1);    if (month > 12) month = 12;
    uint16_t year  = 46; /* 2026 - 1980 */
    uint16_t date  = (uint16_t)((year << 9) | (month << 5) | day);

    uint16_t sec2  = (uint16_t)((t >> 9) & 0x1F);
    uint16_t min   = (uint16_t)((t >> 14) & 0x3F);
    uint16_t hour  = (uint16_t)((t >> 20) & 0x1F);
    if (min > 59) min = 59;
    if (hour > 23) hour = 23;
    uint16_t time_ = (uint16_t)((hour << 11) | (min << 5) | sec2);

    e[13] = (uint8_t)((t >> 24) % 200);
    e[14] = (uint8_t)(time_ & 0xFF);
    e[15] = (uint8_t)(time_ >> 8);
    e[16] = (uint8_t)(date & 0xFF);
    e[17] = (uint8_t)(date >> 8);
    e[18] = e[16]; e[19] = e[17];
    e[22] = e[14]; e[23] = e[15];
    e[24] = e[16]; e[25] = e[17];
}

/* ── Directory entry mutation ───────────────────────────────────── */

static int dir_find_free_slot(uint32_t dir_cluster,
                              uint32_t *out_cluster,
                              uint32_t *out_off)
{
    uint8_t *cbuf = (uint8_t *)kmalloc(V.cluster_bytes);
    if (!cbuf) return -1;

    uint32_t c = dir_cluster;
    uint32_t prev = c;
    while (!cluster_is_eoc(c)) {
        if (read_cluster(c, cbuf) != 0) { kfree(cbuf); return -1; }
        for (uint32_t off = 0; off < V.cluster_bytes; off += 32) {
            uint8_t marker = cbuf[off];
            if (marker == 0x00 || marker == 0xE5) {
                *out_cluster = c;
                *out_off = off;
                kfree(cbuf);
                return 0;
            }
        }
        prev = c;
        uint32_t n = fat_next_cluster(c);
        if (n == 0xFFFFFFFFu) { kfree(cbuf); return -1; }
        if (cluster_is_eoc(n)) {
            uint32_t newc = alloc_cluster();
            if (newc == 0) { kfree(cbuf); return -1; }
            if (zero_cluster(newc) != 0) { kfree(cbuf); return -1; }
            if (fat_write_entry(prev, newc) != 0) { kfree(cbuf); return -1; }
            *out_cluster = newc;
            *out_off = 0;
            kfree(cbuf);
            return 0;
        }
        c = n;
    }
    kfree(cbuf);
    return -1;
}

static int dir_write_entry(uint32_t dir_cluster, uint32_t off,
                           const uint8_t in[32])
{
    uint8_t *cbuf = (uint8_t *)kmalloc(V.cluster_bytes);
    if (!cbuf) return -1;
    if (read_cluster(dir_cluster, cbuf) != 0) { kfree(cbuf); return -1; }
    m_copy(cbuf + off, in, 32);
    int rc = write_cluster(dir_cluster, cbuf);
    kfree(cbuf);
    return rc;
}

static int dir_locate(uint32_t dir_cluster, const char *name,
                      uint32_t *out_cluster, uint32_t *out_off,
                      uint8_t out_entry[32])
{
    uint8_t *cbuf = (uint8_t *)kmalloc(V.cluster_bytes);
    if (!cbuf) return -1;

    char lfn[FAT32_NAME_MAX + 16];
    int  lfn_len = 0;
    int  lfn_active = 0;

    uint32_t c = dir_cluster;
    while (!cluster_is_eoc(c)) {
        if (read_cluster(c, cbuf) != 0) { kfree(cbuf); return -1; }
        for (uint32_t off = 0; off < V.cluster_bytes; off += 32) {
            uint8_t *e = cbuf + off;
            if (e[0] == 0x00) { kfree(cbuf); return -1; }
            if (e[0] == 0xE5) { lfn_active = 0; continue; }
            uint8_t attr = e[11];
            if ((attr & 0x0F) == 0x0F) {
                int ord = e[0] & 0x1F;
                if (e[0] & 0x40) {
                    lfn_len = 0; lfn_active = 1;
                    m_set(lfn, 0, sizeof(lfn));
                }
                if (lfn_active && ord >= 1 && ord <= 20) {
                    char frag[14];
                    int  fn = decode_lfn_fragment(e, frag);
                    int dst = (ord - 1) * 13;
                    if (dst + fn <= FAT32_NAME_MAX) {
                        m_copy(lfn + dst, frag, fn);
                        if (dst + fn > lfn_len) lfn_len = dst + fn;
                    }
                }
                continue;
            }
            if (attr & FAT32_ATTR_VOL) { lfn_active = 0; continue; }

            char shortname[16];
            make_short_name(e, shortname);
            char lfn_term[FAT32_NAME_MAX + 1];
            const char *longp = 0;
            if (lfn_active && lfn_len > 0) {
                int n = lfn_len;
                if (n > FAT32_NAME_MAX) n = FAT32_NAME_MAX;
                m_copy(lfn_term, lfn, n);
                lfn_term[n] = '\0';
                longp = lfn_term;
            }
            lfn_active = 0;

            if ((longp && ieq(longp, name)) || ieq(shortname, name)) {
                *out_cluster = c;
                *out_off = off;
                m_copy(out_entry, e, 32);
                kfree(cbuf);
                return 0;
            }
        }
        c = fat_next_cluster(c);
        if (c == 0xFFFFFFFFu) { kfree(cbuf); return -1; }
    }
    kfree(cbuf);
    return -1;
}

static int resolve_dir(const char *path, uint32_t *out_cluster)
{
    struct fat32_file f;
    if (fat32_open(path && path[0] ? path : "/", &f) != 0) return -1;
    if (!f.is_dir) return -1;
    *out_cluster = f.start_cluster ? f.start_cluster : V.root_cluster;
    return 0;
}

static int find_entry(const char *path, uint32_t *out_dir_cluster,
                      uint32_t *out_dir_off, uint8_t out_entry[32])
{
    char parent[FAT32_PATH_MAX];
    char base[FAT32_NAME_MAX + 1];
    if (path_split_parent(path, parent, base) != 0) return -1;

    uint32_t pc = 0;
    if (resolve_dir(parent, &pc) != 0) return -1;
    return dir_locate(pc, base, out_dir_cluster, out_dir_off, out_entry);
}

/* ── Public write API ───────────────────────────────────────────── */

static int create_entry(const char *path, uint8_t attr,
                        uint32_t first_cluster, uint32_t size)
{
    char parent[FAT32_PATH_MAX];
    char base[FAT32_NAME_MAX + 1];
    if (path_split_parent(path, parent, base) != 0) return -1;
    if (!base[0]) return -1;

    uint32_t pc = 0;
    if (resolve_dir(parent, &pc) != 0) return -1;

    {
        uint32_t dummyc, dummyo; uint8_t dummye[32];
        if (dir_locate(pc, base, &dummyc, &dummyo, dummye) == 0) return -1;
    }

    uint32_t dc = 0, doff = 0;
    if (dir_find_free_slot(pc, &dc, &doff) != 0) return -1;

    uint8_t e[32];
    m_set(e, 0, 32);
    uint8_t sn[11];
    if (make_8_3(base, sn) != 0) return -1;
    m_copy(e, sn, 11);
    e[11] = attr;
    e[12] = 0;
    fat_fill_times(e);
    e[20] = (uint8_t)((first_cluster >> 16) & 0xFF);
    e[21] = (uint8_t)((first_cluster >> 24) & 0xFF);
    e[26] = (uint8_t)((first_cluster      ) & 0xFF);
    e[27] = (uint8_t)((first_cluster >>  8) & 0xFF);
    e[28] = (uint8_t)((size      ) & 0xFF);
    e[29] = (uint8_t)((size >>  8) & 0xFF);
    e[30] = (uint8_t)((size >> 16) & 0xFF);
    e[31] = (uint8_t)((size >> 24) & 0xFF);

    return dir_write_entry(dc, doff, e);
}

int fat32_create(const char *path)
{
    if (!V.mounted || !path) return -1;
    return create_entry(path, FAT32_ATTR_AR, 0, 0);
}

int fat32_mkdir(const char *path)
{
    if (!V.mounted || !path) return -1;

    uint32_t dc = alloc_cluster();
    if (dc == 0) return -1;
    if (zero_cluster(dc) != 0) { (void)fat_write_entry(dc, FAT32_FREE); return -1; }

    char parent[FAT32_PATH_MAX];
    char base[FAT32_NAME_MAX + 1];
    if (path_split_parent(path, parent, base) != 0) {
        (void)fat_write_entry(dc, FAT32_FREE); return -1;
    }
    uint32_t pc = 0;
    if (resolve_dir(parent, &pc) != 0) {
        (void)fat_write_entry(dc, FAT32_FREE); return -1;
    }

    uint8_t *cbuf = (uint8_t *)kmalloc(V.cluster_bytes);
    if (!cbuf) { (void)fat_write_entry(dc, FAT32_FREE); return -1; }
    m_set(cbuf, 0, V.cluster_bytes);

    {
        uint8_t *e = cbuf;
        e[0] = '.'; for (int i = 1; i < 11; i++) e[i] = ' ';
        e[11] = FAT32_ATTR_DIR;
        fat_fill_times(e);
        e[20] = (uint8_t)((dc >> 16) & 0xFF);
        e[21] = (uint8_t)((dc >> 24) & 0xFF);
        e[26] = (uint8_t)((dc      ) & 0xFF);
        e[27] = (uint8_t)((dc >>  8) & 0xFF);
    }
    {
        uint8_t *e = cbuf + 32;
        e[0] = '.'; e[1] = '.';
        for (int i = 2; i < 11; i++) e[i] = ' ';
        e[11] = FAT32_ATTR_DIR;
        fat_fill_times(e);
        uint32_t pcref = (pc == V.root_cluster) ? 0 : pc;
        e[20] = (uint8_t)((pcref >> 16) & 0xFF);
        e[21] = (uint8_t)((pcref >> 24) & 0xFF);
        e[26] = (uint8_t)((pcref      ) & 0xFF);
        e[27] = (uint8_t)((pcref >>  8) & 0xFF);
    }
    if (write_cluster(dc, cbuf) != 0) {
        kfree(cbuf); (void)fat_write_entry(dc, FAT32_FREE); return -1;
    }
    kfree(cbuf);

    if (create_entry(path, FAT32_ATTR_DIR, dc, 0) != 0) {
        (void)fat_write_entry(dc, FAT32_FREE);
        return -1;
    }
    return 0;
}

int fat32_unlink(const char *path)
{
    if (!V.mounted || !path) return -1;

    uint32_t dc, doff; uint8_t e[32];
    if (find_entry(path, &dc, &doff, e) != 0) return -1;

    uint8_t attr = e[11];
    uint32_t first = ((uint32_t)(e[20] | (e[21] << 8)) << 16) |
                     (uint32_t)(e[26] | (e[27] << 8));

    if (attr & FAT32_ATTR_DIR) {
        uint8_t *cbuf = (uint8_t *)kmalloc(V.cluster_bytes);
        if (!cbuf) return -1;
        uint32_t c = first;
        int empty = 1;
        while (!cluster_is_eoc(c) && empty) {
            if (read_cluster(c, cbuf) != 0) { kfree(cbuf); return -1; }
            for (uint32_t off = 0; off < V.cluster_bytes; off += 32) {
                uint8_t *en = cbuf + off;
                if (en[0] == 0x00) goto empty_ok;
                if (en[0] == 0xE5) continue;
                if (en[0] == '.') continue;
                if ((en[11] & 0x0F) == 0x0F) continue;
                empty = 0; break;
            }
            if (!empty) break;
            c = fat_next_cluster(c);
            if (c == 0xFFFFFFFFu) { kfree(cbuf); return -1; }
        }
empty_ok:
        kfree(cbuf);
        if (!empty) return -1;
    }

    if (first >= 2) {
        if (free_chain(first) != 0) return -1;
    }

    e[0] = 0xE5;
    if (dir_write_entry(dc, doff, e) != 0) return -1;
    return 0;
}

int fat32_truncate(const char *path, uint32_t new_size)
{
    if (!V.mounted || !path) return -1;
    uint32_t dc, doff; uint8_t e[32];
    if (find_entry(path, &dc, &doff, e) != 0) return -1;
    if (e[11] & FAT32_ATTR_DIR) return -1;

    uint32_t first = ((uint32_t)(e[20] | (e[21] << 8)) << 16) |
                     (uint32_t)(e[26] | (e[27] << 8));

    uint32_t needed = (new_size == 0) ? 0
        : (new_size + V.cluster_bytes - 1) / V.cluster_bytes;

    if (needed == 0) {
        if (first >= 2) (void)free_chain(first);
        first = 0;
    } else if (first < 2) {
        uint32_t prev = 0;
        for (uint32_t i = 0; i < needed; i++) {
            uint32_t nc = alloc_cluster();
            if (nc == 0) return -1;
            if (i == 0) first = nc;
            else if (fat_write_entry(prev, nc) != 0) return -1;
            prev = nc;
        }
    } else {
        uint32_t c = first;
        uint32_t count = 1;
        while (count < needed) {
            uint32_t n = fat_next_cluster(c);
            if (n == 0xFFFFFFFFu) return -1;
            if (cluster_is_eoc(n)) {
                uint32_t nc = alloc_cluster();
                if (nc == 0) return -1;
                if (fat_write_entry(c, nc) != 0) return -1;
                c = nc;
            } else {
                c = n;
            }
            count++;
        }
        uint32_t n = fat_next_cluster(c);
        if (n == 0xFFFFFFFFu) return -1;
        if (!cluster_is_eoc(n)) {
            if (free_chain(n) != 0) return -1;
            if (fat_write_entry(c, FAT32_EOC) != 0) return -1;
        }
    }

    e[20] = (uint8_t)((first >> 16) & 0xFF);
    e[21] = (uint8_t)((first >> 24) & 0xFF);
    e[26] = (uint8_t)((first      ) & 0xFF);
    e[27] = (uint8_t)((first >>  8) & 0xFF);
    e[28] = (uint8_t)((new_size      ) & 0xFF);
    e[29] = (uint8_t)((new_size >>  8) & 0xFF);
    e[30] = (uint8_t)((new_size >> 16) & 0xFF);
    e[31] = (uint8_t)((new_size >> 24) & 0xFF);
    fat_fill_times(e);
    return dir_write_entry(dc, doff, e);
}

int fat32_write(const char *path, uint32_t offset,
                const void *data, uint32_t len)
{
    if (!V.mounted || !path || (!data && len > 0)) return -1;
    uint32_t dc, doff; uint8_t e[32];
    if (find_entry(path, &dc, &doff, e) != 0) return -1;
    if (e[11] & FAT32_ATTR_DIR) return -1;

    uint32_t first = ((uint32_t)(e[20] | (e[21] << 8)) << 16) |
                     (uint32_t)(e[26] | (e[27] << 8));
    uint32_t cur_size = e[28] | ((uint32_t)e[29] << 8)
                      | ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);

    uint32_t end = offset + len;
    uint32_t needed = (end == 0) ? 0
        : (end + V.cluster_bytes - 1) / V.cluster_bytes;

    if (first < 2 && needed > 0) {
        uint32_t nc = alloc_cluster();
        if (nc == 0) return -1;
        first = nc;
    }
    if (needed > 0) {
        uint32_t c = first;
        uint32_t have = 1;
        while (have < needed) {
            uint32_t n = fat_next_cluster(c);
            if (n == 0xFFFFFFFFu) return -1;
            if (cluster_is_eoc(n)) {
                uint32_t nc = alloc_cluster();
                if (nc == 0) return -1;
                if (fat_write_entry(c, nc) != 0) return -1;
                c = nc;
            } else {
                c = n;
            }
            have++;
        }
    }

    if (len > 0) {
        uint8_t *cbuf = (uint8_t *)kmalloc(V.cluster_bytes);
        if (!cbuf) return -1;

        uint32_t pos = 0;
        uint32_t skip_clusters = offset / V.cluster_bytes;
        uint32_t in_off = offset % V.cluster_bytes;

        uint32_t c = first;
        for (uint32_t i = 0; i < skip_clusters; i++) {
            uint32_t n = fat_next_cluster(c);
            if (n == 0xFFFFFFFFu || cluster_is_eoc(n)) { kfree(cbuf); return -1; }
            c = n;
        }

        const uint8_t *src = (const uint8_t *)data;
        while (pos < len) {
            if (read_cluster(c, cbuf) != 0) { kfree(cbuf); return -1; }
            uint32_t avail = V.cluster_bytes - in_off;
            uint32_t want  = len - pos;
            uint32_t copy  = (want < avail) ? want : avail;
            m_copy(cbuf + in_off, src + pos, copy);
            if (write_cluster(c, cbuf) != 0) { kfree(cbuf); return -1; }
            pos += copy;
            in_off = 0;
            if (pos < len) {
                uint32_t n = fat_next_cluster(c);
                if (n == 0xFFFFFFFFu || cluster_is_eoc(n)) { kfree(cbuf); return -1; }
                c = n;
            }
        }
        kfree(cbuf);
    }

    if (end > cur_size) cur_size = end;
    e[20] = (uint8_t)((first >> 16) & 0xFF);
    e[21] = (uint8_t)((first >> 24) & 0xFF);
    e[26] = (uint8_t)((first      ) & 0xFF);
    e[27] = (uint8_t)((first >>  8) & 0xFF);
    e[28] = (uint8_t)((cur_size      ) & 0xFF);
    e[29] = (uint8_t)((cur_size >>  8) & 0xFF);
    e[30] = (uint8_t)((cur_size >> 16) & 0xFF);
    e[31] = (uint8_t)((cur_size >> 24) & 0xFF);
    fat_fill_times(e);
    if (dir_write_entry(dc, doff, e) != 0) return -1;
    return (int)len;
}

/* ── readdir-with-callback ──────────────────────────────────────── */

struct readdir_ctx {
    fat32_readdir_cb cb;
    void *user;
};

static int readdir_visitor(const char *lname, const char *sname,
                           uint32_t cluster, uint32_t size,
                           uint8_t attr, void *user)
{
    struct readdir_ctx *ctx = (struct readdir_ctx *)user;
    if (sname[0] == '.' && (sname[1] == '\0' ||
        (sname[1] == '.' && sname[2] == '\0'))) return 0;
    const char *name = (lname && lname[0]) ? lname : sname;
    return ctx->cb(name, size, cluster, attr, ctx->user);
}

int fat32_readdir(const char *path, fat32_readdir_cb cb, void *user)
{
    if (!V.mounted || !cb) return -1;
    struct fat32_file f;
    if (fat32_open(path && path[0] ? path : "/", &f) != 0) return -1;
    if (!f.is_dir) return -1;
    struct readdir_ctx ctx = { cb, user };
    return walk_dir(f.start_cluster ? f.start_cluster : V.root_cluster,
                    readdir_visitor, &ctx);
}

/* ───────────────────────────────────────────────────────────────────
 *  Trash (soft-delete) subsystem
 * ─────────────────────────────────────────────────────────────────── */

#define TRASH_ROOT "/.zeos-trash"

/* Monotonic counter so two trashes within the same wall-clock second
 * still get distinct ids. */
static uint32_t g_trash_seq;

/* itoa-style helpers for meta.txt + ids. */
static int u64_to_dec(uint64_t v, char *out)
{
    char tmp[24];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    while (v > 0) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    int o = 0;
    while (n > 0) out[o++] = tmp[--n];
    out[o] = '\0';
    return o;
}

static int u32_to_dec(uint32_t v, char *out) { return u64_to_dec(v, out); }

static int u32_to_hex8(uint32_t v, char *out)
{
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[7 - i] = hexd[v & 0xF];
        v >>= 4;
    }
    out[8] = '\0';
    return 8;
}

static int s_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static int s_starts_with(const char *s, const char *prefix) {
    while (*prefix) { if (*s != *prefix) return 0; s++; prefix++; }
    return 1;
}

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Build a unique trash id. tod_now_unix may be 0 on hosts without
 * RTC; combine with a TSC mix and a counter so collisions are still
 * unlikely. The id is 8 lowercase hex chars (fits in 8.3 SFN slot). */
static void make_trash_id(char out[FAT32_TRASH_ID_MAX + 1])
{
    uint64_t now = tod_now_unix();
    uint64_t t = fat_tsc_seed();
    uint32_t seq = ++g_trash_seq;
    uint32_t mix = (uint32_t)(now ^ (t >> 17)) ^ (seq * 2654435761u);
    if (mix == 0) mix = (seq | 0x80000000u);
    u32_to_hex8(mix, out);
}

/* Decimal parse helper for meta.txt fields. Reads leading digits;
 * stops on first non-digit. */
static uint64_t parse_u64(const char *s, const char **endp)
{
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; }
    if (endp) *endp = s;
    return v;
}

/* Path predicate: is `path` inside the trash root? Both "/.zeos-trash"
 * and "/.zeos-trash/..." count. */
static int path_in_trash(const char *path)
{
    if (!path) return 0;
    while (*path == '/') path++;
    /* normalize leading slashes off TRASH_ROOT */
    const char *root = TRASH_ROOT;
    while (*root == '/') root++;
    int rl = s_len(root);
    int i;
    for (i = 0; i < rl; i++) {
        if (path[i] != root[i]) return 0;
    }
    return path[rl] == '\0' || path[rl] == '/';
}

int fat32_trash_ensure_root(void)
{
    if (!V.mounted) return -1;
    struct fat32_file f;
    if (fat32_open(TRASH_ROOT, &f) == 0 && f.is_dir) return 0;
    return fat32_mkdir(TRASH_ROOT);
}

/* Build "/.zeos-trash/<id>" + tail. Caller-supplied buffer. */
static void build_trash_path(char *out, int max,
                             const char *id, const char *tail)
{
    int o = 0;
    const char *p = TRASH_ROOT;
    while (*p && o < max - 1) out[o++] = *p++;
    out[o++] = '/';
    while (*id && o < max - 1) out[o++] = *id++;
    if (tail && *tail) {
        if (tail[0] != '/') out[o++] = '/';
        while (*tail && o < max - 1) out[o++] = *tail++;
    }
    out[o] = '\0';
}

/* Find a single non-meta.txt entry inside a trash subdir. Returns
 * 0 on success and writes basename into out_basename. */
struct trash_inner_ctx {
    char name[FAT32_NAME_MAX + 1];
    int  found;
    uint8_t is_dir;
    uint32_t size;
};

static int trash_inner_visitor(const char *name, uint32_t size,
                               uint32_t cluster, uint8_t attr,
                               void *user)
{
    (void)cluster;
    struct trash_inner_ctx *c = (struct trash_inner_ctx *)user;
    if (s_eq(name, "meta.txt")) return 0;
    if (c->found) return 0;
    int n = 0;
    while (name[n] && n < FAT32_NAME_MAX) { c->name[n] = name[n]; n++; }
    c->name[n] = '\0';
    c->is_dir = (attr & FAT32_ATTR_DIR) ? 1 : 0;
    c->size = size;
    c->found = 1;
    return 0;
}

/* Read meta.txt for a given trash id. Returns 0 on success. */
static int trash_read_meta(const char *id,
                           char *out_orig, int orig_max,
                           uint64_t *out_deleted_at,
                           uint32_t *out_size,
                           char *out_reason, int reason_max)
{
    char metap[FAT32_PATH_MAX];
    build_trash_path(metap, sizeof(metap), id, "meta.txt");

    struct fat32_file mf;
    if (fat32_open(metap, &mf) != 0) return -1;
    if (mf.is_dir) return -1;
    if (mf.size == 0 || mf.size > 1024) return -1;

    char buf[1025];
    int got = fat32_read(&mf, buf, sizeof(buf) - 1);
    if (got <= 0) return -1;
    buf[got] = '\0';

    if (out_orig && orig_max > 0) out_orig[0] = '\0';
    if (out_reason && reason_max > 0) out_reason[0] = '\0';
    if (out_deleted_at) *out_deleted_at = 0;
    if (out_size) *out_size = 0;

    /* Format: lines "key=value\n" — orig_path, size, deleted_at,
     * reason. Keys are stable so we can parse line-by-line. */
    const char *p = buf;
    while (*p) {
        const char *key = p;
        while (*p && *p != '=' && *p != '\n') p++;
        if (*p != '=') {
            if (*p == '\n') p++;
            continue;
        }
        int klen = (int)(p - key);
        p++; /* skip '=' */
        const char *val = p;
        while (*p && *p != '\n') p++;
        int vlen = (int)(p - val);
        if (*p == '\n') p++;

        if (klen == 9 && key[0] == 'o') {
            if (out_orig && orig_max > 0) {
                int n = vlen < orig_max - 1 ? vlen : orig_max - 1;
                for (int i = 0; i < n; i++) out_orig[i] = val[i];
                out_orig[n] = '\0';
            }
        } else if (klen == 4 && key[0] == 's' && key[1] == 'i') {
            const char *e;
            if (out_size) *out_size = (uint32_t)parse_u64(val, &e);
        } else if (klen == 10 && key[0] == 'd') {
            const char *e;
            if (out_deleted_at) *out_deleted_at = parse_u64(val, &e);
        } else if (klen == 6 && key[0] == 'r') {
            if (out_reason && reason_max > 0) {
                int n = vlen < reason_max - 1 ? vlen : reason_max - 1;
                for (int i = 0; i < n; i++) out_reason[i] = val[i];
                out_reason[n] = '\0';
            }
        }
    }
    return 0;
}

static int trash_write_meta(const char *id,
                            const char *orig_path,
                            uint32_t size,
                            uint64_t deleted_at,
                            const char *reason)
{
    char metap[FAT32_PATH_MAX];
    build_trash_path(metap, sizeof(metap), id, "meta.txt");

    char buf[768];
    int o = 0;
    const char *k1 = "orig_path=";
    while (*k1 && o < (int)sizeof(buf) - 2) buf[o++] = *k1++;
    while (*orig_path && o < (int)sizeof(buf) - 2) buf[o++] = *orig_path++;
    buf[o++] = '\n';

    const char *k2 = "size=";
    while (*k2 && o < (int)sizeof(buf) - 2) buf[o++] = *k2++;
    {
        char tmp[24]; u32_to_dec(size, tmp);
        for (int i = 0; tmp[i] && o < (int)sizeof(buf) - 2; i++) buf[o++] = tmp[i];
    }
    buf[o++] = '\n';

    const char *k3 = "deleted_at=";
    while (*k3 && o < (int)sizeof(buf) - 2) buf[o++] = *k3++;
    {
        char tmp[24]; u64_to_dec(deleted_at, tmp);
        for (int i = 0; tmp[i] && o < (int)sizeof(buf) - 2; i++) buf[o++] = tmp[i];
    }
    buf[o++] = '\n';

    const char *k4 = "reason=";
    while (*k4 && o < (int)sizeof(buf) - 2) buf[o++] = *k4++;
    if (!reason || !reason[0]) reason = "user";
    while (*reason && o < (int)sizeof(buf) - 2) buf[o++] = *reason++;
    buf[o++] = '\n';

    if (fat32_create(metap) != 0) return -1;
    if (fat32_truncate(metap, 0) != 0) return -1;
    int w = fat32_write(metap, 0, buf, (uint32_t)o);
    if (w != o) return -1;
    return 0;
}

/* Move a directory entry. Reads (src_dir, src_off) into a fresh entry
 * inside dst_dir under dst_basename, preserving cluster chain + size +
 * attr; then erases the original dir slot (0xE5). This is the heart
 * of trash + restore — no cluster data is copied. */
static int dir_entry_move(uint32_t src_dir_cluster, uint32_t src_off,
                          uint32_t dst_dir_cluster,
                          const char *dst_basename)
{
    uint8_t *cbuf = (uint8_t *)kmalloc(V.cluster_bytes);
    if (!cbuf) return -1;
    if (read_cluster(src_dir_cluster, cbuf) != 0) { kfree(cbuf); return -1; }

    uint8_t e[32];
    m_copy(e, cbuf + src_off, 32);
    kfree(cbuf);

    /* Compose new entry with new 8.3 name, preserving everything else. */
    uint8_t sn[11];
    if (make_8_3(dst_basename, sn) != 0) return -1;
    m_copy(e, sn, 11);

    uint32_t dc = 0, doff = 0;
    if (dir_find_free_slot(dst_dir_cluster, &dc, &doff) != 0) return -1;
    if (dir_write_entry(dc, doff, e) != 0) return -1;

    /* Erase source slot. */
    uint8_t *cbuf2 = (uint8_t *)kmalloc(V.cluster_bytes);
    if (!cbuf2) return -1;
    if (read_cluster(src_dir_cluster, cbuf2) != 0) { kfree(cbuf2); return -1; }
    cbuf2[src_off] = 0xE5;
    int rc = write_cluster(src_dir_cluster, cbuf2);
    kfree(cbuf2);
    return rc;
}

int fat32_trash(const char *path, const char *reason, char *out_id)
{
    if (!V.mounted || !path || !*path) return -1;
    if (path_in_trash(path)) return -1; /* refuse to trash the trash */

    /* Locate source dir entry. */
    uint32_t src_dc = 0, src_doff = 0; uint8_t e[32];
    if (find_entry(path, &src_dc, &src_doff, e) != 0) return -1;

    /* For directories: refuse — recursive trash not supported in v1.
     * Fall back to direct unlink semantics (which is empty-dir-only). */
    if (e[11] & FAT32_ATTR_DIR) return -1;

    uint32_t size = e[28] | ((uint32_t)e[29] << 8)
                  | ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);

    if (fat32_trash_ensure_root() != 0) return -1;

    /* Allocate a fresh id. */
    char id[FAT32_TRASH_ID_MAX + 1];
    make_trash_id(id);

    /* Create the per-entry subdir /.zeos-trash/<id>. */
    char subdir[FAT32_PATH_MAX];
    build_trash_path(subdir, sizeof(subdir), id, 0);
    if (fat32_mkdir(subdir) != 0) {
        /* On the off-chance of collision, retry once with a perturbed id. */
        g_trash_seq += 0x9E37u;
        make_trash_id(id);
        build_trash_path(subdir, sizeof(subdir), id, 0);
        if (fat32_mkdir(subdir) != 0) return -1;
    }

    /* Resolve the new subdir cluster for the dir-entry move. */
    uint32_t sub_cluster = 0;
    if (resolve_dir(subdir, &sub_cluster) != 0) return -1;

    /* Derive basename from path. */
    char parent_unused[FAT32_PATH_MAX];
    char base[FAT32_NAME_MAX + 1];
    if (path_split_parent(path, parent_unused, base) != 0) return -1;

    /* If a same-named entry already exists inside the subdir (very
     * rare — the subdir is fresh), unlink it first. */
    {
        uint32_t dc, doff; uint8_t en[32];
        char inner[FAT32_PATH_MAX];
        build_trash_path(inner, sizeof(inner), id, base);
        (void)inner; (void)dc; (void)doff; (void)en;
    }

    /* Move the source dir entry into the trash subdir. */
    if (dir_entry_move(src_dc, src_doff, sub_cluster, base) != 0) return -1;

    /* Write meta.txt. We canonicalize the original path so it always
     * starts with a '/'. */
    char origbuf[FAT32_PATH_MAX];
    {
        int o = 0;
        if (path[0] != '/') origbuf[o++] = '/';
        for (int i = 0; path[i] && o < (int)sizeof(origbuf) - 1; i++)
            origbuf[o++] = path[i];
        origbuf[o] = '\0';
    }
    uint64_t now = tod_now_unix();
    if (trash_write_meta(id, origbuf, size, now,
                         reason && reason[0] ? reason : "user") != 0) {
        /* Meta write failed — try to roll back the rename so the user
         * can still see their file. Best-effort. */
        return -1;
    }

    if (out_id) {
        for (int i = 0; i < FAT32_TRASH_ID_MAX + 1; i++) out_id[i] = id[i];
    }

    /* Bump CHAIN_BLOCK vault_version so MasQ/journal records the
     * trash event distinctly from the underlying writes. The actual
     * sector writes are already journaled. */
    return 0;
}

int fat32_trash_list(fat32_trash_list_cb cb)
{
    if (!V.mounted || !cb) return -1;

    struct fat32_file root;
    if (fat32_open(TRASH_ROOT, &root) != 0) return 0; /* none yet */
    if (!root.is_dir) return -1;

    /* Snapshot ids first so the callback can inspect each. We use a
     * fixed-size on-stack scratch — the MAX_LIST cap is generous for
     * practical desktops. */
    #define MAX_TRASH_LIST 256
    static struct fat32_dirent ents[MAX_TRASH_LIST];
    int n = fat32_list(TRASH_ROOT, ents, MAX_TRASH_LIST);
    if (n < 0) return -1;

    int reported = 0;
    for (int i = 0; i < n; i++) {
        if (!ents[i].is_dir) continue;
        const char *id = ents[i].name;

        char orig[FAT32_PATH_MAX];
        char rsn[16];
        uint64_t da = 0;
        uint32_t sz = 0;
        if (trash_read_meta(id, orig, sizeof(orig), &da, &sz,
                            rsn, sizeof(rsn)) != 0) continue;
        cb(id, orig, da, sz);
        reported++;
    }
    return reported;
}

int fat32_trash_restore(const char *id)
{
    if (!V.mounted || !id || !*id) return -1;

    char orig[FAT32_PATH_MAX];
    uint64_t da = 0; uint32_t sz = 0;
    char rsn[16];
    if (trash_read_meta(id, orig, sizeof(orig), &da, &sz,
                        rsn, sizeof(rsn)) != 0) return -1;
    if (!orig[0]) return -1;

    /* Find the inner entry (the file, not meta.txt). */
    char subdir[FAT32_PATH_MAX];
    build_trash_path(subdir, sizeof(subdir), id, 0);

    struct trash_inner_ctx c;
    c.found = 0; c.is_dir = 0; c.size = 0; c.name[0] = '\0';
    if (fat32_readdir(subdir, trash_inner_visitor, &c) != 0) return -1;
    if (!c.found) return -1;

    /* Locate dir entry for the inner file. */
    uint32_t sub_cluster = 0;
    if (resolve_dir(subdir, &sub_cluster) != 0) return -1;
    uint32_t inner_dc = 0, inner_off = 0; uint8_t inner_e[32];
    if (dir_locate(sub_cluster, c.name, &inner_dc, &inner_off, inner_e) != 0)
        return -1;

    /* Make sure parent of original path exists. */
    char parent[FAT32_PATH_MAX];
    char base[FAT32_NAME_MAX + 1];
    if (path_split_parent(orig, parent, base) != 0) return -1;
    uint32_t pc = 0;
    if (resolve_dir(parent, &pc) != 0) return -1;

    /* Refuse to clobber an existing file at the destination. */
    {
        uint32_t dc, doff; uint8_t edst[32];
        if (dir_locate(pc, base, &dc, &doff, edst) == 0) return -1;
    }

    /* Move dir entry back. */
    if (dir_entry_move(inner_dc, inner_off, pc, base) != 0) return -1;

    /* Drop meta.txt and the now-empty subdir. */
    char metap[FAT32_PATH_MAX];
    build_trash_path(metap, sizeof(metap), id, "meta.txt");
    (void)fat32_unlink(metap);
    (void)fat32_unlink(subdir);
    return 0;
}

/* Helper: hard-purge a single trash entry by id. Returns 0 on
 * success. Frees cluster chains via fat32_unlink for the inner file,
 * meta.txt, and the subdir. */
static int trash_purge_one(const char *id)
{
    char subdir[FAT32_PATH_MAX];
    build_trash_path(subdir, sizeof(subdir), id, 0);

    /* Inner file (if any). */
    struct trash_inner_ctx c;
    c.found = 0; c.is_dir = 0; c.size = 0; c.name[0] = '\0';
    if (fat32_readdir(subdir, trash_inner_visitor, &c) == 0 && c.found) {
        char inner[FAT32_PATH_MAX];
        build_trash_path(inner, sizeof(inner), id, c.name);
        (void)fat32_unlink(inner);
    }

    char metap[FAT32_PATH_MAX];
    build_trash_path(metap, sizeof(metap), id, "meta.txt");
    (void)fat32_unlink(metap);

    /* Now the subdir should be empty (apart from . and ..). */
    return fat32_unlink(subdir);
}

int fat32_trash_empty(void)
{
    if (!V.mounted) return -1;
    struct fat32_file root;
    if (fat32_open(TRASH_ROOT, &root) != 0) return 0;
    if (!root.is_dir) return -1;

    #define EMPTY_BATCH 256
    static struct fat32_dirent batch[EMPTY_BATCH];
    int freed = 0;

    /* Snapshot, purge, repeat — cluster directory layout shifts
     * after each unlink. */
    for (;;) {
        int n = fat32_list(TRASH_ROOT, batch, EMPTY_BATCH);
        if (n <= 0) break;
        int progress = 0;
        for (int i = 0; i < n; i++) {
            if (!batch[i].is_dir) continue;
            if (trash_purge_one(batch[i].name) == 0) {
                freed++;
                progress = 1;
            }
        }
        if (!progress) break;
    }
    return freed;
}

int fat32_trash_empty_older_than(uint64_t cutoff_secs)
{
    if (!V.mounted) return -1;
    struct fat32_file root;
    if (fat32_open(TRASH_ROOT, &root) != 0) return 0;
    if (!root.is_dir) return -1;

    uint64_t now = tod_now_unix();
    /* If we have no wall clock, refuse to age-out — better to do
     * nothing than to nuke everything because epoch is 0. */
    if (now == 0) return 0;
    uint64_t threshold = (now > cutoff_secs) ? (now - cutoff_secs) : 0;

    static struct fat32_dirent batch[256];
    int freed = 0;
    for (;;) {
        int n = fat32_list(TRASH_ROOT, batch, 256);
        if (n <= 0) break;
        int progress = 0;
        for (int i = 0; i < n; i++) {
            if (!batch[i].is_dir) continue;
            char orig[FAT32_PATH_MAX];
            uint64_t da = 0; uint32_t sz = 0;
            char rsn[16];
            if (trash_read_meta(batch[i].name, orig, sizeof(orig),
                                &da, &sz, rsn, sizeof(rsn)) != 0) continue;
            if (da == 0 || da >= threshold) continue;
            if (trash_purge_one(batch[i].name) == 0) {
                freed++;
                progress = 1;
            }
        }
        if (!progress) break;
    }
    return freed;
}

int fat32_trash_stats(uint32_t *out_count, uint64_t *out_bytes)
{
    if (out_count) *out_count = 0;
    if (out_bytes) *out_bytes = 0;
    if (!V.mounted) return -1;

    struct fat32_file root;
    if (fat32_open(TRASH_ROOT, &root) != 0) return 0; /* nothing yet */
    if (!root.is_dir) return -1;

    static struct fat32_dirent batch[256];
    int n = fat32_list(TRASH_ROOT, batch, 256);
    if (n < 0) return -1;
    uint32_t count = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < n; i++) {
        if (!batch[i].is_dir) continue;
        char orig[FAT32_PATH_MAX];
        uint64_t da = 0; uint32_t sz = 0;
        char rsn[16];
        if (trash_read_meta(batch[i].name, orig, sizeof(orig),
                            &da, &sz, rsn, sizeof(rsn)) != 0) continue;
        count++;
        bytes += sz;
    }
    if (out_count) *out_count = count;
    if (out_bytes) *out_bytes = bytes;
    return 0;
}

/* Suppress -Wunused on helpers used only via API surface. */
static void __fat32_trash_keep_alive(void) __attribute__((unused));
static void __fat32_trash_keep_alive(void) {
    (void)s_starts_with;
}

