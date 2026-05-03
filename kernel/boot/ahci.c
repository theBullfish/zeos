/*
 * Zeos — AHCI / SATA driver (polling, single-port, single-command)
 *
 * AHCI 1.3 spec reference. Covers any controller that exposes the
 * standard MMIO interface at PCI BAR5: Intel ICH9+, AMD SBxxx/FCH,
 * Marvell, Promise, JMicron, etc.
 *
 * Memory layout (one 4 KB physical page holds everything):
 *   offset 0x000  Command list (32 × 32 bytes = 1024) — only slot 0 used
 *   offset 0x400  Received FIS (256 bytes)
 *   offset 0x500  Command table (1 × 256 = command FIS + ATAPI + PRDT)
 *
 * Per command:
 *   - Build a Command FIS (FIS H2D Register, type 0x27) at cmd_table
 *   - Build a PRDT entry pointing at the user's buffer
 *   - Set CMDH.prdtl=1, CMDH.cfl=5 (5 dwords), CMDH.w=write?1:0
 *   - Issue: PxCI |= 1, wait for the bit to clear
 *
 * IDENTIFY (ATA cmd 0xEC) gives capacity (LBA48 max). READ/WRITE
 * DMA EXT (0x25/0x35) for I/O. FLUSH EXT (0xEA) for sync.
 */

#include "ahci.h"
#include "pci.h"
#include "pmm.h"
#include "kprint.h"

#include <stdint.h>

/* HBA generic registers (offsets from ABAR base) */
#define HBA_CAP        0x00
#define HBA_GHC        0x04
#define HBA_IS         0x08
#define HBA_PI         0x0C
#define HBA_VS         0x10
#define HBA_PORTS_BASE 0x100  /* + N*0x80 for port N */

#define GHC_HR         (1u << 0)   /* HBA reset */
#define GHC_IE         (1u << 1)
#define GHC_AE         (1u << 31)  /* AHCI enable */

/* Per-port register offsets (relative to port base) */
#define PxCLB     0x00   /* Command List Base */
#define PxCLBU    0x04
#define PxFB      0x08   /* Received FIS Base */
#define PxFBU     0x0C
#define PxIS      0x10
#define PxIE      0x14
#define PxCMD     0x18
#define PxTFD     0x20
#define PxSIG     0x24
#define PxSSTS    0x28
#define PxSCTL    0x2C
#define PxSERR    0x30
#define PxSACT    0x34
#define PxCI      0x38

#define PxCMD_ST   (1u << 0)   /* Start */
#define PxCMD_FRE  (1u << 4)   /* FIS Receive Enable */
#define PxCMD_FR   (1u << 14)
#define PxCMD_CR   (1u << 15)

#define PxTFD_BSY  (1u << 7)
#define PxTFD_DRQ  (1u << 3)

/* Port signatures (PxSIG) */
#define SATA_SIG_ATA   0x00000101u

/* ATA commands */
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_FLUSH_EXT     0xEA

/* FIS types */
#define FIS_TYPE_REG_H2D 0x27

/* Command list header (32 bytes per AHCI 1.3) */
struct cmd_header {
    uint16_t flags;        /* cfl[4:0], a, w, p, r, b, c, prdtl is high 16 */
    uint16_t prdtl;
    uint32_t prdbc;        /* PRD byte count transferred */
    uint32_t ctba;         /* Command table base low */
    uint32_t ctbau;        /* Command table base high */
    uint32_t reserved[4];
};

/* PRDT entry (16 bytes) */
struct prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc_i;        /* bits[21:0] = byte count - 1; bit 31 = interrupt */
};

/* Command table (offset within page) */
#define CMD_TABLE_OFFSET 0x500

/* Command FIS H2D Register (offset within command table) */
struct fis_h2d {
    uint8_t  type;
    uint8_t  flags;        /* C bit = 0x80 to issue command */
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

/* ────────────────────────────────────────────────── */

static volatile uint32_t *abar;
static int port_idx = -1;
static volatile uint32_t *port_regs;     /* abar + 0x100 + idx*0x80, in dwords */

static uint8_t  *dma_page;               /* 4 KB virtual = phys */
static uint64_t  dma_page_phys;
static struct cmd_header *cmd_list;      /* dma_page + 0 */
static uint8_t  *recv_fis;               /* dma_page + 0x400 */
static uint8_t  *cmd_table;              /* dma_page + 0x500 */

static uint64_t total_blocks;
static uint32_t blk_size = 512;
static int      driver_ready;

/* ─── helpers ─── */

static inline uint32_t r(uint32_t off) { return *(volatile uint32_t *)((uint8_t *)abar + off); }
static inline void     w(uint32_t off, uint32_t v) { *(volatile uint32_t *)((uint8_t *)abar + off) = v; }
static inline uint32_t pr(uint32_t off) { return *(volatile uint32_t *)((uint8_t *)port_regs + off); }
static inline void     pw(uint32_t off, uint32_t v) { *(volatile uint32_t *)((uint8_t *)port_regs + off) = v; }

static void udelay(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++) __asm__ volatile("pause");
}

static int wait_clear(uint32_t off, uint32_t mask, int max_loops) {
    for (int i = 0; i < max_loops; i++) {
        if (!(pr(off) & mask)) return 0;
        udelay(1000);
    }
    return -1;
}

/* Stop the port (clear ST + FRE, wait for FR + CR to clear) */
static int port_stop(void) {
    uint32_t cmd = pr(PxCMD);
    cmd &= ~(PxCMD_ST | PxCMD_FRE);
    pw(PxCMD, cmd);
    if (wait_clear(PxCMD, PxCMD_FR | PxCMD_CR, 5000) < 0) return -1;
    return 0;
}

/* Start the port */
static void port_start(void) {
    while (pr(PxCMD) & PxCMD_CR) udelay(100);
    uint32_t cmd = pr(PxCMD);
    cmd |= PxCMD_FRE;
    pw(PxCMD, cmd);
    cmd |= PxCMD_ST;
    pw(PxCMD, cmd);
}

/* Build a command and issue it. Returns 0 on success, -1 on error. */
static int issue_cmd(uint8_t ata_cmd, uint64_t lba, uint16_t count,
                     void *buf, uint32_t buf_bytes, int write) {
    if (!driver_ready && ata_cmd != ATA_CMD_IDENTIFY) return -1;

    /* Wait for port to be idle */
    if (wait_clear(PxTFD, PxTFD_BSY | PxTFD_DRQ, 1000) < 0) return -1;

    /* Clear stale errors */
    pw(PxIS, 0xFFFFFFFFu);
    pw(PxSERR, 0xFFFFFFFFu);

    /* Set up command header for slot 0 */
    cmd_list[0].flags = (5 & 0x1F) | (write ? (1u << 6) : 0);  /* cfl=5, w bit */
    cmd_list[0].prdtl = (buf_bytes > 0) ? 1 : 0;
    cmd_list[0].prdbc = 0;
    cmd_list[0].ctba  = (uint32_t)(dma_page_phys + CMD_TABLE_OFFSET);
    cmd_list[0].ctbau = (uint32_t)((dma_page_phys + CMD_TABLE_OFFSET) >> 32);

    /* Zero command table */
    for (int i = 0; i < 256; i++) cmd_table[i] = 0;

    /* Build H2D FIS at start of command table */
    struct fis_h2d *fis = (struct fis_h2d *)cmd_table;
    fis->type     = FIS_TYPE_REG_H2D;
    fis->flags    = 0x80;        /* C bit — this is a command */
    fis->command  = ata_cmd;
    fis->device   = 1u << 6;     /* LBA mode */

    fis->lba0 = (uint8_t)(lba       & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

    fis->count_lo = (uint8_t)(count & 0xFF);
    fis->count_hi = (uint8_t)((count >> 8) & 0xFF);

    /* PRDT entry follows the FIS at offset 0x80 in the command table */
    if (buf_bytes > 0 && buf) {
        struct prdt_entry *prd = (struct prdt_entry *)(cmd_table + 0x80);
        prd->dba    = (uint32_t)((uint64_t)(uintptr_t)buf);
        prd->dbau   = (uint32_t)(((uint64_t)(uintptr_t)buf) >> 32);
        prd->reserved = 0;
        /* dbc is byte count minus 1, must be even. No-interrupt bit. */
        prd->dbc_i  = (buf_bytes - 1) & 0x3FFFFFu;
    }

    /* Issue: set bit 0 in PxCI */
    pw(PxCI, 1u);

    /* Poll for completion */
    for (int i = 0; i < 5000000; i++) {
        if (!(pr(PxCI) & 1u)) {
            /* Check for error in PxIS or PxTFD */
            if (pr(PxIS) & (1u << 30)) return -1;   /* Task File Error */
            if (pr(PxTFD) & 1u) return -1;          /* ERR bit in status */
            return 0;
        }
        udelay(10);
    }
    return -1;  /* timeout */
}

/* ─── public API ─── */

int ahci_init(void) {
    driver_ready = 0;

    /* Find AHCI controller: PCI class 0x01, subclass 0x06, prog-if 0x01 */
    int n = pci_device_count();
    struct pci_device *dev = 0;
    for (int i = 0; i < n; i++) {
        struct pci_device *d = pci_get_device(i);
        if (d && d->class_code == 0x01 && d->subclass == 0x06 && d->prog_if == 0x01) {
            dev = d; break;
        }
    }
    if (!dev) {
        kputs("ahci: no AHCI controller found\n");
        return -1;
    }

    /* BAR5 is the AHCI ABAR */
    uint32_t bar5 = dev->bar[5];
    if (bar5 & 1) { kputs("ahci: ABAR is I/O space — unexpected\n"); return -1; }
    uint64_t bar = bar5 & 0xFFFFFFF0u;
    if ((bar5 & 0x06) == 0x04 && dev->bar[6 - 1] /* unused */) {
        /* 64-bit BAR — ABAR is BAR5 only in spec, no high half. */
    }
    abar = (volatile uint32_t *)(uintptr_t)bar;

    /* Bus master + memory space enable in PCI command register */
    uint32_t cmd = pci_config_read32(dev->bus, dev->dev, dev->func, 0x04);
    pci_config_write32(dev->bus, dev->dev, dev->func, 0x04, cmd | 0x06);

    /* AHCI enable */
    w(HBA_GHC, r(HBA_GHC) | GHC_AE);

    /* Find the first port with an ATA drive attached */
    uint32_t pi = r(HBA_PI);
    int found = -1;
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        port_regs = (volatile uint32_t *)((uint8_t *)abar + HBA_PORTS_BASE + i * 0x80);
        uint32_t ssts = pr(PxSSTS);
        uint32_t det = ssts & 0xF;
        uint32_t ipm = (ssts >> 8) & 0xF;
        if (det != 3 || ipm != 1) continue;            /* not present + active */
        uint32_t sig = pr(PxSIG);
        if (sig == SATA_SIG_ATA) { found = i; break; } /* skip ATAPI/PM/SEMB */
    }
    if (found < 0) { kputs("ahci: no SATA ATA drive on any port\n"); return -1; }
    port_idx = found;
    port_regs = (volatile uint32_t *)((uint8_t *)abar + HBA_PORTS_BASE + found * 0x80);

    kputs("ahci: SATA drive on port ");
    kput_dec(found);
    kputs("\n");

    /* Allocate one 4 KB physical page for command list + FIS + command table */
    uint64_t phys = pmm_alloc_contiguous(1);
    if (!phys) { kputs("ahci: pmm alloc failed\n"); return -1; }
    dma_page = (uint8_t *)(uintptr_t)phys;
    dma_page_phys = phys;
    for (int i = 0; i < 4096; i++) dma_page[i] = 0;

    cmd_list  = (struct cmd_header *)(dma_page);
    recv_fis  = dma_page + 0x400;
    cmd_table = dma_page + CMD_TABLE_OFFSET;

    /* Stop port, set base addresses, start port */
    if (port_stop() < 0) { kputs("ahci: port_stop timeout\n"); return -1; }

    pw(PxCLB,  (uint32_t)(dma_page_phys & 0xFFFFFFFFu));
    pw(PxCLBU, (uint32_t)(dma_page_phys >> 32));
    pw(PxFB,   (uint32_t)((dma_page_phys + 0x400) & 0xFFFFFFFFu));
    pw(PxFBU,  (uint32_t)((dma_page_phys + 0x400) >> 32));

    pw(PxIS, 0xFFFFFFFFu);
    pw(PxSERR, 0xFFFFFFFFu);
    port_start();

    /* IDENTIFY DEVICE — fills 512 bytes with drive info */
    static uint8_t id_buf[512] __attribute__((aligned(2)));
    if (issue_cmd(ATA_CMD_IDENTIFY, 0, 1, id_buf, 512, 0) < 0) {
        kputs("ahci: IDENTIFY failed\n");
        return -1;
    }

    /* Words 100-103: Total Number of User Addressable Sectors (LBA48) */
    uint64_t lba48 = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t w16 = ((uint16_t)id_buf[200 + i*2 + 1] << 8) | id_buf[200 + i*2];
        lba48 |= (uint64_t)w16 << (i * 16);
    }
    total_blocks = lba48;

    /* Word 106: physical/logical sector size info; default 512 */
    blk_size = 512;
    driver_ready = 1;

    kputs("ahci: ready, ");
    kput_dec(total_blocks);
    kputs(" sectors (");
    kput_dec((uint32_t)(total_blocks * 512 / (1024 * 1024)));
    kputs(" MB)\n");
    return 0;
}

int ahci_read(uint64_t lba, uint32_t count, void *buf) {
    if (!driver_ready) return -1;
    if (count == 0) return 0;
    if (count > 65535) return -1;
    return issue_cmd(ATA_CMD_READ_DMA_EXT, lba, (uint16_t)count, buf, count * blk_size, 0);
}

int ahci_write(uint64_t lba, uint32_t count, const void *buf) {
    if (!driver_ready) return -1;
    if (count == 0) return 0;
    if (count > 65535) return -1;
    return issue_cmd(ATA_CMD_WRITE_DMA_EXT, lba, (uint16_t)count, (void *)buf, count * blk_size, 1);
}

int ahci_flush(void) {
    if (!driver_ready) return -1;
    return issue_cmd(ATA_CMD_FLUSH_EXT, 0, 0, 0, 0, 0);
}

uint64_t ahci_num_blocks(void) { return total_blocks; }
uint32_t ahci_block_size(void) { return blk_size; }
int      ahci_ready(void)      { return driver_ready; }
