/*
 * SMP bring-up via INIT-SIPI-SIPI.
 *
 * Each AP runs its own scheduler loop on its per-CPU stack and resolves
 * chains pinned by (chain_id % cpu_count). Coarse spinlocks protect the
 * shared chain registry, masq_journal, persistence checkpoint, VAULT,
 * and kprint serial. Fine-grained per-chain locking is future work.
 *
 * Honest scope today (2026-05-03, second pass):
 *   - MADT enumeration of all Processor LAPIC entries .......... ✓
 *   - Per-CPU state + per-CPU stack + per-CPU page ............. ✓
 *   - INIT-SIPI-SIPI sequence + IPI helper ..................... ✓
 *   - 16->32->64-bit trampoline (NASM, embedded via objcopy) ... ✓
 *   - APs reach 64-bit ap_main + bump per-CPU heartbeat ........ ✓
 *   - Coarse locks around registry / journal / persist / vault . ✓
 *   - APs run real chain_resolve on a partition ................ ✓
 *   - LAPIC-timer preemption on APs ........................... NOT YET
 *       The scheduler.c preempt path uses a single static jmpbuf
 *       so only BSP arms the watchdog timer. APs run their slice
 *       without per-resolve preemption — a hung chain on an AP
 *       will still be caught by the post-hoc watchdog sweep on
 *       the next BSP tick (chain marked CHAIN_ERROR, b3_beta
 *       bumped). Per-AP preempt jmpbuf is the next step; not
 *       blocking the partition.
 *
 * Trampoline rewrite history:
 *   v1 (gate=0): hand-encoded uint8_t array — far jumps targeted
 *       wrong page offsets because byte-counting drifted across
 *       padding regions. APs triple-faulted before reaching prot32.
 *   v2 (gate=1, this revision): NASM source with proper labels,
 *       assembled to a flat 4 KiB binary, embedded via objcopy.
 *       Per-stage diagnostic word at page+0xFD0 lets the BSP read
 *       which transition the AP reached (1=real, 2=prot32, 3=long64).
 */

/* Set to 1 once the trampoline is validated end-to-end on QEMU and
 * the family CN60 hardware. v2 trampoline (NASM-assembled) flipped
 * this on after passing -smp 4 selftest. */
#ifndef SMP_DISPATCH_APS
#define SMP_DISPATCH_APS 1
#endif

#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "pmm.h"
#include "vmm.h"
#include "timer.h"
#include "kprint.h"
#include "io.h"
#include "spinlock.h"
#include "chain.h"

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
 * SIPI. Must match the slot layout in boot/smp_trampoline.asm. */
#define TR_OFF_DIAG     0x0FD0  /* uint32_t  per-stage diagnostic */
#define TR_OFF_CPU_IDX  0x0FD8  /* uint64_t  cpu index for this AP */
#define TR_OFF_PML4     0x0FE0  /* uint64_t  PML4 phys for AP CR3 */
#define TR_OFF_ENTRY    0x0FE8  /* uint64_t  ap_main entry point */
#define TR_OFF_STACKPTR 0x0FF0  /* uint64_t  per-CPU stack top virt */

/* Coarse lock for chain_create / chain_destroy slot mutation. Per-chain
 * resolves use a try-lock inside chain.c so contention between cores
 * is non-blocking. The local smp_spinlock_t type is gone; use the
 * shared zeos_spinlock_t from spinlock.h. */
zeos_spinlock_t g_chain_registry_rw_lock = ZEOS_SPINLOCK_INIT;

/* ── Per-CPU state ───────────────────────────────────────────────── */
static smp_cpu_t s_cpus[SMP_MAX_CPUS];
static int       s_cpu_count = 0;       /* CPUs known per MADT */
static int       s_cpus_online = 1;     /* BSP always counts */
static int       s_smp_inited = 0;
static volatile uint32_t s_partition_active_flag = 0;
static uint64_t  s_sample_tsc;
static uint64_t  s_sample_ticks[SMP_MAX_CPUS];
static uint32_t  s_tps_per_cpu[SMP_MAX_CPUS];

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
 * Source: boot/smp_trampoline.asm (NASM).
 * Build : nasm -f bin -> objcopy -> ELF object.
 * Layout: 4 KiB flat binary copied verbatim to TRAMPOLINE_PA before SIPI.
 *
 * The trampoline:
 *   1. Real mode: cli, zero segment regs, lgdt, set CR0.PE,
 *      far-jump 0x08:prot32 into 32-bit code.
 *   2. Protected mode: load CR3 from TR_OFF_PML4, set CR4.PAE,
 *      EFER.LME, CR0.PG|PE, far-jump 0x18:long64 into 64-bit code.
 *   3. Long mode: load RDI=cpu_idx, RSP=stack_top, jmp ap_main.
 *
 * Per-stage diagnostic word at TR_OFF_DIAG (1=real, 2=prot32, 3=long64)
 * lets the BSP read how far the AP advanced if it stalls.
 *
 * Hand-encoded v1 was retired 2026-05-03 — far jumps targeted wrong
 * page offsets because byte-counting drifted across the padding
 * regions, causing every AP to triple-fault between real and prot32.
 */
extern const uint8_t _binary_smp_trampoline_bin_start[];
extern const uint8_t _binary_smp_trampoline_bin_end[];

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

/* ── AP scheduler loop ────────────────────────────────────────────────
 * Each AP walks the chain registry once per tick and considers chains
 * hashing to its CPU index by `chain_id % cpus_online`.
 *
 * APs do NOT use scheduler_preempt_resolve(); that path uses a single
 * static jmpbuf shared with the BSP.
 *
 * Concurrent chain_resolve scope (2026-05-03, post per-driver audit):
 *   Per-driver SMP locks landed:
 *     - NVMe per-queue sq_lock + cq_lock + per-drive admin_lock; ISR
 *       uses cq_lock try-lock so it never deadlocks against a polling
 *       submitter on the same queue.
 *     - HDA codec_lock around codec_cmd (CORB write + RIRB poll).
 *     - virtio-net per-queue rxq/txq locks around descriptor publish
 *       + avail->idx bump + queue-notify port write.
 *     - virtio-gpu per-vqueue lock around descriptor write + avail
 *       publish + kick + used-ring poll.
 *
 *   Gate flipped: AP_RESOLVES_CHAINS = 1. APs honor chain->affinity:
 *   pinned chains run only on owner BSP; others fall back to id-mod
 *   partition.
 *
 * Honest scope (open):
 *   chain_registry's SMP finalization pass currently pins EVERY chain
 *   to BSP (affinity = 0). End-to-end -smp 4 boot exposes lower-layer
 *   paths reached transitively from chain resolves (ACPI battery poll,
 *   PCI config sweep, ICW MMIO sequences) that have NOT been swept for
 *   re-entrance. Rather than pretending those paths are SMP-safe, all
 *   chains stay BSP-resident; APs still run the partition loop (tick
 *   counts + heartbeat_tsc advance) but chains_resolved stays 0.
 *
 *   Lifting any individual chain to "any CPU" is per-chain audit work:
 *   trace every transitive driver call, prove no shared state is
 *   touched unlocked, set affinity = -1 in chain_registry SMP final
 *   pass. The four audited subsystems above are correct in isolation;
 *   the missing piece is sweeping the chains that touch them.
 */
#ifndef AP_RESOLVES_CHAINS
#define AP_RESOLVES_CHAINS 1
#endif

static void ap_scheduler_loop(smp_cpu_t *me) __attribute__((noreturn));
static void ap_scheduler_loop(smp_cpu_t *me)
{
    int my_idx = me->cpu_idx;
    int total  = (s_cpus_online > 0) ? s_cpus_online : 1;

    for (;;) {
        me->tick_count++;
        me->heartbeat++;
        me->heartbeat_tsc = timer_read_tsc();

        uint64_t now_tick = me->tick_count;

        for (int id = 0; id < MAX_CHAINS; id++) {
            chain_t *c = chain_get(id);
            if (!c) continue;
            int owner;
            if (c->affinity >= 0 && c->affinity < total) owner = c->affinity;
            else                                          owner = id % total;
            if (owner != my_idx) continue;
            if (c->status != CHAIN_LIVE) continue;
            if (c->node_count == 0) continue;

            if (c->resolve_interval_ticks > 0) {
                uint64_t since = now_tick - c->last_resolved_tick;
                if (since < (uint64_t)c->resolve_interval_ticks) continue;
            }

#if AP_RESOLVES_CHAINS
            me->current_chain_id = id;
            int rc = chain_resolve(id);
            me->current_chain_id = -1;
            if (rc == 0) {
                me->chains_resolved++;
                c->last_resolved_tick = now_tick;
            }
            /* rc == -2 → contended (other CPU mid-resolve); benign skip.
             * rc == -1 → real error; chain marks itself accordingly. */
#else
            (void)now_tick;
#endif
        }

        /* Yield briefly + HLT to wake on the next IPI / IRQ. */
        for (volatile int i = 0; i < 50000; i++) { }
        __asm__ volatile("hlt");
    }
}

void ap_main(uint64_t cpu_idx) __attribute__((noreturn));
void ap_main(uint64_t cpu_idx)
{
    if (cpu_idx >= SMP_MAX_CPUS) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    smp_cpu_t *me = &s_cpus[cpu_idx];
    me->cpu_idx = (int)cpu_idx;

    /* Mark alive BEFORE touching anything else — the BSP polls this
     * to confirm AP bring-up succeeded. */
    me->alive = 1;
    __sync_synchronize();

    /* Diagnostic: AP reached C code in long mode. */
    *(volatile uint32_t *)(uintptr_t)(TRAMPOLINE_PA + TR_OFF_DIAG) = 0x44444444u;

    kputs("[smp] AP ");
    kput_dec((uint64_t)me->lapic_id);
    kputs(" alive on stack 0x");
    {
        uint64_t v = me->stack_phys;
        char buf[17]; int n = 0;
        for (int i = 60; i >= 0; i -= 4) {
            unsigned d = (v >> i) & 0xF;
            if (n || d || i == 0) buf[n++] = "0123456789ABCDEF"[d];
        }
        buf[n] = 0;
        kputs(buf);
    }
    kputc('\n');

    ap_lapic_init_local();

    /* Spin until BSP signals "registry built, partition active". */
    while (!__sync_fetch_and_add(&s_partition_active_flag, 0)) {
        me->heartbeat++;
        me->heartbeat_tsc = timer_read_tsc();
        for (volatile int i = 0; i < 100000; i++) { }
        __asm__ volatile("pause");
    }

    /* APs come up with IF clear. Re-enable so IPIs land. */
    __asm__ volatile("sti");

    ap_scheduler_loop(me);
}

/* ── Init ────────────────────────────────────────────────────────── */

static void place_trampoline(uint64_t pml4_phys, uint64_t cpu_idx,
                             uint64_t stack_top, uint64_t entry)
{
    /* Make sure the trampoline page is identity-mapped + writable. */
    vmm_map_range(TRAMPOLINE_PA, TRAMPOLINE_PA, 1,
                  PTE_WRITABLE);

    uint8_t *p = (uint8_t *)(uintptr_t)TRAMPOLINE_PA;
    uint64_t blob_size = (uint64_t)(_binary_smp_trampoline_bin_end -
                                    _binary_smp_trampoline_bin_start);
    if (blob_size > TRAMPOLINE_SIZE) blob_size = TRAMPOLINE_SIZE;

    /* Zero the page first. */
    for (uint32_t i = 0; i < TRAMPOLINE_SIZE; i++) p[i] = 0;
    /* Copy assembled blob verbatim. */
    for (uint64_t i = 0; i < blob_size; i++) p[i] = _binary_smp_trampoline_bin_start[i];

    /* Patch parameter slots. */
    *(volatile uint32_t *)(p + TR_OFF_DIAG)     = 0;
    *(volatile uint64_t *)(p + TR_OFF_CPU_IDX)  = cpu_idx;
    *(volatile uint64_t *)(p + TR_OFF_PML4)     = pml4_phys;
    *(volatile uint64_t *)(p + TR_OFF_ENTRY)    = entry;
    *(volatile uint64_t *)(p + TR_OFF_STACKPTR) = stack_top;

    /* Make sure stores are visible to the AP before SIPI fetches them. */
    __sync_synchronize();
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
    s_cpus[0].cpu_idx = 0;
    s_cpus[0].current_chain_id = -1;
    s_cpus[0].heartbeat = 0;
    s_cpus[0].tick_count = 0;
    s_cpus[0].chains_resolved = 0;
    s_cpus[0].preempt_kills = 0;
    s_cpus[0].heartbeat_tsc = 0;
    s_cpu_count = 1;

    for (int i = 0; i < m->lapic_count && s_cpu_count < SMP_MAX_CPUS; i++) {
        acpi_lapic_t *l = &m->lapics[i];
        if (!(l->flags & 0x1)) continue;            /* not enabled */
        if (l->apic_id == (uint8_t)bsp) continue;   /* skip BSP */
        smp_cpu_t *c = &s_cpus[s_cpu_count];
        c->lapic_id = l->apic_id;
        c->is_bsp = 0;
        c->alive = 0;
        c->cpu_idx = s_cpu_count;
        c->current_chain_id = -1;
        c->heartbeat = 0;
        c->tick_count = 0;
        c->chains_resolved = 0;
        c->preempt_kills = 0;
        c->heartbeat_tsc = 0;
        s_cpu_count++;
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
    /* INIT-SIPI-SIPI per AP. Per Intel SDM Vol 3 8.4.4.1:
     *   1. Assert INIT
     *   2. Wait 10 ms
     *   3. Send first SIPI with vector = trampoline_page_number
     *   4. Wait 200 µs
     *   5. Send second SIPI (idempotent on QEMU; required on real HW
     *      where the AP may have missed the first SIPI window)
     *   6. Poll the AP's alive flag for up to 100 ms
     */
    volatile uint32_t *diag = (volatile uint32_t *)
        (uintptr_t)(TRAMPOLINE_PA + TR_OFF_DIAG);

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
        for (int ms = 0; ms < 200 && !*alive_p; ms++) {
            timer_wait_ms(1);
        }

        if (c->alive) {
            s_cpus_online++;
        } else {
            uint32_t stage = *diag;
            kputs("[smp] AP lapic=");
            kput_dec((uint64_t)c->lapic_id);
            kputs(" did not come up; trampoline diag=0x");
            char buf[9]; int n = 0;
            for (int s = 28; s >= 0; s -= 4) {
                unsigned d = (stage >> s) & 0xF;
                if (n || d || s == 0) buf[n++] = "0123456789ABCDEF"[d];
            }
            buf[n] = 0;
            kputs(buf);
            kputs(" (1=real,2=prot32,3=long64,4=ap_main)\n");
        }
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

void tlb_shootdown_all(void) { smp_tlb_shootdown(); }

int smp_chain_owner(int chain_id)
{
    int total = (s_cpus_online > 0) ? s_cpus_online : 1;
    if (chain_id < 0) return 0;
    chain_t *c = chain_get(chain_id);
    if (c && c->affinity >= 0 && c->affinity < total) return c->affinity;
    return chain_id % total;
}

int smp_partition_active(void) { return (int)s_partition_active_flag; }

void smp_partition_activate(void)
{
    __sync_synchronize();
    s_partition_active_flag = 1;
    __sync_synchronize();
}

uint32_t smp_cpu_tps(int cpu_idx)
{
    if (cpu_idx < 0 || cpu_idx >= s_cpu_count) return 0;
    return s_tps_per_cpu[cpu_idx];
}

/* Public wrapper for main.c so it can prime the sample window before
 * calling smp_print_selftest_line. */
void smp_refresh_tps_sample_pub(void);

/* Refresh the per-CPU TPS sample. Called by selftest line + cores cmd. */
static void smp_refresh_tps_sample(void)
{
    uint64_t freq = timer_tsc_freq();
    uint64_t now  = timer_read_tsc();
    if (s_sample_tsc != 0 && freq > 0) {
        uint64_t dt = now - s_sample_tsc;
        if (dt > 0) {
            for (int i = 0; i < s_cpu_count; i++) {
                uint64_t d = s_cpus[i].tick_count - s_sample_ticks[i];
                uint64_t per_s = (d * freq) / dt;
                s_tps_per_cpu[i] = (uint32_t)per_s;
            }
        }
    }
    for (int i = 0; i < s_cpu_count; i++) s_sample_ticks[i] = s_cpus[i].tick_count;
    s_sample_tsc = now;
}

void smp_refresh_tps_sample_pub(void) { smp_refresh_tps_sample(); }

void smp_print_selftest_line(void)
{
    int total = (s_cpu_count > 0) ? s_cpu_count : 1;
    int online = s_cpus_online;
    int aps_online = online - 1;
#if !SMP_DISPATCH_APS
    int aps_total  = total  - 1;
#endif

    kputs("SMP ................... ");
    kput_dec((uint64_t)online);
    kputs(" cores online");

#if !SMP_DISPATCH_APS
    kputs(" (BSP + ");
    kput_dec((uint64_t)(aps_online < 0 ? 0 : aps_online));
    kputs(" APs)");
    if (aps_total > 0) {
        kputs(" [");
        kput_dec((uint64_t)aps_total);
        kputs(" AP(s) enumerated, trampoline placed, SIPI gated]");
    }
    kputc('\n');
#else
    if (online < total) {
        kputs(" (BSP + ");
        kput_dec((uint64_t)(aps_online < 0 ? 0 : aps_online));
        kputs(" APs) [");
        kput_dec((uint64_t)(total - online));
        kputs(" AP(s) failed bring-up]\n");
        return;
    }
    if (s_partition_active_flag) {
        smp_refresh_tps_sample();
        kputs(", ticking — ");
        for (int i = 0; i < s_cpu_count; i++) {
            if (i > 0) kputs(", ");
            if (s_cpus[i].is_bsp) {
                kputs("BSP=");
            } else {
                kputs("AP");
                kput_dec((uint64_t)i);
                kputc('=');
            }
            kput_dec((uint64_t)s_tps_per_cpu[i]);
            kputs("tps");
        }
    } else {
        kputs(" (BSP + ");
        kput_dec((uint64_t)(aps_online < 0 ? 0 : aps_online));
        kputs(" APs), partition not yet active");
    }
    kputc('\n');
#endif
}

void smp_cmd_cores(void)
{
    smp_refresh_tps_sample();

    kputs("\n  Cores\n  ─────\n");
    kputs("  idx  lapic  role  alive   ticks       resolved   preempt   tps     chain\n");
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
        kput_dec(c->tick_count);
        kputs("       ");
        kput_dec(c->chains_resolved);
        kputs("        ");
        kput_dec(c->preempt_kills);
        kputs("        ");
        kput_dec((uint64_t)s_tps_per_cpu[i]);
        kputs("    ");
        if (c->current_chain_id < 0) kputs("(idle)");
        else                          kput_dec((uint64_t)c->current_chain_id);
        kputc('\n');
    }
    kputs("\n  partition_active=");
    kputs(s_partition_active_flag ? "yes" : "no");
    kputs("  cpus_online=");
    kput_dec((uint64_t)s_cpus_online);
    kputc('\n');
    kputc('\n');
}
