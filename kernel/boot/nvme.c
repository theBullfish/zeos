/*
 * Zeos -- NVMe Block Device Driver
 *
 * Bare-metal NVMe 1.0+ driver. Polling only (no MSI/MSI-X).
 * Single admin queue + single I/O queue pair.
 *
 * Flow:
 *   1. PCI scan for class 0x01 subclass 0x08
 *   2. Map BAR0 (MMIO registers)
 *   3. Controller reset + admin queue setup
 *   4. Identify Controller / Identify Namespace
 *   5. Create I/O Completion Queue + I/O Submission Queue
 *   6. Ready for nvme_read / nvme_write
 */

#include "nvme.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "heap.h"
#include "kprint.h"

/* ── String / Memory helpers (freestanding) ─────────────────────── */

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);

/* ── NVMe Register Offsets ──────────────────────────────────────── */

#define NVME_REG_CAP        0x00    /* Controller Capabilities (64-bit) */
#define NVME_REG_VS         0x08    /* Version */
#define NVME_REG_INTMS      0x0C    /* Interrupt Mask Set */
#define NVME_REG_INTMC      0x10    /* Interrupt Mask Clear */
#define NVME_REG_CC         0x14    /* Controller Configuration */
#define NVME_REG_CSTS       0x1C    /* Controller Status */
#define NVME_REG_AQA        0x24    /* Admin Queue Attributes */
#define NVME_REG_ASQ        0x28    /* Admin Submission Queue Base (64-bit) */
#define NVME_REG_ACQ        0x30    /* Admin Completion Queue Base (64-bit) */

/* Doorbell base offset */
#define NVME_REG_SQ0TDBL    0x1000

/* CC bits */
#define NVME_CC_EN          (1U << 0)
#define NVME_CC_CSS_NVM     (0U << 4)   /* NVM command set */
#define NVME_CC_MPS_4K      (0U << 7)   /* Memory Page Size = 4K (2^(12+0)) */
#define NVME_CC_AMS_RR      (0U << 11)  /* Round Robin arbitration */
#define NVME_CC_SHN_NONE    (0U << 14)
#define NVME_CC_IOSQES      (6U << 16)  /* I/O SQ entry size = 2^6 = 64 bytes */
#define NVME_CC_IOCQES      (4U << 20)  /* I/O CQ entry size = 2^4 = 16 bytes */

/* CSTS bits */
#define NVME_CSTS_RDY       (1U << 0)
#define NVME_CSTS_CFS       (1U << 1)   /* Controller Fatal Status */

/* CAP fields */
#define NVME_CAP_MQES(cap)  ((uint16_t)((cap) & 0xFFFF))        /* Max Queue Entries Supported */
#define NVME_CAP_TO(cap)    ((uint8_t)(((cap) >> 24) & 0xFF))   /* Timeout (500ms units) */
#define NVME_CAP_DSTRD(cap) ((uint8_t)(((cap) >> 32) & 0xF))    /* Doorbell Stride */
#define NVME_CAP_MPSMIN(cap) ((uint8_t)(((cap) >> 48) & 0xF))

/* NVMe Admin Opcodes */
#define NVME_ADMIN_IDENTIFY         0x06
#define NVME_ADMIN_CREATE_IO_CQ     0x05
#define NVME_ADMIN_CREATE_IO_SQ     0x01

/* NVMe I/O Opcodes (NVM Command Set) */
#define NVME_IO_FLUSH       0x00
#define NVME_IO_WRITE       0x01
#define NVME_IO_READ        0x02

/* ── NVMe Command (Submission Queue Entry — 64 bytes) ───────────── */

typedef struct {
    /* Dword 0 */
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t command_id;

    /* Dword 1 */
    uint32_t nsid;

    /* Dwords 2-3 */
    uint64_t reserved;

    /* Dwords 4-5: Metadata pointer */
    uint64_t mptr;

    /* Dwords 6-9: Data pointer (PRP1 + PRP2) */
    uint64_t prp1;
    uint64_t prp2;

    /* Dwords 10-15: Command-specific */
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_cmd_t;

/* ── NVMe Completion Queue Entry (16 bytes) ─────────────────────── */

typedef struct {
    uint32_t result;        /* Command-specific result */
    uint32_t reserved;
    uint16_t sq_head;       /* SQ Head pointer */
    uint16_t sq_id;         /* SQ Identifier */
    uint16_t command_id;
    uint16_t status;        /* Status field (bit 0 = phase tag) */
} __attribute__((packed)) nvme_cqe_t;

/* ── Register access helpers ────────────────────────────────────── */

static nvme_dev_t nvme_dev;

static inline uint32_t nvme_read32(uint32_t offset)
{
    return *(volatile uint32_t *)((uint8_t *)nvme_dev.regs + offset);
}

static inline void nvme_write32(uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)((uint8_t *)nvme_dev.regs + offset) = val;
}

static inline uint64_t nvme_read64(uint32_t offset)
{
    uint32_t lo = nvme_read32(offset);
    uint32_t hi = nvme_read32(offset + 4);
    return ((uint64_t)hi << 32) | lo;
}

static inline void nvme_write64(uint32_t offset, uint64_t val)
{
    nvme_write32(offset, (uint32_t)(val & 0xFFFFFFFF));
    nvme_write32(offset + 4, (uint32_t)(val >> 32));
}

/* ── Doorbell helpers ───────────────────────────────────────────── */

static inline void nvme_ring_sq_doorbell(int qid, int tail)
{
    uint32_t offset = NVME_REG_SQ0TDBL + (2 * qid) * nvme_dev.doorbell_stride;
    nvme_write32(offset, (uint32_t)tail);
}

static inline void nvme_ring_cq_doorbell(int qid, int head)
{
    uint32_t offset = NVME_REG_SQ0TDBL + (2 * qid + 1) * nvme_dev.doorbell_stride;
    nvme_write32(offset, (uint32_t)head);
}

/* ── Polling timeout ────────────────────────────────────────────── */

/* Simple spin-wait. No timer yet — just a large iteration count.
 * At ~1 GHz that's roughly 10 seconds. Good enough for Alpha. */
#define NVME_POLL_LIMIT  100000000U

static void spin_delay(uint32_t count)
{
    for (volatile uint32_t i = 0; i < count; i++)
        ;
}

/* ── Admin command submission ───────────────────────────────────── */

static uint16_t admin_cmd_id;

/*
 * Submit a command to the Admin Submission Queue, ring doorbell,
 * and poll the Admin Completion Queue for the result.
 * Returns status (0 = success).
 */
static int nvme_admin_cmd(nvme_cmd_t *cmd)
{
    /* Place command in ASQ at current tail */
    nvme_cmd_t *asq = (nvme_cmd_t *)nvme_dev.asq;
    cmd->command_id = admin_cmd_id++;

    memcpy(&asq[nvme_dev.asq_tail], cmd, sizeof(nvme_cmd_t));

    /* Advance tail */
    nvme_dev.asq_tail = (nvme_dev.asq_tail + 1) % NVME_ADMIN_QSIZE;

    /* Ring admin SQ doorbell (queue 0) */
    nvme_ring_sq_doorbell(0, nvme_dev.asq_tail);

    /* Poll ACQ for completion */
    nvme_cqe_t *acq = (nvme_cqe_t *)nvme_dev.acq;

    for (uint32_t i = 0; i < NVME_POLL_LIMIT; i++) {
        nvme_cqe_t *cqe = &acq[nvme_dev.acq_head];

        /* Check phase bit (bit 0 of status) */
        int phase = cqe->status & 1;
        if (phase != nvme_dev.acq_phase)
            continue;

        /* Got a completion */
        uint16_t status = (cqe->status >> 1) & 0x7FFF;

        /* Advance ACQ head */
        nvme_dev.acq_head = (nvme_dev.acq_head + 1) % NVME_ADMIN_QSIZE;
        if (nvme_dev.acq_head == 0)
            nvme_dev.acq_phase ^= 1;

        /* Ring admin CQ doorbell */
        nvme_ring_cq_doorbell(0, nvme_dev.acq_head);

        return (int)status;
    }

    kputs("[nvme] admin command timeout\n");
    return -1;
}

/* ── I/O command submission ─────────────────────────────────────── */

static uint16_t io_cmd_id;

static int nvme_io_cmd(nvme_cmd_t *cmd)
{
    nvme_cmd_t *iosq = (nvme_cmd_t *)nvme_dev.iosq;
    cmd->command_id = io_cmd_id++;

    memcpy(&iosq[nvme_dev.iosq_tail], cmd, sizeof(nvme_cmd_t));

    /* Advance tail */
    nvme_dev.iosq_tail = (nvme_dev.iosq_tail + 1) % NVME_IO_QSIZE;

    /* Ring I/O SQ doorbell (queue 1) */
    nvme_ring_sq_doorbell(1, nvme_dev.iosq_tail);

    /* Poll I/O CQ for completion */
    nvme_cqe_t *iocq = (nvme_cqe_t *)nvme_dev.iocq;

    for (uint32_t i = 0; i < NVME_POLL_LIMIT; i++) {
        nvme_cqe_t *cqe = &iocq[nvme_dev.iocq_head];

        int phase = cqe->status & 1;
        if (phase != nvme_dev.iocq_phase)
            continue;

        uint16_t status = (cqe->status >> 1) & 0x7FFF;

        nvme_dev.iocq_head = (nvme_dev.iocq_head + 1) % NVME_IO_QSIZE;
        if (nvme_dev.iocq_head == 0)
            nvme_dev.iocq_phase ^= 1;

        /* Ring I/O CQ doorbell */
        nvme_ring_cq_doorbell(1, nvme_dev.iocq_head);

        return (int)status;
    }

    kputs("[nvme] I/O command timeout\n");
    return -1;
}

/* ── Copy trimmed string from NVMe identify data ────────────────── */

static void nvme_copy_string(char *dst, const char *src, int len)
{
    /* NVMe strings are space-padded, not null-terminated.
     * Also stored as pairs of bytes swapped (ASCII in each word). */
    for (int i = 0; i < len; i++)
        dst[i] = src[i];
    dst[len] = '\0';

    /* Trim trailing spaces */
    for (int i = len - 1; i >= 0 && dst[i] == ' '; i--)
        dst[i] = '\0';
}

/* ── PCI: Enable bus mastering ──────────────────────────────────── */

static void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t cmd = pci_config_read32(bus, dev, func, 0x04);
    cmd |= (1 << 2);   /* Bus Master Enable */
    cmd |= (1 << 1);   /* Memory Space Enable */
    pci_config_write32(bus, dev, func, 0x04, cmd);
}

/* ── PCI: Read full 64-bit BAR ──────────────────────────────────── */

static uint64_t pci_read_bar64(uint8_t bus, uint8_t dev, uint8_t func, int bar_idx)
{
    uint32_t bar_lo = pci_config_read32(bus, dev, func, 0x10 + bar_idx * 4);
    uint64_t addr;

    if (bar_lo & 0x04) {
        /* 64-bit BAR: spans two BARs */
        uint32_t bar_hi = pci_config_read32(bus, dev, func, 0x10 + (bar_idx + 1) * 4);
        addr = ((uint64_t)bar_hi << 32) | (bar_lo & ~0xFULL);
    } else {
        addr = bar_lo & ~0xFULL;
    }
    return addr;
}

/* ── Initialization ─────────────────────────────────────────────── */

int nvme_init(void)
{
    memset(&nvme_dev, 0, sizeof(nvme_dev));
    admin_cmd_id = 0;
    io_cmd_id = 0;

    kputs("[nvme] scanning PCI for NVMe controller...\n");

    /* ── Find NVMe device ─────────────────────────────────────────── */
    struct pci_device *pdev = 0;
    int count = pci_device_count();

    for (int i = 0; i < count; i++) {
        struct pci_device *d = pci_get_device(i);
        if (d && d->class_code == 0x01 && d->subclass == 0x08) {
            pdev = d;
            break;
        }
    }

    if (!pdev) {
        kputs("[nvme] no NVMe controller found\n");
        return -1;
    }

    nvme_dev.pci_bus  = pdev->bus;
    nvme_dev.pci_dev  = pdev->dev;
    nvme_dev.pci_func = pdev->func;

    kputs("[nvme] found NVMe at PCI ");
    kput_hex(pdev->bus);
    kputs(":");
    kput_hex(pdev->dev);
    kputs(".");
    kput_hex(pdev->func);
    kputs("  vendor=");
    kput_hex(pdev->vendor_id);
    kputs(" device=");
    kput_hex(pdev->device_id);
    kputs("\n");

    /* ── Enable bus mastering + memory space ──────────────────────── */
    pci_enable_bus_master(pdev->bus, pdev->dev, pdev->func);

    /* ── Map BAR0 ─────────────────────────────────────────────────── */
    uint64_t bar0 = pci_read_bar64(pdev->bus, pdev->dev, pdev->func, 0);
    if (bar0 == 0) {
        kputs("[nvme] BAR0 is zero -- cannot map registers\n");
        return -1;
    }

    nvme_dev.bar0_phys = bar0;
    /* Identity-mapped in Z-OS (no MMU paging yet) */
    nvme_dev.regs = (volatile uint32_t *)(uintptr_t)bar0;

    kputs("[nvme] BAR0 = ");
    kput_hex(bar0);
    kputs("\n");

    /* ── Read capabilities ────────────────────────────────────────── */
    uint64_t cap = nvme_read64(NVME_REG_CAP);
    uint32_t vs  = nvme_read32(NVME_REG_VS);
    uint16_t mqes = NVME_CAP_MQES(cap);
    uint8_t  timeout_500ms = NVME_CAP_TO(cap);
    uint8_t  dstrd = NVME_CAP_DSTRD(cap);

    nvme_dev.doorbell_stride = 4 << dstrd;  /* 4 * 2^DSTRD bytes */

    kputs("[nvme] version=");
    kput_hex(vs);
    kputs(" mqes=");
    kput_dec(mqes + 1);
    kputs(" timeout=");
    kput_dec(timeout_500ms * 500);
    kputs("ms dstrd=");
    kput_dec(dstrd);
    kputs("\n");

    /* ── Disable controller ───────────────────────────────────────── */
    uint32_t cc = nvme_read32(NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        nvme_write32(NVME_REG_CC, cc & ~NVME_CC_EN);

        /* Wait for CSTS.RDY = 0 */
        for (uint32_t i = 0; i < NVME_POLL_LIMIT; i++) {
            if (!(nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY))
                break;
            if (i == NVME_POLL_LIMIT - 1) {
                kputs("[nvme] timeout waiting for controller disable\n");
                return -1;
            }
        }
    }
    kputs("[nvme] controller disabled\n");

    /* ── Allocate Admin Queues (page-aligned, 4KB each) ───────────── */
    uint64_t asq_phys = pmm_alloc();
    uint64_t acq_phys = pmm_alloc();
    if (!asq_phys || !acq_phys) {
        kputs("[nvme] failed to allocate admin queues\n");
        return -1;
    }

    nvme_dev.asq = (void *)(uintptr_t)asq_phys;
    nvme_dev.acq = (void *)(uintptr_t)acq_phys;
    nvme_dev.asq_phys = asq_phys;
    nvme_dev.acq_phys = acq_phys;

    memset(nvme_dev.asq, 0, 4096);
    memset(nvme_dev.acq, 0, 4096);

    nvme_dev.asq_tail  = 0;
    nvme_dev.acq_head  = 0;
    nvme_dev.acq_phase = 1;  /* Phase starts at 1 for a zeroed CQ */

    /* ── Configure Admin Queue Attributes ─────────────────────────── */
    /* AQA: bits [27:16] = ACQS-1, bits [11:0] = ASQS-1 */
    uint32_t aqa = ((NVME_ADMIN_QSIZE - 1) << 16) | (NVME_ADMIN_QSIZE - 1);
    nvme_write32(NVME_REG_AQA, aqa);
    nvme_write64(NVME_REG_ASQ, asq_phys);
    nvme_write64(NVME_REG_ACQ, acq_phys);

    /* ── Enable controller ────────────────────────────────────────── */
    cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K |
         NVME_CC_AMS_RR | NVME_CC_SHN_NONE |
         NVME_CC_IOSQES | NVME_CC_IOCQES;
    nvme_write32(NVME_REG_CC, cc);

    /* Wait for CSTS.RDY = 1 */
    for (uint32_t i = 0; i < NVME_POLL_LIMIT; i++) {
        uint32_t csts = nvme_read32(NVME_REG_CSTS);
        if (csts & NVME_CSTS_CFS) {
            kputs("[nvme] controller fatal status during enable\n");
            return -1;
        }
        if (csts & NVME_CSTS_RDY)
            break;
        if (i == NVME_POLL_LIMIT - 1) {
            kputs("[nvme] timeout waiting for controller enable\n");
            return -1;
        }
    }
    kputs("[nvme] controller enabled\n");

    /* ── Mask all interrupts (polling only) ───────────────────────── */
    nvme_write32(NVME_REG_INTMS, 0xFFFFFFFF);

    /* ── Identify Controller ──────────────────────────────────────── */
    uint64_t identify_phys = pmm_alloc();
    if (!identify_phys) {
        kputs("[nvme] failed to allocate identify buffer\n");
        return -1;
    }
    void *identify_buf = (void *)(uintptr_t)identify_phys;
    memset(identify_buf, 0, 4096);

    {
        nvme_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = NVME_ADMIN_IDENTIFY;
        cmd.nsid   = 0;
        cmd.prp1   = identify_phys;
        cmd.cdw10  = 1;  /* CNS = 1: Identify Controller */

        int status = nvme_admin_cmd(&cmd);
        if (status != 0) {
            kputs("[nvme] Identify Controller failed, status=");
            kput_hex(status);
            kputs("\n");
            pmm_free(identify_phys);
            return -1;
        }
    }

    /* Parse Identify Controller data */
    uint8_t *id = (uint8_t *)identify_buf;
    nvme_copy_string(nvme_dev.serial, (char *)&id[4], 20);
    nvme_copy_string(nvme_dev.model,  (char *)&id[24], 40);

    kputs("[nvme] serial: ");
    kputs(nvme_dev.serial);
    kputs("\n");
    kputs("[nvme] model:  ");
    kputs(nvme_dev.model);
    kputs("\n");

    pmm_free(identify_phys);

    /* ── Identify Namespace (NSID=1) ──────────────────────────────── */
    uint64_t nsid_phys = pmm_alloc();
    if (!nsid_phys) {
        kputs("[nvme] failed to allocate namespace identify buffer\n");
        return -1;
    }
    void *nsid_buf = (void *)(uintptr_t)nsid_phys;
    memset(nsid_buf, 0, 4096);

    nvme_dev.nsid = 1;

    {
        nvme_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = NVME_ADMIN_IDENTIFY;
        cmd.nsid   = 1;
        cmd.prp1   = nsid_phys;
        cmd.cdw10  = 0;  /* CNS = 0: Identify Namespace */

        int status = nvme_admin_cmd(&cmd);
        if (status != 0) {
            kputs("[nvme] Identify Namespace failed, status=");
            kput_hex(status);
            kputs("\n");
            pmm_free(nsid_phys);
            return -1;
        }
    }

    /* Parse Identify Namespace data */
    uint8_t *ns = (uint8_t *)nsid_buf;

    /* NSZE: Namespace Size (bytes 0-7) = total LBAs */
    nvme_dev.num_blocks = *(uint64_t *)&ns[0];

    /* FLBAS: byte 26, bits [3:0] = index into LBA Format table */
    uint8_t flbas = ns[26] & 0x0F;

    /* LBA Format table starts at byte 128, each entry is 4 bytes.
     * Bits [23:16] = LBADS (LBA Data Size as power of 2) */
    uint32_t lbaf = *(uint32_t *)&ns[128 + flbas * 4];
    uint8_t lbads = (lbaf >> 16) & 0xFF;
    nvme_dev.block_size = 1U << lbads;

    kputs("[nvme] namespace 1: ");
    kput_dec(nvme_dev.num_blocks);
    kputs(" blocks x ");
    kput_dec(nvme_dev.block_size);
    kputs(" bytes = ");
    kput_dec((nvme_dev.num_blocks * nvme_dev.block_size) / (1024 * 1024));
    kputs(" MB\n");

    pmm_free(nsid_phys);

    /* ── Create I/O Completion Queue (ID=1) ───────────────────────── */
    uint64_t iocq_phys = pmm_alloc();
    if (!iocq_phys) {
        kputs("[nvme] failed to allocate I/O CQ\n");
        return -1;
    }
    nvme_dev.iocq = (void *)(uintptr_t)iocq_phys;
    nvme_dev.iocq_phys = iocq_phys;
    memset(nvme_dev.iocq, 0, 4096);
    nvme_dev.iocq_head  = 0;
    nvme_dev.iocq_phase = 1;

    {
        nvme_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = NVME_ADMIN_CREATE_IO_CQ;
        cmd.prp1   = iocq_phys;
        /* CDW10: [31:16] = QSIZE-1, [15:0] = QID */
        cmd.cdw10  = ((NVME_IO_QSIZE - 1) << 16) | 1;
        /* CDW11: [1] = IEN (0=no interrupts), [0] = PC (1=physically contiguous) */
        cmd.cdw11  = 1;  /* Physically contiguous, no interrupts */

        int status = nvme_admin_cmd(&cmd);
        if (status != 0) {
            kputs("[nvme] Create I/O CQ failed, status=");
            kput_hex(status);
            kputs("\n");
            return -1;
        }
    }
    kputs("[nvme] I/O completion queue created\n");

    /* ── Create I/O Submission Queue (ID=1) ───────────────────────── */
    uint64_t iosq_phys = pmm_alloc();
    if (!iosq_phys) {
        kputs("[nvme] failed to allocate I/O SQ\n");
        return -1;
    }
    nvme_dev.iosq = (void *)(uintptr_t)iosq_phys;
    nvme_dev.iosq_phys = iosq_phys;
    memset(nvme_dev.iosq, 0, 4096);
    nvme_dev.iosq_tail = 0;

    {
        nvme_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = NVME_ADMIN_CREATE_IO_SQ;
        cmd.prp1   = iosq_phys;
        /* CDW10: [31:16] = QSIZE-1, [15:0] = QID */
        cmd.cdw10  = ((NVME_IO_QSIZE - 1) << 16) | 1;
        /* CDW11: [31:16] = CQID, [2:1] = QPRIO, [0] = PC */
        cmd.cdw11  = (1 << 16) | 1;  /* Associated with CQ 1, physically contiguous */

        int status = nvme_admin_cmd(&cmd);
        if (status != 0) {
            kputs("[nvme] Create I/O SQ failed, status=");
            kput_hex(status);
            kputs("\n");
            return -1;
        }
    }
    kputs("[nvme] I/O submission queue created\n");

    /* ── Ready ────────────────────────────────────────────────────── */
    nvme_dev.ready = 1;
    kputs("[nvme] driver ready\n");

    return 0;
}

/* ── Block Read ─────────────────────────────────────────────────── */

int nvme_read(uint64_t lba, uint32_t count, void *buf)
{
    if (!nvme_dev.ready)
        return -1;
    if (count == 0)
        return 0;

    uint32_t bytes = count * nvme_dev.block_size;

    /* NVMe DMA needs page-aligned physical addresses.
     * Allocate a DMA buffer, do the transfer, then copy out. */
    uint32_t pages_needed = (bytes + 4095) / 4096;
    uint64_t dma_phys;

    if (pages_needed == 1) {
        dma_phys = pmm_alloc();
    } else {
        dma_phys = pmm_alloc_contiguous(pages_needed);
    }

    if (!dma_phys) {
        kputs("[nvme] read: failed to allocate DMA buffer\n");
        return -1;
    }

    void *dma_buf = (void *)(uintptr_t)dma_phys;
    memset(dma_buf, 0, (uint64_t)pages_needed * 4096);

    /* Build NVMe Read command */
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_IO_READ;
    cmd.nsid   = nvme_dev.nsid;
    cmd.prp1   = dma_phys;

    /* PRP2: if transfer spans two pages, point to second page.
     * For transfers > 2 pages we would need a PRP list, but
     * Alpha caps at what fits in two PRP entries. */
    if (pages_needed == 2) {
        cmd.prp2 = dma_phys + 4096;
    } else if (pages_needed > 2) {
        /* Build a PRP list for multi-page transfers */
        uint64_t prp_list_phys = pmm_alloc();
        if (!prp_list_phys) {
            kputs("[nvme] read: failed to allocate PRP list\n");
            goto read_free;
        }
        uint64_t *prp_list = (uint64_t *)(uintptr_t)prp_list_phys;
        for (uint32_t i = 1; i < pages_needed; i++) {
            prp_list[i - 1] = dma_phys + (uint64_t)i * 4096;
        }
        cmd.prp2 = prp_list_phys;
    }

    /* CDW10-11: Starting LBA (64-bit) */
    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11 = (uint32_t)(lba >> 32);

    /* CDW12: [15:0] = Number of Logical Blocks - 1 */
    cmd.cdw12 = count - 1;

    int status = nvme_io_cmd(&cmd);

    if (status == 0) {
        memcpy(buf, dma_buf, bytes);
    } else {
        kputs("[nvme] read failed at LBA ");
        kput_hex(lba);
        kputs(" status=");
        kput_hex(status);
        kputs("\n");
    }

    /* Free PRP list if we allocated one */
    if (pages_needed > 2 && cmd.prp2) {
        pmm_free(cmd.prp2);
    }

read_free:
    /* Free DMA buffer */
    for (uint32_t i = 0; i < pages_needed; i++) {
        pmm_free(dma_phys + (uint64_t)i * 4096);
    }

    return (status == 0) ? 0 : -1;
}

/* ── Block Write ────────────────────────────────────────────────── */

int nvme_write(uint64_t lba, uint32_t count, const void *buf)
{
    if (!nvme_dev.ready)
        return -1;
    if (count == 0)
        return 0;

    uint32_t bytes = count * nvme_dev.block_size;
    uint32_t pages_needed = (bytes + 4095) / 4096;
    uint64_t dma_phys;

    if (pages_needed == 1) {
        dma_phys = pmm_alloc();
    } else {
        dma_phys = pmm_alloc_contiguous(pages_needed);
    }

    if (!dma_phys) {
        kputs("[nvme] write: failed to allocate DMA buffer\n");
        return -1;
    }

    void *dma_buf = (void *)(uintptr_t)dma_phys;

    /* Copy data into DMA buffer */
    memcpy(dma_buf, buf, bytes);

    /* Build NVMe Write command */
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_IO_WRITE;
    cmd.nsid   = nvme_dev.nsid;
    cmd.prp1   = dma_phys;

    if (pages_needed == 2) {
        cmd.prp2 = dma_phys + 4096;
    } else if (pages_needed > 2) {
        uint64_t prp_list_phys = pmm_alloc();
        if (!prp_list_phys) {
            kputs("[nvme] write: failed to allocate PRP list\n");
            goto write_free;
        }
        uint64_t *prp_list = (uint64_t *)(uintptr_t)prp_list_phys;
        for (uint32_t i = 1; i < pages_needed; i++) {
            prp_list[i - 1] = dma_phys + (uint64_t)i * 4096;
        }
        cmd.prp2 = prp_list_phys;
    }

    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1;

    int status = nvme_io_cmd(&cmd);

    if (status != 0) {
        kputs("[nvme] write failed at LBA ");
        kput_hex(lba);
        kputs(" status=");
        kput_hex(status);
        kputs("\n");
    }

    if (pages_needed > 2 && cmd.prp2) {
        pmm_free(cmd.prp2);
    }

write_free:
    for (uint32_t i = 0; i < pages_needed; i++) {
        pmm_free(dma_phys + (uint64_t)i * 4096);
    }

    return (status == 0) ? 0 : -1;
}

/* ── Flush ──────────────────────────────────────────────────────── */

int nvme_flush(void)
{
    if (!nvme_dev.ready)
        return -1;

    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_IO_FLUSH;
    cmd.nsid   = nvme_dev.nsid;

    int status = nvme_io_cmd(&cmd);
    if (status != 0) {
        kputs("[nvme] flush failed, status=");
        kput_hex(status);
        kputs("\n");
        return -1;
    }

    return 0;
}

/* ── Get device info ────────────────────────────────────────────── */

const nvme_dev_t *nvme_get_dev(void)
{
    if (!nvme_dev.ready)
        return 0;
    return &nvme_dev;
}
