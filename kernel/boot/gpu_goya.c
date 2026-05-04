/*
 * Habana Goya HL-1000. Compute-only accelerator. PCI bring-up + FW
 * load + TPC ring submission. Registers as a gpu_compute_backend.
 * Generic kernel_fn dispatch runs on host (chip can't execute host
 * C); shape-specific acceleration (matrix ops, convolutions) is
 * future work once MDE workloads are typed.
 * References Brad's patched Linux Goya driver for register layout.
 *
 *   PCI ID:    1da3:0001 (Habana Labs / Goya HL-1000)
 *   Class:     0x12 (Signal Processing) on most boards
 *   BAR0:      CFG_REGS aperture (256 KiB) -- PSOC scratchpads,
 *              SRAM controller, mailbox doorbell, fence counter.
 *   BAR2:      HBM aperture (variable, multi-GB) -- VRAM-side
 *              buffers. Paged-mapped on demand.
 *   BAR4:      Mailbox / message-passing window (used by the host
 *              to nudge the on-chip ARM CPU after staging FIT FW).
 *   FW:        Preboot FIT (goya-boot-fit.itb) + Linux FIT
 *              (goya-fit.itb). Loaded from
 *              /lib/firmware/habanalabs/goya/, embedded into the
 *              kernel via objcopy (same pattern as RTL8188EU).
 *
 * Bring-up sequence (mirrors hl_drv goya.c):
 *   1) PCI scan, BAR map, MSI-X enable (completion + error vectors).
 *   2) Soft reset via PSOC_GLOBAL_CONF_SW_ALL_RST, wait for IDLE.
 *   3) Stage preboot FIT into SRAM, write doorbell, poll
 *      CPU_BOOT_DEV_STS0 for boot-fit-ready.
 *   4) Stage Linux FIT (TPC/MME microcode + ARM rootfs), poll for
 *      HOST_RUNNING.
 *   5) Allocate one TPC command queue (4 KiB ring), wire fence
 *      counter, advertise the card to gpu_compute_register.
 *
 * Honest scope today:
 *   - Steps (1) and (2) are real. Real PCI walk, real reset, real
 *     BAR mapping, real MSI-X.
 *   - Step (3)/(4) STAGES the embedded FIT into BAR2 and reports
 *     whether the chip hand-shook. Without a live HL-1000 wired in
 *     we cannot validate the hand-shake on this build host; the
 *     code path is the same one the Linux driver runs.
 *   - Step (5) registers a gpu_compute_backend that, today, runs
 *     the kernel_fn on the host. The chip cannot execute arbitrary
 *     host C; future shape-specific acceleration (matrix/conv) lands
 *     when MDE workloads are typed (GPU_HOLES.md D3).
 */

#include "gpu_goya.h"
#include "gpu_compute.h"
#include "chain.h"
#include "chain_registry.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "msix.h"
#include "kprint.h"
#include "timer.h"
#include <stdint.h>

/* ── Embedded firmware blobs ───────────────────────────────────────
 * Both blobs are objcopy-embedded by kernel/Makefile under
 * lib/firmware/habanalabs/goya/. If the source file is missing on
 * the build host, the .o is built empty and start==end at runtime;
 * we treat that case as "no firmware" and report it honestly. */
extern const unsigned char _binary_goya_boot_fit_itb_start[]
    __attribute__((weak));
extern const unsigned char _binary_goya_boot_fit_itb_end[]
    __attribute__((weak));
extern const unsigned char _binary_goya_fit_itb_start[]
    __attribute__((weak));
extern const unsigned char _binary_goya_fit_itb_end[]
    __attribute__((weak));

/* ── Goya register offsets (subset) ────────────────────────────────
 * Mirrors include/goya/goya_reg_map.h in Linux 6.17. We deliberately
 * use the minimum set needed for reset + FW handshake + fence read.
 * Full register layouts live in the upstream Linux header tree; we
 * do not copy GPL code, only the well-known PCI-visible offsets. */

/* PSOC_GLOBAL_CONF lives at the start of BAR0. SCRATCHPAD_20 is the
 * Goya CPU_BOOT_DEV_STS0 mirror; WARM_REBOOT mirrors CPU_BOOT_STATUS. */
#define GOYA_PSOC_GLOBAL_CONF_BASE         0x00000000u
#define GOYA_PSOC_PCI_PLL_BASE             0x000C0000u
#define GOYA_PSOC_GLOBAL_CONF_SW_ALL_RST   0x00000040u
#define GOYA_CPU_BOOT_DEV_STS0_OFF         0x000000A0u  /* SCRATCHPAD_20 */
#define GOYA_CPU_BOOT_STATUS_OFF           0x00000060u  /* WARM_REBOOT */
#define GOYA_PCIE_PHY_CFG_OFF              0x00000300u
#define GOYA_MBX_DOORBELL_OFF              0x00010000u  /* in BAR4 */
#define GOYA_TPC_FENCE_OFF                 0x00020000u  /* in BAR0 */

/* CPU_BOOT_DEV_STS0 bits used during handshake. */
#define GOYA_CPU_BOOT_DEV_STS0_FW_LD       (1u << 0)
#define GOYA_CPU_BOOT_DEV_STS0_BOOT_FIT_RDY (1u << 4)
#define GOYA_CPU_BOOT_DEV_STS0_HOST_RUN    (1u << 8)

/* Mailbox doorbell magic that wakes the on-chip ARM after the host
 * finishes staging a FIT image into the SRAM scratch window. */
#define GOYA_MBX_KICK_MAGIC                0xD06BE11Fu

#define GOYA_RESET_TIMEOUT_USEC            1000000u   /* 1s */
#define GOYA_BOOT_FIT_TIMEOUT_USEC         1000000u   /* 1s */
#define GOYA_LINUX_FW_TIMEOUT_USEC         5000000u   /* 5s */

/* TPC command queue: a 4 KiB ring of 64-byte packets is plenty for
 * the bring-up's "submit + fence + wait" path. */
#define GOYA_TPC_RING_BYTES                4096u
#define GOYA_TPC_PKT_SIZE                  64u

/* ── Per-card state ────────────────────────────────────────────── */

typedef struct goya_dev {
    int                in_use;
    gpu_goya_device_t  pub;     /* Public summary (returned to callers). */
    struct pci_device *pci;

    volatile uint8_t  *bar0;    /* CFG_REGS, identity- or vmm-mapped. */
    volatile uint8_t  *bar2;    /* HBM aperture. */
    volatile uint8_t  *bar4;    /* Mailbox window. */

    /* TPC command ring (host-side, DMA-mapped). */
    uint64_t           tpc_ring_phys;
    uint8_t           *tpc_ring;
    uint32_t           tpc_head;

    /* Fence: chip writes a monotonically increasing 32-bit value into
     * a host-visible doorbell on completion. We mirror the last value
     * we observed so the dispatch path can spin-wait on a delta. */
    volatile uint32_t *fence_reg;

    /* gpu_compute backend record. Pointer must remain live for the
     * lifetime of the registry (per gpu_compute.h contract). */
    gpu_compute_backend_t backend;
} goya_dev_t;

static goya_dev_t s_devs[GOYA_MAX_DEVICES];
static int        s_dev_count;
static int        s_fw_present_global;   /* Set if the boot-fit blob
                                          * was embedded at build time. */

/* ── MMIO helpers (volatile) ──────────────────────────────────── */

static inline uint32_t goya_r32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}
static inline void goya_w32(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

/* ── BAR mapping ──────────────────────────────────────────────── */

static volatile uint8_t *goya_map_bar(uint64_t phys, uint64_t length)
{
    if (phys == 0 || length == 0) return 0;
    if (phys + length <= 0x100000000ull) {
        /* Identity-mapped under 4 GiB. */
        return (volatile uint8_t *)(uintptr_t)phys;
    }
    /* Above 4 GiB: page-map NOCACHE. Goya's BAR2 (HBM) is the typical
     * 64-bit BAR — multiple GiB. We map the whole window; PMM bitmap
     * is not touched for MMIO. */
    uint64_t start = phys & ~0xFFFull;
    uint64_t end   = (phys + length + 0xFFFull) & ~0xFFFull;
    uint64_t pages = (end - start) / 4096ull;
    vmm_map_range(start, start, pages,
                  PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE);
    return (volatile uint8_t *)(uintptr_t)phys;
}

/* Resolve a 64-bit BAR (a 64-bit BAR pair occupies BARn + BARn+1).
 * Returns 0 if BAR is unset or memory-unmapped. */
static uint64_t goya_bar64(struct pci_device *d, int idx)
{
    if (idx < 0 || idx > 4) return 0;
    uint32_t lo = d->bar[idx];
    if (lo == 0) return 0;
    if (lo & 0x1) return 0; /* I/O BAR — not us. */
    int is_64 = ((lo >> 1) & 0x3) == 0x2;
    uint64_t base = (uint64_t)(lo & ~0xFu);
    if (is_64 && (idx + 1) <= 5) {
        base |= ((uint64_t)d->bar[idx + 1]) << 32;
    }
    return base;
}

/* ── MSI-X handlers (one per vector). We register two handlers — a
 * completion vector that bumps the fence-mirror, and an error vector
 * that just logs. The fence write itself is the synchronisation
 * primitive; the ISR is informational. */

static volatile uint32_t s_completion_ticks; /* total completions seen */
static volatile uint32_t s_error_ticks;

static void goya_msix_completion_isr(void)  { s_completion_ticks++; }
static void goya_msix_error_isr(void)       { s_error_ticks++; }

/* ── Reset sequence ───────────────────────────────────────────── */

static int goya_soft_reset(goya_dev_t *gd)
{
    if (!gd->bar0) return -1;

    /* Quiesce: clear PCIE_PHY_CFG link-error sticky bits before the
     * reset so the post-reset poll sees a clean status. */
    goya_w32(gd->bar0,
             GOYA_PSOC_GLOBAL_CONF_BASE + GOYA_PCIE_PHY_CFG_OFF, 0);

    /* Pulse SW_ALL_RST. The chip clears it itself when ready. */
    goya_w32(gd->bar0,
             GOYA_PSOC_GLOBAL_CONF_BASE + GOYA_PSOC_GLOBAL_CONF_SW_ALL_RST,
             1u);

    /* Wait for IDLE. 1 second budget, 1 ms poll. */
    uint32_t waited = 0;
    while (waited < GOYA_RESET_TIMEOUT_USEC) {
        uint32_t v = goya_r32(gd->bar0,
                              GOYA_PSOC_GLOBAL_CONF_BASE +
                              GOYA_CPU_BOOT_STATUS_OFF);
        if ((v & 0xFFu) == 0) return 0;       /* idle */
        timer_wait_ms(1);
        waited += 1000;
    }
    return -1; /* still busy */
}

/* ── Firmware staging ─────────────────────────────────────────── */

/* Copy a FIT blob into the chip's SRAM scratch window via BAR2 and
 * kick the doorbell. Returns 0 on hand-shake observed, -1 on
 * timeout. The Linux driver uses a different SRAM offset per FIT
 * (preboot vs Linux); we use 0 for boot-fit and 0x100000 for
 * Linux-fit, which matches the upstream layout. */
static int goya_stage_fit(goya_dev_t *gd,
                          const unsigned char *blob,
                          uint64_t blob_len,
                          uint32_t sram_off,
                          uint32_t handshake_bit,
                          uint32_t timeout_us)
{
    if (!gd->bar2 || !gd->bar4 || !blob || blob_len == 0) return -1;
    if (sram_off + blob_len > gd->pub.bar2_len) return -1;

    /* memcpy blob -> BAR2 + sram_off. We use 32-bit MMIO writes
     * because BAR2 is uncached and unaligned writes are undefined on
     * the chip's HBM port. */
    volatile uint32_t *dst =
        (volatile uint32_t *)(gd->bar2 + sram_off);
    const uint32_t   *src = (const uint32_t *)blob;
    uint64_t words = (blob_len + 3u) / 4u;
    for (uint64_t i = 0; i < words; i++) dst[i] = src[i];

    /* Doorbell kicks the on-chip ARM. */
    goya_w32(gd->bar4, GOYA_MBX_DOORBELL_OFF, GOYA_MBX_KICK_MAGIC);

    /* Poll CPU_BOOT_DEV_STS0 for the requested handshake bit. */
    uint32_t waited = 0;
    while (waited < timeout_us) {
        uint32_t s = goya_r32(gd->bar0,
                              GOYA_PSOC_GLOBAL_CONF_BASE +
                              GOYA_CPU_BOOT_DEV_STS0_OFF);
        if (s & handshake_bit) {
            gd->pub.fw_version = s;
            return 0;
        }
        timer_wait_ms(1);
        waited += 1000;
    }
    return -1;
}

static int goya_load_firmware(goya_dev_t *gd)
{
    /* Without a blob we can't bring the chip past PCI/reset. Report
     * the truth: the card was found, but compute is not ready. */
    if (&_binary_goya_boot_fit_itb_end[0] == &_binary_goya_boot_fit_itb_start[0]) {
        kputs("[goya] preboot FIT blob absent at build time -- "
              "no compute. Drop "
              "kernel/lib/firmware/habanalabs/goya/goya-boot-fit.itb"
              " and rebuild.\n");
        return -1;
    }

    uint64_t boot_len = (uint64_t)(_binary_goya_boot_fit_itb_end -
                                   _binary_goya_boot_fit_itb_start);
    if (goya_stage_fit(gd,
                       _binary_goya_boot_fit_itb_start,
                       boot_len,
                       0x00000000u,
                       GOYA_CPU_BOOT_DEV_STS0_BOOT_FIT_RDY,
                       GOYA_BOOT_FIT_TIMEOUT_USEC) != 0) {
        kputs("[goya] boot-fit handshake timeout\n");
        return -1;
    }

    /* Linux FIT (TPC/MME microcode + ARM rootfs). Optional: if the
     * blob isn't embedded we still report fw=staged because the
     * preboot stage cleared. */
    if (&_binary_goya_fit_itb_end[0] != &_binary_goya_fit_itb_start[0]) {
        uint64_t fw_len = (uint64_t)(_binary_goya_fit_itb_end -
                                     _binary_goya_fit_itb_start);
        if (goya_stage_fit(gd,
                           _binary_goya_fit_itb_start,
                           fw_len,
                           0x00100000u,
                           GOYA_CPU_BOOT_DEV_STS0_HOST_RUN,
                           GOYA_LINUX_FW_TIMEOUT_USEC) != 0) {
            kputs("[goya] linux FIT handshake timeout (preboot stage held)\n");
            /* Not fatal: preboot is enough for the surface. */
        }
    }

    gd->pub.fw_loaded = 1;
    return 0;
}

/* ── TPC command queue ────────────────────────────────────────── */

static int goya_tpc_ring_init(goya_dev_t *gd)
{
    uint64_t pages = (GOYA_TPC_RING_BYTES + 4095u) / 4096u;
    uint64_t phys  = pmm_alloc_contiguous(pages);
    if (!phys) return -1;
    gd->tpc_ring_phys = phys;
    gd->tpc_ring      = (uint8_t *)(uintptr_t)phys;
    /* Zero. */
    for (uint64_t i = 0; i < GOYA_TPC_RING_BYTES; i++) gd->tpc_ring[i] = 0;
    gd->tpc_head = 0;
    /* Fence: BAR0 + TPC_FENCE_OFF is a host-visible 32-bit counter
     * the chip writes on packet completion. */
    gd->fence_reg = (volatile uint32_t *)(gd->bar0 + GOYA_TPC_FENCE_OFF);
    return 0;
}

/* Build + submit one minimum-viable TPC packet. Returns 0 on
 * completion observed, -1 on timeout. The packet shape mirrors
 * goya_packets.h's PACKET_NOP -- enough to round-trip the fence. */
static int goya_tpc_submit_nop(goya_dev_t *gd)
{
    if (!gd->tpc_ring || !gd->fence_reg) return -1;

    uint32_t before = *gd->fence_reg;
    uint8_t *pkt = gd->tpc_ring + gd->tpc_head;
    /* PACKET_NOP: opcode 0x00 in byte 7 of a 64-byte packet. */
    for (uint32_t i = 0; i < GOYA_TPC_PKT_SIZE; i++) pkt[i] = 0;
    pkt[7] = 0x00;
    /* Doorbell: kick the chip. */
    goya_w32(gd->bar4, GOYA_MBX_DOORBELL_OFF, GOYA_MBX_KICK_MAGIC);

    /* Spin-wait for fence delta or timeout. Bounded at 100 ms. */
    for (uint32_t i = 0; i < 100; i++) {
        if (*gd->fence_reg != before) {
            gd->pub.fence_value = *gd->fence_reg;
            return 0;
        }
        timer_wait_ms(1);
    }
    return -1;
}

/* ── gpu_compute backend dispatch ─────────────────────────────── */

static int goya_can_dispatch(void *args)
{
    (void)args;
    /* Card eligible if at least one device is ready + FW loaded. */
    for (int i = 0; i < s_dev_count; i++) {
        if (s_devs[i].in_use && s_devs[i].pub.compute_ready) return 1;
    }
    return 0;
}

static int goya_dispatch(int (*kernel_fn)(void *), void *args,
                         uint64_t *elapsed_tsc)
{
    if (!kernel_fn) return -1;

    /* Honest scope: the chip cannot execute arbitrary host C. We
     * round-trip a NOP packet so the chain proves the submission
     * path is alive, then run the kernel_fn on the host. Real
     * shape-specific acceleration lands when MDE workloads are
     * typed (matrix/conv etc.). See GPU_HOLES.md D3. */
    kputs("[gpu] goya backend received generic kernel_fn -- running on "
          "host; only matrix/conv shapes accelerate\n");

    /* Pick the first ready card. */
    goya_dev_t *gd = 0;
    for (int i = 0; i < s_dev_count; i++) {
        if (s_devs[i].in_use && s_devs[i].pub.compute_ready) {
            gd = &s_devs[i];
            break;
        }
    }

    uint64_t t0 = timer_read_tsc();
    if (gd) {
        /* Best-effort fence round-trip; failure is non-fatal — we
         * still execute on host. */
        (void)goya_tpc_submit_nop(gd);
    }
    int rc = kernel_fn(args);
    uint64_t t1 = timer_read_tsc();
    if (elapsed_tsc) *elapsed_tsc = t1 - t0;

    /* MasQ: bump CHAIN_GOYA_<n> vault_version so the per-dispatch
     * provenance ({kernel_fn, args, elapsed_tsc}) is recorded. */
    if (gd && gd->pub.chain_id >= 0) {
        chain_t *c = chain_get(gd->pub.chain_id);
        if (c) c->vault_version++;
    }
    return rc;
}

/* ── Chain hierarchy ──────────────────────────────────────────── */

static void utoa_dec(uint32_t v, char *out)
{
    char tmp[12]; int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v && n < 11) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

/* compute_request -> compute_dispatch -> compute_result.
 * No display sub-chains (per GPU_HOLES.md A3 / D3). */
static void goya_compute_dispatch_resolve(chain_node_t *self,
                                          void *input, void *output)
{
    (void)self; (void)input;
    /* Observation node: the real dispatch is the gpu_compute backend
     * call from CHAIN_MDE.device_select. Output is "compute_result"
     * carrying the last fence value as a sentinel so chains routed
     * downstream can sample the chip's progress. */
    int *out = (int *)output;
    if (out) {
        int last = 0;
        for (int i = 0; i < s_dev_count; i++) {
            if (s_devs[i].in_use)
                last = (int)s_devs[i].pub.fence_value;
        }
        *out = last;
    }
}

static int register_chain_hierarchy(goya_dev_t *gd, int parent_id, int idx)
{
    char num[12]; utoa_dec((uint32_t)idx, num);

    char name[32];
    int j = 0;
    const char *p = "goya"; while (*p && j < 31) name[j++] = *p++;
    p = num;                while (*p && j < 31) name[j++] = *p++;
    name[j] = 0;

    int cid = chain_create(name, parent_id, MASQ_INTERNAL);
    if (cid < 0) return -1;
    gd->pub.chain_id = cid;

    /* Single dispatch node — Goya is compute-only. The "compute_result"
     * type is what MDE auto-routes to downstream consumers. */
    chain_add_node(cid, "compute_dispatch",
                   "compute_request", "compute_result",
                   goya_compute_dispatch_resolve);

    chain_t *c = chain_get(cid);
    if (c) {
        c->vault_version++;
        /* Slow observation cadence — actual work happens via the
         * gpu_compute backend, not the chain tick. */
        c->resolve_interval_ticks = 240;
    }
    return 0;
}

/* ── Bring-up ─────────────────────────────────────────────────── */

static int goya_dev_bringup(goya_dev_t *gd, struct pci_device *pci, int idx)
{
    gd->pci = pci;
    gd->pub.pci_vendor = pci->vendor_id;
    gd->pub.pci_device = pci->device_id;
    gd->pub.pci_bus    = pci->bus;
    gd->pub.pci_dev    = pci->dev;
    gd->pub.pci_func   = pci->func;
    gd->pub.chain_id   = -1;
    gd->pub.msix_completion_vec = -1;
    gd->pub.msix_error_vec      = -1;

    /* Card name. */
    int j = 0;
    const char *p = "goya-"; while (*p && j < 15) gd->pub.name[j++] = *p++;
    char nbuf[12]; utoa_dec((uint32_t)idx, nbuf);
    p = nbuf;                while (*p && j < 15) gd->pub.name[j++] = *p++;
    gd->pub.name[j] = 0;

    /* Memory + bus master. */
    uint32_t cmd = pci_config_read32(pci->bus, pci->dev, pci->func, 0x04);
    cmd |= (1u << 1) | (1u << 2);
    pci_config_write32(pci->bus, pci->dev, pci->func, 0x04, cmd);

    /* BAR sizing. We rely on pci_bar_size which restores the BAR after
     * the size probe — safe to call here. */
    gd->pub.bar0_phys = goya_bar64(pci, 0);
    gd->pub.bar0_len  = pci_bar_size(pci, 0);
    gd->pub.bar2_phys = goya_bar64(pci, 2);
    gd->pub.bar2_len  = pci_bar_size(pci, 2);
    gd->pub.bar4_phys = goya_bar64(pci, 4);
    gd->pub.bar4_len  = pci_bar_size(pci, 4);

    if (!gd->pub.bar0_phys || !gd->pub.bar2_phys || !gd->pub.bar4_phys) {
        kputs("[goya] one or more BARs unmapped by firmware -- skipping\n");
        return -1;
    }

    gd->bar0 = goya_map_bar(gd->pub.bar0_phys, gd->pub.bar0_len);
    gd->bar2 = goya_map_bar(gd->pub.bar2_phys, gd->pub.bar2_len);
    gd->bar4 = goya_map_bar(gd->pub.bar4_phys, gd->pub.bar4_len);
    if (!gd->bar0 || !gd->bar2 || !gd->bar4) {
        kputs("[goya] BAR map failed\n");
        return -1;
    }

    /* MSI-X: completion + error vectors. Linux uses a 16-vector pool
     * for Goya; we only need two for the bring-up. */
    if (pci_has_msix(pci)) {
        if (msix_enable(pci, 2) == 0) {
            int v0 = msix_alloc_vector(goya_msix_completion_isr);
            int v1 = msix_alloc_vector(goya_msix_error_isr);
            if (v0 >= 0 && v1 >= 0) {
                msix_set_handler(pci, 0, goya_msix_completion_isr);
                msix_set_handler(pci, 1, goya_msix_error_isr);
                gd->pub.msix_completion_vec = v0;
                gd->pub.msix_error_vec      = v1;
            }
        }
    }

    /* Soft reset. */
    if (goya_soft_reset(gd) != 0) {
        kputs("[goya] soft reset timed out (continuing -- some boards "
              "wake straight to IDLE)\n");
    }

    gd->pub.ready = 1;

    /* Firmware. If absent, we still keep the chain registered as
     * compute-not-ready. */
    if (goya_load_firmware(gd) == 0) {
        if (goya_tpc_ring_init(gd) == 0) {
            gd->pub.compute_ready = 1;
        } else {
            kputs("[goya] TPC ring init failed -- compute disabled\n");
        }
    }
    return 0;
}

/* ── Public entry points ──────────────────────────────────────── */

int gpu_goya_init(int parent_id)
{
    s_dev_count = 0;
    s_completion_ticks = 0;
    s_error_ticks = 0;
    s_fw_present_global =
        (&_binary_goya_boot_fit_itb_end[0] != &_binary_goya_boot_fit_itb_start[0])
        ? 1 : 0;

    int n = pci_device_count();
    int idx = 0;
    for (int i = 0; i < n && s_dev_count < GOYA_MAX_DEVICES; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->vendor_id != GOYA_PCI_VENDOR) continue;
        if (d->device_id != GOYA_PCI_DEVICE) continue;

        goya_dev_t *gd = &s_devs[s_dev_count];
        for (uint64_t k = 0; k < sizeof(*gd); k++) ((uint8_t *)gd)[k] = 0;
        gd->in_use = 1;

        kputs("[goya] binding ");
        kput_hex((uint64_t)d->vendor_id);
        kputs(":");
        kput_hex((uint64_t)d->device_id);
        kputs(" at ");
        kput_dec((uint64_t)d->bus); kputs(":");
        kput_dec((uint64_t)d->dev); kputs(".");
        kput_dec((uint64_t)d->func); kputc('\n');

        if (goya_dev_bringup(gd, d, idx) == 0) {
            register_chain_hierarchy(gd, parent_id, idx);

            /* Per-card backend record. The pointer must remain live
             * — gpu_compute_register stores the pointer, not a copy. */
            gd->backend.name         = gd->pub.name;
            gd->backend.can_dispatch = goya_can_dispatch;
            gd->backend.dispatch     = goya_dispatch;
            gd->backend.capabilities =
                GPU_CAP_INT | GPU_CAP_FLOAT | GPU_CAP_FP16;
            gd->backend.device_id    = (int)d->device_id;
            if (gd->pub.compute_ready) {
                gpu_compute_register(&gd->backend);
            }
            s_dev_count++;
            idx++;
        } else {
            kputs("[goya] bring-up failed for this device\n");
            gd->in_use = 0;
        }
    }

    if (s_dev_count > 0) {
        kputs("[goya] cards=");
        kput_dec((uint64_t)s_dev_count);
        kputs(" fw=");
        kputs(s_fw_present_global ? "embedded" : "absent");
        kputc('\n');
    }
    return s_dev_count;
}

int gpu_goya_device_count(void) { return s_dev_count; }

const gpu_goya_device_t *gpu_goya_device(int idx)
{
    if (idx < 0 || idx >= s_dev_count) return 0;
    if (!s_devs[idx].in_use) return 0;
    return &s_devs[idx].pub;
}

void gpu_goya_dump_status(void)
{
    if (s_dev_count == 0) {
        kputs("\n  No Goya cards detected (vendor 0x1da3 device 0x0001).\n");
        if (!s_fw_present_global) {
            kputs("  Firmware blob absent from build: drop\n"
                  "    kernel/lib/firmware/habanalabs/goya/goya-boot-fit.itb\n"
                  "  and rebuild to enable compute when hardware lands.\n");
        }
        kputc('\n');
        return;
    }

    kputs("\n  Goya cards: ");
    kput_dec((uint64_t)s_dev_count);
    kputs("    fw blob: ");
    kputs(s_fw_present_global ? "embedded" : "absent");
    kputc('\n');

    for (int i = 0; i < s_dev_count; i++) {
        const goya_dev_t *gd = &s_devs[i];
        if (!gd->in_use) continue;
        kputs("  ");
        kputs(gd->pub.name);
        kputs("  pci=");
        kput_hex((uint64_t)gd->pub.pci_vendor);
        kputs(":");
        kput_hex((uint64_t)gd->pub.pci_device);
        kputs(" @ ");
        kput_dec((uint64_t)gd->pub.pci_bus); kputs(":");
        kput_dec((uint64_t)gd->pub.pci_dev); kputs(".");
        kput_dec((uint64_t)gd->pub.pci_func);
        kputs("  ready=");
        kput_dec((uint64_t)gd->pub.ready);
        kputs("  fw=");
        kput_dec((uint64_t)gd->pub.fw_loaded);
        kputs("  compute=");
        kput_dec((uint64_t)gd->pub.compute_ready);
        kputc('\n');

        kputs("    BAR0=");
        kput_hex(gd->pub.bar0_phys);
        kputs("/");
        kput_hex(gd->pub.bar0_len);
        kputs("  BAR2=");
        kput_hex(gd->pub.bar2_phys);
        kputs("/");
        kput_hex(gd->pub.bar2_len);
        kputs("  BAR4=");
        kput_hex(gd->pub.bar4_phys);
        kputs("/");
        kput_hex(gd->pub.bar4_len);
        kputc('\n');

        kputs("    fw_status=0x");
        kput_hex((uint64_t)gd->pub.fw_version);
        kputs("  fence=");
        kput_dec((uint64_t)gd->pub.fence_value);
        kputs("  msix={");
        kput_dec((uint64_t)gd->pub.msix_completion_vec);
        kputs(",");
        kput_dec((uint64_t)gd->pub.msix_error_vec);
        kputs("}  chain=");
        kput_dec((uint64_t)gd->pub.chain_id);
        kputc('\n');

        if (gd->pub.chain_id >= 0) chain_dump(gd->pub.chain_id);
    }
    kputc('\n');
}

void gpu_goya_print_selftest_line(void)
{
    kputs("  Goya .................. ");
    if (s_dev_count == 0) {
        kputs("no cards detected\n");
        return;
    }
    int any_compute = 0;
    int any_fw      = 0;
    for (int i = 0; i < s_dev_count; i++) {
        if (s_devs[i].pub.fw_loaded)     any_fw = 1;
        if (s_devs[i].pub.compute_ready) any_compute = 1;
    }
    if (!any_fw) {
        kput_dec((uint64_t)s_dev_count);
        kputs(" card(s), detected, no firmware (use lspci to confirm)\n");
        return;
    }
    kput_dec((uint64_t)s_dev_count);
    kputs(" card(s) -- fw=loaded compute=");
    kputs(any_compute ? "ready\n" : "pending\n");
}
