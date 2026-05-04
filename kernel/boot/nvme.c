/*
 * Zeos -- NVMe Block Device Driver (multi-drive, multi-queue)
 *
 * Polling only. Walks the PCI bus and brings up every NVMe controller
 * it finds. Each drive gets an admin queue plus up to NVME_IO_QUEUES
 * I/O queue pairs (capped by MQES). Round-robin I/O across queues.
 * If extra queues won't come up the drive falls back to whatever did,
 * never less than 1.
 */

#include "nvme.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "heap.h"
#include "kprint.h"
#include "vmm.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);

#define NVME_REG_CAP        0x00
#define NVME_REG_VS         0x08
#define NVME_REG_INTMS      0x0C
#define NVME_REG_INTMC      0x10
#define NVME_REG_CC         0x14
#define NVME_REG_CSTS       0x1C
#define NVME_REG_AQA        0x24
#define NVME_REG_ASQ        0x28
#define NVME_REG_ACQ        0x30
#define NVME_REG_SQ0TDBL    0x1000

#define NVME_CC_EN          (1U << 0)
#define NVME_CC_CSS_NVM     (0U << 4)
#define NVME_CC_MPS_4K      (0U << 7)
#define NVME_CC_AMS_RR      (0U << 11)
#define NVME_CC_SHN_NONE    (0U << 14)
#define NVME_CC_IOSQES      (6U << 16)
#define NVME_CC_IOCQES      (4U << 20)

#define NVME_CSTS_RDY       (1U << 0)
#define NVME_CSTS_CFS       (1U << 1)

#define NVME_CAP_MQES(cap)  ((uint16_t)((cap) & 0xFFFF))
#define NVME_CAP_DSTRD(cap) ((uint8_t)(((cap) >> 32) & 0xF))

#define NVME_ADMIN_IDENTIFY         0x06
#define NVME_ADMIN_CREATE_IO_CQ     0x05
#define NVME_ADMIN_CREATE_IO_SQ     0x01

#define NVME_IO_FLUSH       0x00
#define NVME_IO_WRITE       0x01
#define NVME_IO_READ        0x02

typedef struct {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t command_id;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed)) nvme_cmd_t;

typedef struct {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;
} __attribute__((packed)) nvme_cqe_t;

static nvme_dev_t g_drives[NVME_MAX_DRIVES];
static int g_drive_count = 0;

#define NVME_POLL_LIMIT  100000000U

static inline uint32_t nvme_read32(nvme_dev_t *d, uint32_t off)
{ return *(volatile uint32_t *)((uint8_t *)d->regs + off); }
static inline void nvme_write32(nvme_dev_t *d, uint32_t off, uint32_t v)
{ *(volatile uint32_t *)((uint8_t *)d->regs + off) = v; }
static inline uint64_t nvme_read64(nvme_dev_t *d, uint32_t off)
{ uint32_t lo = nvme_read32(d, off); uint32_t hi = nvme_read32(d, off + 4); return ((uint64_t)hi << 32) | lo; }
static inline void nvme_write64(nvme_dev_t *d, uint32_t off, uint64_t v)
{ nvme_write32(d, off, (uint32_t)v); nvme_write32(d, off + 4, (uint32_t)(v >> 32)); }

static inline void ring_sq(nvme_dev_t *d, int qid, int tail)
{ nvme_write32(d, NVME_REG_SQ0TDBL + (2 * qid) * d->doorbell_stride, (uint32_t)tail); }
static inline void ring_cq(nvme_dev_t *d, int qid, int head)
{ nvme_write32(d, NVME_REG_SQ0TDBL + (2 * qid + 1) * d->doorbell_stride, (uint32_t)head); }

static int admin_cmd(nvme_dev_t *d, nvme_cmd_t *cmd)
{
    nvme_cmd_t *asq = (nvme_cmd_t *)d->asq;
    cmd->command_id = d->admin_cmd_id++;
    memcpy(&asq[d->asq_tail], cmd, sizeof(nvme_cmd_t));
    d->asq_tail = (d->asq_tail + 1) % NVME_ADMIN_QSIZE;
    ring_sq(d, 0, d->asq_tail);
    nvme_cqe_t *acq = (nvme_cqe_t *)d->acq;
    for (uint32_t i = 0; i < NVME_POLL_LIMIT; i++) {
        nvme_cqe_t *cqe = &acq[d->acq_head];
        if ((cqe->status & 1) != d->acq_phase) continue;
        uint16_t status = (cqe->status >> 1) & 0x7FFF;
        d->acq_head = (d->acq_head + 1) % NVME_ADMIN_QSIZE;
        if (d->acq_head == 0) d->acq_phase ^= 1;
        ring_cq(d, 0, d->acq_head);
        return (int)status;
    }
    kputs("[nvme] admin timeout\n");
    return -1;
}

static int io_cmd(nvme_dev_t *d, nvme_ioq_t *q, nvme_cmd_t *cmd)
{
    nvme_cmd_t *iosq = (nvme_cmd_t *)q->sq;
    cmd->command_id = q->cmd_id++;
    memcpy(&iosq[q->sq_tail], cmd, sizeof(nvme_cmd_t));
    q->sq_tail = (q->sq_tail + 1) % NVME_IO_QSIZE;
    ring_sq(d, q->qid, q->sq_tail);
    nvme_cqe_t *iocq = (nvme_cqe_t *)q->cq;
    for (uint32_t i = 0; i < NVME_POLL_LIMIT; i++) {
        nvme_cqe_t *cqe = &iocq[q->cq_head];
        if ((cqe->status & 1) != q->cq_phase) continue;
        uint16_t status = (cqe->status >> 1) & 0x7FFF;
        q->cq_head = (q->cq_head + 1) % NVME_IO_QSIZE;
        if (q->cq_head == 0) q->cq_phase ^= 1;
        ring_cq(d, q->qid, q->cq_head);
        return (int)status;
    }
    kputs("[nvme] I/O timeout\n");
    return -1;
}

static nvme_ioq_t *pick_queue(nvme_dev_t *d)
{
    if (d->io_queue_count <= 0) return 0;
    nvme_ioq_t *q = &d->ioq[d->io_rr % d->io_queue_count];
    d->io_rr = (d->io_rr + 1) % d->io_queue_count;
    return q;
}

static void copy_str(char *dst, const char *src, int len)
{
    for (int i = 0; i < len; i++) dst[i] = src[i];
    dst[len] = '\0';
    for (int i = len - 1; i >= 0 && dst[i] == ' '; i--) dst[i] = '\0';
}

static void enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t cmd = pci_config_read32(bus, dev, func, 0x04);
    cmd |= (1 << 2) | (1 << 1);
    pci_config_write32(bus, dev, func, 0x04, cmd);
}

static uint64_t read_bar64(uint8_t bus, uint8_t dev, uint8_t func, int idx)
{
    uint32_t lo = pci_config_read32(bus, dev, func, 0x10 + idx * 4);
    if (lo & 0x04) {
        uint32_t hi = pci_config_read32(bus, dev, func, 0x10 + (idx + 1) * 4);
        return ((uint64_t)hi << 32) | (lo & ~0xFULL);
    }
    return lo & ~0xFULL;
}

static int bring_up(nvme_dev_t *d, struct pci_device *pdev)
{
    d->pci_bus = pdev->bus;
    d->pci_dev = pdev->dev;
    d->pci_func = pdev->func;
    kputs("[nvme] drive ");
    kput_dec(d->slot);
    kputs(": PCI ");
    kput_hex(pdev->bus); kputs(":"); kput_hex(pdev->dev); kputs(".");
    kput_hex(pdev->func); kputs(" v="); kput_hex(pdev->vendor_id);
    kputs(" d="); kput_hex(pdev->device_id); kputs("\n");
    enable_bus_master(pdev->bus, pdev->dev, pdev->func);
    uint64_t bar0 = read_bar64(pdev->bus, pdev->dev, pdev->func, 0);
    if (!bar0) { kputs("[nvme] BAR0 zero\n"); return -1; }
    d->bar0_phys = bar0;
    d->regs = (volatile uint32_t *)(uintptr_t)bar0;

    /* When the firmware places BAR0 above 4GB, our default identity
     * map (low 4GB) doesn't cover it. Map 16KB of MMIO on demand. */
    if (bar0 >= 0x100000000ULL) {
        kputs("[nvme] BAR0 above 4GB, mapping MMIO\n");
        uint64_t base = bar0 & ~0xFFFULL;
        vmm_map_range(base, base, 4, PTE_WRITABLE | PTE_NOCACHE);
    }
    uint64_t cap = nvme_read64(d, NVME_REG_CAP);
    uint16_t mqes = NVME_CAP_MQES(cap);
    uint8_t  dstrd = NVME_CAP_DSTRD(cap);
    d->doorbell_stride = 4u << dstrd;
    kputs("[nvme] mqes="); kput_dec(mqes + 1);
    kputs(" dstrd="); kput_dec(dstrd); kputs("\n");
    uint32_t cc = nvme_read32(d, NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        nvme_write32(d, NVME_REG_CC, cc & ~NVME_CC_EN);
        for (uint32_t i = 0; i < NVME_POLL_LIMIT; i++) {
            if (!(nvme_read32(d, NVME_REG_CSTS) & NVME_CSTS_RDY)) break;
            if (i == NVME_POLL_LIMIT - 1) { kputs("[nvme] disable timeout\n"); return -1; }
        }
    }
    uint64_t asqp = pmm_alloc(); uint64_t acqp = pmm_alloc();
    if (!asqp || !acqp) { kputs("[nvme] admin q alloc fail\n"); return -1; }
    d->asq = (void *)(uintptr_t)asqp; d->acq = (void *)(uintptr_t)acqp;
    d->asq_phys = asqp; d->acq_phys = acqp;
    memset(d->asq, 0, 4096); memset(d->acq, 0, 4096);
    d->asq_tail = 0; d->acq_head = 0; d->acq_phase = 1; d->admin_cmd_id = 0;
    nvme_write32(d, NVME_REG_AQA, ((NVME_ADMIN_QSIZE - 1) << 16) | (NVME_ADMIN_QSIZE - 1));
    nvme_write64(d, NVME_REG_ASQ, asqp);
    nvme_write64(d, NVME_REG_ACQ, acqp);
    cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K |
         NVME_CC_AMS_RR | NVME_CC_SHN_NONE | NVME_CC_IOSQES | NVME_CC_IOCQES;
    nvme_write32(d, NVME_REG_CC, cc);
    for (uint32_t i = 0; i < NVME_POLL_LIMIT; i++) {
        uint32_t st = nvme_read32(d, NVME_REG_CSTS);
        if (st & NVME_CSTS_CFS) { kputs("[nvme] CFS\n"); return -1; }
        if (st & NVME_CSTS_RDY) break;
        if (i == NVME_POLL_LIMIT - 1) { kputs("[nvme] enable timeout\n"); return -1; }
    }
    nvme_write32(d, NVME_REG_INTMS, 0xFFFFFFFF);
    uint64_t idp = pmm_alloc();
    if (!idp) return -1;
    void *idbuf = (void *)(uintptr_t)idp;
    memset(idbuf, 0, 4096);
    {
        nvme_cmd_t c; memset(&c, 0, sizeof(c));
        c.opcode = NVME_ADMIN_IDENTIFY; c.prp1 = idp; c.cdw10 = 1;
        if (admin_cmd(d, &c) != 0) { pmm_free(idp); return -1; }
    }
    uint8_t *id = (uint8_t *)idbuf;
    copy_str(d->serial, (char *)&id[4], 20);
    copy_str(d->model,  (char *)&id[24], 40);
    kputs("[nvme] sn="); kputs(d->serial);
    kputs(" model="); kputs(d->model); kputs("\n");
    pmm_free(idp);
    uint64_t nsp = pmm_alloc();
    if (!nsp) return -1;
    void *nsbuf = (void *)(uintptr_t)nsp;
    memset(nsbuf, 0, 4096);
    d->nsid = 1;
    {
        nvme_cmd_t c; memset(&c, 0, sizeof(c));
        c.opcode = NVME_ADMIN_IDENTIFY; c.nsid = 1; c.prp1 = nsp; c.cdw10 = 0;
        if (admin_cmd(d, &c) != 0) { pmm_free(nsp); return -1; }
    }
    uint8_t *ns = (uint8_t *)nsbuf;
    d->num_blocks = *(uint64_t *)&ns[0];
    uint8_t flbas = ns[26] & 0x0F;
    uint32_t lbaf = *(uint32_t *)&ns[128 + flbas * 4];
    d->block_size = 1U << ((lbaf >> 16) & 0xFF);
    kputs("[nvme] ns1: "); kput_dec(d->num_blocks);
    kputs(" x "); kput_dec(d->block_size);
    kputs(" = "); kput_dec((d->num_blocks * d->block_size) / (1024 * 1024));
    kputs(" MB\n");
    pmm_free(nsp);
    int want = NVME_IO_QUEUES;
    if ((mqes + 1) < NVME_IO_QSIZE) {
        kputs("[nvme] MQES below QSIZE, falling back to 1 queue\n");
        want = 1;
    }
    d->io_queue_count = 0; d->io_rr = 0;
    for (int qi = 0; qi < want; qi++) {
        nvme_ioq_t *q = &d->ioq[qi];
        memset(q, 0, sizeof(*q));
        q->qid = qi + 1;
        uint64_t cqp = pmm_alloc();
        if (!cqp) break;
        q->cq = (void *)(uintptr_t)cqp; q->cq_phys = cqp;
        memset(q->cq, 0, 4096);
        q->cq_head = 0; q->cq_phase = 1;
        {
            nvme_cmd_t c; memset(&c, 0, sizeof(c));
            c.opcode = NVME_ADMIN_CREATE_IO_CQ;
            c.prp1 = cqp;
            c.cdw10 = ((NVME_IO_QSIZE - 1) << 16) | q->qid;
            c.cdw11 = 1;
            int s = admin_cmd(d, &c);
            if (s != 0) {
                kputs("[nvme] CreateCQ "); kput_dec(q->qid);
                kputs(" status="); kput_hex(s);
                kputs(" -- stopping queue setup\n");
                pmm_free(cqp);
                break;
            }
        }
        uint64_t sqp = pmm_alloc();
        if (!sqp) { pmm_free(cqp); break; }
        q->sq = (void *)(uintptr_t)sqp; q->sq_phys = sqp;
        memset(q->sq, 0, 4096);
        q->sq_tail = 0; q->cmd_id = 0;
        {
            nvme_cmd_t c; memset(&c, 0, sizeof(c));
            c.opcode = NVME_ADMIN_CREATE_IO_SQ;
            c.prp1 = sqp;
            c.cdw10 = ((NVME_IO_QSIZE - 1) << 16) | q->qid;
            c.cdw11 = ((uint32_t)q->qid << 16) | 1;
            int s = admin_cmd(d, &c);
            if (s != 0) {
                kputs("[nvme] CreateSQ "); kput_dec(q->qid);
                kputs(" status="); kput_hex(s);
                kputs(" -- stopping queue setup\n");
                pmm_free(sqp); pmm_free(cqp);
                break;
            }
        }
        q->active = 1;
        d->io_queue_count++;
    }
    if (d->io_queue_count == 0) { kputs("[nvme] no I/O queues\n"); return -1; }
    kputs("[nvme] drive "); kput_dec(d->slot);
    kputs(" ready, "); kput_dec(d->io_queue_count); kputs(" I/O queue(s)\n");
    d->ready = 1;
    return 0;
}

int nvme_init(void)
{
    memset(g_drives, 0, sizeof(g_drives));
    g_drive_count = 0;
    kputs("[nvme] scanning PCI for NVMe controllers...\n");
    int n = pci_device_count();
    for (int i = 0; i < n && g_drive_count < NVME_MAX_DRIVES; i++) {
        struct pci_device *p = pci_get_device(i);
        if (!p) continue;
        if (p->class_code != 0x01 || p->subclass != 0x08) continue;
        nvme_dev_t *d = &g_drives[g_drive_count];
        memset(d, 0, sizeof(*d));
        d->slot = g_drive_count;
        if (bring_up(d, p) == 0) g_drive_count++;
        else { kputs("[nvme] drive bring-up failed, continuing\n"); memset(d, 0, sizeof(*d)); }
    }
    if (g_drive_count == 0) { kputs("[nvme] no usable NVMe drives\n"); return -1; }
    kputs("[nvme] "); kput_dec(g_drive_count); kputs(" drive(s) ready\n");
    return 0;
}

int nvme_drive_count(void) { return g_drive_count; }

nvme_dev_t *nvme_get_drive(int idx)
{
    if (idx < 0 || idx >= g_drive_count) return 0;
    return &g_drives[idx];
}

int nvme_read_drive(int idx, uint64_t lba, uint32_t count, void *buf)
{
    nvme_dev_t *d = nvme_get_drive(idx);
    if (!d || !d->ready) return -1;
    if (count == 0) return 0;
    nvme_ioq_t *q = pick_queue(d);
    if (!q) return -1;
    uint32_t bytes = count * d->block_size;
    uint32_t pages = (bytes + 4095) / 4096;
    uint64_t dma = (pages == 1) ? pmm_alloc() : pmm_alloc_contiguous(pages);
    if (!dma) return -1;
    void *dbuf = (void *)(uintptr_t)dma;
    memset(dbuf, 0, (uint64_t)pages * 4096);
    nvme_cmd_t c; memset(&c, 0, sizeof(c));
    c.opcode = NVME_IO_READ;
    c.nsid = d->nsid;
    c.prp1 = dma;
    if (pages == 2) c.prp2 = dma + 4096;
    else if (pages > 2) {
        uint64_t plp = pmm_alloc();
        if (!plp) goto rfree;
        uint64_t *pl = (uint64_t *)(uintptr_t)plp;
        for (uint32_t i = 1; i < pages; i++) pl[i - 1] = dma + (uint64_t)i * 4096;
        c.prp2 = plp;
    }
    c.cdw10 = (uint32_t)lba;
    c.cdw11 = (uint32_t)(lba >> 32);
    c.cdw12 = count - 1;
    int status = io_cmd(d, q, &c);
    if (status == 0) memcpy(buf, dbuf, bytes);
    else { kputs("[nvme] read fail lba="); kput_hex(lba);
           kputs(" status="); kput_hex(status); kputs("\n"); }
    if (pages > 2 && c.prp2) pmm_free(c.prp2);
rfree:
    for (uint32_t i = 0; i < pages; i++) pmm_free(dma + (uint64_t)i * 4096);
    return (status == 0) ? 0 : -1;
}

int nvme_write_drive(int idx, uint64_t lba, uint32_t count, const void *buf)
{
    nvme_dev_t *d = nvme_get_drive(idx);
    if (!d || !d->ready) return -1;
    if (count == 0) return 0;
    nvme_ioq_t *q = pick_queue(d);
    if (!q) return -1;
    uint32_t bytes = count * d->block_size;
    uint32_t pages = (bytes + 4095) / 4096;
    uint64_t dma = (pages == 1) ? pmm_alloc() : pmm_alloc_contiguous(pages);
    if (!dma) return -1;
    void *dbuf = (void *)(uintptr_t)dma;
    memcpy(dbuf, buf, bytes);
    nvme_cmd_t c; memset(&c, 0, sizeof(c));
    c.opcode = NVME_IO_WRITE;
    c.nsid = d->nsid;
    c.prp1 = dma;
    if (pages == 2) c.prp2 = dma + 4096;
    else if (pages > 2) {
        uint64_t plp = pmm_alloc();
        if (!plp) goto wfree;
        uint64_t *pl = (uint64_t *)(uintptr_t)plp;
        for (uint32_t i = 1; i < pages; i++) pl[i - 1] = dma + (uint64_t)i * 4096;
        c.prp2 = plp;
    }
    c.cdw10 = (uint32_t)lba;
    c.cdw11 = (uint32_t)(lba >> 32);
    c.cdw12 = count - 1;
    int status = io_cmd(d, q, &c);
    if (status != 0) { kputs("[nvme] write fail lba="); kput_hex(lba);
                       kputs(" status="); kput_hex(status); kputs("\n"); }
    if (pages > 2 && c.prp2) pmm_free(c.prp2);
wfree:
    for (uint32_t i = 0; i < pages; i++) pmm_free(dma + (uint64_t)i * 4096);
    return (status == 0) ? 0 : -1;
}

int nvme_flush_drive(int idx)
{
    nvme_dev_t *d = nvme_get_drive(idx);
    if (!d || !d->ready) return -1;
    nvme_ioq_t *q = pick_queue(d);
    if (!q) return -1;
    nvme_cmd_t c; memset(&c, 0, sizeof(c));
    c.opcode = NVME_IO_FLUSH;
    c.nsid = d->nsid;
    int s = io_cmd(d, q, &c);
    if (s != 0) { kputs("[nvme] flush status="); kput_hex(s); kputs("\n"); return -1; }
    return 0;
}

int nvme_read(uint64_t lba, uint32_t count, void *buf)        { return nvme_read_drive(0, lba, count, buf); }
int nvme_write(uint64_t lba, uint32_t count, const void *buf) { return nvme_write_drive(0, lba, count, buf); }
int nvme_flush(void)                                          { return nvme_flush_drive(0); }
const nvme_dev_t *nvme_get_dev(void)
{
    if (g_drive_count == 0 || !g_drives[0].ready) return 0;
    return &g_drives[0];
}
