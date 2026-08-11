/*
 * Zeos -- NVMe Block Device Driver (multi-drive, multi-queue)
 */
#ifndef ZEOS_NVME_H
#define ZEOS_NVME_H
#include <stdint.h>
#include "spinlock.h"
#define NVME_MAX_DRIVES     8
#define NVME_IO_QUEUES      4
#define NVME_ADMIN_QSIZE    64
#define NVME_IO_QSIZE       64
typedef struct {
    int qid;
    int active;
    void *sq;
    void *cq;
    uint64_t sq_phys;
    uint64_t cq_phys;
    int sq_tail;
    int cq_head;
    int cq_phase;
    uint16_t cmd_id;
    /* MSI-X completion signaling. ISR clears this flag after draining
     * the CQ; submitter waits on it with short-poll fallback. */
    volatile uint32_t pending_completion;
    volatile uint16_t last_status;
    volatile uint16_t last_cmd_id;
    /* SMP locks. sq_lock serializes ring writes (sq_tail bump + descriptor
     * memcpy + doorbell). cq_lock serializes drain (cq_head + cq_phase +
     * doorbell). The submitter holds sq_lock to publish a command, then
     * releases it before waiting; drain_cq() takes cq_lock — the ISR uses
     * a try-lock so it never blocks behind a submitter polling for its
     * own completion. */
    zeos_spinlock_t sq_lock;
    zeos_spinlock_t cq_lock;
} nvme_ioq_t;
typedef struct {
    int slot;
    volatile uint32_t *regs;
    uint64_t bar0_phys;
    void *asq;
    void *acq;
    uint64_t asq_phys;
    uint64_t acq_phys;
    int asq_tail;
    int acq_head;
    int acq_phase;
    uint16_t admin_cmd_id;
    nvme_ioq_t ioq[NVME_IO_QUEUES];
    int io_queue_count;
    int io_rr;
    uint32_t doorbell_stride;
    uint32_t nsid;
    uint64_t num_blocks;
    uint32_t block_size;
    char serial[21];
    char model[41];
    int ready;
    uint8_t pci_bus;
    uint8_t pci_dev;
    uint8_t pci_func;
    /* MSI-X completion. msix_vector >= 0 = interrupt-driven I/O CQ
     * draining; -1 = polling fallback. One vector per drive aggregates
     * all I/O CQs (ISR walks every CQ on this drive). */
    int msix_vector;
    /* Admin-queue lock — admin_cmd is serialized end-to-end (submit +
     * wait) because the admin path is one shot at boot / config change
     * and re-entrancy would corrupt asq_tail. */
    zeos_spinlock_t admin_lock;
} nvme_dev_t;
int nvme_init(void);
int nvme_drive_count(void);
nvme_dev_t *nvme_get_drive(int idx);
int nvme_read_drive(int idx, uint64_t lba, uint32_t count, void *buf);
int nvme_write_drive(int idx, uint64_t lba, uint32_t count, const void *buf);
int nvme_flush_drive(int idx);
int nvme_read(uint64_t lba, uint32_t count, void *buf);
int nvme_write(uint64_t lba, uint32_t count, const void *buf);
int nvme_flush(void);
const nvme_dev_t *nvme_get_dev(void);
#endif
