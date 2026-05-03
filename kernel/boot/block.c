/*
 * Zeos — Block device dispatcher
 *
 * Probes NVMe first (most common on post-2014 hardware), then AHCI
 * (most common on 2008-2014 hardware). First success wins.
 */

#include "block.h"
#include "nvme.h"
#include "ahci.h"
#include "kprint.h"

enum { BK_NONE = 0, BK_NVME, BK_AHCI };
static int kind = BK_NONE;

int block_init(void) {
    if (nvme_init() == 0) {
        kind = BK_NVME;
        kputs("BLOCK: using NVMe\n");
        return 0;
    }
    if (ahci_init() == 0) {
        kind = BK_AHCI;
        kputs("BLOCK: using AHCI/SATA\n");
        return 0;
    }
    kputs("BLOCK: no storage controller detected\n");
    return -1;
}

int block_read(uint64_t lba, uint32_t count, void *buf) {
    switch (kind) {
        case BK_NVME: return nvme_read(lba, count, buf);
        case BK_AHCI: return ahci_read(lba, count, buf);
        default:      return -1;
    }
}

int block_write(uint64_t lba, uint32_t count, const void *buf) {
    switch (kind) {
        case BK_NVME: return nvme_write(lba, count, buf);
        case BK_AHCI: return ahci_write(lba, count, buf);
        default:      return -1;
    }
}

int block_flush(void) {
    switch (kind) {
        case BK_NVME: return nvme_flush();
        case BK_AHCI: return ahci_flush();
        default:      return -1;
    }
}

uint32_t block_size(void) {
    switch (kind) {
        case BK_NVME: return 512;  /* nvme_get_dev() exposes the real value */
        case BK_AHCI: return ahci_block_size();
        default:      return 0;
    }
}

const char *block_kind(void) {
    switch (kind) {
        case BK_NVME: return "nvme";
        case BK_AHCI: return "ahci";
        default:      return "none";
    }
}
