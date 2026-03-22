/*
 * Zeos — Kernel Heap Allocator
 *
 * Free-list allocator with coalescing on free.
 * Each block has a header with size and free/used flag.
 * Adjacent free blocks are merged to reduce fragmentation.
 *
 * The heap grows by requesting pages from the PMM.
 * Currently identity-mapped, so phys == virt.
 */

#include "heap.h"
#include "pmm.h"
#include "fb.h"

#define HEAP_MAGIC  0x5A454F53   /* "ZEOS" */
#define ALIGN8(x)   (((x) + 7) & ~7ULL)
#define PAGE_SIZE    4096

struct block_header {
    uint32_t magic;
    uint32_t free;      /* 1 = free, 0 = used */
    uint64_t size;      /* Size of data area (not including header) */
    struct block_header *next;
    struct block_header *prev;
};

#define HEADER_SIZE  ALIGN8(sizeof(struct block_header))

static struct block_header *heap_start;
static struct block_header *heap_end;
static uint64_t total_size;
static uint64_t used_size;

/*
 * Expand the heap by N pages.
 */
static int heap_expand(uint64_t pages)
{
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t page = pmm_alloc();
        if (!page)
            return -1;
    }
    return 0;
}

void heap_init(uint64_t initial_pages)
{
    /* Allocate contiguous physical pages for the initial heap */
    uint64_t heap_phys = pmm_alloc_contiguous(initial_pages);
    if (!heap_phys) {
        fb_puts("HEAP: FATAL — cannot allocate initial heap!\n");
        return;
    }

    total_size = initial_pages * PAGE_SIZE;
    used_size = 0;

    /* Set up the first free block spanning the entire heap */
    heap_start = (struct block_header *)heap_phys;
    heap_start->magic = HEAP_MAGIC;
    heap_start->free = 1;
    heap_start->size = total_size - HEADER_SIZE;
    heap_start->next = 0;
    heap_start->prev = 0;

    heap_end = heap_start;
}

/*
 * Find a free block large enough using first-fit.
 */
static struct block_header *find_free(uint64_t size)
{
    struct block_header *curr = heap_start;
    while (curr) {
        if (curr->free && curr->size >= size)
            return curr;
        curr = curr->next;
    }
    return 0;
}

/*
 * Split a block if it's large enough to hold a second block.
 */
static void split_block(struct block_header *block, uint64_t size)
{
    uint64_t remaining = block->size - size - HEADER_SIZE;

    /* Only split if the remainder can hold at least 16 bytes of data */
    if (remaining < 16)
        return;

    struct block_header *new_block =
        (struct block_header *)((uint8_t *)block + HEADER_SIZE + size);

    new_block->magic = HEAP_MAGIC;
    new_block->free = 1;
    new_block->size = remaining;
    new_block->next = block->next;
    new_block->prev = block;

    if (block->next)
        block->next->prev = new_block;

    block->next = new_block;
    block->size = size;

    if (block == heap_end)
        heap_end = new_block;
}

void *kmalloc(uint64_t size)
{
    if (size == 0)
        return 0;

    size = ALIGN8(size);

    struct block_header *block = find_free(size);
    if (!block) {
        /* Try to expand the heap */
        uint64_t pages_needed = (size + HEADER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
        if (pages_needed < 4)
            pages_needed = 4;  /* Minimum expansion */

        uint64_t new_phys = pmm_alloc_contiguous(pages_needed);
        if (!new_phys)
            return 0;

        /* Create a new block at the expansion */
        struct block_header *new_block = (struct block_header *)new_phys;
        new_block->magic = HEAP_MAGIC;
        new_block->free = 1;
        new_block->size = pages_needed * PAGE_SIZE - HEADER_SIZE;
        new_block->next = 0;
        new_block->prev = heap_end;

        if (heap_end)
            heap_end->next = new_block;
        heap_end = new_block;

        total_size += pages_needed * PAGE_SIZE;

        block = find_free(size);
        if (!block)
            return 0;
    }

    split_block(block, size);
    block->free = 0;
    used_size += block->size + HEADER_SIZE;

    return (void *)((uint8_t *)block + HEADER_SIZE);
}

/*
 * Coalesce adjacent free blocks.
 */
static void coalesce(struct block_header *block)
{
    /* Merge with next */
    while (block->next && block->next->free &&
           block->next->magic == HEAP_MAGIC) {
        struct block_header *next = block->next;
        block->size += HEADER_SIZE + next->size;
        block->next = next->next;
        if (next->next)
            next->next->prev = block;
        if (next == heap_end)
            heap_end = block;
    }

    /* Merge with prev */
    if (block->prev && block->prev->free &&
        block->prev->magic == HEAP_MAGIC) {
        struct block_header *prev = block->prev;
        prev->size += HEADER_SIZE + block->size;
        prev->next = block->next;
        if (block->next)
            block->next->prev = prev;
        if (block == heap_end)
            heap_end = prev;
    }
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    struct block_header *block =
        (struct block_header *)((uint8_t *)ptr - HEADER_SIZE);

    if (block->magic != HEAP_MAGIC) {
        fb_puts("HEAP: corrupt block in kfree!\n");
        return;
    }

    block->free = 1;
    used_size -= block->size + HEADER_SIZE;

    coalesce(block);
}

uint64_t heap_total_bytes(void)  { return total_size; }
uint64_t heap_used_bytes(void)   { return used_size; }
uint64_t heap_free_bytes(void)   { return total_size - used_size; }
