/*
 * Zeos -- VAULT On-Disk Persistence Layer
 *
 * Phase 2 of VAULT: real storage backend.
 * Provides reboot persistence via block device callbacks.
 *
 * This is intentionally simple: sequential allocation, static
 * file table, no free-list reclamation. Files are small (chain
 * state ~200 bytes, config ~100 bytes). Total usage under 1MB
 * for thousands of chains. Complexity is the enemy.
 *
 * On-disk layout:
 *   Block 0:               Superblock
 *   Blocks 1..FT_BLOCKS:   File table (256 entries)
 *   Blocks FT_BLOCKS+1...: Data blocks
 */

#include "vault_disk.h"
#include "vault.h"
#include "kprint.h"
#include "heap.h"
#include "timer.h"   /* timer_read_tsc() — portable cycle counter */

/* ---- Internal helpers (duplicated to stay freestanding) ---- */

static int vd_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void vd_strcpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void vd_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++)
        d[i] = s[i];
}

static void vd_memset(void *dst, uint8_t val, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++)
        d[i] = val;
}

static uint64_t vd_read_tsc(void)
{
    return timer_read_tsc();   /* HAL: was raw rdtsc */
}

/* ---- Static state ----------------------------------------- */

/* Block device callbacks */
static vault_block_read_fn  g_read_fn;
static vault_block_write_fn g_write_fn;

/* Partition geometry */
static uint64_t g_part_start;       /* First LBA of VAULT partition */
static uint64_t g_part_blocks;      /* Total blocks in partition */

/* In-memory copies */
static vault_superblock_t   g_super;
static vault_file_entry_t   g_files[VAULT_MAX_FILES];

/* File table spans this many blocks on disk */
#define FT_ENTRY_SIZE   ((uint32_t)sizeof(vault_file_entry_t))
#define FT_BLOCKS       ((VAULT_MAX_FILES * FT_ENTRY_SIZE + VAULT_DISK_BLOCK - 1) / VAULT_DISK_BLOCK)

/* First data block (after superblock + file table) */
#define DATA_START      (1 + FT_BLOCKS)

/* Initialization flag */
static int g_disk_mounted = 0;

/* Scratch buffer for block I/O (one block) */
static uint8_t g_block_buf[VAULT_DISK_BLOCK] __attribute__((aligned(4096)));

/* ---- Block I/O wrappers ----------------------------------- */

/*
 * Read one VAULT block (4096 bytes) from the partition.
 * block_offset is relative to the partition start.
 */
static int read_block(uint32_t block_offset, void *buf)
{
    if (!g_read_fn) return -1;
    uint64_t lba = g_part_start + (uint64_t)block_offset * (VAULT_DISK_BLOCK / 512);
    uint32_t sectors = VAULT_DISK_BLOCK / 512;
    return g_read_fn(lba, sectors, buf);
}

/*
 * Write one VAULT block (4096 bytes) to the partition.
 * block_offset is relative to the partition start.
 */
static int write_block(uint32_t block_offset, const void *buf)
{
    if (!g_write_fn) return -1;
    uint64_t lba = g_part_start + (uint64_t)block_offset * (VAULT_DISK_BLOCK / 512);
    uint32_t sectors = VAULT_DISK_BLOCK / 512;
    return g_write_fn(lba, sectors, buf);
}

/* ---- Superblock I/O --------------------------------------- */

static int write_superblock(void)
{
    vd_memset(g_block_buf, 0, VAULT_DISK_BLOCK);
    vd_memcpy(g_block_buf, &g_super, sizeof(vault_superblock_t));
    return write_block(0, g_block_buf);
}

static int read_superblock(void)
{
    if (read_block(0, g_block_buf) != 0)
        return -1;
    vd_memcpy(&g_super, g_block_buf, sizeof(vault_superblock_t));
    return 0;
}

/* ---- File table I/O --------------------------------------- */

/*
 * Write the entire file table to disk (blocks 1..FT_BLOCKS).
 * Serializes the g_files array block by block.
 */
static int write_file_table(void)
{
    uint8_t *src = (uint8_t *)g_files;
    uint32_t remaining = VAULT_MAX_FILES * FT_ENTRY_SIZE;

    for (uint32_t b = 0; b < FT_BLOCKS; b++) {
        uint32_t chunk = remaining;
        if (chunk > VAULT_DISK_BLOCK) chunk = VAULT_DISK_BLOCK;

        vd_memset(g_block_buf, 0, VAULT_DISK_BLOCK);
        vd_memcpy(g_block_buf, src, chunk);

        if (write_block(1 + b, g_block_buf) != 0)
            return -1;

        src += chunk;
        remaining -= chunk;
    }
    return 0;
}

/*
 * Read the entire file table from disk into g_files.
 */
static int read_file_table(void)
{
    uint8_t *dst = (uint8_t *)g_files;
    uint32_t remaining = VAULT_MAX_FILES * FT_ENTRY_SIZE;

    for (uint32_t b = 0; b < FT_BLOCKS; b++) {
        if (read_block(1 + b, g_block_buf) != 0)
            return -1;

        uint32_t chunk = remaining;
        if (chunk > VAULT_DISK_BLOCK) chunk = VAULT_DISK_BLOCK;

        vd_memcpy(dst, g_block_buf, chunk);
        dst += chunk;
        remaining -= chunk;
    }
    return 0;
}

/* ---- Find file entry by path ------------------------------ */

static int find_entry(const char *path)
{
    for (int i = 0; i < VAULT_MAX_FILES; i++) {
        if (g_files[i].active && vd_streq(g_files[i].path, path))
            return i;
    }
    return -1;
}

/* Find first empty slot */
static int find_free_slot(void)
{
    for (int i = 0; i < VAULT_MAX_FILES; i++) {
        if (!g_files[i].active)
            return i;
    }
    return -1;
}

/* ---- Public API ------------------------------------------- */

int vault_disk_format(void)
{
    if (!g_write_fn) return -1;

    kputs("VAULT: formatting disk partition...\n");

    /* Zero and populate superblock */
    vd_memset(&g_super, 0, sizeof(vault_superblock_t));
    g_super.magic           = VAULT_DISK_MAGIC;
    g_super.version         = VAULT_DISK_VERSION;
    g_super.total_blocks    = (uint32_t)g_part_blocks;
    g_super.used_blocks     = DATA_START;   /* superblock + file table are used */
    g_super.file_count      = 0;
    g_super.first_free_block = DATA_START;

    if (write_superblock() != 0) {
        kputs("VAULT: format failed (superblock write)\n");
        return -1;
    }

    /* Zero file table */
    vd_memset(g_files, 0, sizeof(g_files));
    if (write_file_table() != 0) {
        kputs("VAULT: format failed (file table write)\n");
        return -1;
    }

    kputs("VAULT: format complete, ");
    kput_dec(g_super.total_blocks);
    kputs(" blocks total, data starts at block ");
    kput_dec(DATA_START);
    kputs("\n");

    return 0;
}

int vault_disk_init(vault_block_read_fn read_fn, vault_block_write_fn write_fn,
                    uint64_t partition_start_lba, uint64_t partition_blocks)
{
    g_read_fn     = read_fn;
    g_write_fn    = write_fn;
    g_part_start  = partition_start_lba;
    g_part_blocks = partition_blocks;
    g_disk_mounted = 0;

    /* Read superblock */
    if (read_superblock() != 0) {
        kputs("VAULT: disk read failed at superblock\n");
        return -1;
    }

    /* Check magic -- format if this isn't a VAULT partition */
    if (g_super.magic != VAULT_DISK_MAGIC) {
        kputs("VAULT: no valid superblock found, formatting\n");
        if (vault_disk_format() != 0)
            return -1;
    } else {
        /* Valid superblock -- load file table */
        if (read_file_table() != 0) {
            kputs("VAULT: failed to read file table\n");
            return -1;
        }
    }

    g_disk_mounted = 1;

    /* Report */
    kputs("VAULT: disk mounted, ");
    kput_dec(g_super.file_count);
    kputs(" files, ");
    kput_dec(g_super.used_blocks);
    kputs("/");
    kput_dec(g_super.total_blocks);
    kputs(" blocks used\n");

    return 0;
}

int vault_disk_write(const char *path, const void *data, uint32_t size)
{
    if (!g_disk_mounted || !path || !data) return -1;

    int idx = find_entry(path);
    int is_new = 0;

    if (idx < 0) {
        /* New file -- find an empty slot */
        idx = find_free_slot();
        if (idx < 0) {
            kputs("VAULT: disk full (max files reached)\n");
            return -1;
        }
        is_new = 1;
    }

    /* Calculate blocks needed */
    uint32_t blocks_needed = (size + VAULT_DISK_BLOCK - 1) / VAULT_DISK_BLOCK;
    if (blocks_needed == 0) blocks_needed = 1;

    /*
     * Allocation strategy:
     * - New files: allocate from first_free_block (sequential append)
     * - Existing files: if new size fits in old allocation, reuse blocks.
     *   Otherwise, allocate new blocks (old blocks are abandoned -- acceptable
     *   for Alpha, total usage is tiny).
     */
    uint32_t start_block;

    if (!is_new && blocks_needed <= ((g_files[idx].size_bytes + VAULT_DISK_BLOCK - 1) / VAULT_DISK_BLOCK)) {
        /* Fits in existing allocation -- reuse */
        start_block = g_files[idx].start_block;
    } else {
        /* Allocate new blocks from the end */
        if (g_super.first_free_block + blocks_needed > g_super.total_blocks) {
            kputs("VAULT: disk full (no space for data)\n");
            return -1;
        }
        start_block = g_super.first_free_block;
        g_super.first_free_block += blocks_needed;
        g_super.used_blocks += blocks_needed;
    }

    /* Write data blocks */
    const uint8_t *src = (const uint8_t *)data;
    uint32_t remaining = size;

    for (uint32_t b = 0; b < blocks_needed; b++) {
        uint32_t chunk = remaining;
        if (chunk > VAULT_DISK_BLOCK) chunk = VAULT_DISK_BLOCK;

        /* Zero the block buffer, then copy data (pads last block with zeros) */
        vd_memset(g_block_buf, 0, VAULT_DISK_BLOCK);
        vd_memcpy(g_block_buf, src, chunk);

        if (write_block(start_block + b, g_block_buf) != 0) {
            kputs("VAULT: data write failed at block ");
            kput_dec(start_block + b);
            kputs("\n");
            return -1;
        }

        src += chunk;
        remaining -= chunk;
    }

    /* Update file entry */
    vd_strcpy(g_files[idx].path, path, VAULT_DISK_MAX_PATH);
    g_files[idx].start_block = start_block;
    g_files[idx].size_bytes  = size;
    g_files[idx].timestamp   = (uint32_t)(vd_read_tsc() >> 20);  /* Coarse timestamp */
    g_files[idx].active      = 1;

    if (is_new) {
        g_files[idx].version = 1;
        g_super.file_count++;
    } else {
        g_files[idx].version++;
    }

    /* Persist file table and superblock */
    if (write_file_table() != 0) {
        kputs("VAULT: file table write failed\n");
        return -1;
    }
    if (write_superblock() != 0) {
        kputs("VAULT: superblock write failed\n");
        return -1;
    }

    return 0;
}

int vault_disk_read(const char *path, void *buf, uint32_t max_size)
{
    if (!g_disk_mounted || !path || !buf) return -1;

    int idx = find_entry(path);
    if (idx < 0) return -1;

    uint32_t to_read = g_files[idx].size_bytes;
    if (to_read > max_size) to_read = max_size;

    uint32_t blocks = (to_read + VAULT_DISK_BLOCK - 1) / VAULT_DISK_BLOCK;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t remaining = to_read;

    for (uint32_t b = 0; b < blocks; b++) {
        if (read_block(g_files[idx].start_block + b, g_block_buf) != 0) {
            kputs("VAULT: data read failed at block ");
            kput_dec(g_files[idx].start_block + b);
            kputs("\n");
            return -1;
        }

        uint32_t chunk = remaining;
        if (chunk > VAULT_DISK_BLOCK) chunk = VAULT_DISK_BLOCK;

        vd_memcpy(dst, g_block_buf, chunk);
        dst += chunk;
        remaining -= chunk;
    }

    return (int)to_read;
}

int vault_disk_exists(const char *path)
{
    if (!g_disk_mounted || !path) return 0;
    return find_entry(path) >= 0;
}

int vault_disk_load(void)
{
    if (!g_disk_mounted) return -1;

    int loaded = 0;

    kputs("VAULT: loading disk state into memory...\n");

    for (int i = 0; i < VAULT_MAX_FILES; i++) {
        if (!g_files[i].active) continue;

        /* Read the file data from disk */
        uint32_t size = g_files[i].size_bytes;
        if (size == 0) continue;

        /* Allocate a temporary buffer for the file data */
        void *tmp = kmalloc(size);
        if (!tmp) {
            kputs("VAULT: load failed, out of heap for ");
            kputs(g_files[i].path);
            kputs("\n");
            continue;
        }

        int bytes = vault_disk_read(g_files[i].path, tmp, size);
        if (bytes > 0) {
            /*
             * Write into the in-memory VAULT.
             * vault_write auto-creates parent directories.
             */
            vault_write(g_files[i].path, tmp, (uint32_t)bytes);
            loaded++;
        }

        kfree(tmp);
    }

    kputs("VAULT: loaded ");
    kput_dec((uint64_t)loaded);
    kputs(" files from disk\n");

    return loaded;
}

int vault_disk_sync(void)
{
    if (!g_disk_mounted) return -1;

    /*
     * Walk in-memory VAULT directories and sync each file to disk.
     * Strategy: list known VAULT directories (/chains, /config, /desktop)
     * and write each file found.
     */
    static const char *sync_dirs[] = {
        "/chains",
        "/config",
        "/desktop",
        0
    };

    int synced = 0;
    struct vault_dirent entries[64];

    for (int d = 0; sync_dirs[d]; d++) {
        if (!vault_exists(sync_dirs[d]))
            continue;

        int count = vault_list(sync_dirs[d], entries, 64);
        if (count <= 0) continue;

        for (int i = 0; i < count; i++) {
            /* Build full path: dir/name */
            char fullpath[VAULT_DISK_MAX_PATH];
            int pi = 0;
            const char *dir = sync_dirs[d];
            while (*dir && pi < VAULT_DISK_MAX_PATH - 2)
                fullpath[pi++] = *dir++;
            fullpath[pi++] = '/';
            const char *name = entries[i].name;
            while (*name && pi < VAULT_DISK_MAX_PATH - 1)
                fullpath[pi++] = *name++;
            fullpath[pi] = '\0';

            /* Read from in-memory VAULT */
            int fsize = vault_size(fullpath);
            if (fsize <= 0) continue;

            void *tmp = kmalloc((uint64_t)fsize);
            if (!tmp) continue;

            int bytes = vault_read(fullpath, tmp, (uint32_t)fsize);
            if (bytes > 0) {
                if (vault_disk_write(fullpath, tmp, (uint32_t)bytes) == 0)
                    synced++;
            }

            kfree(tmp);
        }
    }

    kputs("VAULT: synced ");
    kput_dec((uint64_t)synced);
    kputs(" files to disk\n");

    return 0;
}
