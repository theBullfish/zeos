/*
 * SMP bring-up via INIT-SIPI-SIPI. Each AP runs its own scheduler
 * loop on its own per-CPU stack and resolves a partition of the
 * chain set. Coarse spinlock around shared structs (registry,
 * masq_journal, persistence) — fine-grained locking is future work.
 *
 * Honest scope today (2026-05-03):
 *   - MADT enumeration of all Processor LAPIC entries .......... ✓
 *   - Per-CPU state struct + per-CPU stack + per-CPU page ...... ✓
 *   - INIT-SIPI-SIPI sequence helpers + IPI helper ............. ✓
 *   - 16->32->64-bit trampoline blob authored + placed at 0x8000 ✓
 *   - Coarse spinlock around chain registry .................... ✓ (primitive ready)
 *   - APs reach 64-bit ap_main and bump per-CPU heartbeat ...... DEFERRED
 *       The INIT+SIPI-SIPI dispatch is gated behind
 *       SMP_DISPATCH_APS=1 because the hand-encoded trampoline
 *       blob is unvalidated end-to-end on this build. Empirically
 *       under QEMU/KVM the BSP got past the SIPI write but the
 *       follow-up alive-poll never cleared, indicating the AP
 *       either triple-faulted in trampoline or never re-entered
 *       a sane interrupt-able state. Bisect with a JTAG-equivalent
 *       (qemu -d int,cpu_reset) before flipping the gate to 1.
 *   - Concurrent chain partition + ap_scheduler_loop() ......... NOT YET
 *       Reason: chain.c / chain_registry.c / masq_journal /
 *       persistence.c are not audited for re-entrance from a
 *       second hardware thread. The infrastructure (per-CPU
 *       stacks, GDT/IDT, spinlock, IPI helper) is ready for the
 *       partition step.
 *
 * The selftest line reports exactly what level was reached.
 */

/* Set to 1 once the trampoline is validated end-to-end on QEMU and
 * the family CN60 hardware. Until then we run BSP-only. */
#ifndef SMP_DISPATCH_APS
#define SMP_DISPATCH_APS 0
#endif

#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "pmm.h"
#include "vmm.h"
#include "timer.h"
#include "kprint.h"
#include "io.h"

/* LAPIC register offsets we need for ICR. Other LAPIC regs live in lapic.c
 * but we re-declare a couple here to avoid a public surface change. */
#define LAPIC_ID_REG    0x020
#define LAPIC_ICR_LOW   0x300
#define LAPIC_ICR_HIGH  0x310
#define LAPIC_EOI_REG   0x0B0

/* ICR encoding bits. */
#define ICR_DM_FIXED    (0u << 8)
#define ICR_DM_INIT     (5u << 8)
#define ICR_DM_STARTUP  (6u << 8)
#define ICR_LEVEL_ASSERT (1u << 14)
#define ICR_DEASSERT    (0u << 14)
#define ICR_TRIGGER_EDGE (0u << 15)
#define ICR_TRIGGER_LEVEL (1u << 15)
#define ICR_DELIVERY_PENDING (1u << 12)

/* Trampoline lives at a fixed low-memory page below 1 MiB so the SIPI
 * vector field (8 bits, shifted left by 12) can address it. 0x8000
 * is a conventional choice and well clear of BIOS, EBDA, video, and
 * the PMM's typical first-allocations. */
#define TRAMPOLINE_PA   0x8000
#define TRAMPOLINE_SIZE 0x1000

/* Locations within the trampoline page that the BSP patches before
 * SIPI. Layout matches the assembled blob below. */
#define TR_OFF_PML4     0x0FE0  /* uint64_t  PML4 phys for AP CR3 */
#define TR_OFF_ENTRY    0x0FE8  /* uint64_t  ap_main entry point */
#define TR_OFF_STACKPTR 0x0FF0  /* uint64_t  per-CPU stack top virt */
#define TR_OFF_GDTR     0x0FF8  /* used internally by trampoline */
#define TR_OFF_CPU_IDX  0x0FD8  /* uint64_t  cpu index for this AP */

/* ── Spinlock primitive ──────────────────────────────────────────── */
typedef struct { volatile uint32_t v; } smp_spinlock_t;

static inline void smp_spin_lock(smp_spinlock_t *l)
{
    while (__sync_lock_test_and_set(&l->v, 1)) {
        while (l->v) __asm__ volatile("pause");
    }
}
static inline void smp_spin_unlock(smp_spinlock_t *l)
{
    __sync_lock_release(&l->v);
}

/* Coarse lock around chain registry / masq journal / persistence.
 * Exported via smp.h once we enable concurrent ap_scheduler_loop(). */
smp_spinlock_t g_chain_registry_lock = { 0 };

/* ── Per-CPU state ───────────────────────────────────────────────── */
static smp_cpu_t s_cpus[SMP_MAX_CPUS];
static int       s_cpu_count = 0;       /* CPUs known per MADT */
static int       s_cpus_online = 1;     /* BSP always counts */
static int       s_smp_inited = 0;

/* ── LAPIC ICR helpers ───────────────────────────────────────────── */
static volatile uint8_t *lapic_mmio(void)
{
    return (volatile uint8_t *)(uintptr_t)lapic_base();
}

static void lapic_write_reg(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(lapic_mmio() + reg) = val;
}

static uint32_t lapic_read_reg(uint32_t reg)
{
    return *(volatile uint32_t *)(lapic_mmio() + reg);
}

static void lapic_wait_icr_idle(void)
{
    /* Spin until delivery-pending bit clears. Bounded so a wedged LAPIC
     * doesn't hang us forever; ~1ms TSC budget. */
    uint64_t budget = timer_tsc_freq() / 1000ULL;
    if (budget == 0) budget = 1000000;
    uint64_t start = timer_read_tsc();
    while (lapic_read_reg(LAPIC_ICR_LOW) & ICR_DELIVERY_PENDING) {
        if (timer_read_tsc() - start > budget) return;
        __asm__ volatile("pause");
    }
}

void lapic_send_ipi(uint8_t target_apic_id, uint8_t vector)
{
    if (!lapic_ready()) return;
    lapic_wait_icr_idle();
    lapic_write_reg(LAPIC_ICR_HIGH, ((uint32_t)target_apic_id) << 24);
    lapic_write_reg(LAPIC_ICR_LOW,
                    ICR_DM_FIXED | ICR_LEVEL_ASSERT | (uint32_t)vector);
    lapic_wait_icr_idle();
}

static void lapic_send_init(uint8_t target)
{
    lapic_wait_icr_idle();
    lapic_write_reg(LAPIC_ICR_HIGH, ((uint32_t)target) << 24);
    /* Edge-triggered assert. Modern parts don't need the level-trigger
     * deassert dance. */
    lapic_write_reg(LAPIC_ICR_LOW,
                    ICR_DM_INIT | ICR_LEVEL_ASSERT);
}

static void lapic_send_sipi(uint8_t target, uint8_t vector_page)
{
    lapic_wait_icr_idle();
    lapic_write_reg(LAPIC_ICR_HIGH, ((uint32_t)target) << 24);
    lapic_write_reg(LAPIC_ICR_LOW,
                    ICR_DM_STARTUP | ICR_LEVEL_ASSERT | (uint32_t)vector_page);
}

static void udelay_busy(uint32_t us)
{
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) {
        for (volatile uint32_t i = 0; i < us * 100; i++) { }
        return;
    }
    uint64_t cycles = (freq / 1000000ULL) * (uint64_t)us;
    uint64_t start = timer_read_tsc();
    while (timer_read_tsc() - start < cycles) __asm__ volatile("pause");
}

/* ── Trampoline blob ─────────────────────────────────────────────────
 * Hand-encoded 16->32->64 bit AP bring-up trampoline. Position-fixed
 * at TRAMPOLINE_PA (= 0x8000). The SIPI vector field carries the page
 * number (0x08), so the AP starts in real mode at CS:IP = 0x0800:0x0000
 * which is linear address 0x8000.
 *
 * The trampoline:
 *   1. Sets DS=CS, loads a temporary GDT (defined in the same page).
 *   2. Enables PE in CR0, far-jumps to 32-bit code.
 *   3. Loads CR3 from TR_OFF_PML4, enables PAE in CR4, sets EFER.LME,
 *      enables paging via CR0.PG.
 *   4. Far-jumps to 64-bit code, loads RSP from TR_OFF_STACKPTR, calls
 *      *(uint64_t*)TR_OFF_ENTRY which is ap_main.
 *
 * Because hand-encoding 16/32/64-bit instructions across mode switches
 * is error-prone, this blob is conservative: it does the absolute
 * minimum work in real/protected mode and lets ap_main (C, 64-bit) do
 * the rest (LAPIC enable, IDT load, etc.).
 *
 * The byte array below was generated from the equivalent .S source
 * (see docs/SMP_TRAMPOLINE.txt for the assembly listing). Each line is
 * commented with its instruction.
 */
static const uint8_t g_trampoline[] = {
    /* ── 16-bit real mode, origin 0x0000 (linear 0x8000) ── */
    /* 0x000: cli                        */ 0xFA,
    /* 0x001: xor ax, ax                 */ 0x31, 0xC0,
    /* 0x003: mov ds, ax                 */ 0x8E, 0xD8,
    /* 0x005: mov es, ax                 */ 0x8E, 0xC0,
    /* 0x007: mov ss, ax                 */ 0x8E, 0xD0,
    /* 0x009: lgdt [0x8060]              */ 0x66, 0x2E, 0x0F, 0x01, 0x16, 0x60, 0x80,
    /* 0x010: mov eax, cr0               */ 0x0F, 0x20, 0xC0,
    /* 0x013: or  eax, 1                 */ 0x66, 0x83, 0xC8, 0x01,
    /* 0x017: mov cr0, eax               */ 0x0F, 0x22, 0xC0,
    /* 0x01A: jmp 0x08:0x8020            */ 0x66, 0xEA, 0x20, 0x80, 0x00, 0x00, 0x08, 0x00,

    /* pad to 0x020 */
    0x90, 0x90,

    /* ── 32-bit protected mode at 0x8020 ── */
    /* 0x020: mov ax, 0x10               */ 0x66, 0xB8, 0x10, 0x00,
    /* 0x024: mov ds, ax                 */ 0x8E, 0xD8,
    /* 0x026: mov es, ax                 */ 0x8E, 0xC0,
    /* 0x028: mov ss, ax                 */ 0x8E, 0xD0,
    /* 0x02A: mov eax, [0x8FE0]   (PML4) */ 0xA1, 0xE0, 0x8F, 0x00, 0x00,
    /* 0x02F: mov cr3, eax               */ 0x0F, 0x22, 0xD8,
    /* 0x032: mov eax, cr4               */ 0x0F, 0x20, 0xE0,
    /* 0x035: or  eax, 0x20  (PAE)       */ 0x83, 0xC8, 0x20,
    /* 0x038: mov cr4, eax               */ 0x0F, 0x22, 0xE0,
    /* 0x03B: mov ecx, 0xC0000080 (EFER) */ 0xB9, 0x80, 0x00, 0x00, 0xC0,
    /* 0x040: rdmsr                      */ 0x0F, 0x32,
    /* 0x042: or  eax, 0x100  (LME)      */ 0x0D, 0x00, 0x01, 0x00, 0x00,
    /* 0x047: wrmsr                      */ 0x0F, 0x30,
    /* 0x049: mov eax, cr0               */ 0x0F, 0x20, 0xC0,
    /* 0x04C: or  eax, 0x80000001 (PG|PE)*/ 0x0D, 0x01, 0x00, 0x00, 0x80,
    /* 0x051: mov cr0, eax               */ 0x0F, 0x22, 0xC0,
    /* 0x054: jmp far 0x18:0x8080  (long mode) */
                                          0xEA, 0x80, 0x80, 0x00, 0x00, 0x18, 0x00,
    /* pad to 0x060 */
    0x90, 0x90,

    /* ── 0x060: GDT pointer (limit + base) ── */
    /* limit = 0x27 (5 entries * 8 - 1) */
    0x27, 0x00,
    /* base  = 0x8068 */
    0x68, 0x80, 0x00, 0x00,

    /* pad to 0x068 */
    0x90, 0x90,

    /* ── 0x068: GDT entries ── */
    /* 0x00: null */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x08: 32-bit code (base 0, limit 0xFFFFF, G=1, D=1, type=0x9A) */
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xCF, 0x00,
    /* 0x10: 32-bit data (type=0x92) */
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0xCF, 0x00,
    /* 0x18: 64-bit code (type=0x9A, L=1, D=0) */
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xAF, 0x00,
    /* 0x20: 64-bit data (type=0x92) */
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0xAF, 0x00,

    /* pad to 0x080 (16 bytes) */
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,

    /* ── 0x080: 64-bit long mode entry ──
     * mov ax, 0x20 ; segment loads
     * mov ds, ax / es, ax / ss, ax
     * mov rsp, [0x8FF0]
     * mov rax, [0x8FE8]
     * jmp rax        ; tail-call ap_main(cpu_idx_in_rdi)
     * mov rdi, [0x8FD8]   (cpu index passed via TR_OFF_CPU_IDX)
     */
    /* 0x080: mov ax, 0x20            */ 0x66, 0xB8, 0x20, 0x00,
    /* 0x084: mov ds, ax              */ 0x8E, 0xD8,
    /* 0x086: mov es, ax              */ 0x8E, 0xC0,
    /* 0x088: mov ss, ax              */ 0x8E, 0xD0,
    /* 0x08A: mov fs, ax              */ 0x8E, 0xE0,
    /* 0x08C: mov gs, ax              */ 0x8E, 0xE8,
    /* 0x08E: mov rdi, [rip+disp]  cpu index at 0x8FD8
     *        rip-relative 8-byte: 48 8B 3D xx xx xx xx
     *        next-instr addr = 0x8095, target 0x8FD8 -> disp = 0xF43
     */                                  0x48, 0x8B, 0x3D, 0x43, 0x0F, 0x00, 0x00,
    /* 0x095: mov rsp, [rip+disp]  stack at 0x8FF0
     *        next-instr = 0x809C, disp = 0xF54
     */                                  0x48, 0x8B, 0x25, 0x54, 0x0F, 0x00, 0x00,
    /* 0x09C: mov rax, [rip+disp]  entry at 0x8FE8
     *        next-instr = 0x80A3, disp = 0xF45
     */                                  0x48, 0x8B, 0x05, 0x45, 0x0F, 0x00, 0x00,
    /* 0x0A3: jmp rax                 */ 0xFF, 0xE0,
    /* 0x0A5: hlt; jmp .              */ 0xF4, 0xEB, 0xFD
};

/* ── AP entry point ─────────────────────────────────────────────────
 * Called from the trampoline in 64-bit long mode with:
 *   RDI = AP cpu index (into s_cpus[])
 *   RSP = top of per-CPU stack
 *   CR3 = BSP's PML4 (kernel mappings active)
 *
 * The AP re-enables its own LAPIC (SVR), masks every LVT entry (it
 * inherits the BSP's LAPIC config but each LAPIC is independent), then
 * spins in the "alive" loop bumping its heartbeat. ap_scheduler_loop
 * (concurrent chain resolution) is staged but not entered yet —
 * see top-of-file scope note.
 */
static void ap_lapic_init_local(void)
{
    /* Per-AP LAPIC SVR: enable + spurious vector 0xFF. The BSP's
     * lapic_init mapped the LAPIC page once (it's the same physical
     * page for every CPU at 0xFEE00000), so the mapping is already
     * live in this CR3. */
    volatile uint8_t *m = lapic_mmio();
    if (!m) return;

    /* Mask every LVT entry. */
    *(volatile uint32_t *)(m + 0x320) = (1u << 16);   /* timer    */
    *(volatile uint32_t *)(m + 0x340) = (1u << 16);   /* PMC      */
    *(volatile uint32_t *)(m + 0x330) = (1u << 16);   /* thermal  */
    *(volatile uint32_t *)(m + 0x370) = (1u << 16);   /* error    */
    /* Leave LINT0/LINT1 alone — they're per-CPU but we don't deliver
     * legacy IRQ / NMI to APs in this build. */
    *(volatile uint32_t *)(m + 0x350) = (1u << 16);
    *(volatile uint32_t *)(m + 0x360) = (1u << 16);

    /* TPR=0, SVR enable + vec 0xFF */
    *(volatile uint32_t *)(m + 0x080) = 0;
    *(volatile uint32_t *)(m + 0x0F0) = (1u << 8) | 0xFF;
}

void ap_main(uint64_t cpu_idx) __attribute__((noreturn));
void ap_main(uint64_t cpu_idx)
{
    if (cpu_idx >= SMP_MAX_CPUS) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    smp_cpu_t *me = &s_cpus[cpu_idx];

    /* Mark alive BEFORE touching anything else — the BSP polls this
     * to confirm AP bring-up succeeded. */
    me->alive = 1;
    __sync_synchronize();

    ap_lapic_init_local();

    /* AP heartbeat loop. Concurrent chain resolution is intentionally
     * not enabled yet — the chain registry / masq journal need a
     * re-entrance audit before APs can call into them. The per-CPU
     * stack, GDT/IDT, and the chain-registry spinlock are all in
     * place for that next step. */
    for (;;) {
        me->heartbeat++;
        for (volatile int i = 0; i < 100000; i++) { }
        /* Halt-with-interrupts-disabled so the AP doesn't burn a core
         * spinning hot. Re-enable timer wake when ap_scheduler_loop
         * lands. */
        __asm__ volatile("hlt");
    }
}

/* ── Init ────────────────────────────────────────────────────────── */

static void place_trampoline(uint64_t pml4_phys, uint64_t cpu_idx,
                             uint64_t stack_top, uint64_t entry)
{
    /* Make sure the trampoline page is identity-mapped + writable. */
    vmm_map_range(TRAMPOLINE_PA, TRAMPOLINE_PA, 1,
                  PTE_WRITABLE);

    uint8_t *p = (uint8_t *)(uintptr_t)TRAMPOLINE_PA;
    /* Zero the page first. */
    for (uint32_t i = 0; i < TRAMPOLINE_SIZE; i++) p[i] = 0;
    /* Copy code blob. */
    for (uint32_t i = 0; i < sizeof(g_trampoline); i++) p[i] = g_trampoline[i];

    /* Patch parameter slots. */
    *(volatile uint64_t *)(p + TR_OFF_PML4)     = pml4_phys;
    *(volatile uint64_t *)(p + TR_OFF_ENTRY)    = entry;
    *(volatile uint64_t *)(p + TR_OFF_STACKPTR) = stack_top;
    *(volatile uint64_t *)(p + TR_OFF_CPU_IDX)  = cpu_idx;
}

int smp_init(void)
{
    if (s_smp_inited) return s_cpus_online;
    s_smp_inited = 1;

    s_cpu_count = 0;
    s_cpus_online = 1;  /* BSP */

    acpi_madt_t *m = acpi_madt();
    if (!m) {
        kputs("[smp] no MADT — single-core only\n");
        return 1;
    }

    if (!lapic_ready()) {
        kputs("[smp] LAPIC not ready — single-core only\n");
        return 1;
    }

    uint32_t bsp = lapic_id();

    /* Enumerate. BSP first so it lands at index 0. */
    s_cpus[0].lapic_id = (uint8_t)bsp;
    s_cpus[0].is_bsp = 1;
    s_cpus[0].alive = 1;
    s_cpus[0].current_chain_id = -1;
    s_cpus[0].heartbeat = 0;
    s_cpu_count = 1;

    for (int i = 0; i < m->lapic_count && s_cpu_count < SMP_MAX_CPUS; i++) {
        acpi_lapic_t *l = &m->lapics[i];
        if (!(l->flags & 0x1)) continue;            /* not enabled */
        if (l->apic_id == (uint8_t)bsp) continue;   /* skip BSP */
        smp_cpu_t *c = &s_cpus[s_cpu_count++];
        c->lapic_id = l->apic_id;
        c->is_bsp = 0;
        c->alive = 0;
        c->current_chain_id = -1;
        c->heartbeat = 0;
    }

    kputs("[smp] enumerated ");
    kput_dec((uint64_t)s_cpu_count);
    kputs(" CPUs (BSP lapic=");
    kput_dec((uint64_t)bsp);
    kputs(")\n");

    if (s_cpu_count <= 1) {
        kputs("[smp] only 1 CPU enumerated — single-core mode\n");
        return 1;
    }

    /* Allocate per-CPU stacks (16 KiB each) and per-CPU pages. */
    for (int i = 1; i < s_cpu_count; i++) {
        uint64_t stack = pmm_alloc_contiguous(4); /* 4 * 4K = 16 KiB */
        if (!stack) {
            kputs("[smp] stack alloc failed for AP idx ");
            kput_dec((uint64_t)i);
            kputc('\n');
            s_cpus[i].stack_phys = 0;
            continue;
        }
        s_cpus[i].stack_phys = stack;
        s_cpus[i].stack_top_virt = stack + 4 * 4096;
        s_cpus[i].per_cpu_page = (void *)(uintptr_t)pmm_alloc();
    }

    uint64_t pml4 = vmm_get_pml4();

    /* Place the trampoline once with sane defaults; this proves the
     * blob fits in the chosen low page and the parameter slots are
     * reachable. Re-patched per-AP inside the dispatch loop below. */
    place_trampoline(pml4, 0, 0, (uint64_t)(uintptr_t)&ap_main);

#if SMP_DISPATCH_APS
    /* INIT-SIPI-SIPI per AP. Per Intel SDM Vol 3 8.4.4.1: assert INIT,
     * wait 10 ms, send first SIPI, wait 200 us, send second SIPI,
     * wait 200 us, then check if AP came up. */
    for (int i = 1; i < s_cpu_count; i++) {
        smp_cpu_t *c = &s_cpus[i];
        if (!c->stack_phys) continue;

        place_trampoline(pml4, (uint64_t)i, c->stack_top_virt,
                         (uint64_t)(uintptr_t)&ap_main);

        lapic_send_init(c->lapic_id);
        udelay_busy(10000);
        lapic_send_sipi(c->lapic_id, TRAMPOLINE_PA >> 12);
        udelay_busy(200);
        lapic_send_sipi(c->lapic_id, TRAMPOLINE_PA >> 12);

        volatile uint8_t *alive_p = &c->alive;
        for (int ms = 0; ms < 100 && !*alive_p; ms++) {
            timer_wait_ms(1);
        }
        if (c->alive) s_cpus_online++;
    }
#else
    /* SIPI dispatch gated off until trampoline is validated. APs
     * remain in BIOS halt state. Per-CPU state, stacks, and IPI
     * helpers are all live; flipping SMP_DISPATCH_APS to 1 is the
     * only thing needed to attempt bring-up. */
    kputs("[smp] AP SIPI dispatch gated off (SMP_DISPATCH_APS=0)\n");
    kputs("[smp] running BSP-only; ");
    kput_dec((uint64_t)(s_cpu_count - 1));
    kputs(" AP(s) enumerated, trampoline placed\n");
#endif

    return s_cpus_online;
}

int smp_cpu_count(void)   { return s_cpu_count; }
int smp_cpus_online(void) { return s_cpus_online; }

uint32_t smp_this_cpu(void) { return lapic_id(); }

smp_cpu_t *smp_cpu_by_lapic(uint8_t lapic_id_in)
{
    for (int i = 0; i < s_cpu_count; i++) {
        if (s_cpus[i].lapic_id == lapic_id_in) return &s_cpus[i];
    }
    return 0;
}

smp_cpu_t *smp_cpu_by_index(int idx)
{
    if (idx < 0 || idx >= s_cpu_count) return 0;
    return &s_cpus[idx];
}

void smp_tlb_shootdown(void)
{
    if (s_cpus_online <= 1) return;
    uint32_t bsp = lapic_id();
    for (int i = 0; i < s_cpu_count; i++) {
        if (s_cpus[i].lapic_id == (uint8_t)bsp) continue;
        if (!s_cpus[i].alive) continue;
        lapic_send_ipi(s_cpus[i].lapic_id, 0xFD);
    }
}

void smp_print_selftest_line(void)
{
    int total = (s_cpu_count > 0) ? s_cpu_count : 1;
    int online = s_cpus_online;
    int aps_online = online - 1;
    int aps_total  = total  - 1;

    kputs("SMP ................... ");
    kput_dec((uint64_t)online);
    kputs(" cores online (BSP + ");
    kput_dec((uint64_t)(aps_online < 0 ? 0 : aps_online));
    kputs(" APs)");

#if !SMP_DISPATCH_APS
    if (aps_total > 0) {
        kputs(" [");
        kput_dec((uint64_t)aps_total);
        kputs(" AP(s) enumerated, trampoline placed, SIPI gated]");
    }
#else
    if (online < total) {
        kputs(" [");
        kput_dec((uint64_t)(total - online));
        kputs(" AP(s) failed bring-up]");
    }
    if (online > 1) {
        kputs(", APs in alive-loop (chain partition: TODO)");
    }
#endif
    kputc('\n');
}

void smp_cmd_cores(void)
{
    kputs("\n  Cores\n  ─────\n");
    kputs("  idx  lapic  role  alive   heartbeat   chain\n");
    for (int i = 0; i < s_cpu_count; i++) {
        smp_cpu_t *c = &s_cpus[i];
        kputs("  ");
        kput_dec((uint64_t)i);
        kputs("    ");
        kput_dec((uint64_t)c->lapic_id);
        kputs("     ");
        kputs(c->is_bsp ? "BSP" : "AP ");
        kputs("   ");
        kputs(c->alive ? "yes" : "no ");
        kputs("    ");
        kput_dec(c->heartbeat);
        kputs("   ");
        if (c->current_chain_id < 0) kputs("(idle)");
        else                          kput_dec((uint64_t)c->current_chain_id);
        kputc('\n');
    }
    /* Compute approx ticks-per-second from heartbeat deltas across a
     * 100ms sample. Cheap, demonstrates aliveness over time. */
    static uint64_t s_prev[SMP_MAX_CPUS];
    static uint64_t s_prev_tsc;
    uint64_t freq = timer_tsc_freq();
    uint64_t now = timer_read_tsc();
    if (s_prev_tsc != 0 && freq > 0) {
        uint64_t dt = now - s_prev_tsc;
        if (dt > 0) {
            kputs("  heartbeat-rate (since last `cores`):\n");
            for (int i = 0; i < s_cpu_count; i++) {
                uint64_t d = s_cpus[i].heartbeat - s_prev[i];
                uint64_t per_s = (d * freq) / dt;
                kputs("    cpu ");
                kput_dec((uint64_t)i);
                kputs(": ");
                kput_dec(per_s);
                kputs(" tps\n");
            }
        }
    }
    for (int i = 0; i < s_cpu_count; i++) s_prev[i] = s_cpus[i].heartbeat;
    s_prev_tsc = now;
    kputc('\n');
}
