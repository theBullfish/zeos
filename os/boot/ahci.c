/*
 * Zeos — AHCI / SATA driver (polling, multi-port, single-command per port)
 *
 * AHCI 1.3 spec. Standard MMIO at PCI BAR5. Per-port:
 *   offset 0x000  Command list (32 × 32 bytes) — only slot 0 used
 *   offset 0x400  Received FIS (256 bytes)
 *   offset 0x500  Command table (FIS + ATAPI + PRDT)
 *
 * IDENTIFY (0xEC) for capacity. READ/WRITE DMA EXT (0x25/0x35) for I/O.
 * FLUSH EXT (0xEA) for sync.
 */

#include "ahci.h"
#include "hal.h"   /* hal_cpu_relax() — was a literal x86 `pause` */
#include "pci.h"
#include "pmm.h"
#include "kprint.h"

#include <stdint.h>

#define HBA_GHC        0x04
#define HBA_PI         0x0C
#define HBA_PORTS_BASE 0x100

#define GHC_AE         (1u << 31)

#define PxCLB     0x00
#define PxCLBU    0x04
#define PxFB      0x08
#define PxFBU     0x0C
#define PxIS      0x10
#define PxCMD     0x18
#define PxTFD     0x20
#define PxSIG     0x24
#define PxSSTS    0x28
#define PxSERR    0x30
#define PxCI      0x38

#define PxCMD_ST   (1u << 0)
#define PxCMD_FRE  (1u << 4)
#define PxCMD_FR   (1u << 14)
#define PxCMD_CR   (1u << 15)

#define PxTFD_BSY  (1u << 7)
#define PxTFD_DRQ  (1u << 3)

#define SATA_SIG_ATA   0x00000101u

#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_FLUSH_EXT     0xEA

#define FIS_TYPE_REG_H2D 0x27

#define AHCI_MAX_DRIVES 32

struct cmd_header {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
};

struct prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc_i;
};

#define CMD_TABLE_OFFSET 0x500

struct fis_h2d {
    uint8_t  type;
    uint8_t  flags;
    uint8_t  command;
    uint8_t  features_lo;
    uint8_t  lba0, lba1, lba2;
    uint8_t  device;
    uint8_t  lba3, lba4, lba5;
    uint8_t  features_hi;
    uint8_t  count_lo, count_hi;
    uint8_t  icc;
    uint8_t  control;
    uint32_t reserved;
};

struct ahci_drive {
    int        port;
    int        ready;
    volatile uint32_t *regs;

    uint8_t   *dma_page;
    uint64_t   dma_page_phys;
    struct cmd_header *cmd_list;
    uint8_t   *recv_fis;
    uint8_t   *cmd_table;

    uint64_t   total_blocks;
    uint32_t   blk_size;
};

static volatile uint32_t *abar;
static struct ahci_drive drives[AHCI_MAX_DRIVES];
static int drive_count;

static inline uint32_t r(uint32_t off) {
    return *(volatile uint32_t *)((uint8_t *)abar + off);
}
static inline void w(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)((uint8_t *)abar + off) = v;
}
static inline uint32_t pr(struct ahci_drive *d, uint32_t off) {
    return *(volatile uint32_t *)((uint8_t *)d->regs + off);
}
static inline void pw(struct ahci_drive *d, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)((uint8_t *)d->regs + off) = v;
}

static void udelay(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++) hal_cpu_relax();
}

static int wait_clear(struct ahci_drive *d, uint32_t off, uint32_t mask, int max_loops) {
    for (int i = 0; i < max_loops; i++) {
        if (!(pr(d, off) & mask)) return 0;
        udelay(1000);
    }
    return -1;
}

static int port_stop(struct ahci_drive *d) {
    uint32_t cmd = pr(d, PxCMD);
    cmd &= ~(PxCMD_ST | PxCMD_FRE);
    pw(d, PxCMD, cmd);
    if (wait_clear(d, PxCMD, PxCMD_FR | PxCMD_CR, 5000) < 0) return -1;
    return 0;
}

static void port_start(struct ahci_drive *d) {
    while (pr(d, PxCMD) & PxCMD_CR) udelay(100);
    uint32_t cmd = pr(d, PxCMD);
    cmd |= PxCMD_FRE;
    pw(d, PxCMD, cmd);
    cmd |= PxCMD_ST;
    pw(d, PxCMD, cmd);
}

static int issue_cmd(struct ahci_drive *d, uint8_t ata_cmd, uint64_t lba,
                     uint16_t count, void *buf, uint32_t buf_bytes, int write) {
    if (wait_clear(d, PxTFD, PxTFD_BSY | PxTFD_DRQ, 1000) < 0) return -1;

    pw(d, PxIS, 0xFFFFFFFFu);
    pw(d, PxSERR, 0xFFFFFFFFu);

    d->cmd_list[0].flags = (5 & 0x1F) | (write ? (1u << 6) : 0);
    d->cmd_list[0].prdtl = (buf_bytes > 0) ? 1 : 0;
    d->cmd_list[0].prdbc = 0;
    d->cmd_list[0].ctba  = (uint32_t)(d->dma_page_phys + CMD_TABLE_OFFSET);
    d->cmd_list[0].ctbau = (uint32_t)((d->dma_page_phys + CMD_TABLE_OFFSET) >> 32);

    for (int i = 0; i < 256; i++) d->cmd_table[i] = 0;

    struct fis_h2d *fis = (struct fis_h2d *)d->cmd_table;
    fis->type     = FIS_TYPE_REG_H2D;
    fis->flags    = 0x80;
    fis->command  = ata_cmd;
    fis->device   = 1u << 6;

    fis->lba0 = (uint8_t)(lba       & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

    fis->count_lo = (uint8_t)(count & 0xFF);
    fis->count_hi = (uint8_t)((count >> 8) & 0xFF);

    if (buf_bytes > 0 && buf) {
        struct prdt_entry *prd = (struct prdt_entry *)(d->cmd_table + 0x80);
        prd->dba    = (uint32_t)((uint64_t)(uintptr_t)buf);
        prd->dbau   = (uint32_t)(((uint64_t)(uintptr_t)buf) >> 32);
        prd->reserved = 0;
        prd->dbc_i  = (buf_bytes - 1) & 0x3FFFFFu;
    }

    pw(d, PxCI, 1u);

    for (int i = 0; i < 5000000; i++) {
        if (!(pr(d, PxCI) & 1u)) {
            if (pr(d, PxIS) & (1u << 30)) return -1;
            if (pr(d, PxTFD) & 1u) return -1;
            return 0;
        }
        udelay(10);
    }
    return -1;
}

static int bring_up_port(int port_num, struct ahci_drive *out) {
    out->port = port_num;
    out->ready = 0;
    out->regs = (volatile uint32_t *)((uint8_t *)abar + HBA_PORTS_BASE + port_num * 0x80);

    uint32_t ssts = pr(out, PxSSTS);
    uint32_t det = ssts & 0xF;
    uint32_t ipm = (ssts >> 8) & 0xF;
    if (det != 3 || ipm != 1) return -1;

    uint32_t sig = pr(out, PxSIG);
    if (sig != SATA_SIG_ATA) return -1;

    uint64_t phys = pmm_alloc_contiguous(1);
    if (!phys) return -1;
    out->dma_page = (uint8_t *)(uintptr_t)phys;
    out->dma_page_phys = phys;
    for (int i = 0; i < 4096; i++) out->dma_page[i] = 0;

    out->cmd_list  = (struct cmd_header *)(out->dma_page);
    out->recv_fis  = out->dma_page + 0x400;
    out->cmd_table = out->dma_page + CMD_TABLE_OFFSET;

    if (port_stop(out) < 0) return -1;

    pw(out, PxCLB,  (uint32_t)(out->dma_page_phys & 0xFFFFFFFFu));
    pw(out, PxCLBU, (uint32_t)(out->dma_page_phys >> 32));
    pw(out, PxFB,   (uint32_t)((out->dma_page_phys + 0x400) & 0xFFFFFFFFu));
    pw(out, PxFBU,  (uint32_t)((out->dma_page_phys + 0x400) >> 32));

    pw(out, PxIS, 0xFFFFFFFFu);
    pw(out, PxSERR, 0xFFFFFFFFu);
    port_start(out);

    /* IDENTIFY DEVICE — single static buffer, init is sequential. */
    static uint8_t id_buf[512] __attribute__((aligned(8)));
    if (issue_cmd(out, ATA_CMD_IDENTIFY, 0, 1, id_buf, 512, 0) < 0) return -1;

    uint64_t lba48 = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t w16 = ((uint16_t)id_buf[200 + i*2 + 1] << 8) | id_buf[200 + i*2];
        lba48 |= (uint64_t)w16 << (i * 16);
    }

    out->total_blocks = lba48;
    out->blk_size = 512;
    out->ready = 1;
    return 0;
}

int ahci_init(void) {
    drive_count = 0;
    for (int i = 0; i < AHCI_MAX_DRIVES; i++) drives[i].ready = 0;

    int n = pci_device_count();
    struct pci_device *dev = 0;
    for (int i = 0; i < n; i++) {
        struct pci_device *cand = pci_get_device(i);
        if (cand && cand->class_code == 0x01 && cand->subclass == 0x06 &&
            cand->prog_if == 0x01) {
            dev = cand;
            break;
        }
    }
    if (!dev) {
        kputs("ahci: no AHCI controller found\n");
        return -1;
    }

    uint32_t bar5 = dev->bar[5];
    if (bar5 & 1) { kputs("ahci: ABAR is I/O space — unexpected\n"); return -1; }
    abar = (volatile uint32_t *)(uintptr_t)(bar5 & 0xFFFFFFF0u);

    uint32_t cmd = pci_config_read32(dev->bus, dev->dev, dev->func, 0x04);
    pci_config_write32(dev->bus, dev->dev, dev->func, 0x04, cmd | 0x06);

    w(HBA_GHC, r(HBA_GHC) | GHC_AE);

    uint32_t pi = r(HBA_PI);
    for (int i = 0; i < 32 && drive_count < AHCI_MAX_DRIVES; i++) {
        if (!(pi & (1u << i))) continue;
        struct ahci_drive *d = &drives[drive_count];
        if (bring_up_port(i, d) == 0) {
            kputs("ahci: drive ");
            kput_dec(drive_count);
            kputs(" on port ");
            kput_dec(i);
            kputs(", ");
            kput_dec(d->total_blocks);
            kputs(" sectors (");
            kput_dec(d->total_blocks * 512 / (1024 * 1024));
            kputs(" MB)\n");
            drive_count++;
        }
    }

    if (drive_count == 0) {
        kputs("ahci: no SATA ATA drive on any port\n");
        return -1;
    }

    return 0;
}

int ahci_drive_count(void) { return drive_count; }

static struct ahci_drive *D(int idx) {
    if (idx < 0 || idx >= drive_count) return 0;
    if (!drives[idx].ready) return 0;
    return &drives[idx];
}

int ahci_read_drive(int idx, uint64_t lba, uint32_t count, void *buf) {
    struct ahci_drive *d = D(idx);
    if (!d) return -1;
    if (count == 0) return 0;
    if (count > 65535) return -1;
    return issue_cmd(d, ATA_CMD_READ_DMA_EXT, lba, (uint16_t)count, buf,
                     count * d->blk_size, 0);
}

int ahci_write_drive(int idx, uint64_t lba, uint32_t count, const void *buf) {
    struct ahci_drive *d = D(idx);
    if (!d) return -1;
    if (count == 0) return 0;
    if (count > 65535) return -1;
    return issue_cmd(d, ATA_CMD_WRITE_DMA_EXT, lba, (uint16_t)count, (void *)buf,
                     count * d->blk_size, 1);
}

int ahci_flush_drive(int idx) {
    struct ahci_drive *d = D(idx);
    if (!d) return -1;
    return issue_cmd(d, ATA_CMD_FLUSH_EXT, 0, 0, 0, 0, 0);
}

uint64_t ahci_drive_blocks(int idx) {
    struct ahci_drive *d = D(idx);
    return d ? d->total_blocks : 0;
}

uint32_t ahci_drive_block_size(int idx) {
    struct ahci_drive *d = D(idx);
    return d ? d->blk_size : 0;
}

int ahci_drive_port(int idx) {
    struct ahci_drive *d = D(idx);
    return d ? d->port : -1;
}

/* Legacy single-drive API (operates on drive 0). */

int ahci_read(uint64_t lba, uint32_t count, void *buf) {
    return ahci_read_drive(0, lba, count, buf);
}
int ahci_write(uint64_t lba, uint32_t count, const void *buf) {
    return ahci_write_drive(0, lba, count, buf);
}
int ahci_flush(void) { return ahci_flush_drive(0); }
uint64_t ahci_num_blocks(void) { return ahci_drive_blocks(0); }
uint32_t ahci_block_size(void) {
    uint32_t bs = ahci_drive_block_size(0);
    return bs ? bs : 512;
}
int ahci_ready(void) {
    return drive_count > 0 && drives[0].ready;
}
