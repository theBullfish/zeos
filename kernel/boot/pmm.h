/*
 * Zeos — Physical Memory Manager (PMM)
 *
 * Bitmap-based page frame allocator. Parses the UEFI memory map
 * and tracks which 4K frames are free/used.
 */

#ifndef ZEOS_PMM_H
#define ZEOS_PMM_H

#include <stdint.h>
#include "zeos_boot.h"

/* Initialize PMM from UEFI memory map */
void pmm_init(struct zeos_memory_map *mmap, struct zeos_framebuffer *fb);

/* Allocate a single 4K page frame. Returns physical address, or 0 on failure. */
uint64_t pmm_alloc(void);

/* Free a single 4K page frame. */
void pmm_free(uint64_t phys_addr);

/* Allocate N contiguous 4K frames. Returns physical address, or 0 on failure. */
uint64_t pmm_alloc_contiguous(uint64_t count);

/* Reserve a physical address range (mark as used). Returns count of newly reserved frames. */
uint64_t pmm_reserve_range(uint64_t phys_start, uint64_t size);

/* Get total/free/used page counts */
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
uint64_t pmm_used_pages(void);

#endif /* ZEOS_PMM_H */
