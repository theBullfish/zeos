/*
 * Zeos — Read-only FAT32 driver
 *
 * See fat32.h for scope and conventions.
 *
 * Design notes:
 *   - All disk access goes through block_read(). One global mount.
 *   - Cluster sizes can exceed the block size (e.g. 4 KiB clusters on
 *     a 512-byte sector device = 8 sectors per cluster). All cluster
 *     I/O goes through read_cluster() which handles the multiplication.
 *   - LFN: VFAT long names are stored as preceding 0x0F-attribute
 *     entries, ordered last-fragment-first with bit 0x40 set on the
 *     final fragment. We assemble them in order, keep ASCII only
 *     (UCS-2 high bytes ignored — adequate for filenames in scope).
 *   - Path matching is case-insensitive ASCII for both 8.3 and LFN.
 */

#include "fat32.h"
#include "block.h"
#include "heap.h"
#include "kprint.h"

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
    if (block_read(V.fat1_lba + fat_block, 1, buf) != 0) {
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
    return block_read(cluster_lba(cluster), V.sectors_per_cluster, buf);
}

/* ── BPB parse / mount ────────────────────────────────────────── */

int fat32_mount(int drive_idx, uint64_t partition_lba)
{
    /* drive_idx reserved for future multi-drive block API. */
    (void)drive_idx;

    uint32_t blk = block_size();
    if (blk == 0) blk = 512;

    uint8_t *bpb = (uint8_t *)kmalloc(blk);
    if (!bpb) return -1;

    if (block_read(partition_lba, 1, bpb) != 0) {
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
static int try_gpt_partition_one(void)
{
    uint32_t blk = block_size();
    if (blk == 0) blk = 512;
    uint8_t *buf = (uint8_t *)kmalloc(blk);
    if (!buf) return -1;

    /* GPT header at LBA 1 */
    if (block_read(1, 1, buf) != 0) { kfree(buf); return -1; }
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
        if (block_read(entry_lba + bi, 1, buf) != 0) break;
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
            if (fat32_mount(0, first_lba) == 0) {
                kfree(buf); return 0;
            }
        }
    }
    kfree(buf);
    return -1;
}

int fat32_automount(void)
{
    /* Whole-disk first (USB sticks / SD cards usually have no GPT). */
    if (fat32_mount(0, 0) == 0) return 0;
    /* Then GPT (NVMe / AHCI installs with an ESP). */
    return try_gpt_partition_one();
}
