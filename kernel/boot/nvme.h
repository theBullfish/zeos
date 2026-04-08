/*
 * Zeos -- NVMe Block Device Driver
 *
 * Bare-metal NVMe controller driver. Polling-only (no interrupts).
 * Finds NVMe device via PCI (class 0x01, subclass 0x08), maps BAR0,
 * initializes admin + one I/O queue pair, and provides block read/write.
 *
 * Designed for CN60 Chromebox and any x86_64 with NVMe storage.
 */

#ifndef ZEOS_NVME_H
#define ZEOS_NVME_H

#include <stdint.h>

#define NVME_MAX_QUEUES     2   /* Admin + 1 I/O queue for Alpha */
#define NVME_ADMIN_QSIZE    64  /* Entries in admin queues */
#define NVME_IO_QSIZE       64  /* Entries in I/O queues */

typedef struct {
    /* PCI BAR0 (memory-mapped registers) */
    volatile uint32_t *regs;
    uint64_t bar0_phys;

    /* Admin queue */
    void *asq;              /* Admin Submission Queue (physical page) */
    void *acq;              /* Admin Completion Queue (physical page) */
    uint64_t asq_phys;
    uint64_t acq_phys;
    int asq_tail;
    int acq_head;
    int acq_phase;

    /* I/O queue */
    void *iosq;             /* I/O Submission Queue */
    void *iocq;             /* I/O Completion Queue */
    uint64_t iosq_phys;
    uint64_t iocq_phys;
    int iosq_tail;
    int iocq_head;
    int iocq_phase;

    /* Doorbell stride (bytes between doorbells, from CAP.DSTRD) */
    uint32_t doorbell_stride;

    /* Device info */
    uint32_t nsid;          /* Namespace ID (usually 1) */
    uint64_t num_blocks;    /* Total LBAs */
    uint32_t block_size;    /* Usually 512 or 4096 */
    char serial[21];
    char model[41];
    int ready;

    /* PCI location (for debug) */
    uint8_t pci_bus;
    uint8_t pci_dev;
    uint8_t pci_func;
} nvme_dev_t;

/* Initialize NVMe controller found via PCI scan. Returns 0 on success. */
int nvme_init(void);

/* Read blocks: lba = starting block, count = number of blocks, buf = output.
 * Returns 0 on success, -1 on error. */
int nvme_read(uint64_t lba, uint32_t count, void *buf);

/* Write blocks. Returns 0 on success, -1 on error. */
int nvme_write(uint64_t lba, uint32_t count, const void *buf);

/* Get device info (NULL if not initialized). */
const nvme_dev_t *nvme_get_dev(void);

/* Flush (ensure writes are persisted). Returns 0 on success. */
int nvme_flush(void);

#endif /* ZEOS_NVME_H */
