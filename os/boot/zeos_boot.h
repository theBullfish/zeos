/*
 * Zeos boot info — passed from UEFI bootstrap to kernel.
 * Everything the kernel needs to start: framebuffer, memory map, ACPI.
 */

#ifndef ZEOS_BOOT_H
#define ZEOS_BOOT_H

/* stdint only. This header describes what the firmware hands the kernel, but it
 * is part of the ARCH- AND FIRMWARE-AGNOSTIC os/ layer, so it must not depend on
 * a UEFI header to name its own field types. It previously included <efi.h> and
 * compiled purely by luck of the x86 build's include order; on aarch64 (whose
 * boot path is not UEFI at all) that broke every consumer of struct
 * zeos_framebuffer. The EFI_ names that remain below are comments describing
 * where a value originates on x86 — not types used here. */
#include <stdint.h>

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

/*
 * Display info captured from UEFI EDID before ExitBootServices.
 * If edid_valid == 0, the EDID protocol wasn't exposed and the
 * mfr/product/native fields are unset.
 */
struct zeos_display_info {
    uint8_t   edid_valid;     /* 1 if EDID was read+checksum-validated */
    uint8_t   edid[128];      /* Raw EDID block 0 (when valid) */
    char      mfr[4];         /* 3-letter manufacturer code + NUL */
    uint16_t  product_id;     /* EDID product ID (little-endian) */
    uint32_t  native_w;       /* Preferred timing horizontal (pixels) */
    uint32_t  native_h;       /* Preferred timing vertical (pixels) */
    uint32_t  native_hz;      /* Preferred refresh rate (Hz, integer) */
};

struct zeos_boot_info {
    struct zeos_framebuffer  fb;
    struct zeos_memory_map   mmap;
    void                    *rsdp;     /* ACPI RSDP pointer */
    struct zeos_display_info display;  /* EDID + native timing */
};

#endif /* ZEOS_BOOT_H */
