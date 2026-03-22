/*
 * Zeos — Virtual Memory Manager (VMM)
 *
 * x86_64 4-level page table management.
 * Maps virtual addresses to physical frames from the PMM.
 */

#ifndef ZEOS_VMM_H
#define ZEOS_VMM_H

#include <stdint.h>

/* Page table entry flags */
#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_WRITETHROUGH (1ULL << 3)
#define PTE_NOCACHE    (1ULL << 4)
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_HUGE       (1ULL << 7)   /* 2MB page (in PD) or 1GB page (in PDPT) */
#define PTE_GLOBAL     (1ULL << 8)
#define PTE_NX         (1ULL << 63)  /* No-execute */

#define PAGE_SIZE      4096
#define ENTRIES_PER_TABLE 512

/* Higher-half kernel base — canonical upper half */
#define KERNEL_VBASE   0xFFFF800000000000ULL

/* Initialize VMM — create kernel page tables, switch to them */
void vmm_init(void);

/* Map a single 4K page: virtual → physical */
void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);

/* Unmap a single 4K page */
void vmm_unmap(uint64_t virt);

/* Get the physical address mapped to a virtual address, or 0 if unmapped */
uint64_t vmm_virt_to_phys(uint64_t virt);

/* Map a range of pages (count 4K pages) */
void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t count, uint64_t flags);

/* Get the current PML4 physical address */
uint64_t vmm_get_pml4(void);

#endif /* ZEOS_VMM_H */
