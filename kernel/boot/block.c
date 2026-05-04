/*
 * Zeos -- Block device dispatcher (multi-drive)
 *
 * Walks every storage class:
 *   - NVMe: any number of controllers
 *   - AHCI: first ATA-capable port
 *   - USB MSC: first BBB LUN0
 */

#include "block.h"
#include "nvme.h"
#include "ahci.h"
#include "usb_msc.h"
#include "kprint.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);

#define BLOCK_MAX_DRIVES 16

typedef struct { int kind; int sub_idx; } drive_slot_t;
static drive_slot_t g_drives[BLOCK_MAX_DRIVES];
static int g_count = 0;

static int copy_str(char *dst, int cap, const char *src)
{
    int i = 0;
    if (!src) { dst[0] = '\0'; return 0; }
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

int block_init(void)
{
    g_count = 0;
    memset(g_drives, 0, sizeof(g_drives));

    if (nvme_init() == 0) {
        int n = nvme_drive_count();
        for (int i = 0; i < n && g_count < BLOCK_MAX_DRIVES; i++) {
            g_drives[g_count].kind = BLOCK_KIND_NVME;
            g_drives[g_count].sub_idx = i;
            g_count++;
        }
    }

    if (ahci_init() == 0) {
        int n = ahci_drive_count();
        for (int i = 0; i < n && g_count < BLOCK_MAX_DRIVES; i++) {
            g_drives[g_count].kind = BLOCK_KIND_AHCI;
            g_drives[g_count].sub_idx = i;
            g_count++;
        }
    }

    if ((usb_msc_ready() || usb_msc_init() == 0) && g_count < BLOCK_MAX_DRIVES) {
        g_drives[g_count].kind = BLOCK_KIND_USB_MSC;
        g_drives[g_count].sub_idx = 0;
        g_count++;
    }

    if (g_count == 0) {
        kputs("BLOCK: no storage controller detected\n");
        return -1;
    }

    kputs("BLOCK: ");
    kput_dec(g_count);
    kputs(" drive(s) ready\n");
    return 0;
}

int block_drive_count(void) { return g_count; }

int block_drive_info(int idx, block_drive_info_t *out)
{
    if (!out) return -1;
    if (idx < 0 || idx >= g_count) return -1;
    memset(out, 0, sizeof(*out));
    out->idx = idx;
    out->kind = g_drives[idx].kind;
    out->sub_idx = g_drives[idx].sub_idx;

    switch (out->kind) {
        case BLOCK_KIND_NVME: {
            nvme_dev_t *d = nvme_get_drive(out->sub_idx);
            if (!d) return -1;
            out->sectors = d->num_blocks;
            out->sector_size = d->block_size;
            copy_str(out->serial, sizeof(out->serial), d->serial);
            copy_str(out->model,  sizeof(out->model),  d->model);
            return 0;
        }
        case BLOCK_KIND_AHCI: {
            int sub = out->sub_idx;
            uint64_t blocks = ahci_drive_blocks(sub);
            uint32_t bs     = ahci_drive_block_size(sub);
            int port        = ahci_drive_port(sub);
            if (!bs) return -1;
            out->sectors = blocks;
            out->sector_size = bs;
            copy_str(out->serial, sizeof(out->serial), "");
            char m[44];
            int j = 0;
            const char *base = "AHCI port ";
            while (base[j] && j < (int)sizeof(m) - 4) { m[j] = base[j]; j++; }
            if (port < 0)        { m[j++] = '?'; }
            else if (port < 10)  { m[j++] = '0' + port; }
            else                 { m[j++] = '0' + (port / 10);
                                   m[j++] = '0' + (port % 10); }
            m[j] = '\0';
            copy_str(out->model, sizeof(out->model), m);
            return 0;
        }
        case BLOCK_KIND_USB_MSC:
            out->sectors = usb_msc_sector_count();
            out->sector_size = usb_msc_block_size();
            copy_str(out->serial, sizeof(out->serial), "");
            copy_str(out->model,  sizeof(out->model),  "USB MSC");
            return 0;
    }
    return -1;
}

int block_read_drive(int idx, uint64_t lba, uint32_t count, void *buf)
{
    if (idx < 0 || idx >= g_count) return -1;
    drive_slot_t *s = &g_drives[idx];
    switch (s->kind) {
        case BLOCK_KIND_NVME:    return nvme_read_drive(s->sub_idx, lba, count, buf);
        case BLOCK_KIND_AHCI:    return ahci_read_drive(s->sub_idx, lba, count, buf);
        case BLOCK_KIND_USB_MSC: return usb_msc_read(lba, count, buf);
    }
    return -1;
}

int block_write_drive(int idx, uint64_t lba, uint32_t count, const void *buf)
{
    if (idx < 0 || idx >= g_count) return -1;
    drive_slot_t *s = &g_drives[idx];
    switch (s->kind) {
        case BLOCK_KIND_NVME:    return nvme_write_drive(s->sub_idx, lba, count, buf);
        case BLOCK_KIND_AHCI:    return ahci_write_drive(s->sub_idx, lba, count, buf);
        case BLOCK_KIND_USB_MSC: return usb_msc_write(lba, count, buf);
    }
    return -1;
}

int block_flush_drive(int idx)
{
    if (idx < 0 || idx >= g_count) return -1;
    drive_slot_t *s = &g_drives[idx];
    switch (s->kind) {
        case BLOCK_KIND_NVME:    return nvme_flush_drive(s->sub_idx);
        case BLOCK_KIND_AHCI:    return ahci_flush_drive(s->sub_idx);
        case BLOCK_KIND_USB_MSC: return usb_msc_flush();
    }
    return -1;
}

int block_read(uint64_t lba, uint32_t count, void *buf)        { return block_read_drive(0, lba, count, buf); }
int block_write(uint64_t lba, uint32_t count, const void *buf) { return block_write_drive(0, lba, count, buf); }
int block_flush(void)                                          { return block_flush_drive(0); }

uint32_t block_size(void)
{
    if (g_count == 0) return 0;
    block_drive_info_t info;
    if (block_drive_info(0, &info) != 0) return 0;
    return info.sector_size ? info.sector_size : 512;
}

const char *block_kind(void)
{
    if (g_count == 0) return "none";
    switch (g_drives[0].kind) {
        case BLOCK_KIND_NVME:    return "nvme";
        case BLOCK_KIND_AHCI:    return "ahci";
        case BLOCK_KIND_USB_MSC: return "usb-msc";
    }
    return "none";
}
