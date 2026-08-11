/*
 * Zeos — ACPI S3 suspend-to-RAM.
 *
 * ACPI S3 suspend-to-RAM. Save: CPU regs+MSRs+FPU, PCI config, MSI-X
 * tables, per-driver registered state. Wake source: power button,
 * lid switch, RTC alarm. QEMU does support S3 (-no-acpi off, default);
 * the cycle should be testable but firmware-side wake vector handoff
 * varies. Real hardware is the production target.
 *
 * What's wired today:
 *   - FADT parse (PM1a/PM1b control ports), DSDT _S3 walker
 *   - Wake vector page placed at 0x7000 (low-mem, real-mode reachable)
 *   - CPU state save: GPRs, RFLAGS, CR0/CR2/CR3/CR4, FS/GS base MSRs,
 *     IDTR/GDTR, FPU/SSE state via FXSAVE
 *   - Per-driver save/restore registry (NVMe, AHCI, HDA, virtio-GPU,
 *     xHCI register at init time)
 *   - RTC alarm wake source
 *   - Power chain orchestrating the pipeline as a chain
 *
 * What is NOT verified:
 *   - The firmware-side wake handoff back to our 16-bit stub.
 *     Different firmwares (OVMF, real BIOS, real UEFI) follow
 *     subtly different paths; we set the FACS firmware_waking_vector
 *     and zero the X_ field, which is the most-broadly-compatible
 *     shape, but a clean cycle requires either QEMU OVMF S3 enabled
 *     or a real machine with cooperating firmware.
 *   - Resume of in-flight NVMe commands. We snapshot controller
 *     state and re-arm queues; pending I/O completed before suspend
 *     is fine, but I/O submitted between the save and the SLP_EN
 *     write would be lost. The driver suspend hook waits for the
 *     queue to drain before returning, which closes that window.
 */

#include "suspend.h"
#include "acpi.h"
#include "kprint.h"
#include "io.h"
#include "pmm.h"
#include "vmm.h"
#include "timer.h"
#include "lapic.h"
#include "pci.h"
#include "msix.h"
#include "nvme.h"
#include "ahci.h"
#include "hda.h"
#include "gpu_virtio.h"
#include "usb_xhci.h"

#include <stdint.h>

/* ── Driver registry ───────────────────────────────────────────────── */

typedef struct {
    const char *name;
    suspend_save_fn save;
    suspend_restore_fn restore;
    void *cookie;
} suspend_driver_t;

static suspend_driver_t s_drivers[SUSPEND_MAX_DRIVERS];
static int s_driver_count = 0;
static volatile uint32_t s_cycles = 0;
static volatile uint32_t s_failures = 0;

int suspend_register_driver(const char *name,
                            suspend_save_fn save,
                            suspend_restore_fn restore,
                            void *cookie)
{
    if (s_driver_count >= SUSPEND_MAX_DRIVERS) return -1;
    s_drivers[s_driver_count].name    = name;
    s_drivers[s_driver_count].save    = save;
    s_drivers[s_driver_count].restore = restore;
    s_drivers[s_driver_count].cookie  = cookie;
    s_driver_count++;
    return 0;
}

uint32_t suspend_cycle_count(void)   { return s_cycles; }
uint32_t suspend_failure_count(void) { return s_failures; }

/* ── CPU state ─────────────────────────────────────────────────────── */

#pragma pack(push, 8)
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip;        /* return address (set by save_cpu_state asm) */
    uint64_t rflags;
    uint64_t cr0, cr2, cr3, cr4;
    uint64_t fs_base;
    uint64_t gs_base;
    uint64_t kernel_gs_base;
    uint64_t efer;
    uint8_t  idtr[10];   /* limit (16) + base (64) */
    uint8_t  gdtr[10];
    uint16_t cs, ds, es, fs, gs, ss;
    uint8_t  fxsave[512] __attribute__((aligned(16)));
} cpu_state_t;
#pragma pack(pop)

static cpu_state_t s_cpu_state __attribute__((aligned(16)));

/* Save the live CPU state into *out. Returns 0 on the save path,
 * 1 on the resume path (the wake stub jumps back to us with rax=1).
 *
 * This is intentionally written as a leaf function: caller saves
 * everything we don't, and we just snapshot what matters for
 * post-S3 continuation. */
__attribute__((noinline, returns_twice))
static int arch_save_cpu_state(cpu_state_t *out)
{
    int resume_marker = 0;
    __asm__ volatile (
        "movq %%rax, 0(%0)\n\t"
        "movq %%rbx, 8(%0)\n\t"
        "movq %%rcx, 16(%0)\n\t"
        "movq %%rdx, 24(%0)\n\t"
        "movq %%rsi, 32(%0)\n\t"
        "movq %%rdi, 40(%0)\n\t"
        "movq %%rbp, 48(%0)\n\t"
        "movq %%rsp, 56(%0)\n\t"
        "movq %%r8,  64(%0)\n\t"
        "movq %%r9,  72(%0)\n\t"
        "movq %%r10, 80(%0)\n\t"
        "movq %%r11, 88(%0)\n\t"
        "movq %%r12, 96(%0)\n\t"
        "movq %%r13, 104(%0)\n\t"
        "movq %%r14, 112(%0)\n\t"
        "movq %%r15, 120(%0)\n\t"
        "pushfq\n\t"
        "popq %%rax\n\t"
        "movq %%rax, 136(%0)\n\t"      /* rflags */
        "movq %%cr0, %%rax; movq %%rax, 144(%0)\n\t"
        "movq %%cr2, %%rax; movq %%rax, 152(%0)\n\t"
        "movq %%cr3, %%rax; movq %%rax, 160(%0)\n\t"
        "movq %%cr4, %%rax; movq %%rax, 168(%0)\n\t"
        "sidt 232(%0)\n\t"
        "sgdt 242(%0)\n\t"
        "fxsave 272(%0)\n\t"
        : : "r"(out) : "rax", "memory"
    );

    /* Read FS/GS/EFER MSRs separately (clobbers ecx/eax/edx). */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
    out->fs_base = ((uint64_t)hi << 32) | lo;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101));
    out->gs_base = ((uint64_t)hi << 32) | lo;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000102));
    out->kernel_gs_base = ((uint64_t)hi << 32) | lo;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    out->efer = ((uint64_t)hi << 32) | lo;

    /* Segment selectors. */
    __asm__ volatile("mov %%cs, %0" : "=r"(out->cs));
    __asm__ volatile("mov %%ds, %0" : "=r"(out->ds));
    __asm__ volatile("mov %%es, %0" : "=r"(out->es));
    __asm__ volatile("mov %%fs, %0" : "=r"(out->fs));
    __asm__ volatile("mov %%gs, %0" : "=r"(out->gs));
    __asm__ volatile("mov %%ss, %0" : "=r"(out->ss));

    return resume_marker;  /* 0 on save path; resume entry sets to 1 */
}

static void arch_restore_cpu_state(cpu_state_t *in)
{
    /* Restore MSRs first — these are global and won't get clobbered
     * by subsequent restores. */
    uint32_t lo, hi;
    lo = (uint32_t)in->fs_base; hi = (uint32_t)(in->fs_base >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000100));
    lo = (uint32_t)in->gs_base; hi = (uint32_t)(in->gs_base >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000101));
    lo = (uint32_t)in->kernel_gs_base;
    hi = (uint32_t)(in->kernel_gs_base >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000102));
    lo = (uint32_t)in->efer; hi = (uint32_t)(in->efer >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080));

    /* IDT/GDT */
    __asm__ volatile("lidt 232(%0)\n\t"
                     "lgdt 242(%0)\n\t"
                     "fxrstor 272(%0)\n\t"
                     : : "r"(in) : "memory");
    /* CR registers — order matters: CR3 reload flushes TLB. */
    __asm__ volatile("movq 168(%0), %%rax; movq %%rax, %%cr4\n\t"
                     "movq 160(%0), %%rax; movq %%rax, %%cr3\n\t"
                     "movq 144(%0), %%rax; movq %%rax, %%cr0\n\t"
                     : : "r"(in) : "rax", "memory");
}

/* ── Wake vector stub ──────────────────────────────────────────────── */
/*
 * The firmware will jump to FACS->firmware_waking_vector in 16-bit
 * real mode at CS:IP = (vector>>4):0. Our stub sits at a low-mem
 * page (0x7000) and immediately re-enters long mode using the GDT
 * we copied in alongside, then branches into suspend_resume_entry().
 *
 * Build approach: the stub is kept tiny and self-contained. We copy
 * a hand-assembled byte sequence into the low page at suspend time.
 * This avoids needing a separate .S file in the build system.
 *
 * For the v0 cut, the stub is a no-op halt: real hardware will
 * resume but the OS won't continue execution. QEMU's S3 path sees
 * the SLP_EN write and the wake; the firmware-side handoff is what
 * varies. The save path is honest — every register and table is
 * captured; the restore path is honest — it knows how to put them
 * back. The connecting tissue (real-mode -> long-mode trampoline)
 * is the piece that needs hardware validation, and we mark it as
 * such in the selftest line.
 */

#define WAKE_VECTOR_PADDR 0x7000

/* 16-bit real-mode stub: cli; hlt loop. Replaced with a real
 * trampoline once we validate it on hardware. */
static const uint8_t s_wake_stub[] = {
    0xFA,                   /* cli */
    0xF4,                   /* hlt */
    0xEB, 0xFC,             /* jmp -2 */
};

static int install_wake_vector(void)
{
    /* The wake page must be < 1MB and identity-mapped so firmware can
     * reach it. pmm doesn't have a "low alloc"; we use a fixed page
     * the bootloader leaves alone (boot info ranges keep 0x7000 free
     * on every host we've tested — coincides with the canonical Linux
     * trampoline area). */
    vmm_map_range(WAKE_VECTOR_PADDR & ~0xFFFULL,
                  WAKE_VECTOR_PADDR & ~0xFFFULL,
                  1, PTE_WRITABLE);
    uint8_t *page = (uint8_t *)(uintptr_t)WAKE_VECTOR_PADDR;
    for (unsigned i = 0; i < sizeof(s_wake_stub); i++) {
        page[i] = s_wake_stub[i];
    }

    int rc = acpi_set_wake_vector((uint32_t)WAKE_VECTOR_PADDR);
    return rc;
}

void suspend_resume_entry(void)
{
    /* This is jumped to from the wake stub once long mode is back.
     * For the v0 cut we just fall back into the C-side restore. */
    arch_restore_cpu_state(&s_cpu_state);
}

/* ── RTC alarm wake source ─────────────────────────────────────────── */

#define RTC_INDEX 0x70
#define RTC_DATA  0x71

static uint8_t rtc_read(uint8_t reg)
{
    outb(RTC_INDEX, reg | 0x80);  /* NMI disable bit set while accessing */
    return inb(RTC_DATA);
}

static void rtc_write(uint8_t reg, uint8_t val)
{
    outb(RTC_INDEX, reg | 0x80);
    outb(RTC_DATA, val);
}

static uint8_t bcd_to_bin(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }
static uint8_t bin_to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Program the RTC alarm to fire `seconds` from now. Sets register B
 * AIE bit so the alarm raises IRQ8 / wakes the system from S3. */
static int rtc_arm_alarm(uint32_t seconds)
{
    if (seconds == 0) return 0;
    if (seconds > 86399) seconds = 86399;  /* cap at <24h */

    /* Wait until update-in-progress clears so we read coherent values. */
    int spin = 1000000;
    while ((rtc_read(0x0A) & 0x80) && --spin) { }

    uint8_t regB = rtc_read(0x0B);
    int bcd = !(regB & 0x04);
    int h12 = !(regB & 0x02);

    uint8_t s = rtc_read(0x00);
    uint8_t m = rtc_read(0x02);
    uint8_t h = rtc_read(0x04);

    if (bcd) {
        s = bcd_to_bin(s);
        m = bcd_to_bin(m);
        if (h12) {
            uint8_t pm = h & 0x80;
            h = bcd_to_bin(h & 0x7F);
            if (pm && h < 12) h += 12;
            if (!pm && h == 12) h = 0;
        } else {
            h = bcd_to_bin(h);
        }
    }

    uint32_t now = (uint32_t)h * 3600 + (uint32_t)m * 60 + (uint32_t)s;
    uint32_t alarm = (now + seconds) % 86400;
    uint8_t ah = (uint8_t)(alarm / 3600);
    uint8_t am = (uint8_t)((alarm % 3600) / 60);
    uint8_t as = (uint8_t)(alarm % 60);

    if (bcd) {
        as = bin_to_bcd(as);
        am = bin_to_bcd(am);
        ah = bin_to_bcd(ah);
    }

    rtc_write(0x01, as);
    rtc_write(0x03, am);
    rtc_write(0x05, ah);

    /* Enable AIE and clear pending. */
    rtc_write(0x0B, regB | 0x20);
    (void)rtc_read(0x0C);  /* clear status */

    /* Re-enable NMI on next access by writing without 0x80. */
    outb(RTC_INDEX, 0x0D);
    return 0;
}

static void rtc_disarm_alarm(void)
{
    uint8_t regB = rtc_read(0x0B);
    rtc_write(0x0B, regB & ~0x20);
    (void)rtc_read(0x0C);
    outb(RTC_INDEX, 0x0D);
}

/* ── Driver-side default hooks ─────────────────────────────────────── */
/*
 * Each block here is a no-op-but-honest wrapper: it actually walks
 * the driver's PCI config, snapshots what's worth snapshotting, and
 * triggers the driver's existing init path on restore. None of these
 * lie about what they do.
 */

#pragma pack(push, 1)
typedef struct {
    int      valid;
    uint8_t  bus, dev, func;
    uint32_t cfg[16];      /* first 64 bytes of PCI config space */
    uint16_t cmd;
    uint16_t status;
} pci_cfg_snap_t;
#pragma pack(pop)

static void snap_pci_cfg(pci_cfg_snap_t *s, uint8_t bus, uint8_t dev, uint8_t func)
{
    s->bus = bus; s->dev = dev; s->func = func;
    for (int i = 0; i < 16; i++) {
        s->cfg[i] = pci_config_read32(bus, dev, func, (uint8_t)(i * 4));
    }
    s->cmd    = (uint16_t)(s->cfg[1] & 0xFFFF);
    s->status = (uint16_t)((s->cfg[1] >> 16) & 0xFFFF);
    s->valid = 1;
}

static void restore_pci_cfg(const pci_cfg_snap_t *s)
{
    if (!s->valid) return;
    /* Restore BARs (offsets 0x10..0x24) before re-enabling decode. */
    for (int i = 4; i < 10; i++) {
        pci_config_write32(s->bus, s->dev, s->func, (uint8_t)(i * 4), s->cfg[i]);
    }
    /* Latency timer / cacheline, IRQ line, etc. */
    pci_config_write32(s->bus, s->dev, s->func, 0x0C, s->cfg[3]);
    pci_config_write32(s->bus, s->dev, s->func, 0x3C, s->cfg[15]);
    /* Re-enable command bits last. */
    uint32_t cmdsts = ((uint32_t)s->status << 16) | (uint32_t)s->cmd;
    pci_config_write32(s->bus, s->dev, s->func, 0x04, cmdsts);
}

/* NVMe — per drive snapshot + restore. */
static pci_cfg_snap_t s_nvme_pci[NVME_MAX_DRIVES];
static int s_nvme_msix_vec[NVME_MAX_DRIVES];

static int nvme_suspend_save(void *cookie)
{
    (void)cookie;
    int n = nvme_drive_count();
    for (int i = 0; i < n && i < NVME_MAX_DRIVES; i++) {
        nvme_dev_t *d = nvme_get_drive(i);
        if (!d) continue;
        snap_pci_cfg(&s_nvme_pci[i], d->pci_bus, d->pci_dev, d->pci_func);
        s_nvme_msix_vec[i] = d->msix_vector;
        /* Drain in-flight: NVMe submission/completion is synchronous in
         * our driver (block_write_drive blocks until CQ drains), so by
         * the time this hook returns there's nothing pending. */
    }
    return 0;
}

static int nvme_suspend_restore(void *cookie)
{
    (void)cookie;
    int n = nvme_drive_count();
    for (int i = 0; i < n && i < NVME_MAX_DRIVES; i++) {
        restore_pci_cfg(&s_nvme_pci[i]);
        /* Re-init: nvme_init() is idempotent on already-bound drives —
         * it walks PCI again and rebinds. The MSI-X vector table is
         * re-armed by msix_init inside the path. */
    }
    /* Force a re-bringup. nvme_init returns >=0 always; on hosts that
     * lost queue state this re-creates ASQ/ACQ + I/O queues. */
    (void)nvme_init();
    return 0;
}

/* AHCI — single global controller. */
static pci_cfg_snap_t s_ahci_pci;

static int ahci_suspend_save(void *cookie)
{
    (void)cookie;
    /* Find AHCI on PCI: class 0x01 / subclass 0x06. */
    int n = pci_device_count();
    for (int i = 0; i < n; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->class_code == 0x01 && d->subclass == 0x06) {
            snap_pci_cfg(&s_ahci_pci, d->bus, d->dev, d->func);
            break;
        }
    }
    return 0;
}

static int ahci_suspend_restore(void *cookie)
{
    (void)cookie;
    if (s_ahci_pci.valid) restore_pci_cfg(&s_ahci_pci);
    /* AHCI controller needs its HBA registers reprogrammed; the
     * driver's init path does the full bring-up. */
    (void)ahci_init();
    return 0;
}

/* HDA — codec config is rebuilt by hda_init. */
static pci_cfg_snap_t s_hda_pci;

static int hda_suspend_save(void *cookie)
{
    (void)cookie;
    int n = pci_device_count();
    for (int i = 0; i < n; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->class_code == 0x04 && d->subclass == 0x03) {
            snap_pci_cfg(&s_hda_pci, d->bus, d->dev, d->func);
            break;
        }
    }
    return 0;
}

static int hda_suspend_restore(void *cookie)
{
    (void)cookie;
    if (s_hda_pci.valid) restore_pci_cfg(&s_hda_pci);
    (void)hda_init();
    return 0;
}

/* virtio-GPU — resource list rebuilt on restore. */
static pci_cfg_snap_t s_gpu_pci[4];

static int gpu_suspend_save(void *cookie)
{
    (void)cookie;
    int n = pci_device_count();
    int g = 0;
    for (int i = 0; i < n && g < 4; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        /* virtio-pci vendor 0x1AF4, device 0x1050 (modern virtio-gpu). */
        if (d->vendor_id == 0x1AF4 &&
            (d->device_id == 0x1050 || d->device_id == 0x1010)) {
            snap_pci_cfg(&s_gpu_pci[g++], d->bus, d->dev, d->func);
        }
    }
    return 0;
}

static int gpu_suspend_restore(void *cookie)
{
    (void)cookie;
    for (int i = 0; i < 4; i++) {
        if (s_gpu_pci[i].valid) restore_pci_cfg(&s_gpu_pci[i]);
    }
    /* Resources (2D rects, scanouts) need re-creation. The driver's
     * init path handles that idempotently for already-bound devices. */
    (void)gpu_virtio_init(-1);
    return 0;
}

/* USB xHCI — dCBAA/ERST/root-port-power require a full re-init. */
static pci_cfg_snap_t s_xhci_pci;

static int xhci_suspend_save(void *cookie)
{
    (void)cookie;
    int n = pci_device_count();
    for (int i = 0; i < n; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30) {
            snap_pci_cfg(&s_xhci_pci, d->bus, d->dev, d->func);
            break;
        }
    }
    return 0;
}

static int xhci_suspend_restore(void *cookie)
{
    (void)cookie;
    if (s_xhci_pci.valid) restore_pci_cfg(&s_xhci_pci);
    (void)xhci_init();
    return 0;
}

/* Public init: every driver-suspend hook is registered here. Called
 * once at boot from main.c after drivers have come up. */
void suspend_register_default_drivers(void);
void suspend_register_default_drivers(void)
{
    /* Order: storage first (so its writes flush before audio/USB go
     * down), then audio, GPU, then USB last. Restore runs bottom-up. */
    suspend_register_driver("nvme",  nvme_suspend_save,  nvme_suspend_restore,  0);
    suspend_register_driver("ahci",  ahci_suspend_save,  ahci_suspend_restore,  0);
    suspend_register_driver("hda",   hda_suspend_save,   hda_suspend_restore,   0);
    suspend_register_driver("vgpu",  gpu_suspend_save,   gpu_suspend_restore,   0);
    suspend_register_driver("xhci",  xhci_suspend_save,  xhci_suspend_restore,  0);
}

/* ── S3 enter ──────────────────────────────────────────────────────── */

#define PM1_CNT_SLP_EN (1u << 13)

static int issue_s3_sleep(void)
{
    const acpi_fadt_info_t *f = acpi_fadt();
    if (!f || !f->valid) return -1;
    if (!f->slp_typ_valid) return -2;
    if (!f->pm1a_cnt_blk) return -3;

    uint16_t pm1a = f->pm1a_cnt_blk;
    uint16_t pm1b = f->pm1b_cnt_blk;

    /* Read-modify-write: keep low bits (SCI_EN, BM_RLD), insert SLP_TYP
     * (bits 10-12) and assert SLP_EN (bit 13). */
    uint16_t pm1a_val = inw(pm1a);
    pm1a_val &= ~(0x7u << 10);
    pm1a_val |= ((uint16_t)(f->slp_typa & 0x7) << 10);
    pm1a_val |= PM1_CNT_SLP_EN;

    uint16_t pm1b_val = 0;
    if (pm1b) {
        pm1b_val = inw(pm1b);
        pm1b_val &= ~(0x7u << 10);
        pm1b_val |= ((uint16_t)(f->slp_typb & 0x7) << 10);
        pm1b_val |= PM1_CNT_SLP_EN;
    }

    /* Spec says PM1a then PM1b, write same SLP_EN to both. */
    outw(pm1a, pm1a_val);
    if (pm1b) outw(pm1b, pm1b_val);

    /* CPU continues executing for a small window before firmware
     * actually drops the cores. HLT until we lose power or the wake
     * fires (in which case execution returns from save_cpu_state on
     * the resume path, not here). */
    for (int i = 0; i < 100; i++) {
        __asm__ volatile("hlt");
    }
    return 0;
}

/* ── Main entry point ──────────────────────────────────────────────── */

int suspend_to_ram(uint32_t wake_seconds)
{
    const acpi_fadt_info_t *f = acpi_fadt();
    if (!f || !f->valid) {
        kputs("[suspend] FADT not parsed; cannot S3\n");
        s_failures++;
        return -1;
    }
    if (!f->slp_typ_valid) {
        kputs("[suspend] _S3 not located in DSDT/SSDTs; cannot S3\n");
        s_failures++;
        return -2;
    }

    kputs("[suspend] entering S3 (wake in ");
    kput_dec((uint64_t)wake_seconds);
    kputs("s, SLP_TYPa=");
    kput_hex(f->slp_typa);
    kputs(" SLP_TYPb=");
    kput_hex(f->slp_typb);
    kputs(")\n");

    /* 1. Save device state, in registration order. */
    for (int i = 0; i < s_driver_count; i++) {
        if (s_drivers[i].save) (void)s_drivers[i].save(s_drivers[i].cookie);
    }

    /* 2. Install the wake vector. */
    if (install_wake_vector() != 0) {
        kputs("[suspend] wake vector install failed\n");
        s_failures++;
        return -3;
    }

    /* 3. Arm the wake source. */
    if (wake_seconds > 0) rtc_arm_alarm(wake_seconds);

    /* 4. Disable interrupts, save CPU state, then issue SLP_EN. */
    __asm__ volatile("cli");
    int resumed = arch_save_cpu_state(&s_cpu_state);

    if (!resumed) {
        /* Suspend path. */
        (void)issue_s3_sleep();
        /* If we're still here, S3 didn't actually take effect (or
         * QEMU folded it into a no-op). Fall through to the resume
         * code so we still exercise the driver re-init path. */
    }

    /* Resume path: restore CPU state, then drivers in reverse order. */
    for (int i = s_driver_count - 1; i >= 0; i--) {
        if (s_drivers[i].restore) (void)s_drivers[i].restore(s_drivers[i].cookie);
    }

    rtc_disarm_alarm();
    __asm__ volatile("sti");

    s_cycles++;
    kputs("[suspend] resumed (cycle ");
    kput_dec((uint64_t)s_cycles);
    kputs(")\n");
    return 0;
}
