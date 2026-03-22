/*
 * Zeos — Physical Memory Manager
 *
 * Bitmap allocator. One bit per 4K page frame.
 * Bit 0 = free, bit 1 = used.
 *
 * The bitmap itself is placed in the first available region
 * large enough to hold it. We mark the bitmap's own pages as used.
 */

#include "pmm.h"
#include "fb.h"
#include <efi.h>

#define PAGE_SIZE 4096

/* Bitmap — one bit per frame */
static uint64_t *bitmap;
static uint64_t bitmap_size;     /* Size in bytes */
static uint64_t total_frames;
static uint64_t used_frames;
static uint64_t highest_addr;

/* Bitmap operations */
static inline void bitmap_set(uint64_t frame)
{
    bitmap[frame / 64] |= (1ULL << (frame % 64));
}

static inline void bitmap_clear(uint64_t frame)
{
    bitmap[frame / 64] &= ~(1ULL << (frame % 64));
}

static inline int bitmap_test(uint64_t frame)
{
    return (bitmap[frame / 64] >> (frame % 64)) & 1;
}

/*
 * Find the highest physical address to determine bitmap size.
 */
static uint64_t find_highest_address(struct zeos_memory_map *mmap)
{
    uint64_t highest = 0;
    uint8_t *entry = (uint8_t *)mmap->entries;
    uint8_t *end = entry + mmap->size;

    while (entry < end) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)entry;
        uint64_t region_end = desc->PhysicalStart + desc->NumberOfPages * PAGE_SIZE;
        if (region_end > highest)
            highest = region_end;
        entry += mmap->desc_size;
    }

    return highest;
}

/*
 * Find a region large enough to hold the bitmap.
 * Returns physical address, or 0 on failure.
 */
static uint64_t find_bitmap_region(struct zeos_memory_map *mmap, uint64_t size)
{
    uint8_t *entry = (uint8_t *)mmap->entries;
    uint8_t *end = entry + mmap->size;

    while (entry < end) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)entry;

        /* Use conventional memory or boot services memory (now ours) */
        if (desc->Type == EfiConventionalMemory ||
            desc->Type == EfiBootServicesCode ||
            desc->Type == EfiBootServicesData) {

            uint64_t region_size = desc->NumberOfPages * PAGE_SIZE;

            /* Skip first 1MB (legacy BIOS area) */
            uint64_t start = desc->PhysicalStart;
            if (start < 0x100000) {
                uint64_t skip = 0x100000 - start;
                if (skip >= region_size)
                    goto next;
                start += skip;
                region_size -= skip;
            }

            if (region_size >= size)
                return start;
        }
next:
        entry += mmap->desc_size;
    }

    return 0;
}

void pmm_init(struct zeos_memory_map *mmap, struct zeos_framebuffer *fb)
{
    /* Find highest physical address */
    highest_addr = find_highest_address(mmap);
    total_frames = highest_addr / PAGE_SIZE;

    /* Calculate bitmap size (one bit per frame, rounded up to 8 bytes) */
    bitmap_size = (total_frames + 63) / 64 * 8;

    /* Find a region for the bitmap */
    uint64_t bitmap_phys = find_bitmap_region(mmap, bitmap_size);
    if (!bitmap_phys) {
        fb_puts("PMM: FATAL — no region for bitmap!\n");
        return;
    }

    bitmap = (uint64_t *)bitmap_phys;

    /* Mark ALL frames as used initially */
    for (uint64_t i = 0; i < bitmap_size / 8; i++)
        bitmap[i] = ~0ULL;

    used_frames = total_frames;

    /* Walk the memory map and free usable regions */
    uint8_t *entry = (uint8_t *)mmap->entries;
    uint8_t *end = entry + mmap->size;

    while (entry < end) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)entry;

        if (desc->Type == EfiConventionalMemory ||
            desc->Type == EfiBootServicesCode ||
            desc->Type == EfiBootServicesData) {

            uint64_t base = desc->PhysicalStart;
            uint64_t pages = desc->NumberOfPages;

            for (uint64_t p = 0; p < pages; p++) {
                uint64_t frame = (base / PAGE_SIZE) + p;
                if (frame < total_frames && frame > 0) {  /* Never free frame 0 */
                    bitmap_clear(frame);
                    used_frames--;
                }
            }
        }

        entry += mmap->desc_size;
    }

    /* Mark the bitmap's own pages as used */
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < bitmap_pages; i++) {
        uint64_t frame = (bitmap_phys / PAGE_SIZE) + i;
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            used_frames++;
        }
    }

    /* Mark the framebuffer as used */
    if (fb && fb->base) {
        uint64_t fb_phys = (uint64_t)(uintptr_t)fb->base;
        uint64_t fb_pages = (fb->size + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t i = 0; i < fb_pages; i++) {
            uint64_t frame = (fb_phys / PAGE_SIZE) + i;
            if (frame < total_frames && !bitmap_test(frame)) {
                bitmap_set(frame);
                used_frames++;
            }
        }
    }

    /* Mark first 1MB as used (legacy BIOS, real mode IVT, etc.) */
    for (uint64_t frame = 0; frame < 256; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            used_frames++;
        }
    }
}

uint64_t pmm_alloc(void)
{
    for (uint64_t i = 0; i < total_frames / 64; i++) {
        if (bitmap[i] != ~0ULL) {
            /* There's a free bit in this word */
            for (int bit = 0; bit < 64; bit++) {
                if (!(bitmap[i] & (1ULL << bit))) {
                    uint64_t frame = i * 64 + bit;
                    if (frame < total_frames) {
                        bitmap_set(frame);
                        used_frames++;
                        return frame * PAGE_SIZE;
                    }
                }
            }
        }
    }
    return 0;  /* Out of memory */
}

void pmm_free(uint64_t phys_addr)
{
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame > 0 && frame < total_frames && bitmap_test(frame)) {
        bitmap_clear(frame);
        used_frames--;
    }
}

uint64_t pmm_alloc_contiguous(uint64_t count)
{
    uint64_t run = 0;
    uint64_t start = 0;

    for (uint64_t frame = 1; frame < total_frames; frame++) {
        if (!bitmap_test(frame)) {
            if (run == 0)
                start = frame;
            run++;
            if (run == count) {
                /* Found a run — mark all as used */
                for (uint64_t i = 0; i < count; i++) {
                    bitmap_set(start + i);
                    used_frames++;
                }
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

uint64_t pmm_total_pages(void)  { return total_frames; }
uint64_t pmm_free_pages(void)   { return total_frames - used_frames; }
uint64_t pmm_used_pages(void)   { return used_frames; }
