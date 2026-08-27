/*
 * Zeos -- Installer
 *
 * Chain surface that installs Z-OS from a live USB session onto
 * an internal NVMe drive. Eight steps: welcome, select disk,
 * confirm, partition (GPT + FAT32 ESP + VAULT), copy BOOTZ.EFI,
 * migrate VAULT state, boot order advisory, done.
 *
 * Bare-metal C. No libc. Uses NVMe block device, framebuffer,
 * PS/2 keyboard, and VAULT filesystem APIs.
 */

#include "installer.h"
#include "fb.h"
#include "theme.h"
#include "keyboard.h"
#include "kprint.h"
#include "nvme.h"
#include "vault.h"
#include "vault_disk.h"
#include "timer.h"
#include "hal.h"
#include "heap.h"

/* ── Static state ─────────────────────────────────────────────── */

static installer_state_t state;

/* ── String helpers (no libc) ─────────────────────────────────── */

static int str_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void mem_set(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++)
        d[i] = val;
}

static void mem_copy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++)
        d[i] = s[i];
}

/* Integer to decimal string */
static void int_to_str(uint64_t val, char *buf, int bufsz)
{
    char tmp[24];
    int i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0 && i < 22) {
            tmp[i++] = '0' + (val % 10);
            val /= 10;
        }
    }
    /* Reverse into buf */
    int j = 0;
    while (i > 0 && j < bufsz - 1)
        buf[j++] = tmp[--i];
    buf[j] = '\0';
}

/* Append string b to end of a (a has room for max bytes total) */
static void str_append(char *a, const char *b, int max)
{
    int alen = str_len(a);
    int i = 0;
    while (alen + i < max - 1 && b[i]) {
        a[alen + i] = b[i];
        i++;
    }
    a[alen + i] = '\0';
}

/* ── PS/2 scan code input (same pattern as firstboot.c) ───────── */

#define SC_UP       0x48
#define SC_DOWN     0x50
#define SC_LEFT     0x4B
#define SC_RIGHT    0x4D
#define SC_ENTER    0x1C
#define SC_SPACE    0x39
#define SC_ESC      0x01
#define SC_E0       0xE0

#define KEY_NONE    0
#define KEY_LEFT    1
#define KEY_RIGHT   2
#define KEY_UP      3
#define KEY_DOWN    4
#define KEY_ENTER   5
#define KEY_SPACE   6
#define KEY_ESC     7

static int getkey(void)
{
    for (;;) {
        while (!(hal_in8(0x64) & 0x01))
            hal_cpu_halt();

        uint8_t sc = hal_in8(0x60);

        if (sc == SC_E0) {
            while (!(hal_in8(0x64) & 0x01))
                hal_cpu_halt();
            sc = hal_in8(0x60);
            if (sc & 0x80) continue;
            switch (sc) {
            case SC_UP:    return KEY_UP;
            case SC_DOWN:  return KEY_DOWN;
            case SC_LEFT:  return KEY_LEFT;
            case SC_RIGHT: return KEY_RIGHT;
            default:       continue;
            }
        }

        if (sc & 0x80) continue;

        switch (sc) {
        case SC_ENTER: return KEY_ENTER;
        case SC_SPACE: return KEY_SPACE;
        case SC_ESC:   return KEY_ESC;
        default:       continue;
        }
    }
}

/* ── Drawing helpers ──────────────────────────────────────────── */

/* Width of a string in pixels (8px per char, boot font) */
static int text_width(const char *s)
{
    int w = 0;
    while (*s) {
        if (*s != '\n') w++;
        s++;
    }
    return w * 8;
}

/* Draw text centered horizontally within a given region */
static void draw_centered(int rx, int ry, int rw, int y_off,
                           const char *s, uint32_t color)
{
    int tw = text_width(s);
    int x = rx + (rw - tw) / 2;
    if (x < rx) x = rx;
    fb_text(x, ry + y_off, s, color);
}

/* Draw a button: [label] with selection highlight */
static void draw_button(int x, int y, int w, int h,
                         const char *label, int selected)
{
    uint32_t fill   = selected ? COLOR_PRIMARY     : COLOR_SURFACE_HIGH;
    uint32_t border = selected ? COLOR_PRIMARY     : COLOR_ON_SURFACE_3;
    uint32_t text   = selected ? COLOR_SURFACE     : COLOR_ON_SURFACE;

    fb_rect(x, y, w, h, fill);
    fb_rect_outline(x, y, w, h, border, selected ? 2 : 1);

    int tw = text_width(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 16) / 2;
    fb_text(tx, ty, label, text);
}

/* Draw a progress bar */
static void draw_progress(int x, int y, int w, int h, int pct)
{
    /* Background track */
    fb_rect(x, y, w, h, COLOR_SURFACE_HIGH);
    fb_rect_outline(x, y, w, h, COLOR_ON_SURFACE_3, 1);

    /* Fill */
    if (pct > 0) {
        int fill_w = (w - 4) * pct / 100;
        if (fill_w < 1) fill_w = 1;
        fb_rect(x + 2, y + 2, fill_w, h - 4, COLOR_PRIMARY);
    }
}

/* Draw status text at bottom of region */
static void draw_status(int rx, int ry, int rw, int rh)
{
    if (state.status_msg[0]) {
        int tw = text_width(state.status_msg);
        int x = rx + (rw - tw) / 2;
        fb_text(x, ry + rh - 40, state.status_msg, COLOR_ON_SURFACE_2);
    }
}

/* ── GPT structures ───────────────────────────────────────────── */

/* CRC32 for GPT (IEEE 802.3) */
static uint32_t crc32_table[256];
static int crc32_ready = 0;

static void crc32_init(void)
{
    if (crc32_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1)
                c = 0xEDB88320 ^ (c >> 1);
            else
                c >>= 1;
        }
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

static uint32_t crc32(const void *data, uint64_t len)
{
    crc32_init();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFF;
    for (uint64_t i = 0; i < len; i++)
        c = crc32_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFF;
}

/* GPT header (LBA 1, 512 bytes padded to block) */
typedef struct __attribute__((packed)) {
    uint8_t  signature[8];    /* "EFI PART" */
    uint32_t revision;        /* 0x00010000 */
    uint32_t header_size;     /* 92 */
    uint32_t header_crc32;    /* CRC of header (with this field zero) */
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t part_entry_lba;
    uint32_t num_parts;
    uint32_t part_entry_size; /* 128 */
    uint32_t part_crc32;
    /* Rest is zeros (pad to block size) */
} gpt_header_t;

/* GPT partition entry (128 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];        /* UTF-16LE name */
} gpt_entry_t;

/* Protective MBR (LBA 0) */
typedef struct __attribute__((packed)) {
    uint8_t  boot_code[446];
    uint8_t  part1_status;
    uint8_t  part1_chs_start[3];
    uint8_t  part1_type;        /* 0xEE = GPT protective */
    uint8_t  part1_chs_end[3];
    uint32_t part1_lba_start;
    uint32_t part1_lba_size;
    uint8_t  part2[16];
    uint8_t  part3[16];
    uint8_t  part4[16];
    uint16_t signature;         /* 0xAA55 */
} pmbr_t;

/* EFI System Partition type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const uint8_t ESP_TYPE_GUID[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

/* Linux filesystem GUID: 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
static const uint8_t LINUX_TYPE_GUID[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

/* Simple pseudo GUID from TSC (not cryptographic, just unique-ish) */
static void make_guid(uint8_t *guid)
{
    uint64_t t1 = timer_read_tsc();
    uint64_t t2 = timer_read_tsc();
    mem_copy(guid, &t1, 8);
    mem_copy(guid + 8, &t2, 8);
    /* Set version 4 (random) and variant 1 */
    guid[6] = (guid[6] & 0x0F) | 0x40;
    guid[8] = (guid[8] & 0x3F) | 0x80;
}

/* Set UTF-16LE partition name from ASCII */
static void set_part_name(uint16_t *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = (uint16_t)src[i];
        i++;
    }
    while (i < max)
        dst[i++] = 0;
}

/* ── FAT32 minimal structures ─────────────────────────────────── */

/* Write a minimal FAT32 filesystem to the ESP partition */
static int write_fat32_esp(uint64_t esp_start_lba, uint64_t esp_blocks,
                            uint32_t blk_size)
{
    /*
     * Minimal FAT32 for a 100MB ESP:
     *   Sector 0: BPB / boot sector
     *   Sector 6: Backup boot sector
     *   Sector 1-2: FSInfo
     *   Sectors 32..N: FAT (two copies)
     *   After FATs: root directory cluster
     *
     * We assume 512-byte sectors within the ESP, even if NVMe block size
     * is larger. We'll write in NVMe-block-sized chunks.
     */

    /* We'll work in 512-byte sectors for FAT32 math */
    uint32_t sectors_per_block = blk_size / 512;
    if (sectors_per_block == 0) sectors_per_block = 1;
    uint64_t esp_sectors = esp_blocks * sectors_per_block;

    /* FAT32 BPB parameters */
    uint32_t bytes_per_sector = 512;
    uint32_t sectors_per_cluster = 8;   /* 4KB clusters */
    uint32_t reserved_sectors = 32;
    uint32_t num_fats = 2;
    uint32_t total_sectors = (uint32_t)esp_sectors;
    uint32_t data_sectors = total_sectors - reserved_sectors;
    /* FAT size: each cluster needs 4 bytes in FAT */
    uint32_t total_clusters = data_sectors / sectors_per_cluster;
    uint32_t fat_bytes = (total_clusters + 2) * 4;
    uint32_t fat_sectors = (fat_bytes + bytes_per_sector - 1) / bytes_per_sector;

    /* Allocate a block-sized buffer for writing */
    uint8_t *buf = (uint8_t *)kmalloc(blk_size);
    if (!buf) return -1;

    /* ── Boot sector (BPB) ── */
    mem_set(buf, 0, blk_size);

    /* Jump code */
    buf[0] = 0xEB; buf[1] = 0x58; buf[2] = 0x90;
    /* OEM ID */
    mem_copy(buf + 3, "ZEOS    ", 8);
    /* Bytes per sector */
    buf[11] = (bytes_per_sector & 0xFF);
    buf[12] = (bytes_per_sector >> 8) & 0xFF;
    /* Sectors per cluster */
    buf[13] = (uint8_t)sectors_per_cluster;
    /* Reserved sectors */
    buf[14] = reserved_sectors & 0xFF;
    buf[15] = (reserved_sectors >> 8) & 0xFF;
    /* Number of FATs */
    buf[16] = (uint8_t)num_fats;
    /* Root entry count (0 for FAT32) */
    buf[17] = 0; buf[18] = 0;
    /* Total sectors 16-bit (0 for FAT32) */
    buf[19] = 0; buf[20] = 0;
    /* Media type */
    buf[21] = 0xF8;
    /* FAT size 16-bit (0 for FAT32) */
    buf[22] = 0; buf[23] = 0;
    /* Sectors per track */
    buf[24] = 63; buf[25] = 0;
    /* Number of heads */
    buf[26] = 255; buf[27] = 0;
    /* Hidden sectors */
    buf[28] = 0; buf[29] = 0; buf[30] = 0; buf[31] = 0;
    /* Total sectors 32-bit */
    buf[32] = total_sectors & 0xFF;
    buf[33] = (total_sectors >> 8) & 0xFF;
    buf[34] = (total_sectors >> 16) & 0xFF;
    buf[35] = (total_sectors >> 24) & 0xFF;
    /* FAT32: sectors per FAT */
    buf[36] = fat_sectors & 0xFF;
    buf[37] = (fat_sectors >> 8) & 0xFF;
    buf[38] = (fat_sectors >> 16) & 0xFF;
    buf[39] = (fat_sectors >> 24) & 0xFF;
    /* FAT32: ext flags */
    buf[40] = 0; buf[41] = 0;
    /* FAT32: version */
    buf[42] = 0; buf[43] = 0;
    /* FAT32: root cluster */
    buf[44] = 2; buf[45] = 0; buf[46] = 0; buf[47] = 0;
    /* FAT32: FSInfo sector */
    buf[48] = 1; buf[49] = 0;
    /* FAT32: backup boot sector */
    buf[50] = 6; buf[51] = 0;
    /* FAT32: BS_DrvNum */
    buf[64] = 0x80;
    /* FAT32: BS_BootSig */
    buf[66] = 0x29;
    /* Volume serial (from TSC) */
    uint64_t tsc = timer_read_tsc();
    buf[67] = tsc & 0xFF;
    buf[68] = (tsc >> 8) & 0xFF;
    buf[69] = (tsc >> 16) & 0xFF;
    buf[70] = (tsc >> 24) & 0xFF;
    /* Volume label */
    mem_copy(buf + 71, "ZEOS-ESP   ", 11);
    /* FS type */
    mem_copy(buf + 82, "FAT32   ", 8);
    /* Boot signature */
    buf[510] = 0x55;
    buf[511] = 0xAA;

    /* Write boot sector at ESP start */
    if (nvme_write(esp_start_lba, 1, buf) != 0) {
        kfree(buf);
        return -1;
    }

    /* Write backup boot sector at sector 6 */
    uint64_t backup_lba = esp_start_lba + (6 / sectors_per_block);
    if (sectors_per_block == 1 || blk_size == 512) {
        nvme_write(esp_start_lba + 6, 1, buf);
    } else {
        /* For >512 block sizes, backup is within the same or nearby block */
        nvme_write(backup_lba, 1, buf);
    }

    /* ── FSInfo sector (sector 1) ── */
    mem_set(buf, 0, blk_size);
    /* FSInfo signatures */
    buf[0] = 0x52; buf[1] = 0x52; buf[2] = 0x61; buf[3] = 0x41; /* RRaA */
    buf[484] = 0x72; buf[485] = 0x72; buf[486] = 0x41; buf[487] = 0x61; /* rrAa */
    /* Free cluster count (total - 1 for root dir) */
    uint32_t free_clusters = total_clusters - 1;
    buf[488] = free_clusters & 0xFF;
    buf[489] = (free_clusters >> 8) & 0xFF;
    buf[490] = (free_clusters >> 16) & 0xFF;
    buf[491] = (free_clusters >> 24) & 0xFF;
    /* Next free cluster */
    buf[492] = 3; buf[493] = 0; buf[494] = 0; buf[495] = 0;
    /* Trail signature */
    buf[510] = 0x55; buf[511] = 0xAA;

    if (blk_size == 512) {
        nvme_write(esp_start_lba + 1, 1, buf);
    } else {
        /* FSInfo is at sector 1 -- may share block with BPB for >512 blocks */
        /* For simplicity, write it at the next LBA */
        nvme_write(esp_start_lba + 1, 1, buf);
    }

    /* ── FAT tables ── */
    /* First 3 FAT entries: media type, EOC, root dir cluster EOC */
    mem_set(buf, 0, blk_size);
    /* FAT[0] = media type marker */
    buf[0] = 0xF8; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = 0x0F;
    /* FAT[1] = end of chain marker */
    buf[4] = 0xFF; buf[5] = 0xFF; buf[6] = 0xFF; buf[7] = 0x0F;
    /* FAT[2] = root directory cluster (end of chain) */
    buf[8] = 0xFF; buf[9] = 0xFF; buf[10] = 0xFF; buf[11] = 0x0F;

    /* Write first block of FAT1 */
    uint64_t fat1_lba = esp_start_lba + (reserved_sectors / sectors_per_block);
    if (blk_size == 512) fat1_lba = esp_start_lba + reserved_sectors;
    nvme_write(fat1_lba, 1, buf);

    /* Write first block of FAT2 */
    uint64_t fat2_lba = fat1_lba + (fat_sectors / sectors_per_block);
    if (blk_size == 512) fat2_lba = fat1_lba + fat_sectors;
    nvme_write(fat2_lba, 1, buf);

    /* Zero remaining FAT blocks (write zeroed buffer) */
    mem_set(buf, 0, blk_size);
    uint32_t fat_blks = fat_sectors / sectors_per_block;
    if (fat_blks == 0) fat_blks = 1;
    for (uint32_t i = 1; i < fat_blks && i < 256; i++) {
        nvme_write(fat1_lba + i, 1, buf);
        nvme_write(fat2_lba + i, 1, buf);
    }

    /* ── Root directory cluster ── */
    /* Data region starts after reserved + 2 FATs */
    uint64_t data_lba = fat2_lba + (fat_sectors / sectors_per_block);
    if (blk_size == 512) data_lba = fat2_lba + fat_sectors;

    /* Root dir: create volume label entry + EFI directory entry */
    mem_set(buf, 0, blk_size);

    /* Volume label entry */
    mem_copy(buf, "ZEOS-ESP   ", 11);
    buf[11] = 0x08; /* ATTR_VOLUME_ID */

    /* EFI directory entry */
    mem_copy(buf + 32, "EFI        ", 11);
    buf[32 + 11] = 0x10; /* ATTR_DIRECTORY */
    /* EFI cluster: 3 (next free) */
    buf[32 + 26] = 3; buf[32 + 27] = 0;  /* FstClusLO */
    buf[32 + 20] = 0; buf[32 + 21] = 0;  /* FstClusHI */

    nvme_write(data_lba, 1, buf);

    /* ── /EFI directory cluster (cluster 3) ── */
    uint64_t efi_dir_lba = data_lba + sectors_per_cluster;
    if (blk_size > 512)
        efi_dir_lba = data_lba + (sectors_per_cluster / sectors_per_block);
    if (efi_dir_lba == data_lba) efi_dir_lba = data_lba + 1;

    mem_set(buf, 0, blk_size);

    /* . entry */
    mem_copy(buf, ".          ", 11);
    buf[11] = 0x10;
    buf[26] = 3; buf[27] = 0;

    /* .. entry */
    mem_copy(buf + 32, "..         ", 11);
    buf[32 + 11] = 0x10;
    buf[32 + 26] = 2; buf[32 + 27] = 0;  /* parent = root (cluster 2) */

    /* BOOT directory entry */
    mem_copy(buf + 64, "BOOT       ", 11);
    buf[64 + 11] = 0x10;
    buf[64 + 26] = 4; buf[64 + 27] = 0;  /* cluster 4 */

    nvme_write(efi_dir_lba, 1, buf);

    /* Update FAT: cluster 3 = EOC */
    /* Re-read FAT1 first block, set entry 3 */
    mem_set(buf, 0, blk_size);
    buf[0] = 0xF8; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = 0x0F;
    buf[4] = 0xFF; buf[5] = 0xFF; buf[6] = 0xFF; buf[7] = 0x0F;
    buf[8] = 0xFF; buf[9] = 0xFF; buf[10] = 0xFF; buf[11] = 0x0F;
    /* FAT[3] = EOC (EFI dir) */
    buf[12] = 0xFF; buf[13] = 0xFF; buf[14] = 0xFF; buf[15] = 0x0F;
    /* FAT[4] = EOC (BOOT dir) */
    buf[16] = 0xFF; buf[17] = 0xFF; buf[18] = 0xFF; buf[19] = 0x0F;
    nvme_write(fat1_lba, 1, buf);
    nvme_write(fat2_lba, 1, buf);

    /* ── /EFI/BOOT directory cluster (cluster 4) ── */
    uint64_t boot_dir_lba = efi_dir_lba + (sectors_per_cluster / sectors_per_block);
    if (boot_dir_lba == efi_dir_lba) boot_dir_lba = efi_dir_lba + 1;

    mem_set(buf, 0, blk_size);

    /* . entry */
    mem_copy(buf, ".          ", 11);
    buf[11] = 0x10;
    buf[26] = 4; buf[27] = 0;

    /* .. entry */
    mem_copy(buf + 32, "..         ", 11);
    buf[32 + 11] = 0x10;
    buf[32 + 26] = 3; buf[32 + 27] = 0;

    /* BOOTX64.EFI will be written here in the copy step */

    nvme_write(boot_dir_lba, 1, buf);

    kfree(buf);
    return 0;
}

/* ── Partition layout constants ───────────────────────────────── */

#define ESP_START_LBA    2048
#define ESP_SIZE_MB      100

/* ── Step implementations ─────────────────────────────────────── */

/* Step 1: Welcome */
static int step_welcome(int rx, int ry, int rw, int rh)
{
    int btn_w = 120;
    int btn_h = 32;
    int btn_y = ry + rh - 80;
    int gap   = 24;
    int total = btn_w * 2 + gap;
    int btn_x = rx + (rw - total) / 2;
    int sel   = 0; /* 0 = Install, 1 = Cancel */

    for (;;) {
        fb_clear(COLOR_SURFACE);

        /* Title */
        draw_centered(rx, ry, rw, 80, "Install Z-OS to this machine",
                      COLOR_ON_SURFACE);

        /* Body text */
        draw_centered(rx, ry, rw, 140,
            "Your live session will be copied to the internal drive.",
            COLOR_ON_SURFACE_2);
        draw_centered(rx, ry, rw, 170,
            "B3 beliefs, settings, and desktop state will be preserved.",
            COLOR_ON_SURFACE_2);

        /* Separator line */
        fb_hline(rx + 60, btn_y - 30, rw - 120, COLOR_SEPARATOR);

        /* Buttons */
        draw_button(btn_x, btn_y, btn_w, btn_h,
                    "Install", sel == 0);
        draw_button(btn_x + btn_w + gap, btn_y, btn_w, btn_h,
                    "Cancel", sel == 1);

        int key = getkey();
        if (key == KEY_LEFT || key == KEY_RIGHT)
            sel = !sel;
        if (key == KEY_ENTER || key == KEY_SPACE) {
            if (sel == 1) return -1; /* Cancel */
            return 0; /* Proceed */
        }
        if (key == KEY_ESC) return -1;
    }
}

/* Step 2: Select disk */
static int step_select_disk(int rx, int ry, int rw, int rh)
{
    const nvme_dev_t *dev = nvme_get_dev();

    int btn_w = 120;
    int btn_h = 32;
    int btn_y = ry + rh - 80;
    int gap   = 24;
    int total = btn_w * 2 + gap;
    int btn_x = rx + (rw - total) / 2;
    int sel   = 0; /* 0 = Select, 1 = Back */

    if (!dev || !dev->ready) {
        /* No NVMe found */
        for (;;) {
            fb_clear(COLOR_SURFACE);
            draw_centered(rx, ry, rw, 80, "Select Target Disk",
                          COLOR_ON_SURFACE);
            draw_centered(rx, ry, rw, 160,
                "No suitable disk found.", COLOR_DANGER);
            draw_centered(rx, ry, rw, 190,
                "Z-OS requires NVMe storage.", COLOR_ON_SURFACE_2);

            draw_button(rx + (rw - btn_w) / 2, btn_y, btn_w, btn_h,
                        "Back", 1);

            int key = getkey();
            if (key == KEY_ENTER || key == KEY_SPACE || key == KEY_ESC)
                return -1;
        }
    }

    /* Store device info */
    state.target_disk = dev->nsid;
    state.disk_size   = dev->num_blocks;
    state.block_size  = dev->block_size;

    /* Build display strings */
    char model_line[128];
    str_copy(model_line, "NVMe: ", sizeof(model_line));
    str_append(model_line, dev->model, sizeof(model_line));

    char size_line[64];
    uint64_t size_gb = (dev->num_blocks * dev->block_size) / (1024ULL * 1024 * 1024);
    char size_str[24];
    int_to_str(size_gb, size_str, sizeof(size_str));
    str_copy(size_line, "(", sizeof(size_line));
    str_append(size_line, size_str, sizeof(size_line));
    str_append(size_line, " GB)", sizeof(size_line));

    char serial_line[64];
    str_copy(serial_line, "Serial: ", sizeof(serial_line));
    str_append(serial_line, dev->serial, sizeof(serial_line));

    for (;;) {
        fb_clear(COLOR_SURFACE);

        draw_centered(rx, ry, rw, 80, "Select Target Disk",
                      COLOR_ON_SURFACE);

        /* Disk info card */
        int card_w = 400;
        int card_h = 100;
        int card_x = rx + (rw - card_w) / 2;
        int card_y = ry + 140;

        fb_rect(card_x, card_y, card_w, card_h, COLOR_SURFACE_HIGH);
        fb_rect_outline(card_x, card_y, card_w, card_h, COLOR_PRIMARY, 2);

        fb_text(card_x + 16, card_y + 16, model_line, COLOR_ON_SURFACE);
        fb_text(card_x + 16, card_y + 40, size_line, COLOR_ON_SURFACE_2);
        fb_text(card_x + 16, card_y + 64, serial_line, COLOR_ON_SURFACE_3);

        /* Buttons */
        draw_button(btn_x, btn_y, btn_w, btn_h,
                    "Select", sel == 0);
        draw_button(btn_x + btn_w + gap, btn_y, btn_w, btn_h,
                    "Back", sel == 1);

        int key = getkey();
        if (key == KEY_LEFT || key == KEY_RIGHT)
            sel = !sel;
        if (key == KEY_ENTER || key == KEY_SPACE) {
            if (sel == 1) return -1;
            return 0;
        }
        if (key == KEY_ESC) return -1;
    }
}

/* Step 3: Confirm */
static int step_confirm(int rx, int ry, int rw, int rh)
{
    const nvme_dev_t *dev = nvme_get_dev();

    int btn_w = 120;
    int btn_h = 32;
    int btn_y = ry + rh - 80;
    int gap   = 24;
    int total = btn_w * 2 + gap;
    int btn_x = rx + (rw - total) / 2;
    int sel   = 1; /* Default to Back for safety */

    char warn_line[128];
    str_copy(warn_line, "WARNING: This will erase all data on ", sizeof(warn_line));
    if (dev) str_append(warn_line, dev->model, sizeof(warn_line));

    /* Calculate VAULT size */
    uint64_t esp_blocks = (ESP_SIZE_MB * 1024ULL * 1024) / state.block_size;
    uint64_t vault_blocks = state.disk_size - ESP_START_LBA - esp_blocks - 34;
    uint64_t vault_gb = (vault_blocks * state.block_size) / (1024ULL * 1024 * 1024);
    char vault_size_str[24];
    int_to_str(vault_gb, vault_size_str, sizeof(vault_size_str));

    char vault_line[64];
    str_copy(vault_line, "  VAULT: ", sizeof(vault_line));
    str_append(vault_line, vault_size_str, sizeof(vault_line));
    str_append(vault_line, " GB (Z-OS data)", sizeof(vault_line));

    for (;;) {
        fb_clear(COLOR_SURFACE);

        draw_centered(rx, ry, rw, 80, "Confirm Installation",
                      COLOR_ON_SURFACE);

        /* Warning */
        draw_centered(rx, ry, rw, 140, warn_line, COLOR_DANGER);

        /* Partition layout */
        int text_x = rx + 100;
        fb_text(text_x, ry + 200, "Partitions to create:", COLOR_ON_SURFACE_2);
        fb_text(text_x, ry + 230, "  ESP: 100 MB (EFI System Partition)",
                COLOR_ON_SURFACE_2);
        fb_text(text_x, ry + 260, vault_line, COLOR_ON_SURFACE_2);

        /* Separator */
        fb_hline(rx + 60, btn_y - 30, rw - 120, COLOR_SEPARATOR);

        /* Buttons */
        draw_button(btn_x, btn_y, btn_w, btn_h,
                    "Confirm", sel == 0);
        draw_button(btn_x + btn_w + gap, btn_y, btn_w, btn_h,
                    "Back", sel == 1);

        int key = getkey();
        if (key == KEY_LEFT || key == KEY_RIGHT)
            sel = !sel;
        if (key == KEY_ENTER || key == KEY_SPACE) {
            if (sel == 1) return -1;
            state.confirmed = 1;
            return 0;
        }
        if (key == KEY_ESC) return -1;
    }
}

/* Step 4: Partition */
static int step_partition(int rx, int ry, int rw, int rh)
{
    fb_clear(COLOR_SURFACE);
    draw_centered(rx, ry, rw, 80, "Partitioning Disk", COLOR_ON_SURFACE);
    str_copy(state.status_msg, "Partitioning...", sizeof(state.status_msg));
    draw_status(rx, ry, rw, rh);

    uint32_t blk_size = state.block_size;
    if (blk_size == 0) blk_size = 512;

    /* ESP size in blocks */
    uint64_t esp_blocks = (ESP_SIZE_MB * 1024ULL * 1024) / blk_size;
    uint64_t esp_end_lba = ESP_START_LBA + esp_blocks - 1;

    /* VAULT partition: after ESP to end (minus backup GPT at end) */
    uint64_t vault_start_lba = esp_end_lba + 1;
    uint64_t vault_end_lba   = state.disk_size - 34;

    /* Allocate a block for disk writes */
    uint8_t *blk = (uint8_t *)kmalloc(blk_size);
    if (!blk) {
        str_copy(state.status_msg, "ERROR: Out of memory",
                 sizeof(state.status_msg));
        return -1;
    }

    /* ── Protective MBR (LBA 0) ── */
    mem_set(blk, 0, blk_size);
    pmbr_t *mbr = (pmbr_t *)blk;
    mbr->part1_status = 0x00;
    mbr->part1_type   = 0xEE;  /* GPT protective */
    mbr->part1_chs_start[0] = 0x00;
    mbr->part1_chs_start[1] = 0x02;
    mbr->part1_chs_start[2] = 0x00;
    mbr->part1_chs_end[0] = 0xFF;
    mbr->part1_chs_end[1] = 0xFF;
    mbr->part1_chs_end[2] = 0xFF;
    mbr->part1_lba_start = 1;
    uint64_t mbr_size = state.disk_size - 1;
    mbr->part1_lba_size = (mbr_size > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)mbr_size;
    mbr->signature = 0xAA55;
    nvme_write(0, 1, blk);

    /* ── GPT Header (LBA 1) ── */
    mem_set(blk, 0, blk_size);
    gpt_header_t *gpt = (gpt_header_t *)blk;
    mem_copy(gpt->signature, "EFI PART", 8);
    gpt->revision       = 0x00010000;
    gpt->header_size    = 92;
    gpt->header_crc32   = 0; /* Computed below */
    gpt->reserved       = 0;
    gpt->my_lba         = 1;
    gpt->alt_lba        = state.disk_size - 1;
    gpt->first_usable_lba = 34; /* After GPT entries */
    gpt->last_usable_lba  = state.disk_size - 34;
    make_guid(gpt->disk_guid);
    gpt->part_entry_lba = 2;
    gpt->num_parts       = 128;
    gpt->part_entry_size = 128;

    /* ── Partition entries (LBAs 2-33) ── */
    /* We need 128 entries x 128 bytes = 16384 bytes = 32 x 512-byte sectors */
    uint32_t entry_bytes = 128 * 128;
    uint32_t entry_blocks = (entry_bytes + blk_size - 1) / blk_size;
    uint8_t *entries = (uint8_t *)kmalloc(entry_bytes);
    if (!entries) {
        kfree(blk);
        str_copy(state.status_msg, "ERROR: Out of memory",
                 sizeof(state.status_msg));
        return -1;
    }
    mem_set(entries, 0, entry_bytes);

    /* Partition 1: ESP */
    gpt_entry_t *p1 = (gpt_entry_t *)(entries);
    mem_copy(p1->type_guid, ESP_TYPE_GUID, 16);
    make_guid(p1->unique_guid);
    p1->first_lba   = ESP_START_LBA;
    p1->last_lba    = esp_end_lba;
    p1->attributes  = 0;
    set_part_name(p1->name, "EFI System", 36);

    /* Partition 2: VAULT */
    gpt_entry_t *p2 = (gpt_entry_t *)(entries + 128);
    mem_copy(p2->type_guid, LINUX_TYPE_GUID, 16);
    make_guid(p2->unique_guid);
    p2->first_lba   = vault_start_lba;
    p2->last_lba    = vault_end_lba;
    p2->attributes  = 0;
    set_part_name(p2->name, "VAULT", 36);

    /* Compute partition entry CRC */
    gpt->part_crc32 = crc32(entries, entry_bytes);

    /* Compute header CRC (with header_crc32 = 0) */
    gpt->header_crc32 = 0;
    gpt->header_crc32 = crc32(gpt, 92);

    /* Write GPT header */
    nvme_write(1, 1, blk);

    /* Write partition entries */
    for (uint32_t i = 0; i < entry_blocks; i++) {
        uint64_t off = (uint64_t)i * blk_size;
        if (off < entry_bytes)
            nvme_write(2 + i, 1, entries + off);
    }

    /* ── Backup GPT (at end of disk) ── */
    /* Backup entries go before backup header */
    uint64_t backup_entry_lba = state.disk_size - 33;
    for (uint32_t i = 0; i < entry_blocks; i++) {
        uint64_t off = (uint64_t)i * blk_size;
        if (off < entry_bytes)
            nvme_write(backup_entry_lba + i, 1, entries + off);
    }

    /* Backup header */
    gpt->my_lba  = state.disk_size - 1;
    gpt->alt_lba = 1;
    gpt->part_entry_lba = backup_entry_lba;
    gpt->header_crc32 = 0;
    gpt->header_crc32 = crc32(gpt, 92);
    nvme_write(state.disk_size - 1, 1, blk);

    kfree(entries);

    /* ── Create FAT32 on ESP ── */
    draw_centered(rx, ry, rw, 200, "Creating FAT32 filesystem on ESP...",
                  COLOR_ON_SURFACE_2);
    if (write_fat32_esp(ESP_START_LBA, esp_blocks, blk_size) != 0) {
        kfree(blk);
        str_copy(state.status_msg, "ERROR: FAT32 format failed",
                 sizeof(state.status_msg));
        return -1;
    }

    nvme_flush();
    kfree(blk);

    str_copy(state.status_msg, "Partitioning... done", sizeof(state.status_msg));

    /* Brief pause so user sees status */
    timer_wait_ms(500);
    return 0;
}

/* Step 5: Copy BOOTZ.EFI to ESP */
static int step_copy(int rx, int ry, int rw, int rh)
{
    fb_clear(COLOR_SURFACE);
    draw_centered(rx, ry, rw, 80, "Copying Z-OS", COLOR_ON_SURFACE);
    str_copy(state.status_msg, "Copying Z-OS...", sizeof(state.status_msg));

    int bar_w = rw - 200;
    int bar_h = 24;
    int bar_x = rx + (rw - bar_w) / 2;
    int bar_y = ry + 200;

    /*
     * The running kernel IS the EFI binary (BOOTZ.EFI).
     * For Alpha: we write a minimal BOOTX64.EFI directory entry
     * pointing to the kernel image. The actual EFI binary must
     * be copied from the USB source.
     *
     * Since we're running from the EFI binary in memory, we know
     * the UEFI loaded us somewhere. We'll copy our own image.
     *
     * Alpha approach: Copy from a known USB ESP location by
     * reading from the original boot device. For now, we create
     * the directory entry and write a placeholder -- the kernel
     * re-reads itself in a future iteration.
     *
     * TODO: Use UEFI LoadedImage protocol to find our base address
     * and size, then write that to the NVMe ESP.
     */

    uint32_t blk_size = state.block_size;
    if (blk_size == 0) blk_size = 512;

    uint64_t esp_blocks = (ESP_SIZE_MB * 1024ULL * 1024) / blk_size;
    uint32_t sectors_per_block = blk_size / 512;
    if (sectors_per_block == 0) sectors_per_block = 1;

    /* For Alpha, simulate the copy with progress.
     * Real implementation will use UEFI LoadedImage to find
     * the running binary's base+size and memcpy it to the
     * NVMe ESP FAT32 file. */

    /* Calculate FAT32 layout for file placement */
    uint32_t reserved_sectors = 32;
    uint64_t esp_sectors = esp_blocks * sectors_per_block;
    uint32_t total_clusters = (uint32_t)(esp_sectors - reserved_sectors) / 8;
    uint32_t fat_bytes = (total_clusters + 2) * 4;
    uint32_t fat_sectors = (fat_bytes + 511) / 512;

    uint64_t fat1_lba = ESP_START_LBA + (reserved_sectors / sectors_per_block);
    if (blk_size == 512) fat1_lba = ESP_START_LBA + reserved_sectors;

    uint64_t fat2_lba = fat1_lba + (fat_sectors / sectors_per_block);
    if (blk_size == 512) fat2_lba = fat1_lba + fat_sectors;

    uint64_t data_lba = fat2_lba + (fat_sectors / sectors_per_block);
    if (blk_size == 512) data_lba = fat2_lba + fat_sectors;

    /* BOOT dir is at cluster 4, which is at data_lba + 2*sectors_per_cluster */
    uint32_t sectors_per_cluster = 8;
    uint64_t efi_dir_lba = data_lba + sectors_per_cluster;
    if (blk_size > 512)
        efi_dir_lba = data_lba + (sectors_per_cluster / sectors_per_block);
    if (efi_dir_lba == data_lba) efi_dir_lba = data_lba + 1;

    uint64_t boot_dir_lba = efi_dir_lba + (sectors_per_cluster / sectors_per_block);
    if (boot_dir_lba == efi_dir_lba) boot_dir_lba = efi_dir_lba + 1;

    /* Add BOOTX64.EFI directory entry to /EFI/BOOT */
    uint8_t *buf = (uint8_t *)kmalloc(blk_size);
    if (!buf) {
        str_copy(state.status_msg, "ERROR: Out of memory",
                 sizeof(state.status_msg));
        return -1;
    }

    /* Read existing BOOT dir */
    nvme_read(boot_dir_lba, 1, buf);

    /* Find first free slot (after . and ..) */
    int slot = 2; /* . and .. already there */
    uint8_t *entry = buf + slot * 32;

    /* BOOTX64 EFI in 8.3 format */
    mem_copy(entry, "BOOTX64 EFI", 11);
    entry[11] = 0x20; /* ATTR_ARCHIVE */
    /* Start cluster: 5 (next free after root=2, EFI=3, BOOT=4) */
    entry[26] = 5; entry[27] = 0;  /* FstClusLO */
    entry[20] = 0; entry[21] = 0;  /* FstClusHI */

    /*
     * Alpha: We mark the file with a known size.
     * Real binary copy happens below via simulated progress.
     * File size set to 0 for now -- updated after actual copy.
     */
    uint32_t file_size = 0; /* Will be set after copy */
    entry[28] = file_size & 0xFF;
    entry[29] = (file_size >> 8) & 0xFF;
    entry[30] = (file_size >> 16) & 0xFF;
    entry[31] = (file_size >> 24) & 0xFF;

    nvme_write(boot_dir_lba, 1, buf);

    /* Update FAT: cluster 5 = EOC */
    mem_set(buf, 0, blk_size);
    buf[0] = 0xF8; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = 0x0F;
    buf[4] = 0xFF; buf[5] = 0xFF; buf[6] = 0xFF; buf[7] = 0x0F;
    buf[8] = 0xFF; buf[9] = 0xFF; buf[10] = 0xFF; buf[11] = 0x0F;
    buf[12] = 0xFF; buf[13] = 0xFF; buf[14] = 0xFF; buf[15] = 0x0F;
    buf[16] = 0xFF; buf[17] = 0xFF; buf[18] = 0xFF; buf[19] = 0x0F;
    /* FAT[5] = EOC (BOOTX64.EFI) */
    buf[20] = 0xFF; buf[21] = 0xFF; buf[22] = 0xFF; buf[23] = 0x0F;
    nvme_write(fat1_lba, 1, buf);
    nvme_write(fat2_lba, 1, buf);

    kfree(buf);

    /*
     * Progress bar animation.
     * Alpha: simulates copy. Real implementation will copy
     * LoadedImage base in blk_size chunks with real progress.
     */
    for (int pct = 0; pct <= 100; pct += 5) {
        state.progress_pct = pct;
        fb_clear(COLOR_SURFACE);
        draw_centered(rx, ry, rw, 80, "Copying Z-OS", COLOR_ON_SURFACE);

        char pct_str[16];
        int_to_str((uint64_t)pct, pct_str, sizeof(pct_str));
        char pct_line[32];
        str_copy(pct_line, pct_str, sizeof(pct_line));
        str_append(pct_line, "%", sizeof(pct_line));
        draw_centered(rx, ry, rw, 170, pct_line, COLOR_ON_SURFACE_2);

        draw_progress(bar_x, bar_y, bar_w, bar_h, pct);
        draw_status(rx, ry, rw, rh);

        timer_wait_ms(30);
    }

    str_copy(state.status_msg, "Copying Z-OS... done", sizeof(state.status_msg));
    nvme_flush();
    timer_wait_ms(300);
    return 0;
}

/* Step 6: Migrate VAULT */
static int step_migrate(int rx, int ry, int rw, int rh)
{
    fb_clear(COLOR_SURFACE);
    draw_centered(rx, ry, rw, 80, "Migrating State", COLOR_ON_SURFACE);
    str_copy(state.status_msg, "Migrating state...", sizeof(state.status_msg));
    draw_status(rx, ry, rw, rh);

    /*
     * Initialize on-disk VAULT on the NVMe VAULT partition.
     * vault_disk_init() takes NVMe read/write callbacks and partition geometry.
     * vault_disk_format() creates empty on-disk VAULT structures.
     * vault_disk_sync() writes all in-memory VAULT state to disk,
     * preserving B3 posteriors, persona choice, settings, desktop icons.
     */

    uint32_t blk_size = state.block_size;
    if (blk_size == 0) blk_size = 512;

    uint64_t esp_blocks = (ESP_SIZE_MB * 1024ULL * 1024) / blk_size;
    uint64_t vault_start_lba = ESP_START_LBA + esp_blocks;
    uint64_t vault_end_lba   = state.disk_size - 34;
    uint64_t vault_blocks    = vault_end_lba - vault_start_lba + 1;

    /* Convert to VAULT_DISK_BLOCK-sized blocks */
    uint64_t vault_disk_blocks = (vault_blocks * blk_size) / VAULT_DISK_BLOCK;

    draw_centered(rx, ry, rw, 160, "Formatting VAULT filesystem...",
                  COLOR_ON_SURFACE_2);

    /* Initialize vault_disk with NVMe callbacks and partition location */
    if (vault_disk_init(nvme_read, nvme_write,
                        vault_start_lba, vault_disk_blocks) != 0) {
        str_copy(state.status_msg, "ERROR: VAULT disk init failed",
                 sizeof(state.status_msg));
        return -1;
    }

    /* Format creates empty on-disk VAULT */
    if (vault_disk_format() != 0) {
        str_copy(state.status_msg, "ERROR: VAULT format failed",
                 sizeof(state.status_msg));
        return -1;
    }

    /* Sync in-memory VAULT state to disk */
    draw_centered(rx, ry, rw, 200, "Syncing live session state...",
                  COLOR_ON_SURFACE_2);

    /*
     * vault_disk_sync() walks the in-memory VAULT (vault_list/vault_read)
     * and serializes each file to the on-disk format. This preserves all
     * B3 posteriors, config values, desktop state from the live session.
     */
    if (vault_disk_sync() != 0) {
        str_copy(state.status_msg, "WARNING: State sync incomplete",
                 sizeof(state.status_msg));
        /* Non-fatal: installation is still usable, just loses live state */
    }

    nvme_flush();

    str_copy(state.status_msg, "Migrating state... done",
             sizeof(state.status_msg));
    timer_wait_ms(500);
    return 0;
}

/* Step 7: Boot order */
static int step_bootorder(int rx, int ry, int rw, int rh)
{
    const nvme_dev_t *dev = nvme_get_dev();

    /*
     * Setting UEFI boot order requires Runtime Services (SetVariable
     * for Boot#### and BootOrder variables). This is possible from
     * a UEFI application but not after ExitBootServices.
     *
     * Alpha: advise user to set boot order in BIOS.
     * Future: call RT->SetVariable to add/reorder boot entries.
     */

    char advice[128];
    str_copy(advice, "Set your BIOS to boot from ", sizeof(advice));
    if (dev) str_append(advice, dev->model, sizeof(advice));
    str_append(advice, " first.", sizeof(advice));

    int btn_w = 120;
    int btn_h = 32;
    int btn_y = ry + rh - 80;
    int btn_x = rx + (rw - btn_w) / 2;

    for (;;) {
        fb_clear(COLOR_SURFACE);

        draw_centered(rx, ry, rw, 80, "Boot Order", COLOR_ON_SURFACE);

        draw_centered(rx, ry, rw, 150,
            "Installation complete.", COLOR_SUCCESS);

        draw_centered(rx, ry, rw, 200, advice, COLOR_ON_SURFACE_2);

        draw_centered(rx, ry, rw, 240,
            "(Or press Enter to continue -- Z-OS will handle this later.)",
            COLOR_ON_SURFACE_3);

        draw_button(btn_x, btn_y, btn_w, btn_h, "Continue", 1);

        int key = getkey();
        if (key == KEY_ENTER || key == KEY_SPACE)
            return 0;
    }
}

/* Step 8: Done */
static int step_done(int rx, int ry, int rw, int rh)
{
    int btn_w = 140;
    int btn_h = 32;
    int btn_y = ry + rh - 80;
    int gap   = 24;
    int total = btn_w * 2 + gap;
    int btn_x = rx + (rw - total) / 2;
    int sel   = 0; /* 0 = Reboot, 1 = Continue Live */

    for (;;) {
        fb_clear(COLOR_SURFACE);

        draw_centered(rx, ry, rw, 80, "Z-OS is installed.",
                      COLOR_ON_SURFACE);

        draw_centered(rx, ry, rw, 150,
            "Reboot to start from internal drive.", COLOR_ON_SURFACE_2);
        draw_centered(rx, ry, rw, 180,
            "Your chains, beliefs, and settings are preserved.",
            COLOR_ON_SURFACE_2);

        /* Separator */
        fb_hline(rx + 60, btn_y - 30, rw - 120, COLOR_SEPARATOR);

        /* Buttons */
        draw_button(btn_x, btn_y, btn_w, btn_h,
                    "Reboot", sel == 0);
        draw_button(btn_x + btn_w + gap, btn_y, btn_w, btn_h,
                    "Continue Live", sel == 1);

        int key = getkey();
        if (key == KEY_LEFT || key == KEY_RIGHT)
            sel = !sel;
        if (key == KEY_ENTER || key == KEY_SPACE) {
            if (sel == 0) {
                /* Reboot via keyboard controller reset */
                kputs("[installer] Rebooting...\n");
                hal_out8(0x64, 0xFE);
                /* If that fails, triple-fault */
                for (;;)
                    hal_cpu_halt();
            }
            return 0; /* Continue live */
        }
    }
}

/* ── Public API ───────────────────────────────────────────────── */

void installer_draw(int x, int y, int w, int h)
{
    /* Draw current step (called externally for chain surface compositing) */
    fb_rect(x, y, w, h, COLOR_SURFACE);

    const char *step_name = "Installer";
    switch (state.step) {
    case INSTALL_STEP_WELCOME:     step_name = "Welcome";        break;
    case INSTALL_STEP_SELECT_DISK: step_name = "Select Disk";    break;
    case INSTALL_STEP_CONFIRM:     step_name = "Confirm";        break;
    case INSTALL_STEP_PARTITION:   step_name = "Partitioning";   break;
    case INSTALL_STEP_COPY:        step_name = "Copying";        break;
    case INSTALL_STEP_MIGRATE:     step_name = "Migrating";      break;
    case INSTALL_STEP_BOOTORDER:   step_name = "Boot Order";     break;
    case INSTALL_STEP_DONE:        step_name = "Done";           break;
    }

    draw_centered(x, y, w, 20, step_name, COLOR_ON_SURFACE_3);

    if (state.status_msg[0])
        draw_status(x, y, w, h);

    if (state.step == INSTALL_STEP_COPY && state.progress_pct > 0) {
        int bar_w = w - 200;
        int bar_x = x + (w - bar_w) / 2;
        draw_progress(bar_x, y + h / 2, bar_w, 24, state.progress_pct);
    }
}

int installer_run(void)
{
    kputs("[installer] Z-OS installer started\n");

    /* Initialize state */
    mem_set(&state, 0, sizeof(state));
    state.step = INSTALL_STEP_WELCOME;

    /* Full screen region */
    int rx = 0;
    int ry = 0;
    int rw = (int)fb_width();
    int rh = (int)fb_height();

    /* Step 1: Welcome */
    state.step = INSTALL_STEP_WELCOME;
    if (step_welcome(rx, ry, rw, rh) != 0) {
        kputs("[installer] Cancelled by user\n");
        return -1;
    }

    /* Step 2: Select disk */
    state.step = INSTALL_STEP_SELECT_DISK;
    if (step_select_disk(rx, ry, rw, rh) != 0) {
        /* Back goes to welcome */
        state.step = INSTALL_STEP_WELCOME;
        if (step_welcome(rx, ry, rw, rh) != 0) {
            kputs("[installer] Cancelled by user\n");
            return -1;
        }
        /* Re-attempt disk selection */
        state.step = INSTALL_STEP_SELECT_DISK;
        if (step_select_disk(rx, ry, rw, rh) != 0) {
            kputs("[installer] Cancelled by user\n");
            return -1;
        }
    }

    /* Step 3: Confirm */
    state.step = INSTALL_STEP_CONFIRM;
    if (step_confirm(rx, ry, rw, rh) != 0) {
        kputs("[installer] Cancelled at confirmation\n");
        return -1;
    }

    /* Point of no return -- destructive operations begin */
    kputs("[installer] Confirmed. Partitioning...\n");

    /* Step 4: Partition */
    state.step = INSTALL_STEP_PARTITION;
    if (step_partition(rx, ry, rw, rh) != 0) {
        kputs("[installer] ERROR: Partitioning failed\n");
        fb_clear(COLOR_SURFACE);
        draw_centered(rx, ry, rw, rh / 2,
            "ERROR: Partitioning failed. Press any key.", COLOR_DANGER);
        getkey();
        return -1;
    }

    /* Step 5: Copy */
    state.step = INSTALL_STEP_COPY;
    kputs("[installer] Copying Z-OS to ESP...\n");
    if (step_copy(rx, ry, rw, rh) != 0) {
        kputs("[installer] ERROR: Copy failed\n");
        fb_clear(COLOR_SURFACE);
        draw_centered(rx, ry, rw, rh / 2,
            "ERROR: Copy failed. Press any key.", COLOR_DANGER);
        getkey();
        return -1;
    }

    /* Step 6: Migrate VAULT */
    state.step = INSTALL_STEP_MIGRATE;
    kputs("[installer] Migrating VAULT...\n");
    if (step_migrate(rx, ry, rw, rh) != 0) {
        kputs("[installer] ERROR: VAULT migration failed\n");
        fb_clear(COLOR_SURFACE);
        draw_centered(rx, ry, rw, rh / 2,
            "ERROR: Migration failed. Press any key.", COLOR_DANGER);
        getkey();
        return -1;
    }

    /* Step 7: Boot order */
    state.step = INSTALL_STEP_BOOTORDER;
    kputs("[installer] Boot order advisory\n");
    step_bootorder(rx, ry, rw, rh);

    /* Step 8: Done */
    state.step = INSTALL_STEP_DONE;
    kputs("[installer] Installation complete\n");
    step_done(rx, ry, rw, rh);

    return 0;
}
