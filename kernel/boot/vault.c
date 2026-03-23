/*
 * Zeos — VAULT Filesystem Implementation
 *
 * In-memory filesystem with temporal versioning.
 * Phase 1: RAM-backed (using the heap allocator).
 * Phase 2: NVMe-backed (when we have a storage driver).
 *
 * Every write preserves the previous version. The fractal
 * database never forgets. You can always rewind.
 */

#include "vault.h"
#include "kprint.h"
#include "heap.h"

/* Global filesystem state */
static struct vault_fs g_vault;

/* The backing memory region */
static uint8_t *g_base;
static uint64_t g_size;

/* ── Helpers ──────────────────────────────────── */

static int v_strlen(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int v_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void v_strcpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void v_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (uint32_t i = 0; i < n; i++)
        d[i] = s[i];
}

static void v_memset(void *dst, uint8_t val, uint32_t n)
{
    uint8_t *d = dst;
    for (uint32_t i = 0; i < n; i++)
        d[i] = val;
}

static uint64_t read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ── Block management ─────────────────────────── */

static uint8_t *block_ptr(uint32_t block_num)
{
    return g_base + (uint64_t)block_num * VAULT_BLOCK_SIZE;
}

static int bitmap_test(uint32_t block)
{
    if (!g_vault.bitmap) return 0;
    return (g_vault.bitmap[block / 8] >> (block % 8)) & 1;
}

static void bitmap_set(uint32_t block)
{
    if (!g_vault.bitmap) return;
    g_vault.bitmap[block / 8] |= (1 << (block % 8));
}

static void bitmap_clear(uint32_t block)
{
    if (!g_vault.bitmap) return;
    g_vault.bitmap[block / 8] &= ~(1 << (block % 8));
}

static int alloc_block(void)
{
    for (uint32_t i = g_vault.super.data_start; i < g_vault.super.total_blocks; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            g_vault.super.free_blocks--;
            v_memset(block_ptr(i), 0, VAULT_BLOCK_SIZE);
            return (int)i;
        }
    }
    return -1;  /* No free blocks */
}

/* ── Inode management ─────────────────────────── */

static struct vault_inode *inode_ptr(uint32_t ino)
{
    if (ino >= g_vault.super.inode_count) return 0;
    return &g_vault.inodes[ino];
}

static int alloc_inode(void)
{
    for (uint32_t i = 1; i < g_vault.super.inode_count; i++) {
        if (g_vault.inodes[i].type == VAULT_TYPE_FREE) {
            g_vault.super.free_inodes--;
            return (int)i;
        }
    }
    return -1;  /* No free inodes */
}

/* ── Path resolution ──────────────────────────── */

/*
 * Walk a path from root, resolving each component.
 * Returns the inode number of the final component, or -1.
 * If create_dirs is true, creates intermediate directories.
 */
static int resolve_path(const char *path, int create_dirs, int *parent_ino)
{
    if (!path || *path == '\0')
        return -1;

    /* Skip leading / */
    if (*path == '/') path++;
    if (*path == '\0') {
        if (parent_ino) *parent_ino = -1;
        return (int)g_vault.super.root_inode;
    }

    uint32_t current = g_vault.super.root_inode;

    while (*path) {
        /* Extract next path component */
        char component[VAULT_MAX_NAME];
        int ci = 0;
        while (*path && *path != '/' && ci < VAULT_MAX_NAME - 1)
            component[ci++] = *path++;
        component[ci] = '\0';

        if (*path == '/') path++;

        /* Is this the last component? */
        int is_last = (*path == '\0');

        /* Look up component in current directory */
        struct vault_inode *dir = inode_ptr(current);
        if (!dir || dir->type != VAULT_TYPE_DIR)
            return -1;

        int found = -1;

        /* Search directory entries in data blocks */
        for (int b = 0; b < VAULT_DIRECT_BLOCKS && b < (int)dir->blocks; b++) {
            if (dir->direct[b] == 0) continue;
            struct vault_dirent *entries = (struct vault_dirent *)block_ptr(dir->direct[b]);
            int entries_per_block = VAULT_BLOCK_SIZE / sizeof(struct vault_dirent);
            for (int e = 0; e < entries_per_block; e++) {
                if (entries[e].inode != 0 && v_streq(entries[e].name, component)) {
                    found = (int)entries[e].inode;
                    break;
                }
            }
            if (found >= 0) break;
        }

        if (found >= 0) {
            if (is_last && parent_ino)
                *parent_ino = (int)current;
            current = (uint32_t)found;
        } else if (!is_last && create_dirs) {
            /* Create intermediate directory */
            int new_ino = alloc_inode();
            if (new_ino < 0) return -1;

            struct vault_inode *new_dir = inode_ptr((uint32_t)new_ino);
            new_dir->type = VAULT_TYPE_DIR;
            new_dir->tier = VAULT_TIER_INTERNAL;
            new_dir->size = 0;
            new_dir->blocks = 1;
            new_dir->created_tsc = read_tsc();
            new_dir->modified_tsc = new_dir->created_tsc;
            new_dir->version = 1;

            int blk = alloc_block();
            if (blk < 0) return -1;
            new_dir->direct[0] = (uint32_t)blk;

            /* Add entry to parent */
            struct vault_inode *pdir = inode_ptr(current);
            for (int b = 0; b < VAULT_DIRECT_BLOCKS; b++) {
                if (pdir->direct[b] == 0) {
                    blk = alloc_block();
                    if (blk < 0) return -1;
                    pdir->direct[b] = (uint32_t)blk;
                    if ((uint32_t)(b + 1) > pdir->blocks)
                        pdir->blocks = (uint32_t)(b + 1);
                }
                struct vault_dirent *entries = (struct vault_dirent *)block_ptr(pdir->direct[b]);
                int entries_per_block = VAULT_BLOCK_SIZE / sizeof(struct vault_dirent);
                for (int e = 0; e < entries_per_block; e++) {
                    if (entries[e].inode == 0) {
                        entries[e].inode = (uint32_t)new_ino;
                        v_strcpy(entries[e].name, component, VAULT_MAX_NAME);
                        goto dir_added;
                    }
                }
            }
            return -1;  /* No space in parent directory */

dir_added:
            current = (uint32_t)new_ino;
        } else if (is_last) {
            /* Not found and this is the final component */
            if (parent_ino) *parent_ino = (int)current;
            return -1;
        } else {
            return -1;  /* Not found and not creating */
        }
    }

    return (int)current;
}

/* Add a directory entry to a directory inode */
static int dir_add_entry(uint32_t dir_ino, const char *name, uint32_t child_ino)
{
    struct vault_inode *dir = inode_ptr(dir_ino);
    if (!dir || dir->type != VAULT_TYPE_DIR)
        return -1;

    for (int b = 0; b < VAULT_DIRECT_BLOCKS; b++) {
        if (dir->direct[b] == 0) {
            int blk = alloc_block();
            if (blk < 0) return -1;
            dir->direct[b] = (uint32_t)blk;
            if ((uint32_t)(b + 1) > dir->blocks)
                dir->blocks = (uint32_t)(b + 1);
        }
        struct vault_dirent *entries = (struct vault_dirent *)block_ptr(dir->direct[b]);
        int entries_per_block = VAULT_BLOCK_SIZE / sizeof(struct vault_dirent);
        for (int e = 0; e < entries_per_block; e++) {
            if (entries[e].inode == 0) {
                entries[e].inode = child_ino;
                v_strcpy(entries[e].name, name, VAULT_MAX_NAME);
                dir->modified_tsc = read_tsc();
                return 0;
            }
        }
    }
    return -1;  /* Directory full */
}

/* Extract filename from path */
static const char *path_filename(const char *path)
{
    const char *last = path;
    while (*path) {
        if (*path == '/') last = path + 1;
        path++;
    }
    return last;
}

/* ── Public API ───────────────────────────────── */

int vault_format(void *base, uint64_t size, const char *label)
{
    if (size < VAULT_BLOCK_SIZE * 64)
        return -1;  /* Too small */

    g_base = (uint8_t *)base;
    g_size = size;

    uint32_t total_blocks = (uint32_t)(size / VAULT_BLOCK_SIZE);

    /* Layout:
     * Block 0:       Superblock
     * Block 1-N:     Bitmap (1 bit per block)
     * Block N+1-M:   Inode table
     * Block M+1-end: Data blocks
     */
    uint32_t bitmap_blocks = (total_blocks + VAULT_BLOCK_SIZE * 8 - 1) / (VAULT_BLOCK_SIZE * 8);
    uint32_t inode_blocks = (VAULT_INODE_COUNT * sizeof(struct vault_inode) + VAULT_BLOCK_SIZE - 1) / VAULT_BLOCK_SIZE;
    uint32_t data_start = 1 + bitmap_blocks + inode_blocks;

    /* Clear everything */
    v_memset(base, 0, size);

    /* Write superblock */
    struct vault_super *sb = (struct vault_super *)base;
    sb->magic = VAULT_MAGIC;
    sb->version = VAULT_VERSION;
    sb->block_size = VAULT_BLOCK_SIZE;
    sb->total_blocks = total_blocks;
    sb->free_blocks = total_blocks - data_start - 1; /* -1 for root dir block */
    sb->inode_count = VAULT_INODE_COUNT;
    sb->free_inodes = VAULT_INODE_COUNT - 2; /* -1 for inode 0 (unused), -1 for root */
    sb->bitmap_start = 1;
    sb->inode_start = 1 + bitmap_blocks;
    sb->data_start = data_start;
    sb->root_inode = 1;
    sb->created_tsc = read_tsc();
    sb->mount_count = 0;
    if (label)
        v_strcpy((char *)sb->label, label, 32);

    /* Mark system blocks as used in bitmap */
    uint8_t *bitmap = (uint8_t *)block_ptr(1);
    for (uint32_t i = 0; i < data_start; i++)
        bitmap[i / 8] |= (1 << (i % 8));

    /* Create root directory inode */
    struct vault_inode *inodes = (struct vault_inode *)block_ptr(sb->inode_start);
    struct vault_inode *root = &inodes[1];
    root->type = VAULT_TYPE_DIR;
    root->tier = VAULT_TIER_REFERENCE;
    root->size = 0;
    root->blocks = 1;
    root->created_tsc = read_tsc();
    root->modified_tsc = root->created_tsc;
    root->version = 1;

    /* Allocate one data block for root directory */
    uint32_t root_block = data_start;
    root->direct[0] = root_block;
    bitmap[root_block / 8] |= (1 << (root_block % 8));

    return 0;
}

int vault_mount(void *base, uint64_t size)
{
    g_base = (uint8_t *)base;
    g_size = size;

    /* Read superblock */
    struct vault_super *sb = (struct vault_super *)base;
    if (sb->magic != VAULT_MAGIC) {
        kputs("VAULT: bad magic\n");
        return -1;
    }
    if (sb->version != VAULT_VERSION) {
        kputs("VAULT: unsupported version\n");
        return -1;
    }

    g_vault.super = *sb;
    g_vault.bitmap = (uint8_t *)block_ptr(sb->bitmap_start);
    g_vault.inodes = (struct vault_inode *)block_ptr(sb->inode_start);
    g_vault.mounted = 1;
    g_vault.device_blocks = sb->total_blocks;

    sb->mount_count++;

    return 0;
}

int vault_create(const char *path, uint32_t tier)
{
    if (!g_vault.mounted) return -1;

    int parent_ino = -1;
    int existing = resolve_path(path, 1, &parent_ino);

    if (existing >= 0)
        return existing;  /* Already exists */

    if (parent_ino < 0)
        return -1;

    /* Allocate inode */
    int ino = alloc_inode();
    if (ino < 0) return -1;

    struct vault_inode *node = inode_ptr((uint32_t)ino);
    node->type = VAULT_TYPE_FILE;
    node->tier = tier;
    node->size = 0;
    node->blocks = 0;
    node->created_tsc = read_tsc();
    node->modified_tsc = node->created_tsc;
    node->version = 1;
    node->prev_version = 0;

    /* Add to parent directory */
    const char *name = path_filename(path);
    if (dir_add_entry((uint32_t)parent_ino, name, (uint32_t)ino) < 0)
        return -1;

    return ino;
}

int vault_write(const char *path, const void *data, uint32_t size)
{
    if (!g_vault.mounted) return -1;

    int ino = resolve_path(path, 0, 0);
    if (ino < 0) {
        /* File doesn't exist — create it */
        ino = vault_create(path, VAULT_TIER_INTERNAL);
        if (ino < 0) return -1;
    }

    struct vault_inode *node = inode_ptr((uint32_t)ino);
    if (!node || node->type != VAULT_TYPE_FILE)
        return -1;

    /* If file already has data, create a new version */
    if (node->size > 0 && node->blocks > 0) {
        /* Save previous version reference */
        int new_ino = alloc_inode();
        if (new_ino >= 0) {
            /* Copy current inode to new slot as "previous version" */
            struct vault_inode *prev = inode_ptr((uint32_t)new_ino);
            *prev = *node;

            /* Current inode now links to previous */
            node->prev_version = (uint32_t)new_ino;
            node->version++;
        }
        /* If we can't alloc a version inode, just overwrite (still works, loses history) */
    }

    /* Write data to blocks */
    uint32_t blocks_needed = (size + VAULT_BLOCK_SIZE - 1) / VAULT_BLOCK_SIZE;
    uint32_t written = 0;

    for (uint32_t b = 0; b < blocks_needed && b < VAULT_DIRECT_BLOCKS; b++) {
        if (node->direct[b] == 0) {
            int blk = alloc_block();
            if (blk < 0) break;
            node->direct[b] = (uint32_t)blk;
        }

        uint32_t chunk = size - written;
        if (chunk > VAULT_BLOCK_SIZE) chunk = VAULT_BLOCK_SIZE;
        v_memcpy(block_ptr(node->direct[b]), (const uint8_t *)data + written, chunk);
        written += chunk;
    }

    node->size = written;
    node->blocks = blocks_needed;
    node->modified_tsc = read_tsc();

    return (int)written;
}

int vault_read(const char *path, void *buf, uint32_t size)
{
    if (!g_vault.mounted) return -1;

    int ino = resolve_path(path, 0, 0);
    if (ino < 0) return -1;

    struct vault_inode *node = inode_ptr((uint32_t)ino);
    if (!node || node->type != VAULT_TYPE_FILE)
        return -1;

    uint32_t to_read = node->size;
    if (to_read > size) to_read = size;

    uint32_t read_total = 0;
    for (uint32_t b = 0; b < node->blocks && b < VAULT_DIRECT_BLOCKS && read_total < to_read; b++) {
        if (node->direct[b] == 0) break;
        uint32_t chunk = to_read - read_total;
        if (chunk > VAULT_BLOCK_SIZE) chunk = VAULT_BLOCK_SIZE;
        v_memcpy((uint8_t *)buf + read_total, block_ptr(node->direct[b]), chunk);
        read_total += chunk;
    }

    return (int)read_total;
}

int vault_read_version(const char *path, void *buf, uint32_t size, uint32_t version)
{
    if (!g_vault.mounted) return -1;
    if (version == 0)
        return vault_read(path, buf, size);

    int ino = resolve_path(path, 0, 0);
    if (ino < 0) return -1;

    /* Walk back through version chain */
    struct vault_inode *node = inode_ptr((uint32_t)ino);
    for (uint32_t v = 0; v < version; v++) {
        if (!node || node->prev_version == 0)
            return -1;  /* Not enough history */
        node = inode_ptr(node->prev_version);
    }

    if (!node) return -1;

    uint32_t to_read = node->size;
    if (to_read > size) to_read = size;

    uint32_t read_total = 0;
    for (uint32_t b = 0; b < node->blocks && b < VAULT_DIRECT_BLOCKS && read_total < to_read; b++) {
        if (node->direct[b] == 0) break;
        uint32_t chunk = to_read - read_total;
        if (chunk > VAULT_BLOCK_SIZE) chunk = VAULT_BLOCK_SIZE;
        v_memcpy((uint8_t *)buf + read_total, block_ptr(node->direct[b]), chunk);
        read_total += chunk;
    }

    return (int)read_total;
}

int vault_list(const char *path, struct vault_dirent *entries, int max_entries)
{
    if (!g_vault.mounted) return -1;

    int ino = resolve_path(path, 0, 0);
    if (ino < 0) return -1;

    struct vault_inode *dir = inode_ptr((uint32_t)ino);
    if (!dir || dir->type != VAULT_TYPE_DIR)
        return -1;

    int count = 0;
    for (int b = 0; b < VAULT_DIRECT_BLOCKS && b < (int)dir->blocks; b++) {
        if (dir->direct[b] == 0) continue;
        struct vault_dirent *dentries = (struct vault_dirent *)block_ptr(dir->direct[b]);
        int entries_per_block = VAULT_BLOCK_SIZE / sizeof(struct vault_dirent);
        for (int e = 0; e < entries_per_block && count < max_entries; e++) {
            if (dentries[e].inode != 0) {
                entries[count] = dentries[e];
                count++;
            }
        }
    }

    return count;
}

int vault_append(const char *path, const void *data, uint32_t size)
{
    if (!g_vault.mounted) return -1;

    int ino = resolve_path(path, 0, 0);
    if (ino < 0) {
        ino = vault_create(path, VAULT_TIER_INTERNAL);
        if (ino < 0) return -1;
    }

    struct vault_inode *node = inode_ptr((uint32_t)ino);
    if (!node || node->type != VAULT_TYPE_FILE)
        return -1;

    /* Find the last block and offset within it */
    uint32_t offset_in_block = node->size % VAULT_BLOCK_SIZE;
    uint32_t block_idx = node->size / VAULT_BLOCK_SIZE;

    uint32_t written = 0;
    while (written < size && block_idx < VAULT_DIRECT_BLOCKS) {
        if (node->direct[block_idx] == 0) {
            int blk = alloc_block();
            if (blk < 0) break;
            node->direct[block_idx] = (uint32_t)blk;
            node->blocks = block_idx + 1;
        }

        uint32_t space = VAULT_BLOCK_SIZE - offset_in_block;
        uint32_t chunk = size - written;
        if (chunk > space) chunk = space;

        v_memcpy(block_ptr(node->direct[block_idx]) + offset_in_block,
                 (const uint8_t *)data + written, chunk);

        written += chunk;
        offset_in_block = 0;  /* Next block starts at 0 */
        block_idx++;
    }

    node->size += written;
    node->modified_tsc = read_tsc();
    return (int)written;
}

int vault_delete(const char *path)
{
    if (!g_vault.mounted) return -1;

    int parent_ino = -1;
    int ino = resolve_path(path, 0, &parent_ino);
    if (ino < 0) return -1;

    /* Mark inode as free (but data blocks persist for temporal history) */
    struct vault_inode *node = inode_ptr((uint32_t)ino);
    if (!node) return -1;

    node->type = VAULT_TYPE_FREE;
    g_vault.super.free_inodes++;

    /* Remove directory entry from parent */
    if (parent_ino >= 0) {
        struct vault_inode *parent = inode_ptr((uint32_t)parent_ino);
        if (parent) {
            const char *name = path_filename(path);
            for (int b = 0; b < VAULT_DIRECT_BLOCKS && b < (int)parent->blocks; b++) {
                if (parent->direct[b] == 0) continue;
                struct vault_dirent *entries = (struct vault_dirent *)block_ptr(parent->direct[b]);
                int epb = VAULT_BLOCK_SIZE / sizeof(struct vault_dirent);
                for (int e = 0; e < epb; e++) {
                    if (entries[e].inode == (uint32_t)ino && v_streq(entries[e].name, name)) {
                        entries[e].inode = 0;
                        entries[e].name[0] = '\0';
                        return 0;
                    }
                }
            }
        }
    }

    return 0;
}

int vault_size(const char *path)
{
    if (!g_vault.mounted) return -1;
    int ino = resolve_path(path, 0, 0);
    if (ino < 0) return -1;
    struct vault_inode *node = inode_ptr((uint32_t)ino);
    if (!node) return -1;
    return (int)node->size;
}

int vault_exists(const char *path)
{
    if (!g_vault.mounted) return 0;
    return resolve_path(path, 0, 0) >= 0;
}

void vault_stat(uint32_t *total_blocks, uint32_t *free_blocks,
                uint32_t *total_inodes, uint32_t *free_inodes)
{
    if (total_blocks) *total_blocks = g_vault.super.total_blocks;
    if (free_blocks) *free_blocks = g_vault.super.free_blocks;
    if (total_inodes) *total_inodes = g_vault.super.inode_count;
    if (free_inodes) *free_inodes = g_vault.super.free_inodes;
}

void vault_sync(void)
{
    /* In RAM-backed mode, superblock is already in memory.
     * When NVMe driver exists, this flushes to disk. */
    if (g_vault.mounted) {
        v_memcpy(g_base, &g_vault.super, sizeof(struct vault_super));
    }
}
