/*
 * Zeos -- Habana Goya HL-1000 driver public surface.
 *
 * Compute-only accelerator. Per docs/GPU_HOLES.md A3 / D3:
 *   - parent CHAIN_CPU
 *   - in:  compute_request
 *   - out: compute_result, accel.zixel
 *   - no display sub-chains
 *
 * Probes PCI vendor 0x1da3 device 0x0001 / class 0x12 (Signal
 * Processing). Maps BAR0 (CFG_REGS), BAR2 (HBM, paged), BAR4
 * (mailbox). Stages preboot FIT firmware embedded via objcopy and
 * registers each detected card as a gpu_compute_backend named
 * "goya-N".
 */

#ifndef ZEOS_GPU_GOYA_H
#define ZEOS_GPU_GOYA_H

#include <stdint.h>

#define GOYA_PCI_VENDOR  0x1DA3
#define GOYA_PCI_DEVICE  0x0001
#define GOYA_MAX_DEVICES 8

/* Per-card public summary, used by the `goya` shell command and the
 * selftest line. */
typedef struct {
    int      chain_id;          /* CHAIN_GOYA_<n> */
    int      ready;             /* PCI/reset bring-up succeeded. */
    int      fw_loaded;         /* Preboot FIT staged. 0 if blob absent. */
    int      compute_ready;     /* Backend registered, dispatch live. */
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint8_t  pci_bus, pci_dev, pci_func;
    uint64_t bar0_phys;
    uint64_t bar2_phys;         /* HBM aperture base. */
    uint64_t bar4_phys;
    uint64_t bar0_len;
    uint64_t bar2_len;
    uint64_t bar4_len;
    uint32_t fw_version;        /* Read from CPU_BOOT_DEV_STS0 if up. */
    uint32_t fence_value;       /* Last completion fence read. */
    int      msix_completion_vec;
    int      msix_error_vec;
    char     name[16];          /* "goya-N" */
} gpu_goya_device_t;

/* Probe + bind every Goya PCI function. Registers CHAIN_GOYA_<n>
 * sub-trees and (when firmware is present) registers a
 * gpu_compute_backend per card. parent_id is CHAIN_CPU.
 *
 * Returns the number of cards successfully brought up. Zero is a
 * normal outcome on any host without Goya hardware. */
int gpu_goya_init(int parent_id);

/* Number of cards bound at boot. */
int gpu_goya_device_count(void);

/* Per-card record, NULL on out-of-range. */
const gpu_goya_device_t *gpu_goya_device(int idx);

/* Print the multi-line `goya` shell command output. Safe to call when
 * no cards are present. */
void gpu_goya_dump_status(void);

/* Selftest line. Prints exactly one of:
 *   "Goya .................. N card(s) -- fw=loaded compute=ready\n"
 *   "Goya .................. detected, no firmware (use lspci to confirm)\n"
 *   "Goya .................. no cards detected\n"
 */
void gpu_goya_print_selftest_line(void);

#endif /* ZEOS_GPU_GOYA_H */
