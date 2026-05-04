/*
 * Zeos -- virtio-gpu driver public surface.
 *
 * Discovery, init, and teardown for virtio-gpu PCI devices.
 * The CHAIN_GPU_<n> / CHAIN_DISPLAY_<n> hierarchy is registered
 * during gpu_virtio_init(); chain_registry_init() calls into this.
 *
 * 2D only. VIRGL/3D explicitly skipped (per docs/GPU_HOLES.md L1).
 */

#ifndef ZEOS_GPU_VIRTIO_H
#define ZEOS_GPU_VIRTIO_H

#include <stdint.h>

/* Maximum scanouts virtio_gpu_get_display_info reports per spec. */
#define GPU_VIRTIO_MAX_SCANOUTS 16
/* Maximum simultaneous virtio-gpu PCI devices we'll bind. */
#define GPU_VIRTIO_MAX_DEVICES   4

/* Per-display public summary, used by gpustat. */
typedef struct {
    int      gpu_index;          /* Which CHAIN_GPU_n parents this. */
    int      scanout_index;      /* Index within the GPU. */
    int      chain_id;           /* CHAIN_DISPLAY_<n> id. */
    int      enabled;            /* Scanout has a valid mode + resource. */
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;         /* Best-effort from EDID; 0 if unknown. */
    int      edid_valid;
    char     monitor_name[16];   /* Extracted from EDID monitor descriptor. */
} gpu_virtio_display_t;

typedef struct {
    int      chain_id;           /* CHAIN_GPU_n id. */
    int      chain_render_id;
    int      chain_display_id;
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint8_t  pci_bus, pci_dev, pci_func;
    int      num_scanouts;
    int      ready;              /* DRIVER_OK was set successfully. */
} gpu_virtio_device_t;

/* Probe + bind every virtio-gpu PCI device. Registers chain hierarchy.
 * Returns the number of devices successfully brought up.
 *
 * If zero virtio-gpu devices are found, registers a single fallback
 * chain CHAIN_DISPLAY_GOP rooted at CHAIN_CPU so the GOP framebuffer
 * still surfaces in the chain graph -- per docs/GPU_HOLES.md M1.
 *
 * parent_id is CHAIN_CPU. */
int gpu_virtio_init(int parent_id);

/* Number of virtio-gpu devices found at boot. */
int gpu_virtio_device_count(void);

/* Pointer to per-device record, or NULL. */
const gpu_virtio_device_t *gpu_virtio_device(int idx);

/* Number of scanouts on a given device. */
int gpu_virtio_scanout_count(int dev_idx);

/* Per-display info; idx is global (across all devices). Returns 0 on
 * success, -1 on out-of-range. */
int gpu_virtio_display_info(int global_idx, gpu_virtio_display_t *out);

/* Total displays across all GPUs (excluding GOP fallback). */
int gpu_virtio_display_count(void);

/* If no virtio-gpu was found, return 1 and the GOP-fallback chain id
 * via *out_chain_id (0 otherwise). */
int gpu_virtio_gop_fallback(int *out_chain_id);

/* Push current framebuffer contents to scanout 0 of device 0. Used by
 * the compositor as a "flip" hook. Safe to call when no virtio-gpu
 * exists -- it returns 0 and does nothing. Returns 0 on success. */
int gpu_virtio_present(void);

#endif /* ZEOS_GPU_VIRTIO_H */
