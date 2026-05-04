/*
 * virtio-gpu driver -- first real GPU chain in Zeos.
 * Per docs/GPU_HOLES.md L1: cheapest end-to-end testable GPU.
 * Validates the CHAIN_GPU / CHAIN_DISPLAY hierarchy that real
 * Intel/AMD/NVIDIA drivers will plug into via the same shape.
 * MasQ tier: INTERNAL. Parent: CHAIN_CPU.
 *
 * Modern virtio (1.0+) transport. We scan PCI for vendor 0x1AF4 +
 * device 0x1050 (modern virtio-gpu / virtio-vga). PCI vendor caps
 * (cap_id 0x09) point at common_cfg / notify_cfg / isr_cfg /
 * device_cfg inside one of the BARs. We map them directly via the
 * 4GB identity map (vmm_init), or vmm_map_range above 4GB.
 *
 * Two virtqueues: control (idx 0) and cursor (idx 1). Both allocated
 * via pmm_alloc_contiguous (page-aligned per spec). For boot bring-up
 * we drive only the control queue; cursor queue is allocated and
 * populated to the device but unused by the chain.
 *
 * Pipeline: pixel_source -> composite -> scanout -> hardware_dma
 * The hardware_dma node issues TRANSFER_TO_HOST_2D + RESOURCE_FLUSH
 * on every chain_resolve call. vault_version on the display chain
 * bumps on each successful flush; vault_version on the GPU chain
 * bumps on init/teardown and EDID change.
 */

#include "gpu_virtio.h"
#include "chain.h"
#include "chain_registry.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "fb.h"
#include "kprint.h"
#include "timer.h"
#include "zeos_boot.h"
#include <stdint.h>

/* ── PCI / virtio modern constants ──────────────────────────────── */

#define VIRTIO_VENDOR              0x1AF4
#define VIRTIO_GPU_DEVICE_MODERN   0x1050
/* The transitional ids 0x1010/0x1011 were assigned to early virtio-gpu
 * drafts but never shipped in mainline QEMU. We accept them as well
 * for completeness but the modern code path is what runs. */
#define VIRTIO_GPU_DEVICE_T_A      0x1010
#define VIRTIO_GPU_DEVICE_T_B      0x1011

/* PCI vendor capability list traversal -- type byte at off+3. */
#define VIRTIO_PCI_CAP_VENDOR      0x09
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4
#define VIRTIO_PCI_CAP_PCI_CFG     5

/* Device status bits */
#define VS_ACK              1
#define VS_DRIVER           2
#define VS_DRIVER_OK        4
#define VS_FEATURES_OK      8
#define VS_FAILED         128

/* virtio-gpu feature bits (low 32) */
#define VIRTIO_GPU_F_VIRGL     (1u << 0)
#define VIRTIO_GPU_F_EDID      (1u << 1)
/* Common features (high 32) */
#define VIRTIO_F_VERSION_1_HI  (1u)         /* bit 32 -> hi[0] */

/* Descriptor flags */
#define VRING_DESC_F_NEXT      1
#define VRING_DESC_F_WRITE     2

/* virtio-gpu commands (subset we use) */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_GET_EDID                0x010A

#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101
#define VIRTIO_GPU_RESP_OK_EDID                0x1104

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM       1
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM       121

#define VIRTIO_GPU_FLAG_FENCE      (1u << 0)

/* ── On-the-wire structs ────────────────────────────────────────── */

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

/* virtio-pci modern common config (BAR window). */
struct virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed));

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct {
        struct virtio_gpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[GPU_VIRTIO_MAX_SCANOUTS];
} __attribute__((packed));

struct virtio_gpu_get_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resp_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t padding;
    uint8_t  edid[1024];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* ── MMIO helpers ───────────────────────────────────────────────── */

static inline uint8_t  mmio_r8 (volatile void *p) { return *(volatile uint8_t  *)p; }
static inline uint16_t mmio_r16(volatile void *p) { return *(volatile uint16_t *)p; }
static inline uint32_t mmio_r32(volatile void *p) { return *(volatile uint32_t *)p; }
static inline uint64_t mmio_r64(volatile void *p) { return *(volatile uint64_t *)p; }
static inline void     mmio_w8 (volatile void *p, uint8_t  v) { *(volatile uint8_t  *)p = v; }
static inline void     mmio_w16(volatile void *p, uint16_t v) { *(volatile uint16_t *)p = v; }
static inline void     mmio_w32(volatile void *p, uint32_t v) { *(volatile uint32_t *)p = v; }
static inline void     mmio_w64(volatile void *p, uint64_t v) { *(volatile uint64_t *)p = v; }

/* Read a 64-bit BAR (combine bar[i] + bar[i+1]) and clear flag bits. */
static uint64_t bar_phys64(struct pci_device *d, int idx)
{
    uint32_t lo = d->bar[idx];
    uint64_t addr;
    if ((lo & 0x6) == 0x4) {
        /* 64-bit memory BAR */
        uint32_t hi = d->bar[idx + 1];
        addr = ((uint64_t)hi << 32) | (lo & ~0xFu);
    } else if ((lo & 0x1) == 0) {
        /* 32-bit memory BAR */
        addr = lo & ~0xFu;
    } else {
        /* I/O BAR -- not used for modern virtio caps but handle gracefully */
        addr = lo & ~0x3u;
    }
    return addr;
}

/* Map a BAR window into the kernel address space. Returns a usable
 * pointer (identity below 4GB, mapped above). */
static volatile void *map_bar_window(uint64_t phys, uint64_t length)
{
    if (phys == 0 || length == 0) return 0;

    if (phys + length <= 0x100000000ull) {
        /* Already identity-mapped by vmm_init's first 4GB. */
        return (volatile void *)(uintptr_t)phys;
    }

    /* Above 4GB: map the spanning pages NOCACHE. */
    uint64_t start = phys & ~0xFFFull;
    uint64_t end   = (phys + length + 0xFFFull) & ~0xFFFull;
    uint64_t pages = (end - start) / 4096;
    vmm_map_range(start, start, pages, PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE);
    return (volatile void *)(uintptr_t)phys;
}

/* ── Per-device state ───────────────────────────────────────────── */

#define VQ_SIZE 64        /* Descriptors per queue. Plenty for 2D. */

typedef struct {
    uint16_t            size;
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint16_t            last_used;
    uint64_t            phys_base;   /* Physical base of the queue mem. */
    uint64_t            phys_avail;
    uint64_t            phys_used;
} vqueue_t;

typedef struct {
    int      valid;
    int      enabled;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;
    uint32_t resource_id;            /* virtio resource id, 1-based. */
    int      chain_id;               /* CHAIN_DISPLAY_<n> */
    /* Per-display vsync decoupling: skip a flush if the previous flush
     * was less than min_flush_interval_us ago. Default = 16667us
     * (60Hz). Higher-Hz panels can lower this; e-paper / low-power
     * panels can raise it. Independent per scanout so a 144Hz monitor
     * never blocks a 60Hz monitor. */
    uint32_t min_flush_interval_us;
    uint64_t last_flush_tsc;
    int      edid_valid;
    uint32_t edid_size;
    uint8_t  edid[256];              /* Block 0 + optional extension. */
    char     monitor_name[16];
} scanout_t;

typedef struct gpu_dev {
    int      in_use;
    int      ready;
    struct pci_device *pci;

    volatile struct virtio_pci_common_cfg *common;
    volatile uint8_t                       *isr;
    volatile uint8_t                       *device_cfg;
    /* Notify region: address per queue = notify_base + queue_notify_off * notify_off_multiplier */
    volatile uint8_t                       *notify_base;
    uint32_t                                notify_off_multiplier;

    vqueue_t controlq;
    vqueue_t cursorq;

    int       num_scanouts;
    scanout_t scanouts[GPU_VIRTIO_MAX_SCANOUTS];

    int       chain_id;          /* CHAIN_GPU_n */
    int       chain_render_id;
    int       chain_display_id;

    /* Bookkeeping for chain wiring. */
    int       gpu_index;
} gpu_dev_t;

static gpu_dev_t s_devs[GPU_VIRTIO_MAX_DEVICES];
static int       s_dev_count = 0;
static int       s_total_scanouts = 0;

/* GOP fallback chain (no virtio-gpu present). */
static int s_gop_fallback_chain = -1;

/* Display index mapping (global flat -> (dev,scanout)). Built after
 * scanouts are enumerated; a helper iterates. */

/* ── Forward decls ──────────────────────────────────────────────── */

static int vq_setup(gpu_dev_t *gd, vqueue_t *vq, uint16_t qidx);
static int gpu_dev_bringup(gpu_dev_t *gd, struct pci_device *pci, int gpu_index);
static int gpu_get_display_info(gpu_dev_t *gd);
static int gpu_get_edid(gpu_dev_t *gd, int scanout_idx);
static int gpu_create_2d(gpu_dev_t *gd, int scanout_idx, uint32_t resource_id,
                         uint32_t width, uint32_t height);
static int gpu_attach_backing(gpu_dev_t *gd, uint32_t resource_id,
                              uint64_t fb_phys, uint32_t fb_len);
static int gpu_set_scanout(gpu_dev_t *gd, int scanout_idx, uint32_t resource_id,
                           uint32_t width, uint32_t height);
static int gpu_transfer_and_flush(gpu_dev_t *gd, int scanout_idx);
static int register_chain_hierarchy(gpu_dev_t *gd, int parent_id);

/* ── Virtqueue allocation + setup ───────────────────────────────── */

/* Spec layout sizes for queue size N:
 *   desc:  16 * N
 *   avail: 6 + 2 * N        (then pad to next 4-byte boundary; we go to 4K)
 *   used:  6 + 8 * N
 * Used must be page-aligned. We allocate one contiguous page-aligned
 * region big enough for all three. */
static int vq_setup(gpu_dev_t *gd, vqueue_t *vq, uint16_t qidx)
{
    /* Select queue, read its allowed size, clamp to ours. */
    mmio_w16(&gd->common->queue_select, qidx);
    uint16_t qsz = mmio_r16(&gd->common->queue_size);
    if (qsz == 0) return -1;
    if (qsz > VQ_SIZE) qsz = VQ_SIZE;
    /* Tell device our chosen size. */
    mmio_w16(&gd->common->queue_size, qsz);

    uint64_t desc_size  = (uint64_t)qsz * 16;
    uint64_t avail_size = 6 + (uint64_t)qsz * 2;
    uint64_t used_off   = (desc_size + avail_size + 4095ull) & ~4095ull;
    uint64_t used_size  = 6 + (uint64_t)qsz * 8;
    uint64_t total      = used_off + used_size;
    uint64_t pages      = (total + 4095ull) / 4096ull;

    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) return -1;
    uint8_t *mem = (uint8_t *)(uintptr_t)phys;
    for (uint64_t i = 0; i < pages * 4096ull; i++) mem[i] = 0;

    vq->size       = qsz;
    vq->desc       = (struct vring_desc  *)mem;
    vq->avail      = (struct vring_avail *)(mem + desc_size);
    vq->used       = (struct vring_used  *)(mem + used_off);
    vq->last_used  = 0;
    vq->phys_base  = phys;
    vq->phys_avail = phys + desc_size;
    vq->phys_used  = phys + used_off;

    /* Hand the device the addresses + enable. */
    mmio_w64(&gd->common->queue_desc,   vq->phys_base);
    mmio_w64(&gd->common->queue_driver, vq->phys_avail);
    mmio_w64(&gd->common->queue_device, vq->phys_used);
    mmio_w16(&gd->common->queue_enable, 1);
    return 0;
}

/* Notify a queue. */
static void vq_kick(gpu_dev_t *gd, uint16_t qidx)
{
    mmio_w16(&gd->common->queue_select, qidx);
    uint16_t off = mmio_r16(&gd->common->queue_notify_off);
    volatile uint16_t *p =
        (volatile uint16_t *)(gd->notify_base + (uint64_t)off * gd->notify_off_multiplier);
    *p = qidx;
}

/* Submit a request (cmd buf -> resp buf) on the control queue and
 * synchronously poll for completion. Two descriptors chained:
 * desc[0] = device-readable cmd
 * desc[1] = device-writable response
 * Returns 0 on completion (does not interpret the response type). */
static int vq_submit_sync(gpu_dev_t *gd,
                          const void *cmd, uint32_t cmd_len,
                          void *resp, uint32_t resp_len)
{
    vqueue_t *vq = &gd->controlq;
    if (vq->size < 2) return -1;

    uint16_t d0 = 0;
    uint16_t d1 = 1;

    vq->desc[d0].addr  = (uint64_t)(uintptr_t)cmd;
    vq->desc[d0].len   = cmd_len;
    vq->desc[d0].flags = VRING_DESC_F_NEXT;
    vq->desc[d0].next  = d1;

    vq->desc[d1].addr  = (uint64_t)(uintptr_t)resp;
    vq->desc[d1].len   = resp_len;
    vq->desc[d1].flags = VRING_DESC_F_WRITE;
    vq->desc[d1].next  = 0;

    uint16_t avail_slot = vq->avail->idx % vq->size;
    vq->avail->ring[avail_slot] = d0;
    __asm__ volatile("" ::: "memory");
    vq->avail->idx++;
    __asm__ volatile("" ::: "memory");

    vq_kick(gd, 0);

    /* Poll for completion. Bound the wait so a misbehaving host doesn't
     * hang boot; ~50M iters is hundreds of ms even on slow VMs. */
    uint64_t spins = 0;
    while (vq->used->idx == vq->last_used) {
        if (++spins > 50000000ull) return -1;
        __asm__ volatile("pause" ::: "memory");
    }
    vq->last_used = vq->used->idx;
    return 0;
}

/* ── Capability walk ────────────────────────────────────────────── */

/* Walk PCI vendor caps (cap_id 0x09); for each, locate which BAR
 * region it points to and stash its base + length on the gpu_dev_t.
 * Returns 0 on success when we have at least common+notify+device. */
static int walk_caps(gpu_dev_t *gd)
{
    struct pci_device *d = gd->pci;

    /* Status register cap-list bit must be set; otherwise nothing. */
    uint32_t status_cmd = pci_config_read32(d->bus, d->dev, d->func, 0x04);
    if (!((status_cmd >> 16) & (1u << 4))) return -1;

    uint8_t htype = d->header_type & 0x7f;
    uint8_t ptr_off = (htype == 0x01 || htype == 0x02) ? 0x14 : 0x34;
    uint32_t pp = pci_config_read32(d->bus, d->dev, d->func, ptr_off);
    uint8_t off = (uint8_t)(pp & 0xfc);

    int hops = 0;
    int got_common = 0, got_notify = 0, got_device = 0;

    while (off && off != 0xff && hops < 48) {
        uint32_t cap_dw0 = pci_config_read32(d->bus, d->dev, d->func, off + 0);
        uint32_t cap_dw1 = pci_config_read32(d->bus, d->dev, d->func, off + 4);
        uint32_t cap_dw2 = pci_config_read32(d->bus, d->dev, d->func, off + 8);
        uint32_t cap_dw3 = pci_config_read32(d->bus, d->dev, d->func, off + 12);
        uint32_t cap_dw4 = pci_config_read32(d->bus, d->dev, d->func, off + 16);

        uint8_t cap_id  = (uint8_t)(cap_dw0 & 0xff);
        uint8_t next    = (uint8_t)((cap_dw0 >> 8) & 0xff);
        /* uint8_t cap_len = (uint8_t)((cap_dw0 >> 16) & 0xff); */
        uint8_t cfg_typ = (uint8_t)((cap_dw0 >> 24) & 0xff);
        uint8_t bar_idx = (uint8_t)(cap_dw1 & 0xff);
        uint32_t cap_off = cap_dw2;
        uint32_t cap_len = cap_dw3;

        if (cap_id == VIRTIO_PCI_CAP_VENDOR && bar_idx < 6) {
            uint64_t bar_base = bar_phys64(d, bar_idx);
            volatile void *win = map_bar_window(bar_base + cap_off, cap_len);
            switch (cfg_typ) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                gd->common = (volatile struct virtio_pci_common_cfg *)win;
                got_common = 1;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                gd->notify_base = (volatile uint8_t *)win;
                gd->notify_off_multiplier = cap_dw4;
                got_notify = 1;
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                gd->isr = (volatile uint8_t *)win;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                gd->device_cfg = (volatile uint8_t *)win;
                got_device = 1;
                break;
            default:
                break;
            }
        }
        off = (uint8_t)(next & 0xfc);
        hops++;
    }

    if (!got_common || !got_notify || !got_device) return -1;
    return 0;
}

/* ── EDID monitor-name extraction ───────────────────────────────── */

/* EDID descriptor blocks live at bytes 54..125, four 18-byte records.
 * A record starting with 00 00 00 FC is a Monitor Name (ASCII, 13 bytes,
 * 0x0a-terminated or space-padded). */
static void edid_extract_monitor_name(const uint8_t *edid, uint32_t size,
                                      char out[16])
{
    out[0] = 0;
    if (size < 128) return;
    for (int d = 0; d < 4; d++) {
        const uint8_t *r = edid + 54 + d * 18;
        if (r[0] == 0x00 && r[1] == 0x00 && r[2] == 0x00 && r[3] == 0xFC) {
            int n = 0;
            for (int i = 5; i < 18 && n < 15; i++) {
                uint8_t c = r[i];
                if (c == 0x0A || c == 0) break;
                if (c < 0x20 || c > 0x7E) c = ' ';
                out[n++] = (char)c;
            }
            /* Strip trailing spaces. */
            while (n > 0 && out[n - 1] == ' ') n--;
            out[n] = 0;
            return;
        }
    }
}

/* ── Static command buffers (one in flight per device) ──────────── */

static struct virtio_gpu_resp_display_info       s_resp_di;
static struct virtio_gpu_get_edid                s_cmd_edid;
static struct virtio_gpu_resp_edid               s_resp_edid;
static struct virtio_gpu_resource_create_2d      s_cmd_create2d;
static struct virtio_gpu_ctrl_hdr                s_resp_hdr_a;
static struct virtio_gpu_resource_attach_backing s_cmd_attach;
static struct {
    struct virtio_gpu_resource_attach_backing hdr;
    struct virtio_gpu_mem_entry               entry;
} __attribute__((packed)) s_cmd_attach_full;
static struct virtio_gpu_ctrl_hdr                s_resp_hdr_b;
static struct virtio_gpu_set_scanout             s_cmd_setscanout;
static struct virtio_gpu_ctrl_hdr                s_resp_hdr_c;
static struct virtio_gpu_transfer_to_host_2d     s_cmd_xfer;
static struct virtio_gpu_ctrl_hdr                s_resp_hdr_d;
static struct virtio_gpu_resource_flush          s_cmd_flush;
static struct virtio_gpu_ctrl_hdr                s_resp_hdr_e;
static struct virtio_gpu_ctrl_hdr                s_cmd_di_hdr;

/* ── virtio-gpu commands ────────────────────────────────────────── */

static int gpu_get_display_info(gpu_dev_t *gd)
{
    s_cmd_di_hdr.type     = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    s_cmd_di_hdr.flags    = 0;
    s_cmd_di_hdr.fence_id = 0;
    s_cmd_di_hdr.ctx_id   = 0;
    s_cmd_di_hdr.padding  = 0;

    /* Zero response. */
    {
        uint8_t *p = (uint8_t *)&s_resp_di;
        for (uint64_t i = 0; i < sizeof(s_resp_di); i++) p[i] = 0;
    }

    if (vq_submit_sync(gd, &s_cmd_di_hdr, sizeof(s_cmd_di_hdr),
                       &s_resp_di, sizeof(s_resp_di)) != 0)
        return -1;
    if (s_resp_di.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) return -1;

    int n = 0;
    for (int i = 0; i < GPU_VIRTIO_MAX_SCANOUTS; i++) {
        scanout_t *s = &gd->scanouts[i];
        s->valid       = 1;
        s->enabled     = s_resp_di.pmodes[i].enabled ? 1 : 0;
        s->width       = s_resp_di.pmodes[i].r.width;
        s->height      = s_resp_di.pmodes[i].r.height;
        s->refresh_hz  = 0;
        s->resource_id = (uint32_t)(i + 1);     /* 1-based per spec */
        s->edid_valid  = 0;
        s->edid_size   = 0;
        s->monitor_name[0] = 0;
        /* 60Hz default flush rate; overridden once EDID parses a real
         * refresh, or by a future per-scanout setter. */
        s->min_flush_interval_us = 16667;
        s->last_flush_tsc        = 0;
        if (s->enabled) n++;
    }
    /* num_scanouts is the largest enabled index + 1, but for our
     * purposes the connected count is what matters. We register one
     * display chain per enabled scanout. */
    gd->num_scanouts = n;
    return 0;
}

static int gpu_get_edid(gpu_dev_t *gd, int scanout_idx)
{
    s_cmd_edid.hdr.type     = VIRTIO_GPU_CMD_GET_EDID;
    s_cmd_edid.hdr.flags    = 0;
    s_cmd_edid.hdr.fence_id = 0;
    s_cmd_edid.hdr.ctx_id   = 0;
    s_cmd_edid.hdr.padding  = 0;
    s_cmd_edid.scanout      = (uint32_t)scanout_idx;
    s_cmd_edid.padding      = 0;

    {
        uint8_t *p = (uint8_t *)&s_resp_edid;
        for (uint64_t i = 0; i < sizeof(s_resp_edid); i++) p[i] = 0;
    }

    if (vq_submit_sync(gd, &s_cmd_edid, sizeof(s_cmd_edid),
                       &s_resp_edid, sizeof(s_resp_edid)) != 0)
        return -1;
    if (s_resp_edid.hdr.type != VIRTIO_GPU_RESP_OK_EDID) return -1;

    scanout_t *s = &gd->scanouts[scanout_idx];
    uint32_t sz = s_resp_edid.size;
    if (sz > sizeof(s->edid)) sz = sizeof(s->edid);
    for (uint32_t i = 0; i < sz; i++) s->edid[i] = s_resp_edid.edid[i];
    s->edid_size  = sz;
    s->edid_valid = (sz >= 128) ? 1 : 0;
    if (s->edid_valid) {
        edid_extract_monitor_name(s->edid, sz, s->monitor_name);
    }
    return 0;
}

static int gpu_create_2d(gpu_dev_t *gd, int scanout_idx, uint32_t resource_id,
                         uint32_t width, uint32_t height)
{
    (void)scanout_idx;
    s_cmd_create2d.hdr.type     = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    s_cmd_create2d.hdr.flags    = 0;
    s_cmd_create2d.hdr.fence_id = 0;
    s_cmd_create2d.hdr.ctx_id   = 0;
    s_cmd_create2d.hdr.padding  = 0;
    s_cmd_create2d.resource_id  = resource_id;
    s_cmd_create2d.format       = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    s_cmd_create2d.width        = width;
    s_cmd_create2d.height       = height;

    {
        uint8_t *p = (uint8_t *)&s_resp_hdr_a;
        for (uint64_t i = 0; i < sizeof(s_resp_hdr_a); i++) p[i] = 0;
    }
    if (vq_submit_sync(gd, &s_cmd_create2d, sizeof(s_cmd_create2d),
                       &s_resp_hdr_a, sizeof(s_resp_hdr_a)) != 0)
        return -1;
    if (s_resp_hdr_a.type != VIRTIO_GPU_RESP_OK_NODATA) return -1;
    return 0;
}

static int gpu_attach_backing(gpu_dev_t *gd, uint32_t resource_id,
                              uint64_t fb_phys, uint32_t fb_len)
{
    s_cmd_attach_full.hdr.hdr.type     = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    s_cmd_attach_full.hdr.hdr.flags    = 0;
    s_cmd_attach_full.hdr.hdr.fence_id = 0;
    s_cmd_attach_full.hdr.hdr.ctx_id   = 0;
    s_cmd_attach_full.hdr.hdr.padding  = 0;
    s_cmd_attach_full.hdr.resource_id  = resource_id;
    s_cmd_attach_full.hdr.nr_entries   = 1;
    s_cmd_attach_full.entry.addr       = fb_phys;
    s_cmd_attach_full.entry.length     = fb_len;
    s_cmd_attach_full.entry.padding    = 0;

    {
        uint8_t *p = (uint8_t *)&s_resp_hdr_b;
        for (uint64_t i = 0; i < sizeof(s_resp_hdr_b); i++) p[i] = 0;
    }

    /* Defeat 'unused' flag on s_cmd_attach */
    (void)s_cmd_attach;

    if (vq_submit_sync(gd, &s_cmd_attach_full, sizeof(s_cmd_attach_full),
                       &s_resp_hdr_b, sizeof(s_resp_hdr_b)) != 0)
        return -1;
    if (s_resp_hdr_b.type != VIRTIO_GPU_RESP_OK_NODATA) return -1;
    return 0;
}

static int gpu_set_scanout(gpu_dev_t *gd, int scanout_idx, uint32_t resource_id,
                           uint32_t width, uint32_t height)
{
    s_cmd_setscanout.hdr.type     = VIRTIO_GPU_CMD_SET_SCANOUT;
    s_cmd_setscanout.hdr.flags    = 0;
    s_cmd_setscanout.hdr.fence_id = 0;
    s_cmd_setscanout.hdr.ctx_id   = 0;
    s_cmd_setscanout.hdr.padding  = 0;
    s_cmd_setscanout.r.x          = 0;
    s_cmd_setscanout.r.y          = 0;
    s_cmd_setscanout.r.width      = width;
    s_cmd_setscanout.r.height     = height;
    s_cmd_setscanout.scanout_id   = (uint32_t)scanout_idx;
    s_cmd_setscanout.resource_id  = resource_id;

    {
        uint8_t *p = (uint8_t *)&s_resp_hdr_c;
        for (uint64_t i = 0; i < sizeof(s_resp_hdr_c); i++) p[i] = 0;
    }
    if (vq_submit_sync(gd, &s_cmd_setscanout, sizeof(s_cmd_setscanout),
                       &s_resp_hdr_c, sizeof(s_resp_hdr_c)) != 0)
        return -1;
    if (s_resp_hdr_c.type != VIRTIO_GPU_RESP_OK_NODATA) return -1;
    return 0;
}

static int gpu_transfer_and_flush(gpu_dev_t *gd, int scanout_idx)
{
    scanout_t *s = &gd->scanouts[scanout_idx];
    if (!s->enabled) return -1;

    /* TRANSFER_TO_HOST_2D: copy from guest backing into host resource. */
    s_cmd_xfer.hdr.type     = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    s_cmd_xfer.hdr.flags    = 0;
    s_cmd_xfer.hdr.fence_id = 0;
    s_cmd_xfer.hdr.ctx_id   = 0;
    s_cmd_xfer.hdr.padding  = 0;
    s_cmd_xfer.r.x          = 0;
    s_cmd_xfer.r.y          = 0;
    s_cmd_xfer.r.width      = s->width;
    s_cmd_xfer.r.height     = s->height;
    s_cmd_xfer.offset       = 0;
    s_cmd_xfer.resource_id  = s->resource_id;
    s_cmd_xfer.padding      = 0;

    {
        uint8_t *p = (uint8_t *)&s_resp_hdr_d;
        for (uint64_t i = 0; i < sizeof(s_resp_hdr_d); i++) p[i] = 0;
    }
    if (vq_submit_sync(gd, &s_cmd_xfer, sizeof(s_cmd_xfer),
                       &s_resp_hdr_d, sizeof(s_resp_hdr_d)) != 0)
        return -1;
    if (s_resp_hdr_d.type != VIRTIO_GPU_RESP_OK_NODATA) return -1;

    /* RESOURCE_FLUSH: tell host to push pixels to the actual scanout. */
    s_cmd_flush.hdr.type     = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    s_cmd_flush.hdr.flags    = 0;
    s_cmd_flush.hdr.fence_id = 0;
    s_cmd_flush.hdr.ctx_id   = 0;
    s_cmd_flush.hdr.padding  = 0;
    s_cmd_flush.r.x          = 0;
    s_cmd_flush.r.y          = 0;
    s_cmd_flush.r.width      = s->width;
    s_cmd_flush.r.height     = s->height;
    s_cmd_flush.resource_id  = s->resource_id;
    s_cmd_flush.padding      = 0;

    {
        uint8_t *p = (uint8_t *)&s_resp_hdr_e;
        for (uint64_t i = 0; i < sizeof(s_resp_hdr_e); i++) p[i] = 0;
    }
    if (vq_submit_sync(gd, &s_cmd_flush, sizeof(s_cmd_flush),
                       &s_resp_hdr_e, sizeof(s_resp_hdr_e)) != 0)
        return -1;
    if (s_resp_hdr_e.type != VIRTIO_GPU_RESP_OK_NODATA) return -1;

    return 0;
}

/* ── Chain node resolves ────────────────────────────────────────── */
/*
 * Pipeline (per CHAIN_DISPLAY_<n>):
 *   pixel_source  -> composite -> scanout -> hardware_dma
 *
 * For boot-up the GOP framebuffer IS the pixel source -- the
 * compositor writes directly into it. composite is a typed pass-
 * through (we already have a ready buffer). scanout records the
 * intended scanout token. hardware_dma issues TRANSFER + FLUSH.
 *
 * Per-node state pointer: scanout_t *.
 */

static void pixel_source_resolve(chain_node_t *self, void *input, void *output)
{
    (void)input;
    /* Output type "pixel_buffer" -- not used between virtio resolves;
     * the actual pixels live in the GOP framebuffer attached as the
     * resource backing. We just mark the output as "ready". */
    int *ok = (int *)output;
    *ok = (self->state != 0) ? 1 : 0;
}

static void composite_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    int *in_ok  = (int *)input;
    int *out_ok = (int *)output;
    *out_ok = *in_ok;
}

static void scanout_resolve(chain_node_t *self, void *input, void *output)
{
    int *in_ok  = (int *)input;
    int *out_token = (int *)output;
    scanout_t *s = (scanout_t *)self->state;
    *out_token = (*in_ok && s && s->enabled) ? 1 : 0;
}

static void virtio_dma_resolve(chain_node_t *self, void *input, void *output)
{
    int *in_token = (int *)input;
    int *out_ok   = (int *)output;
    *out_ok = 0;
    if (!*in_token) return;

    scanout_t *target = (scanout_t *)self->state;
    if (!target) return;

    /* Per-display vsync gate: don't flush faster than the panel can
     * actually scan out. Cheap TSC compare, no IO. Different displays
     * keep independent intervals so a 144Hz panel doesn't bottleneck
     * on a 60Hz one. */
    uint64_t now = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    if (target->min_flush_interval_us > 0 && freq > 0
        && target->last_flush_tsc != 0) {
        uint64_t elapsed_us = (now - target->last_flush_tsc) * 1000000ULL / freq;
        if (elapsed_us < (uint64_t)target->min_flush_interval_us) {
            return;  /* too soon: skip this flush */
        }
    }

    for (int gi = 0; gi < s_dev_count; gi++) {
        gpu_dev_t *gd = &s_devs[gi];
        if (!gd->ready) continue;
        for (int si = 0; si < GPU_VIRTIO_MAX_SCANOUTS; si++) {
            if (&gd->scanouts[si] == target) {
                if (gpu_transfer_and_flush(gd, si) == 0) {
                    *out_ok = 1;
                    target->last_flush_tsc = now;
                    chain_t *c = chain_get(target->chain_id);
                    if (c) c->vault_version++;
                }
                return;
            }
        }
    }
}

/* GOP-fallback hardware_dma: no virtio device, the GOP framebuffer
 * is already the visible surface. The "flush" is a no-op but we
 * still bump vault_version per resolve to keep MasQ honest. */
static void gop_dma_resolve(chain_node_t *self, void *input, void *output)
{
    (void)input;
    int *out_ok = (int *)output;
    *out_ok = 1;
    chain_t *c = chain_get(s_gop_fallback_chain);
    if (c) c->vault_version++;
    (void)self;
}

/* ── Chain hierarchy registration ───────────────────────────────── */

/* Numeric-to-ascii into a small buf. */
static void utoa_small(uint32_t v, char *out)
{
    char tmp[12]; int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v && n < 11) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

static void name_concat(char *out, int cap, const char *a, const char *b, const char *c)
{
    int j = 0;
    for (const char *p = a; p && *p && j < cap - 1; p++) out[j++] = *p;
    for (const char *p = b; p && *p && j < cap - 1; p++) out[j++] = *p;
    for (const char *p = c; p && *p && j < cap - 1; p++) out[j++] = *p;
    out[j] = 0;
}

static int register_chain_hierarchy(gpu_dev_t *gd, int parent_id)
{
    char name[64];
    char num[12]; utoa_small((uint32_t)gd->gpu_index, num);

    /* CHAIN_GPU_n */
    name_concat(name, sizeof(name), "gpu", num, "");
    int gpu_id = chain_create(name, parent_id, MASQ_INTERNAL);
    if (gpu_id < 0) return -1;
    gd->chain_id = gpu_id;
    chain_t *gpu_chain = chain_get(gpu_id);
    if (gpu_chain) gpu_chain->vault_version++;  /* init bump */

    /* CHAIN_GPU_n.render */
    name_concat(name, sizeof(name), "gpu", num, ".render");
    gd->chain_render_id = chain_create(name, gpu_id, MASQ_INTERNAL);

    /* CHAIN_GPU_n.display */
    name_concat(name, sizeof(name), "gpu", num, ".display");
    gd->chain_display_id = chain_create(name, gpu_id, MASQ_INTERNAL);

    /* Per scanout that is ENABLED: build the 4-node display pipeline. */
    for (int si = 0; si < GPU_VIRTIO_MAX_SCANOUTS; si++) {
        scanout_t *s = &gd->scanouts[si];
        if (!s->valid || !s->enabled) continue;

        /* Build name: display.gpu<n>.scanout<si> */
        char snum[12]; utoa_small((uint32_t)si, snum);
        char head[32]; name_concat(head, sizeof(head), "display.gpu", num, ".scanout");
        char full[64]; name_concat(full, sizeof(full), head, snum, "");

        int dchain = chain_create(full, gd->chain_display_id, MASQ_INTERNAL);
        if (dchain < 0) continue;
        s->chain_id = dchain;

        int n0 = chain_add_node(dchain, "pixel_source",
                                "frame_request", "pixel_buffer",
                                pixel_source_resolve);
        (void)chain_add_node(dchain, "composite",
                             "pixel_buffer", "pixel_buffer",
                             composite_resolve);
        int n2 = chain_add_node(dchain, "scanout",
                                "pixel_buffer", "scanout_token",
                                scanout_resolve);
        int n3 = chain_add_node(dchain, "hardware_dma",
                                "scanout_token", "scanout_complete",
                                virtio_dma_resolve);

        chain_t *c = chain_get(dchain);
        if (c) {
            if (n0 >= 0) c->nodes[n0].state = s;   /* "ready if state non-null" */
            if (n2 >= 0) c->nodes[n2].state = s;
            if (n3 >= 0) c->nodes[n3].state = s;
            /* Bump for the EDID/mode-set events that just landed. */
            c->vault_version++;
            /* Display chains resolve at most every 16 scheduler ticks;
             * the per-scanout min_flush_interval_us is the second gate.
             * Together they keep flushes bounded by both tick cadence
             * and physical vsync. */
            c->resolve_interval_ticks = 16;
        }
        s_total_scanouts++;
    }

    return 0;
}

/* ── Per-device bring-up ────────────────────────────────────────── */

static int gpu_dev_bringup(gpu_dev_t *gd, struct pci_device *pci, int gpu_index)
{
    gd->pci = pci;
    gd->gpu_index = gpu_index;

    /* Enable memory + bus master in command register. */
    uint32_t cmd = pci_config_read32(pci->bus, pci->dev, pci->func, 0x04);
    cmd |= (1u << 1);   /* Memory space enable */
    cmd |= (1u << 2);   /* Bus master enable */
    pci_config_write32(pci->bus, pci->dev, pci->func, 0x04, cmd);

    if (walk_caps(gd) != 0) {
        kputs("[gpu_virtio] capability walk failed\n");
        return -1;
    }

    /* Reset, ACK, DRIVER. */
    mmio_w8(&gd->common->device_status, 0);
    /* Tiny spin so the device sees the reset. */
    for (volatile int i = 0; i < 1000; i++) { __asm__ volatile("pause"); }
    mmio_w8(&gd->common->device_status, VS_ACK);
    mmio_w8(&gd->common->device_status, VS_ACK | VS_DRIVER);

    /* Negotiate features. We REQUIRE VIRTIO_F_VERSION_1 (bit 32) and
     * accept VIRTIO_GPU_F_EDID if offered. We never accept VIRGL. */
    mmio_w32(&gd->common->device_feature_select, 0);
    uint32_t feat_lo = mmio_r32(&gd->common->device_feature);
    mmio_w32(&gd->common->device_feature_select, 1);
    uint32_t feat_hi = mmio_r32(&gd->common->device_feature);

    if (!(feat_hi & VIRTIO_F_VERSION_1_HI)) {
        /* Modern transport without VERSION_1 is illegal; bail. */
        mmio_w8(&gd->common->device_status, VS_FAILED);
        return -1;
    }
    uint32_t accept_lo = 0;
    uint32_t accept_hi = VIRTIO_F_VERSION_1_HI;
    if (feat_lo & VIRTIO_GPU_F_EDID) accept_lo |= VIRTIO_GPU_F_EDID;

    mmio_w32(&gd->common->driver_feature_select, 0);
    mmio_w32(&gd->common->driver_feature, accept_lo);
    mmio_w32(&gd->common->driver_feature_select, 1);
    mmio_w32(&gd->common->driver_feature, accept_hi);

    mmio_w8(&gd->common->device_status, VS_ACK | VS_DRIVER | VS_FEATURES_OK);
    uint8_t st = mmio_r8(&gd->common->device_status);
    if (!(st & VS_FEATURES_OK)) {
        mmio_w8(&gd->common->device_status, VS_FAILED);
        return -1;
    }

    /* Setup virtqueues. */
    if (vq_setup(gd, &gd->controlq, 0) != 0) {
        kputs("[gpu_virtio] controlq setup failed\n");
        mmio_w8(&gd->common->device_status, VS_FAILED);
        return -1;
    }
    if (vq_setup(gd, &gd->cursorq, 1) != 0) {
        kputs("[gpu_virtio] cursorq setup failed\n");
        mmio_w8(&gd->common->device_status, VS_FAILED);
        return -1;
    }

    /* DRIVER_OK. */
    mmio_w8(&gd->common->device_status,
            VS_ACK | VS_DRIVER | VS_FEATURES_OK | VS_DRIVER_OK);

    /* Enumerate scanouts. */
    if (gpu_get_display_info(gd) != 0) {
        kputs("[gpu_virtio] GET_DISPLAY_INFO failed\n");
        return -1;
    }

    /* For each enabled scanout: best-effort EDID, create_2d, attach
     * the GOP framebuffer as backing, set scanout, transfer+flush. */
    uint32_t fb_w = fb_width();
    uint32_t fb_h = fb_height();
    uint64_t fb_phys = fb_phys_base();
    if (fb_phys == 0 || fb_w == 0 || fb_h == 0) {
        kputs("[gpu_virtio] no usable framebuffer for backing\n");
        return -1;
    }
    uint32_t fb_len = fb_w * fb_h * 4u;

    for (int si = 0; si < GPU_VIRTIO_MAX_SCANOUTS; si++) {
        scanout_t *s = &gd->scanouts[si];
        if (!s->valid || !s->enabled) continue;

        /* Try EDID if feature negotiated. Failure is non-fatal. */
        if (accept_lo & VIRTIO_GPU_F_EDID) {
            (void)gpu_get_edid(gd, si);
        }

        /* For QEMU's default, scanout reports the host-window size
         * which may not match our GOP framebuffer. We override
         * width/height to the GOP size so the backing matches. */
        s->width  = fb_w;
        s->height = fb_h;

        if (gpu_create_2d(gd, si, s->resource_id, s->width, s->height) != 0) {
            kputs("[gpu_virtio] CREATE_2D failed\n");
            s->enabled = 0;
            continue;
        }
        if (gpu_attach_backing(gd, s->resource_id, fb_phys, fb_len) != 0) {
            kputs("[gpu_virtio] ATTACH_BACKING failed\n");
            s->enabled = 0;
            continue;
        }
        if (gpu_set_scanout(gd, si, s->resource_id, s->width, s->height) != 0) {
            kputs("[gpu_virtio] SET_SCANOUT failed\n");
            s->enabled = 0;
            continue;
        }
        /* Initial flush so the GOP contents reach the host scanout. */
        (void)gpu_transfer_and_flush(gd, si);
    }

    gd->ready = 1;
    return 0;
}

/* ── Public entry points ────────────────────────────────────────── */

int gpu_virtio_init(int parent_id)
{
    s_dev_count = 0;
    s_total_scanouts = 0;
    s_gop_fallback_chain = -1;

    int n = pci_device_count();
    int gpu_index = 0;
    for (int i = 0; i < n && s_dev_count < GPU_VIRTIO_MAX_DEVICES; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->vendor_id != VIRTIO_VENDOR) continue;
        if (d->device_id != VIRTIO_GPU_DEVICE_MODERN &&
            d->device_id != VIRTIO_GPU_DEVICE_T_A &&
            d->device_id != VIRTIO_GPU_DEVICE_T_B) continue;

        gpu_dev_t *gd = &s_devs[s_dev_count];
        for (uint64_t j = 0; j < sizeof(*gd); j++) ((uint8_t*)gd)[j] = 0;
        gd->in_use = 1;

        kputs("[gpu_virtio] binding ");
        kput_hex((uint64_t)d->vendor_id);
        kputs(":");
        kput_hex((uint64_t)d->device_id);
        kputs(" at ");
        kput_dec((uint64_t)d->bus); kputs(":");
        kput_dec((uint64_t)d->dev); kputs(".");
        kput_dec((uint64_t)d->func); kputc('\n');

        if (gpu_dev_bringup(gd, d, gpu_index) == 0) {
            register_chain_hierarchy(gd, parent_id);
            s_dev_count++;
            gpu_index++;
        } else {
            kputs("[gpu_virtio] bring-up failed for this device\n");
        }
    }

    if (s_dev_count == 0) {
        /* GOP fallback: register a degenerate display chain so the
         * compositor still sees one display target. */
        s_gop_fallback_chain =
            chain_create("display.gop", parent_id, MASQ_INTERNAL);
        if (s_gop_fallback_chain >= 0) {
            chain_add_node(s_gop_fallback_chain, "pixel_source",
                           "frame_request", "pixel_buffer",
                           pixel_source_resolve);
            chain_add_node(s_gop_fallback_chain, "composite",
                           "pixel_buffer", "pixel_buffer",
                           composite_resolve);
            chain_add_node(s_gop_fallback_chain, "scanout",
                           "pixel_buffer", "scanout_token",
                           scanout_resolve);
            chain_add_node(s_gop_fallback_chain, "hardware_dma",
                           "scanout_token", "scanout_complete",
                           gop_dma_resolve);
            /* Dummy non-NULL state so pixel_source/scanout report
             * "ready". Use a static sentinel. */
            static scanout_t s_gop_sentinel = { .valid = 1, .enabled = 1 };
            s_gop_sentinel.width = fb_width();
            s_gop_sentinel.height = fb_height();
            s_gop_sentinel.chain_id = s_gop_fallback_chain;
            s_gop_sentinel.min_flush_interval_us = 16667;
            s_gop_sentinel.last_flush_tsc = 0;
            chain_t *c = chain_get(s_gop_fallback_chain);
            if (c) {
                for (int k = 0; k < c->node_count; k++) {
                    if (k == 0 || k == 2 || k == 3)
                        c->nodes[k].state = &s_gop_sentinel;
                }
                c->vault_version++;
                /* GOP "flush" is a no-op but we still rate-limit the
                 * resolve so vault_version doesn't tick at full speed. */
                c->resolve_interval_ticks = 16;
            }
            kputs("[gpu_virtio] no virtio-gpu found -- GOP fallback chain registered\n");
        }
    } else {
        kputs("[gpu_virtio] devices=");
        kput_dec((uint64_t)s_dev_count);
        kputs(" displays=");
        kput_dec((uint64_t)s_total_scanouts);
        kputc('\n');
    }
    return s_dev_count;
}

int gpu_virtio_device_count(void) { return s_dev_count; }

const gpu_virtio_device_t *gpu_virtio_device(int idx)
{
    static gpu_virtio_device_t view;
    if (idx < 0 || idx >= s_dev_count) return 0;
    gpu_dev_t *gd = &s_devs[idx];
    view.chain_id         = gd->chain_id;
    view.chain_render_id  = gd->chain_render_id;
    view.chain_display_id = gd->chain_display_id;
    view.pci_vendor       = gd->pci ? gd->pci->vendor_id : 0;
    view.pci_device       = gd->pci ? gd->pci->device_id : 0;
    view.pci_bus          = gd->pci ? gd->pci->bus  : 0;
    view.pci_dev          = gd->pci ? gd->pci->dev  : 0;
    view.pci_func         = gd->pci ? gd->pci->func : 0;
    view.num_scanouts     = gd->num_scanouts;
    view.ready            = gd->ready;
    return &view;
}

int gpu_virtio_scanout_count(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= s_dev_count) return 0;
    return s_devs[dev_idx].num_scanouts;
}

int gpu_virtio_display_count(void) { return s_total_scanouts; }

int gpu_virtio_display_info(int global_idx, gpu_virtio_display_t *out)
{
    if (!out) return -1;
    int seen = 0;
    for (int gi = 0; gi < s_dev_count; gi++) {
        gpu_dev_t *gd = &s_devs[gi];
        for (int si = 0; si < GPU_VIRTIO_MAX_SCANOUTS; si++) {
            scanout_t *s = &gd->scanouts[si];
            if (!s->valid || !s->enabled) continue;
            if (seen == global_idx) {
                out->gpu_index     = gi;
                out->scanout_index = si;
                out->chain_id      = s->chain_id;
                out->enabled       = s->enabled;
                out->width         = s->width;
                out->height        = s->height;
                out->refresh_hz    = s->refresh_hz;
                out->edid_valid    = s->edid_valid;
                for (int k = 0; k < 16; k++) out->monitor_name[k] = s->monitor_name[k];
                return 0;
            }
            seen++;
        }
    }
    return -1;
}

int gpu_virtio_gop_fallback(int *out_chain_id)
{
    if (s_gop_fallback_chain < 0) {
        if (out_chain_id) *out_chain_id = -1;
        return 0;
    }
    if (out_chain_id) *out_chain_id = s_gop_fallback_chain;
    return 1;
}

int gpu_virtio_present(void)
{
    if (s_dev_count == 0) return 0;
    gpu_dev_t *gd = &s_devs[0];
    if (!gd->ready) return 0;
    for (int si = 0; si < GPU_VIRTIO_MAX_SCANOUTS; si++) {
        scanout_t *s = &gd->scanouts[si];
        if (!s->valid || !s->enabled) continue;
        (void)gpu_transfer_and_flush(gd, si);
        chain_t *c = chain_get(s->chain_id);
        if (c) c->vault_version++;
    }
    return 0;
}
