/* Zeos aarch64 — kernel heap (M4). Drop-in for heap.h.
 * Bump allocator over an 8 MiB BSS arena; kfree is a stub for now
 * (real free-list allocator is a later grind). Honest, not silent. */
#include <stdint.h>

static uint8_t  arena[8u << 20];         /* 8 MiB */
static uint64_t off = 0;
static const uint64_t total = sizeof(arena);

void heap_init(uint64_t initial_pages) { (void)initial_pages; off = 0; }

void *kmalloc(uint64_t size)
{
    size = (size + 15) & ~15UL;          /* 16-byte align */
    if (off + size > total) return 0;
    void *p = &arena[off];
    off += size;
    return p;
}

void kfree(void *ptr) { (void)ptr; }     /* bump allocator: reclaim later */

uint64_t heap_total_bytes(void) { return total; }
uint64_t heap_used_bytes(void)  { return off; }
uint64_t heap_free_bytes(void)  { return total - off; }
