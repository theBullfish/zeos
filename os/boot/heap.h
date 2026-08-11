/*
 * Zeos — Kernel Heap Allocator
 *
 * Simple first-fit free-list allocator.
 * Backs onto the PMM for page allocation.
 */

#ifndef ZEOS_HEAP_H
#define ZEOS_HEAP_H

#include <stdint.h>

/* Initialize the kernel heap with an initial size */
void heap_init(uint64_t initial_pages);

/* Allocate size bytes, 8-byte aligned */
void *kmalloc(uint64_t size);

/* Free a previously allocated pointer */
void kfree(void *ptr);

/* Get heap stats */
uint64_t heap_total_bytes(void);
uint64_t heap_used_bytes(void);
uint64_t heap_free_bytes(void);

#endif /* ZEOS_HEAP_H */
