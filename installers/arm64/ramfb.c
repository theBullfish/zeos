/*
 * Zeos aarch64 — framebuffer SURFACE DISCOVERY (QEMU ramfb via fw_cfg).
 *
 * This file used to be `fb.c` and carried its own 151-line drawing engine:
 * fb_rect, fb_gradient, a 5x7 uppercase-only glyph table, fb_text. All of that
 * is gone, because os/boot/fb.c already implements the full framebuffer — 732
 * lines of clipping, alpha blending, back-buffer present, an 8x16 font,
 * scaled/background text, lines, circles, blits — in portable C with zero
 * arch-specific constructs. ARM was reimplementing a worse copy of a module it
 * could simply use.
 *
 * So the split now matches the rest of the port: os/ owns the drawing, the
 * installer owns finding the hardware. Everything here is about locating a
 * scanout surface on this board and describing it in the arch-neutral
 * `struct zeos_framebuffer`. Nothing here draws a pixel.
 *
 * ramfb is the QEMU virt path. A real SoC (Snapdragon, Rockchip, Ampere) will
 * add a sibling probe — simple-framebuffer from the device tree, or a real
 * display controller — filling in the same struct. The OS above does not learn
 * that anything changed.
 */
#include <stdint.h>
#include "zeos_boot.h"

extern int strcmp(const char *, const char *);

#define FWCFG_BASE   0x09020000UL
#define FWCFG_DMA    (FWCFG_BASE + 16)
#define CTL_ERROR    0x01
#define CTL_READ     0x02
#define CTL_SELECT   0x08
#define CTL_WRITE    0x10
#define FW_CFG_FILE_DIR 0x0019
#define DRM_XRGB8888 0x34325258u

#define FB_W 800
#define FB_H 600

/* The actual scanout memory handed to the device. Page-aligned because ramfb
 * DMAs directly out of it. */
uint32_t fb_backing[FB_W * FB_H] __attribute__((aligned(4096)));

static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t be64(uint64_t v) { return __builtin_bswap64(v); }
static inline uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }

struct DmaAccess { uint32_t control; uint32_t length; uint64_t address; } __attribute__((packed));
struct FwCfgFile { uint32_t size; uint16_t select; uint16_t reserved; char name[56]; } __attribute__((packed));
struct RamfbCfg  { uint64_t addr; uint32_t fourcc; uint32_t flags; uint32_t width; uint32_t height; uint32_t stride; } __attribute__((packed));

static void fwcfg_dma(uint32_t control, uint32_t length, void *buf)
{
    static volatile struct DmaAccess acc __attribute__((aligned(64)));
    acc.control = be32(control);
    acc.length  = be32(length);
    acc.address = be64((uint64_t)buf);
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint64_t *)FWCFG_DMA = be64((uint64_t)&acc);
    __asm__ volatile("dsb sy" ::: "memory");
    while (be32(acc.control) & ~CTL_ERROR) { }
}

static int fwcfg_find(const char *name)
{
    uint32_t cnt_be;
    fwcfg_dma((FW_CFG_FILE_DIR << 16) | CTL_SELECT | CTL_READ, 4, &cnt_be);
    uint32_t count = be32(cnt_be);
    for (uint32_t i = 0; i < count; i++) {
        struct FwCfgFile f;
        fwcfg_dma(CTL_READ, sizeof(f), &f);
        if (strcmp(f.name, name) == 0)
            return be16(f.select);
    }
    return -1;
}

/* Find a scanout surface and describe it. Returns 0 and fills *out on success,
 * -1 if this board has no ramfb (in which case the caller runs headless — the
 * bring-up ladder still reports over the UART). */
int ramfb_probe(struct zeos_framebuffer *out)
{
    int sel = fwcfg_find("etc/ramfb");
    if (sel < 0) return -1;

    struct RamfbCfg cfg;
    cfg.addr   = be64((uint64_t)fb_backing);
    cfg.fourcc = be32(DRM_XRGB8888);
    cfg.flags  = be32(0);
    cfg.width  = be32(FB_W);
    cfg.height = be32(FB_H);
    cfg.stride = be32(FB_W * 4);
    fwcfg_dma(((uint32_t)sel << 16) | CTL_SELECT | CTL_WRITE, sizeof(cfg), &cfg);

    out->base         = fb_backing;
    out->size         = (uint64_t)FB_W * FB_H * 4;
    out->width        = FB_W;
    out->height       = FB_H;
    out->pitch        = FB_W;      /* pixels per scanline, not bytes */
    out->pixel_format = 1;         /* XRGB8888, matching the fourcc above */
    return 0;
}
