/*
 * Zeos -- NVIDIA GPU driver public surface.
 *
 * CHAIN_GPU_<n> peer to virtio-gpu. Stage 1 covers Turing-class
 * scanout (no GSP firmware required): the card is left in the
 * mode the UEFI GOP firmware programmed, we map BAR0 + BAR1 +
 * BAR3, read PMC_BOOT_0 to identify chip family, and register
 * the chain hierarchy + per-output display sub-chains so MasQ /
 * the compositor surface the card.
 *
 * Stage 2 covers Ampere GSP firmware load + RM control path
 * and is wired as an honest stub returning -ENOTREADY when GSP
 * firmware blobs are not embedded. See gpu_nvidia.c for the
 * Stage 2 entry point.
 *
 * Compute backend ("nvidia-N") is registered ONLY when the GSP
 * RM path is up. Stage 1 alone never registers a compute backend.
 */

#ifndef ZEOS_GPU_NVIDIA_H
#define ZEOS_GPU_NVIDIA_H

#include <stdint.h>

#define GPU_NVIDIA_MAX_DEVICES   4
#define GPU_NVIDIA_MAX_OUTPUTS   8

/* PCI vendor for NVIDIA. */
#define NVIDIA_VENDOR_ID         0x10DE

/* PMC_BOOT_0 chip family nibbles (bits 20..23 of the register).
 * Reference: nouveau drivers/gpu/drm/nouveau/include/nvif/class.h
 * (used here only as a layout reference -- no GPL code copied). */
#define NV_FAMILY_KELVIN     0x00
#define NV_FAMILY_RANKINE    0x10
#define NV_FAMILY_CURIE      0x40
#define NV_FAMILY_TESLA      0x50
#define NV_FAMILY_FERMI      0xC0
#define NV_FAMILY_KEPLER     0xE0
#define NV_FAMILY_MAXWELL    0x110
#define NV_FAMILY_PASCAL     0x130
#define NV_FAMILY_VOLTA      0x140
#define NV_FAMILY_TURING     0x160
#define NV_FAMILY_AMPERE     0x170
#define NV_FAMILY_ADA        0x190
#define NV_FAMILY_HOPPER     0x180
#define NV_FAMILY_BLACKWELL  0x1B0
#define NV_FAMILY_UNKNOWN    0xFFFF

typedef enum {
    NV_OUTPUT_UNKNOWN = 0,
    NV_OUTPUT_VGA,
    NV_OUTPUT_TV,
    NV_OUTPUT_TMDS,    /* DVI */
    NV_OUTPUT_LVDS,
    NV_OUTPUT_HDMI,
    NV_OUTPUT_DP,
    NV_OUTPUT_eDP,
} nv_output_kind_t;

typedef struct {
    int              valid;
    int              chain_id;        /* CHAIN_DISPLAY_nvidia<gi>_<port> */
    nv_output_kind_t kind;
    uint8_t          dcb_index;       /* Index in DCB. */
    uint8_t          head;            /* PDISP head/CRTC index. */
    uint8_t          dpaux_port;      /* DPAUX channel for DP/eDP, 0xFF = none. */
    uint8_t          i2c_port;        /* I2C port for HDMI/DVI EDID, 0xFF = none. */
    uint32_t         width;           /* Mode width (from EDID/GOP). */
    uint32_t         height;
    uint32_t         refresh_hz;
    int              edid_valid;
    uint32_t         edid_size;
    uint8_t          edid[256];
    char             monitor_name[16];
} nv_output_t;

typedef struct {
    int      chain_id;                /* CHAIN_GPU_nvidia<n> */
    int      chain_render_id;
    int      chain_display_id;
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint8_t  pci_bus, pci_dev, pci_func;
    uint32_t pmc_boot_0;
    uint16_t chip_family;             /* NV_FAMILY_* */
    int      output_count;
    int      ready;                   /* Stage 1 bring-up succeeded. */
    int      gsp_ready;               /* Stage 2 GSP firmware loaded + handshaked. */
} gpu_nvidia_device_t;

/* Probe + bind every NVIDIA PCI VGA controller. Registers chain
 * hierarchy as a peer to virtio-gpu under parent_id (CHAIN_CPU).
 * Returns the number of cards successfully brought up at Stage 1. */
int gpu_nvidia_init(int parent_id);

int gpu_nvidia_device_count(void);
const gpu_nvidia_device_t *gpu_nvidia_device(int idx);

int gpu_nvidia_output_count(int dev_idx);
int gpu_nvidia_output_info(int dev_idx, int out_idx, nv_output_t *out);

/* Returns the chain_id of CHAIN_GPU_nvidia<idx>, or -1. */
int gpu_nvidia_gpu_chain(int dev_idx);

/* Stage 2 entry point. Loads GSP firmware into the GSP RISC-V
 * core, waits for the GSP-ready handshake, and (on success)
 * registers a "nvidia-N" gpu_compute backend. Returns 0 on
 * success, -ENOTREADY (-22) when GSP firmware blobs are not
 * present in the kernel image. */
int gpu_nvidia_gsp_bringup(int dev_idx);

/* Dump human-readable status to kputs (for the `nvidia` shell
 * command). Lists every detected card, chip family, every
 * output's connector kind / mode / EDID monitor name, fence /
 * perf state. */
void gpu_nvidia_dump_status(void);

/* Selftest summary string suitable for the boot test renderer.
 * Writes up to cap-1 chars + NUL to out. */
void gpu_nvidia_selftest_summary(char *out, int cap);

/* MasQ vault_version bumps on init, mode change, and (Stage 2)
 * GSP load complete + vsync. Currently exposed for tests. */
uint32_t gpu_nvidia_vault_version(int dev_idx);

#endif /* ZEOS_GPU_NVIDIA_H */
