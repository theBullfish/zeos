/*
 * Zeos boot info — passed from UEFI bootstrap to kernel.
 * Everything the kernel needs to start: framebuffer, memory map, ACPI.
 */

#ifndef ZEOS_BOOT_H
#define ZEOS_BOOT_H

#include <efi.h>

struct zeos_framebuffer {
    uint32_t *base;          /* Pointer to pixel data */
    uint64_t  size;          /* Framebuffer size in bytes */
    uint32_t  width;         /* Horizontal resolution */
    uint32_t  height;        /* Vertical resolution */
    uint32_t  pitch;         /* Pixels per scan line (may be > width) */
    uint32_t  pixel_format;  /* EFI_GRAPHICS_PIXEL_FORMAT */
};

struct zeos_memory_map {
    void     *entries;       /* Array of EFI_MEMORY_DESCRIPTOR */
    uint64_t  size;          /* Total size of the map in bytes */
    uint64_t  desc_size;     /* Size of each descriptor */
    uint32_t  desc_ver;      /* Descriptor version */
    uint64_t  map_key;       /* Key for ExitBootServices */
};

struct zeos_boot_info {
    struct zeos_framebuffer fb;
    struct zeos_memory_map  mmap;
    void                   *rsdp;   /* ACPI RSDP pointer */
};

#endif /* ZEOS_BOOT_H */
