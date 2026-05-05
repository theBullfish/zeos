/*
 * Zeos — SMP (multi-core) bring-up.
 *
 * Walks the MADT, allocates a per-CPU state struct for every Processor
 * LAPIC entry, places a low-memory trampoline that drives 16-bit -> 32-bit
 * -> 64-bit long-mode bring-up, and issues INIT-SIPI-SIPI to each AP.
 *
 * Honest scope: this is the foundation. APs reach a 64-bit "alive"
 * loop and bump their per-CPU heartbeat counter so the BSP can prove
 * they're running. Full chain partition + concurrent resolve across
 * cores is staged behind a coarse spinlock around the chain registry
 * (see ap_scheduler_loop()), but is intentionally NOT enabled by default
 * yet — the chain registry / masq journal / persistence layer are not
 * audited for re-entrance from a second core. Selftest reports exactly
 * the level reached.
 */

#ifndef ZEOS_SMP_H
#define ZEOS_SMP_H

#include <stdint.h>

#define SMP_MAX_CPUS 64

typedef struct {
    uint8_t   lapic_id;          /* APIC ID from MADT */
    uint8_t   is_bsp;            /* 1 if this is the bootstrap CPU */
    uint8_t   alive;             /* set by AP once it reaches ap_main */
    uint8_t   _pad;
    int       cpu_idx;           /* index in s_cpus[] (0 = BSP) */
    uint64_t  stack_phys;        /* per-CPU stack physical base */
    uint64_t  stack_top_virt;    /* per-CPU stack top (virt addr) */
    uint64_t  heartbeat;         /* AP increments while alive */
    int       current_chain_id;  /* -1 when idle */

    /* Per-CPU runtime stats (updated by ap_scheduler_loop / BSP). */
    uint64_t  tick_count;        /* completed scheduler ticks on this CPU */
    uint64_t  chains_resolved;   /* chain_resolve calls completed */
    uint64_t  preempt_kills;     /* timer-driven preempts (BSP only today) */
    uint64_t  heartbeat_tsc;     /* TSC at last loop iteration */

    /* TLB shootdown plumbing — see tlb.c (folded into smp.c). The BSP
     * writes a pending range into this CPU's slot, sends IPI 0xF0, and
     * spins on tlb_ack. The AP's ISR reads the range, INVLPGs (or
     * reloads CR3 for a full flush), bumps tlb_shootdowns_received,
     * then bumps tlb_ack to match tlb_req. Idle when req==ack. */
    volatile uint64_t tlb_req;             /* BSP-incremented request seq */
    volatile uint64_t tlb_ack;             /* AP-incremented ack seq */
    volatile uint64_t tlb_pending_va;      /* range start, 0 = full flush */
    volatile uint32_t tlb_pending_pages;   /* page count (0 ⇒ full flush) */
    uint64_t  tlb_shootdowns_received;     /* this CPU handled this many */
    uint64_t  tlb_shootdowns_failed;       /* ack timeouts seen by BSP for this AP */

    /* gdt/idt/preempt_jmpbuf live as offsets into per-CPU page; kept
     * out of this header to avoid pulling gdt/idt internals everywhere. */
    void     *per_cpu_page;
} smp_cpu_t;

/* Initialize SMP: parse MADT, allocate per-CPU state, place trampoline,
 * send INIT-SIPI-SIPI to every non-BSP LAPIC. Returns the number of CPUs
 * online (including the BSP). On failure or single-core, returns 1 and
 * the system continues running on the BSP only. */
int      smp_init(void);

/* Total CPUs known (BSP + APs) per MADT, regardless of how many came
 * online. smp_cpus_online() is what came up. */
int      smp_cpu_count(void);
int      smp_cpus_online(void);

/* Read LAPIC ID of the calling core. */
uint32_t smp_this_cpu(void);

/* Lookup state for a given LAPIC id. Returns NULL if unknown. */
smp_cpu_t *smp_cpu_by_lapic(uint8_t lapic_id);
smp_cpu_t *smp_cpu_by_index(int idx);

/* TLB shootdown: send a flush IPI to every other core. Vector 0xF0.
 * Synchronous from the BSP's POV — broadcasts a per-CPU range slot,
 * IPIs each AP, then spin-waits ≤1ms per AP for ack. No-op if SMP
 * didn't bring up any APs. Safe to call from BSP path only.
 *
 * Range form: invalidate `pages` 4 KiB pages starting at `va`. If
 * `pages == 0` the call degenerates to a full flush (CR3 reload on
 * each AP). `tlb_shootdown_one` targets a specific cpu index. */
void     smp_tlb_shootdown(void);
void     tlb_shootdown_all(void);
void     tlb_shootdown_range(uint64_t va, uint64_t pages);
void     tlb_shootdown_one(int cpu_idx);

/* Print the TLB shootdown selftest line ("wired (...)") to kprint. */
void     tlb_print_selftest_line(void);

/* The IPI ISR registered at vector 0xF0 — APs land here and INVLPG
 * the pending range. Exposed so idt.c can register it. */
void     tlb_shootdown_isr(uint64_t vector, uint64_t error_code);

/* Total shootdowns received by `cpu_idx` (BSP entry returns 0). */
uint64_t tlb_shootdowns_received(int cpu_idx);

/* Partition helpers — chain_id % cpus_online maps to owning CPU index. */
int      smp_chain_owner(int chain_id);
int      smp_partition_active(void);
void     smp_partition_activate(void);
uint32_t smp_cpu_tps(int cpu_idx);

/* Send an IPI to a specific LAPIC ID with the given vector. */
void     lapic_send_ipi(uint8_t target_apic_id, uint8_t vector);

/* Print the SMP selftest line. */
void     smp_print_selftest_line(void);

/* Shell command implementation for `cores`. */
void     smp_cmd_cores(void);

#endif /* ZEOS_SMP_H */
