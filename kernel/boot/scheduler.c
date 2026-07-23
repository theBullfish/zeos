/*
 * Zeos scheduler -- chain resolution replaces the poll loop.
 * Per docs/PARADIGM_CONVERSION.md section 7: every node fires when
 * its inputs are ready; the runtime resolves the whole graph each
 * tick. Time-slicing is dead.
 *
 * B3 belief feeds back into scheduling: chains with high failure
 * rate (b3_beta > b3_alpha) skip every Nth tick until they recover.
 * Both threshold and skip period are tunable per chain via
 * chain->backoff_skip_threshold / chain->backoff_skip_every.
 *
 * MasQ records every tick with errors, slow-resolve incidents,
 * aggregate slow ticks (whole-tick budget overrun), preempt kills
 * (chains hard-killed mid-resolve by the LAPIC timer), and post-hoc
 * watchdog kills (defensive: chains whose deadline expired but
 * preempt-longjmp itself faulted somehow).
 *
 * NOTE: Watchdog enforces forward progress via LAPIC timer +
 * setjmp/longjmp. The post-hoc deadline sweep at start-of-tick is
 * the defensive secondary path (covers the case where the longjmp
 * itself faulted; should normally stay 0 on a healthy boot).
 *
 * PREEMPTION INTEGRATION POINT — WIRED
 * Per-resolve preemption uses LAPIC timer vector 0xEF:
 *   1. scheduler_preempt_resolve() saves a setjmp checkpoint, sets
 *      g_resolving_chain_id, arms lapic_timer_oneshot() for the
 *      chain's watchdog_timeout_us, then calls chain_resolve().
 *   2. On normal return: timer is disarmed, checkpoint cleared.
 *   3. On timer expiry: lapic_timer_isr marks the chain CHAIN_ERROR,
 *      bumps b3_beta, logs a preempt-kill record, EOIs the LAPIC,
 *      and longjmps back to step 1's checkpoint with rc=1. The
 *      scheduler then continues with the next chain.
 *
 * Idle: HLT until next interrupt. Wake: any IRQ-driven chain
 * (kbd, mouse, NIC RX, NVMe completion, GPU vsync) bumps a pending
 * counter that the next tick consumes.
 *
 * Serial input (UART) flows through CHAIN_SERIAL_IN; PS/2 through
 * CHAIN_KEYBOARD. Both produce input_event which MDE routes to
 * the shell pump.
 */

#include "scheduler.h"
#include "chain.h"
#include "chain_registry.h"
#include "mde.h"
#include "keyboard.h"
#include "mouse.h"
#include "shell.h"
#include "serial.h"
#include "timer.h"
#include "kprint.h"
#include "io.h"
#include "lockscreen.h"
#include "lapic.h"
#include "idt.h"

#define SCHED_PREEMPT_VECTOR 0xEFu

#define SCHED_LOG_RING 256

/* Log entry types in the MasQ ring. */
#define SLE_TICK         0u
#define SLE_AGG_SLOW     1u
#define SLE_WDOG_KILL    2u
#define SLE_PREEMPT_KILL 3u

typedef struct {
    uint8_t  kind;                  /* SLE_* */
    uint8_t  _pad[1];
    uint16_t chains_resolved;
    uint16_t live_chains;
    uint16_t errors;
    uint16_t slow_resolves;
    uint16_t _pad2;
    uint64_t tick;
    uint64_t tsc_start;
    uint64_t tsc_end;

    /* Extra fields for SLE_AGG_SLOW:
     *   over_us       = how far over budget (us)
     *   top_ids[3]    = chain ids of top-3 slowest in this tick
     *   top_ms[3]     = per-chain resolve_ms for those
     * Extra fields for SLE_WDOG_KILL:
     *   over_us       = unused (0)
     *   top_ids[0]    = chain id killed
     *   top_ms[0]     = how long it ran (ms) before being declared dead
     */
    uint32_t over_us;
    int16_t  top_ids[3];
    float    top_ms[3];
} sched_log_entry_t;

static sched_log_entry_t s_log[SCHED_LOG_RING];
static uint32_t s_log_head;
static uint64_t s_log_count;        /* total entries written */

static uint64_t s_tick;             /* monotonically increasing */
static uint32_t s_quantum_us = 1000;
static uint32_t s_chains_resolved_last;
static uint32_t s_errors_last;
static uint32_t s_slow_resolves_total;
static uint32_t s_live_chains_last;

/* New aggregate counters. */
static uint32_t s_agg_slow_total;     /* whole-tick budget overruns */
static uint32_t s_watchdog_kills;     /* post-hoc watchdog kills (defensive secondary) */
static uint32_t s_preempt_kills;      /* LAPIC-timer-driven mid-resolve kills (primary) */

/* ── Preemption state ─────────────────────────────────────────────────
 * Per-CPU. Each core that calls scheduler_preempt_resolve() writes its
 * own checkpoint + currently-resolving chain id into its per-CPU slot
 * on smp_cpu_t. The LAPIC timer ISR runs on whichever core actually
 * expired and reads THAT core's slot. Single globals were a wedge:
 * BSP and AP would overwrite each other's checkpoints, and a timer
 * fire on either core could longjmp to the wrong core's stack.
 *
 * Pre-SMP-init fallback: a single static slot used by the BSP before
 * smp_init() runs. After smp_init the BSP's slot is the s_cpus[0]
 * struct entry; we route to it via smp_cpu_by_lapic(lapic_id()).
 */
typedef uint64_t zeos_jmpbuf_t[8];   /* rbx, rbp, r12-r15, rsp, rip */

/* Pre-SMP fallback (BSP only, used before smp_init runs). After SMP
 * comes up we redirect to the per-CPU slot on smp_cpu_t. */
static volatile int      s_pre_smp_resolving_chain_id = -1;
static volatile uint64_t s_pre_smp_resolve_arm_tsc = 0;
static volatile int      s_pre_smp_preempt_armed = 0;
static zeos_jmpbuf_t     s_pre_smp_preempt_jmpbuf;

/* Helpers: route to per-CPU slot if SMP is up, else the static fallback.
 * smp_cpu_by_lapic returns NULL before smp_init has populated s_cpus[]
 * (or if the calling LAPIC isn't enumerated). */
struct smp_cpu;
extern struct smp_cpu *smp_cpu_by_lapic(uint8_t lapic_id);
extern uint32_t lapic_id(void);

static inline volatile int *cpu_resolving_chain_id_ptr(void)
{
    struct smp_cpu *c = smp_cpu_by_lapic((uint8_t)lapic_id());
    if (!c) return &s_pre_smp_resolving_chain_id;
    /* Field offset hand-computed because smp.h smp_cpu_t isn't in this
     * TU; see layout in smp.h. The accessor below resolves it cleanly. */
    extern volatile int *smp_cpu_preempt_resolving_chain_id_ptr(struct smp_cpu *);
    return smp_cpu_preempt_resolving_chain_id_ptr(c);
}
static inline volatile uint64_t *cpu_resolve_arm_tsc_ptr(void)
{
    struct smp_cpu *c = smp_cpu_by_lapic((uint8_t)lapic_id());
    if (!c) return &s_pre_smp_resolve_arm_tsc;
    extern volatile uint64_t *smp_cpu_preempt_resolve_arm_tsc_ptr(struct smp_cpu *);
    return smp_cpu_preempt_resolve_arm_tsc_ptr(c);
}
static inline volatile int *cpu_preempt_armed_ptr(void)
{
    struct smp_cpu *c = smp_cpu_by_lapic((uint8_t)lapic_id());
    if (!c) return &s_pre_smp_preempt_armed;
    extern volatile int *smp_cpu_preempt_armed_ptr(struct smp_cpu *);
    return smp_cpu_preempt_armed_ptr(c);
}
static inline uint64_t *cpu_preempt_jmpbuf_ptr(void)
{
    struct smp_cpu *c = smp_cpu_by_lapic((uint8_t)lapic_id());
    if (!c) return (uint64_t *)s_pre_smp_preempt_jmpbuf;
    extern uint64_t *smp_cpu_preempt_jmpbuf_ptr(struct smp_cpu *);
    return smp_cpu_preempt_jmpbuf_ptr(c);
}

/* Minimal x86_64 setjmp / longjmp for kernel use. We're freestanding
 * so the libc versions aren't available. Save callee-saved regs + rsp
 * + return rip; restore on longjmp. Ring 0 only. */
/* NOTE: parameters appear unused to the compiler because we read them
 * directly from %rdi/%rsi in the asm body. Suppress with #pragma. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

static __attribute__((returns_twice, naked, noinline))
int zeos_setjmp(zeos_jmpbuf_t buf)
{
    __asm__ volatile(
        "movq %%rbx, 0(%%rdi)\n"
        "movq %%rbp, 8(%%rdi)\n"
        "movq %%r12, 16(%%rdi)\n"
        "movq %%r13, 24(%%rdi)\n"
        "movq %%r14, 32(%%rdi)\n"
        "movq %%r15, 40(%%rdi)\n"
        "leaq 8(%%rsp), %%rax\n"      /* rsp at caller (pre-call) */
        "movq %%rax, 48(%%rdi)\n"
        "movq (%%rsp), %%rax\n"       /* return address */
        "movq %%rax, 56(%%rdi)\n"
        "xorl %%eax, %%eax\n"
        "ret\n"
        ::: "memory");
}

static __attribute__((noreturn, naked, noinline))
void zeos_longjmp(zeos_jmpbuf_t buf, int rc)
{
    __asm__ volatile(
        "movq 0(%%rdi),  %%rbx\n"
        "movq 8(%%rdi),  %%rbp\n"
        "movq 16(%%rdi), %%r12\n"
        "movq 24(%%rdi), %%r13\n"
        "movq 32(%%rdi), %%r14\n"
        "movq 40(%%rdi), %%r15\n"
        "movq 48(%%rdi), %%rsp\n"
        "movq 56(%%rdi), %%rdx\n"      /* saved return rip into rdx */
        "movl %%esi, %%eax\n"          /* rc */
        "testl %%eax, %%eax\n"
        "jnz 1f\n"
        "movl $1, %%eax\n"             /* longjmp(0) -> return 1 */
        "1:\n"
        "jmp *%%rdx\n"
        ::: "memory");
}

#pragma GCC diagnostic pop

/* Per-chain cumulative timing for "top slow chain" reporting. */
static uint64_t s_resolve_count[MAX_CHAINS];
static float    s_resolve_total_ms[MAX_CHAINS];

/* Backoff state per chain (skip every Nth tick). */
static uint8_t  s_skip_phase[MAX_CHAINS];

/* TPS sampling. */
static uint64_t s_sample_tsc;       /* tsc at start of current sample */
static uint64_t s_sample_tick;      /* tick count at start of sample  */
static uint32_t s_tps_last;         /* last completed sample's tps    */
static uint32_t s_tick_avg_us_last; /* last sample's avg tick duration */

void scheduler_quantum_us(uint32_t us)
{
    if (us == 0) us = 1;
    s_quantum_us = us;
}

uint32_t scheduler_quantum_us_get(void) { return s_quantum_us; }
uint64_t scheduler_tick_count(void)     { return s_tick; }
uint32_t scheduler_chains_resolved_last(void) { return s_chains_resolved_last; }
uint32_t scheduler_errors_last(void)    { return s_errors_last; }
uint32_t scheduler_slow_resolves_total(void) { return s_slow_resolves_total; }
uint32_t scheduler_aggregate_slow_total(void) { return s_agg_slow_total; }
uint32_t scheduler_watchdog_kills(void) { return s_watchdog_kills; }
uint32_t scheduler_preempt_kills(void) { return s_preempt_kills; }

void scheduler_init(void)
{
    s_log_head = 0;
    s_log_count = 0;
    s_tick = 0;
    s_quantum_us = 1000;
    s_chains_resolved_last = 0;
    s_errors_last = 0;
    s_slow_resolves_total = 0;
    s_live_chains_last = 0;
    s_agg_slow_total = 0;
    s_watchdog_kills = 0;
    s_preempt_kills = 0;
    s_pre_smp_resolving_chain_id = -1;
    s_pre_smp_resolve_arm_tsc = 0;
    s_pre_smp_preempt_armed = 0;
    for (int i = 0; i < 8; i++) s_pre_smp_preempt_jmpbuf[i] = 0;
    for (int i = 0; i < MAX_CHAINS; i++) {
        s_skip_phase[i] = 0;
        s_resolve_count[i] = 0;
        s_resolve_total_ms[i] = 0.0f;
    }
    s_sample_tsc = timer_read_tsc();
    s_sample_tick = 0;
    s_tps_last = 0;
    s_tick_avg_us_last = 0;
    kputs("[scheduler] chain resolution as scheduling primitive\n");
    kputs("[scheduler] quantum=1000us, B3 backoff enabled (per-chain tunable)\n");
    kputs("[scheduler] preemption=LAPIC timer vec 0xEF + setjmp/longjmp\n");
    kputs("[scheduler] watchdog=post-hoc (defensive secondary path)\n");

    /* Install LAPIC timer ISR for preemption. The handler runs from
     * the standard isr_dispatch path; on a real expiry it longjmps
     * out (never returns), abandoning the interrupt stack frame. */
    extern void scheduler_lapic_timer_isr(uint64_t vec, uint64_t err);
    idt_register((uint8_t)SCHED_PREEMPT_VECTOR, scheduler_lapic_timer_isr);
}

int scheduler_should_skip(int chain_id)
{
    chain_t *c = chain_get(chain_id);
    if (!c) return 0;
    /* B3 backoff: if failure beta exceeds alpha and we have enough
     * observations, skip ticks per the chain's tunables. */
    if (c->b3_observations < 4) return 0;
    if (c->b3_beta <= c->b3_alpha) return 0;

    float threshold = c->backoff_skip_threshold;
    if (threshold <= 0.0f || threshold > 1.0f) threshold = 0.5f;

    uint32_t base_period = c->backoff_skip_every;
    if (base_period < 1) base_period = 4;

    /* Severity: ratio of failures. */
    float ratio = c->b3_beta / (c->b3_alpha + c->b3_beta);
    if (ratio < threshold) return 0;

    /* Scale period with severity around the chain's configured base.
     * At threshold: period = base/2 (more aggressive cadence; min 2).
     * Mid (>0.7):    period = base.
     * High (>0.9):   period = base*2. */
    uint32_t period = base_period;
    if (ratio <= 0.7f) {
        period = base_period / 2;
        if (period < 2) period = 2;
    } else if (ratio > 0.9f) {
        period = base_period * 2;
    }

    s_skip_phase[chain_id]++;
    if ((s_skip_phase[chain_id] % period) != 0)
        return 1;
    return 0;
}

int scheduler_set_backoff(int chain_id, float threshold, uint32_t every)
{
    chain_t *c = chain_get(chain_id);
    if (!c) return -1;
    if (threshold <= 0.0f || threshold > 1.0f) return -1;
    if (every < 1) return -1;
    c->backoff_skip_threshold = threshold;
    c->backoff_skip_every     = every;
    return 0;
}

static sched_log_entry_t *log_alloc(void)
{
    sched_log_entry_t *e = &s_log[s_log_head];
    /* Zero all extra fields so consumers don't see stale data. */
    e->kind = 0;
    e->chains_resolved = 0;
    e->live_chains = 0;
    e->errors = 0;
    e->slow_resolves = 0;
    e->tick = s_tick;
    e->tsc_start = 0;
    e->tsc_end = 0;
    e->over_us = 0;
    e->top_ids[0] = -1; e->top_ids[1] = -1; e->top_ids[2] = -1;
    e->top_ms[0] = 0.0f; e->top_ms[1] = 0.0f; e->top_ms[2] = 0.0f;
    s_log_head = (s_log_head + 1) % SCHED_LOG_RING;
    s_log_count++;
    return e;
}

static void log_tick(uint64_t tsc_start, uint64_t tsc_end,
                     uint32_t resolved, uint32_t live,
                     uint32_t errors, uint32_t slow)
{
    sched_log_entry_t *e = log_alloc();
    e->kind = SLE_TICK;
    e->tsc_start = tsc_start;
    e->tsc_end = tsc_end;
    e->chains_resolved = (uint16_t)resolved;
    e->live_chains = (uint16_t)live;
    e->errors = (uint16_t)errors;
    e->slow_resolves = (uint16_t)slow;
}

static void log_aggregate_slow(uint64_t tsc_start, uint64_t tsc_end,
                               uint32_t over_us,
                               int top_ids[3], float top_ms[3])
{
    sched_log_entry_t *e = log_alloc();
    e->kind = SLE_AGG_SLOW;
    e->tsc_start = tsc_start;
    e->tsc_end = tsc_end;
    e->over_us = over_us;
    for (int i = 0; i < 3; i++) {
        e->top_ids[i] = (int16_t)top_ids[i];
        e->top_ms[i] = top_ms[i];
    }
}

static void log_watchdog_kill(int chain_id, float ran_ms)
{
    sched_log_entry_t *e = log_alloc();
    e->kind = SLE_WDOG_KILL;
    e->top_ids[0] = (int16_t)chain_id;
    e->top_ms[0] = ran_ms;
}

static void log_preempt_kill(int chain_id, float ran_ms)
{
    sched_log_entry_t *e = log_alloc();
    e->kind = SLE_PREEMPT_KILL;
    e->top_ids[0] = (int16_t)chain_id;
    e->top_ms[0] = ran_ms;
}

/* ── LAPIC preempt-timer ISR ──────────────────────────────────────────
 * Called from isr_dispatch when vector 0xEF fires. If a chain_resolve
 * is in flight (g_resolving_chain_id >= 0), the resolve hung past its
 * watchdog budget: we mark the chain CHAIN_ERROR, bump b3_beta, log a
 * preempt_kill, EOI the LAPIC, and longjmp back to the scheduler loop.
 * Otherwise the timer fired after we already disarmed (race) -- EOI
 * and return normally.
 *
 * NOTE: longjmp from inside an ISR is safe only because we're ring 0
 * with no privilege transition; the iretq frame and pushed registers
 * on the interrupted stack just become abandoned scratch space below
 * the restored RSP. The LAPIC EOI must precede the longjmp so the
 * next interrupt on this vector can be accepted.
 */
void scheduler_lapic_timer_isr(uint64_t vec, uint64_t err)
{
    (void)vec; (void)err;

    /* Read THIS core's per-CPU preempt slot. Critical: every read+write
     * here must hit the same core's slot, so call each accessor once
     * up-front (each looks up by lapic_id) — don't stagger them. */
    volatile int      *p_id    = cpu_resolving_chain_id_ptr();
    volatile uint64_t *p_arm   = cpu_resolve_arm_tsc_ptr();
    volatile int      *p_armed = cpu_preempt_armed_ptr();
    uint64_t          *p_jmp   = cpu_preempt_jmpbuf_ptr();

    int id = *p_id;
    if (id < 0) {
        /* Spurious — timer fired after we'd already disarmed and this
         * core isn't currently inside any chain_resolve. EOI and
         * return without longjmping. */
        lapic_timer_disarm();
        lapic_eoi();
        return;
    }

    /* Defensive: clear state BEFORE the longjmp so a re-entrant
     * timer can't double-fire through us. */
    *p_armed = 0;
    *p_id = -1;
    lapic_timer_disarm();

    chain_t *c = chain_get(id);
    uint64_t now_tsc = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    float ran_ms = 0.0f;
    if (freq > 0 && *p_arm != 0) {
        ran_ms = (float)(now_tsc - *p_arm) * 1000.0f / (float)freq;
    }

    if (c) {
        c->status = CHAIN_ERROR;
        c->b3_beta += 1.0f;
        c->b3_observations++;
        c->watchdog_deadline_tsc = 0;
    }

    /* Critical: release the per-chain SMP try-lock that chain_resolve
     * acquired before we abandon its stack. Without this, the chain
     * stays "in flight" forever and every future try-lock skips it. */
    chain_resolve_force_unlock(id);

    s_preempt_kills++;
    log_preempt_kill(id, ran_ms);

    kputs("[sched] preempted chain ");
    kput_dec((uint64_t)id);
    kputs(" \"");
    kputs(c ? c->name : "?");
    kputs("\" after ");
    kput_dec((uint64_t)(ran_ms * 1000.0f));   /* us */
    kputs("us\n");

    lapic_eoi();

    /* Bypass the rest of isr_dispatch + the assembly stub epilogue;
     * resume directly at THIS core's setjmp checkpoint. The interrupt
     * stack frame becomes unreachable but harmless. */
    zeos_longjmp(p_jmp, 1);
}

/* Wrap a single chain_resolve(id) with LAPIC-timer-driven preemption.
 * Returns 0 on normal resolve completion, -1 if the resolve was
 * preempted (chain has already been marked CHAIN_ERROR by the ISR). */
int scheduler_preempt_resolve(int id)
{
    chain_t *c = chain_get(id);
    if (!c) return chain_resolve(id);

    /* Compute timer arm count in LAPIC ticks. If LAPIC isn't ready
     * (calibration failed), fall through to unprotected resolve --
     * post-hoc watchdog still catches hangs at next tick boundary. */
    uint32_t to_us = c->watchdog_timeout_us;
    if (to_us == 0) to_us = 100000;
    uint32_t lt_per_us = lapic_ticks_per_us();
    if (!lapic_ready() || lt_per_us == 0) {
        return chain_resolve(id);
    }

    /* Cap the arm count so we don't overflow the 32-bit LAPIC init
     * count register on absurdly large timeouts. */
    uint64_t ticks64 = (uint64_t)to_us * (uint64_t)lt_per_us;
    if (ticks64 > 0xFFFFFFFFULL) ticks64 = 0xFFFFFFFFULL;
    if (ticks64 < 1) ticks64 = 1;

    /* Pull the per-CPU slot pointers ONCE for this resolve. The ISR will
     * look these up again on its core. As long as we stay on the same
     * core through the resolve+disarm (we do; no preempt enabled in
     * scheduler_run), the pointers we cache here match what the ISR
     * sees if the timer fires on this core. */
    volatile int      *p_id    = cpu_resolving_chain_id_ptr();
    volatile uint64_t *p_arm   = cpu_resolve_arm_tsc_ptr();
    volatile int      *p_armed = cpu_preempt_armed_ptr();
    uint64_t          *p_jmp   = cpu_preempt_jmpbuf_ptr();

    /* setjmp returns 0 on initial save, nonzero on longjmp from ISR. */
    int rc = zeos_setjmp(p_jmp);
    if (rc != 0) {
        /* Came back via the ISR — chain has been killed.
         * State was cleared before the longjmp; just report. */
        return -1;
    }

    *p_arm = timer_read_tsc();
    *p_id = id;
    /* Set the armed flag last, just before the LAPIC write that
     * actually starts the countdown. */
    *p_armed = 1;
    lapic_timer_oneshot((uint32_t)ticks64, (uint8_t)SCHED_PREEMPT_VECTOR);

    int err = chain_resolve(id);

    /* Successful return — clear the armed flag FIRST so any pending
     * stale interrupt the LAPIC has queued is treated as spurious by
     * the ISR, then disarm the LVT. */
    *p_armed = 0;
    lapic_timer_disarm();
    *p_id = -1;
    *p_arm = 0;
    return err;
}

/* Walk the chain registry: count LIVE chains, count chains whose last
 * resolve exceeded the quantum (slow), and collect top-3 slowest by
 * last_resolve_ms. Done after mde_resolve_all so timings are fresh. */
static void post_tick_scan(uint32_t *live_out, uint32_t *slow_out,
                           int top_ids[3], float top_ms[3])
{
    uint32_t live = 0, slow = 0;
    float quantum_ms = (float)s_quantum_us / 1000.0f;

    top_ids[0] = top_ids[1] = top_ids[2] = -1;
    top_ms[0] = top_ms[1] = top_ms[2] = 0.0f;

    for (int id = 0; id < MAX_CHAINS; id++) {
        chain_t *c = chain_get(id);
        if (!c) continue;
        if (c->status == CHAIN_LIVE) live++;
        if (c->last_resolve_ms > quantum_ms) slow++;

        /* Track cumulative for "top slow chain" reporting. */
        if (c->last_resolve_ms > 0.0f) {
            s_resolve_count[id]++;
            s_resolve_total_ms[id] += c->last_resolve_ms;
        }

        /* Insertion sort into top-3 by last_resolve_ms. */
        float ms = c->last_resolve_ms;
        if (ms > top_ms[0]) {
            top_ms[2] = top_ms[1]; top_ids[2] = top_ids[1];
            top_ms[1] = top_ms[0]; top_ids[1] = top_ids[0];
            top_ms[0] = ms;        top_ids[0] = id;
        } else if (ms > top_ms[1]) {
            top_ms[2] = top_ms[1]; top_ids[2] = top_ids[1];
            top_ms[1] = ms;        top_ids[1] = id;
        } else if (ms > top_ms[2]) {
            top_ms[2] = ms;        top_ids[2] = id;
        }
    }
    *live_out = live;
    *slow_out = slow;
}

int scheduler_top_slow_chain(int *id_out, float *avg_ms_out)
{
    int   best_id = -1;
    float best_total = 0.0f;
    for (int id = 0; id < MAX_CHAINS; id++) {
        if (s_resolve_count[id] == 0) continue;
        if (s_resolve_total_ms[id] > best_total) {
            best_total = s_resolve_total_ms[id];
            best_id = id;
        }
    }
    if (best_id < 0) {
        if (id_out) *id_out = -1;
        if (avg_ms_out) *avg_ms_out = 0.0f;
        return 0;
    }
    if (id_out) *id_out = best_id;
    if (avg_ms_out) {
        *avg_ms_out = s_resolve_total_ms[best_id]
                    / (float)s_resolve_count[best_id];
    }
    return 1;
}

void scheduler_idle(void)
{
    /* HLT until next interrupt. The PIT (1kHz) plus any device IRQ
     * (kbd, mouse, NIC, NVMe, etc.) wakes us promptly. */
    __asm__ volatile("hlt");
}

uint32_t scheduler_tps(void)
{
    return s_tps_last;
}

uint32_t scheduler_tick_avg_us(void)
{
    return s_tick_avg_us_last;
}

static void update_tps_sample(uint64_t now_tsc)
{
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) return;
    /* 100 ms window */
    uint64_t window = freq / 10ULL;
    if (now_tsc - s_sample_tsc >= window) {
        uint64_t dt = now_tsc - s_sample_tsc;
        uint64_t dticks = s_tick - s_sample_tick;
        if (dt > 0) {
            /* tps = dticks * freq / dt */
            uint64_t tps = (dticks * freq) / dt;
            s_tps_last = (uint32_t)tps;
            /* avg tick duration in us = (dt / dticks) / (freq/1e6) */
            if (dticks > 0) {
                uint64_t avg_us = (dt * 1000000ULL) / (dticks * freq);
                s_tick_avg_us_last = (uint32_t)avg_us;
            }
        }
        s_sample_tsc = now_tsc;
        s_sample_tick = s_tick;
    }
}

/* Pre-tick watchdog sweep: any chain whose deadline expired before we
 * arrived is presumed hung from the previous tick. Mark CHAIN_ERROR,
 * bump b3_beta, log a kill record. Clears the deadline either way. */
static void watchdog_sweep(uint64_t now_tsc)
{
    uint64_t freq = timer_tsc_freq();
    for (int id = 0; id < MAX_CHAINS; id++) {
        chain_t *c = chain_get(id);
        if (!c) continue;
        if (c->watchdog_deadline_tsc == 0) continue;
        if (now_tsc > c->watchdog_deadline_tsc) {
            /* Hung last tick. */
            uint64_t over_cycles = now_tsc - c->watchdog_deadline_tsc;
            float ran_ms = 0.0f;
            if (freq > 0) {
                /* Total run = configured timeout + how much we overshot. */
                ran_ms = (float)c->watchdog_timeout_us / 1000.0f
                       + (float)over_cycles * 1000.0f / (float)freq;
            }
            c->status = CHAIN_ERROR;
            c->b3_beta += 1.0f;
            c->b3_observations++;
            s_watchdog_kills++;
            log_watchdog_kill(id, ran_ms);
            kputs("[scheduler] watchdog: chain id=");
            kput_dec((uint64_t)id);
            kputs(" hung -> CHAIN_ERROR\n");
        }
        c->watchdog_deadline_tsc = 0;
    }
}

/* Pre-resolve: arm watchdog deadlines for every LIVE chain so that a
 * chain that wedges its resolve gets caught at the next tick boundary.
 * Cheap: one TSC math per LIVE chain. */
static void watchdog_arm_all(uint64_t now_tsc, uint64_t cycles_per_us)
{
    for (int id = 0; id < MAX_CHAINS; id++) {
        chain_t *c = chain_get(id);
        if (!c) continue;
        if (c->status != CHAIN_LIVE) {
            c->watchdog_deadline_tsc = 0;
            continue;
        }
        uint32_t to_us = c->watchdog_timeout_us;
        if (to_us == 0) to_us = 100000;  /* repair zero defaults */
        c->watchdog_deadline_tsc = now_tsc
                                 + (uint64_t)to_us * cycles_per_us;
    }
}

/* Post-resolve: clear deadlines on chains that completed normally. Any
 * deadline that survives until the next tick's watchdog_sweep counts as
 * a hang. */
static void watchdog_clear_completed(void)
{
    for (int id = 0; id < MAX_CHAINS; id++) {
        chain_t *c = chain_get(id);
        if (!c) continue;
        c->watchdog_deadline_tsc = 0;
    }
}

void scheduler_run(void)
{
    /* Banner reaffirms the paradigm switch. */
    kputs("[scheduler] entering chain-resolution main loop\n");

    for (;;) {
        s_tick++;
        uint64_t tsc_start = timer_read_tsc();

        /* 0. Watchdog sweep first: any chain whose deadline was armed
         *    last tick and never cleared = hung. Mark + log. */
        watchdog_sweep(tsc_start);

        /* Live desktop (outside the armed watchdog window): pump HID input and
         * advance anim/cursor/overlay state BEFORE the resolve so springs are
         * fresh when wm_draw_all reads window geometry this same tick. */
        {
            extern void usb_hid_poll(void);
            extern void compositor_advance(void);
            extern void net_service(void);
            usb_hid_poll();
            net_service();          /* pump RX + async DHCP under the scheduler */
            compositor_advance();
        }

        /* 1. Arm watchdog deadlines for the upcoming resolve pass. */
        uint64_t freq = timer_tsc_freq();
        uint64_t cycles_per_us = (freq > 0) ? (freq / 1000000ULL) : 0;
        if (cycles_per_us == 0) cycles_per_us = 1;  /* defensive */
        watchdog_arm_all(tsc_start, cycles_per_us);

        /* 2. Resolve the entire chain graph in dependency order. */
        int errors = chain_registry_tick();

        /* 3. We made it back. Clear armed deadlines so survivors aren't
         *    falsely killed next tick. */
        watchdog_clear_completed();

        /* Live desktop: draw gated overlays + the cursor (every tick) on top of
         * the freshly-resolved frame. */
        { extern void compositor_present(void); compositor_present(); }

        uint64_t tsc_end = timer_read_tsc();

        /* 4. Post-tick stats + top-3 slow chains. */
        uint32_t live = 0, slow = 0;
        int   top_ids[3];
        float top_ms[3];
        post_tick_scan(&live, &slow, top_ids, top_ms);
        s_live_chains_last = live;
        s_chains_resolved_last = (uint32_t)live;  /* resolve_all walks all live */
        s_errors_last = (uint32_t)errors;
        if (slow > 0) {
            s_slow_resolves_total += slow;
            if ((s_tick & 0xFFF) == 0) {
                kputs("[scheduler] slow resolve(s) this tick: ");
                kput_dec((uint64_t)slow);
                kputc('\n');
            }
        }

        /* 5. Aggregate slow-tick detection: did the WHOLE tick exceed
         *    the quantum? This is the line that makes slow ticks
         *    actually findable -- per-chain slow_total is meaningless
         *    against 12-31 ms ticks. */
        uint64_t dt_cycles = tsc_end - tsc_start;
        uint64_t tick_us = (cycles_per_us > 0)
                         ? (dt_cycles / cycles_per_us)
                         : 0;
        if (tick_us > (uint64_t)s_quantum_us) {
            uint32_t over_us = (uint32_t)(tick_us - (uint64_t)s_quantum_us);
            s_agg_slow_total++;
            log_aggregate_slow(tsc_start, tsc_end, over_us, top_ids, top_ms);

            /* Throttle the console line so a long stall doesn't drown
             * the boot output, but always print the first few. */
            if (s_agg_slow_total <= 4 || (s_agg_slow_total & 0x3F) == 0) {
                kputs("[scheduler] tick ");
                kput_dec(s_tick);
                kputs(" exceeded budget by ");
                /* Print as ms for human readability. */
                kput_dec((uint64_t)(over_us / 1000));
                kputs("ms; top:");
                for (int i = 0; i < 3; i++) {
                    if (top_ids[i] < 0) break;
                    kputs(" id=");
                    kput_dec((uint64_t)top_ids[i]);
                    kputs("(");
                    kput_dec((uint64_t)top_ms[i]);
                    kputs("ms)");
                }
                kputc('\n');
            }
        }

        log_tick(tsc_start, tsc_end, s_chains_resolved_last,
                 live, (uint32_t)errors, slow);
        update_tps_sample(tsc_end);

        /* 6. Drain the keyboard ASCII buffer into the shell pump.
         * CHAIN_KEYBOARD has already converted scancodes -> kb_buf
         * during chain_registry_tick(). The shell is just another
         * consumer of input_event signals. */
        char c;
        int dispatched = 0;
        /* Both PS/2 (CHAIN_KEYBOARD) and UART (CHAIN_SERIAL_IN) feed
         * kb_buf with ASCII characters during chain_registry_tick().
         * The shell is just another consumer of input_event signals;
         * we drain the merged ring once per tick. */
        while (keyboard_try_getc(&c)) {
            /* Defensive gate: keyboard.c already diverts ASCII to
             * lockscreen_input() while the overlay is active, so kb_buf
             * normally doesn't accumulate during a lock. But if input
             * landed in kb_buf in the same tick the IDLE chain
             * transitioned to LOCKED, route those leftover bytes to the
             * lock screen instead of the shell. */
            if (lockscreen_active()) {
                lockscreen_input(c);
            } else {
                (void)shell_pump_char(c);
            }
            dispatched++;
            if (dispatched >= 128) break;  /* fairness */
        }

        /* 7. If nothing happened this tick, idle until an IRQ wakes us. Do NOT
         *    HLT while a spring is animating -- it would freeze between IRQs. */
        extern int anim_active_count(void);
        if (dispatched == 0 && live > 0 && errors == 0 && slow == 0 &&
            anim_active_count() == 0) {
            /* Quick check: is anything pending we should service first? */
            if (keyboard_chain_pending() == 0 &&
                mouse_chain_pending()    == 0) {
                scheduler_idle();
            }
        }
    }
}

void scheduler_log_dump(int last_n)
{
    if (last_n <= 0 || last_n > SCHED_LOG_RING) last_n = 16;
    uint64_t total = s_log_count;
    if ((uint64_t)last_n > total) last_n = (int)total;

    kputs("\n  Scheduler MasQ log (last ");
    kput_dec((uint64_t)last_n);
    kputs(" of ");
    kput_dec(total);
    kputs(" records)\n");
    kputs("  ────────────────────────────────────────────\n");
    kputs("  agg_slow_total=");
    kput_dec((uint64_t)s_agg_slow_total);
    kputs(" watchdog_kills=");
    kput_dec((uint64_t)s_watchdog_kills);
    kputs(" preempt_kills=");
    kput_dec((uint64_t)s_preempt_kills);
    kputc('\n');

    /* Walk back from head. */
    uint32_t idx = s_log_head;
    /* Move to oldest of the requested range. */
    for (int i = 0; i < last_n; i++) {
        idx = (idx == 0) ? (SCHED_LOG_RING - 1) : (idx - 1);
    }

    uint64_t freq = timer_tsc_freq();
    for (int i = 0; i < last_n; i++) {
        sched_log_entry_t *e = &s_log[idx];
        switch (e->kind) {
        case SLE_TICK: {
            uint64_t dt = e->tsc_end - e->tsc_start;
            uint64_t us = (freq > 0) ? (dt * 1000000ULL / freq) : 0;
            kputs("  tick=");
            kput_dec(e->tick);
            kputs(" live=");
            kput_dec((uint64_t)e->live_chains);
            kputs(" resolved=");
            kput_dec((uint64_t)e->chains_resolved);
            kputs(" err=");
            kput_dec((uint64_t)e->errors);
            kputs(" slow=");
            kput_dec((uint64_t)e->slow_resolves);
            kputs(" dur=");
            kput_dec(us);
            kputs("us\n");
            break;
        }
        case SLE_AGG_SLOW: {
            kputs("  AGG_SLOW tick=");
            kput_dec(e->tick);
            kputs(" over=");
            kput_dec((uint64_t)(e->over_us / 1000));
            kputs("ms top:");
            for (int j = 0; j < 3; j++) {
                if (e->top_ids[j] < 0) break;
                kputs(" id=");
                kput_dec((uint64_t)e->top_ids[j]);
                kputs("(");
                kput_dec((uint64_t)e->top_ms[j]);
                kputs("ms)");
            }
            kputc('\n');
            break;
        }
        case SLE_WDOG_KILL: {
            kputs("  WDOG_KILL tick=");
            kput_dec(e->tick);
            kputs(" id=");
            kput_dec((uint64_t)e->top_ids[0]);
            kputs(" ran=");
            kput_dec((uint64_t)e->top_ms[0]);
            kputs("ms\n");
            break;
        }
        case SLE_PREEMPT_KILL: {
            kputs("  PREEMPT_KILL tick=");
            kput_dec(e->tick);
            kputs(" id=");
            kput_dec((uint64_t)e->top_ids[0]);
            kputs(" ran=");
            kput_dec((uint64_t)e->top_ms[0]);
            kputs("ms\n");
            break;
        }
        default:
            break;
        }
        idx = (idx + 1) % SCHED_LOG_RING;
    }
    kputs("\n");
}
