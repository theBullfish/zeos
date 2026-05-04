/*
 * Zeos -- Chain Primitive (implementation)
 *
 * Static registry, no malloc, bare-metal.
 */

#include "chain.h"
#include "cfa_handle.h"
#include "kprint.h"
#include "timer.h"
#include "persistence.h"
#include "spinlock.h"

/* ── Static registry ─────────────────────────────────────────────── */

static chain_t  registry[MAX_CHAINS];
static int      registry_used[MAX_CHAINS];  /* 0 = free, 1 = occupied */
static int      chain_total;                /* live count */
static int      next_child_index[MAX_CHAINS]; /* per-chain child counter for CFA */

/* ── SMP locking ─────────────────────────────────────────────────────
 * Two layers:
 *   chain_locks[id]      — per-chain mutex. chain_resolve() try-locks
 *                          to serialize same-chain resolves across
 *                          CPUs. Try-lock not blocking-lock so a
 *                          stuck-resolving chain on one CPU doesn't
 *                          stall another CPU's tick.
 *   g_chain_registry_rw_lock — owned by smp.c; held only across
 *                              chain_create / chain_destroy slot
 *                              mutation. Per-chain reads do NOT need
 *                              this lock (slots are stable once
 *                              registered, and registry_used is
 *                              read with relaxed semantics).
 */
extern zeos_spinlock_t g_chain_registry_rw_lock;
static zeos_spinlock_t chain_locks[MAX_CHAINS];

/* ── Helpers ─────────────────────────────────────────────────────── */

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int str_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ── CFA ─────────────────────────────────────────────────────────── */

cfa_addr_t cfa_derive(const cfa_addr_t *parent, uint32_t child_index)
{
    cfa_addr_t child;
    int i;

    for (i = 0; i < 8; i++)
        child.segments[i] = parent->segments[i];

    if (parent->depth < 8)
        child.segments[parent->depth] = child_index;

    child.depth = parent->depth + 1;
    if (child.depth > 8) child.depth = 8;

    child.birth_tsc = timer_read_tsc();
    return child;
}

static cfa_addr_t cfa_root(uint32_t root_index)
{
    cfa_addr_t addr;
    int i;
    for (i = 0; i < 8; i++)
        addr.segments[i] = 0;
    addr.segments[0] = root_index;
    addr.depth = 1;
    addr.birth_tsc = timer_read_tsc();
    return addr;
}

/* ── Init ────────────────────────────────────────────────────────── */

void chain_init(void)
{
    int i, j;

    for (i = 0; i < MAX_CHAINS; i++) {
        registry_used[i] = 0;
        next_child_index[i] = 0;
        chain_locks[i].v = 0;
        registry[i].id = -1;
        registry[i].name[0] = '\0';
        registry[i].node_count = 0;
        registry[i].status = CHAIN_DETACHED;
        registry[i].tier = MASQ_REFERENCE;
        registry[i].b3_alpha = 1.0f;
        registry[i].b3_beta = 1.0f;
        registry[i].b3_observations = 0;
        registry[i].created_tsc = 0;
        registry[i].last_resolve_tsc = 0;
        registry[i].last_resolve_ms = 0.0f;
        registry[i].parent_id = -1;
        registry[i].vault_version = 0;
        registry[i].watchdog_deadline_tsc = 0;
        registry[i].watchdog_timeout_us   = 500000;  /* 500ms default */
        registry[i].backoff_skip_threshold = 0.5f;
        registry[i].backoff_skip_every     = 4;
        registry[i].resolve_interval_ticks = 0;
        registry[i].last_resolved_tick     = 0;
        registry[i].addr.depth = 0;
        registry[i].addr.birth_tsc = 0;
        for (j = 0; j < 8; j++)
            registry[i].addr.segments[j] = 0;
    }

    chain_total = 0;
    kputs("[chain] registry initialized (");
    kput_dec(MAX_CHAINS);
    kputs(" slots)\n");
}

/* ── Create ──────────────────────────────────────────────────────── */

int chain_create(const char *name, int parent_id, masq_tier_t tier)
{
    int slot = -1;
    int i;
    uint32_t child_idx;

    spin_lock(&g_chain_registry_rw_lock);

    /* Find free slot */
    for (i = 0; i < MAX_CHAINS; i++) {
        if (!registry_used[i]) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spin_unlock(&g_chain_registry_rw_lock);
        kputs("[chain] ERROR: registry full\n");
        return -1;
    }

    /* Derive CFA address */
    if (parent_id >= 0 && parent_id < MAX_CHAINS && registry_used[parent_id]) {
        child_idx = (uint32_t)next_child_index[parent_id];
        next_child_index[parent_id]++;
        registry[slot].addr = cfa_derive(&registry[parent_id].addr, child_idx);
    } else {
        /* Root chain -- use slot as root index */
        registry[slot].addr = cfa_root((uint32_t)slot);
        parent_id = -1;
    }

    registry[slot].id = slot;
    str_copy(registry[slot].name, name, 64);
    registry[slot].tier = tier;
    registry[slot].status = CHAIN_LIVE;
    registry[slot].node_count = 0;
    registry[slot].b3_alpha = 1.0f;
    registry[slot].b3_beta = 1.0f;
    registry[slot].b3_observations = 0;
    registry[slot].created_tsc = timer_read_tsc();
    registry[slot].last_resolve_tsc = 0;
    registry[slot].last_resolve_ms = 0.0f;
    registry[slot].parent_id = parent_id;
    registry[slot].vault_version = 1;
    registry[slot].watchdog_deadline_tsc = 0;
    registry[slot].watchdog_timeout_us   = 500000;  /* 500ms default; preempt
                                                       only fires for clearly
                                                       hung chains. */
    registry[slot].backoff_skip_threshold = 0.5f;
    registry[slot].backoff_skip_every     = 4;
    registry[slot].resolve_interval_ticks = 0;
    registry[slot].last_resolved_tick     = 0;

    registry_used[slot] = 1;
    chain_total++;

    spin_unlock(&g_chain_registry_rw_lock);

    kputs("[chain] created \"");
    kputs(registry[slot].name);
    kputs("\" id=");
    kput_dec((uint64_t)slot);
    kputs(" depth=");
    kput_dec((uint64_t)registry[slot].addr.depth);
    kputc('\n');

    return slot;
}

/* ── Destroy ─────────────────────────────────────────────────────── */

void chain_destroy(int id)
{
    if (id < 0 || id >= MAX_CHAINS) return;

    spin_lock(&g_chain_registry_rw_lock);
    if (!registry_used[id]) {
        spin_unlock(&g_chain_registry_rw_lock);
        return;
    }

    kputs("[chain] destroyed \"");
    kputs(registry[id].name);
    kputs("\" id=");
    kput_dec((uint64_t)id);
    kputc('\n');

    registry_used[id] = 0;
    registry[id].id = -1;
    registry[id].name[0] = '\0';
    registry[id].status = CHAIN_DETACHED;
    registry[id].node_count = 0;
    chain_total--;
    spin_unlock(&g_chain_registry_rw_lock);
}

/* ── Add Node ────────────────────────────────────────────────────── */

int chain_add_node(int chain_id, const char *name,
                   const char *input_type, const char *output_type,
                   void (*resolve_fn)(chain_node_t *self, void *input, void *output))
{
    chain_t *c;
    int idx;

    if (chain_id < 0 || chain_id >= MAX_CHAINS || !registry_used[chain_id])
        return -1;

    c = &registry[chain_id];
    if (c->node_count >= MAX_CHAIN_NODES)
        return -1;

    idx = c->node_count;
    str_copy(c->nodes[idx].name, name, 32);
    str_copy(c->nodes[idx].input_type, input_type, 32);
    str_copy(c->nodes[idx].output_type, output_type, 32);
    c->nodes[idx].resolve = resolve_fn;
    c->nodes[idx].state = (void *)0;
    c->node_count++;

    return idx;
}

/* ── Resolve ─────────────────────────────────────────────────────── */

/* Per-CPU scratch buffers. Each CPU resolves at most one chain at a
 * time on its own callstack, so per-CPU pair is sufficient. Indexed by
 * cpu index from smp_cpu_by_lapic(); BSP-only systems collapse to [0].
 *
 * SMP_MAX_CPUS_FOR_SCRATCH must match smp.h's SMP_MAX_CPUS but we don't
 * include smp.h here to keep chain.c standalone in single-core builds.
 * The constant is small enough that overprovisioning costs nothing. */
#define CHAIN_SCRATCH_CPUS 64
static uint8_t s_scratch_a[CHAIN_SCRATCH_CPUS][4096];
static uint8_t s_scratch_b[CHAIN_SCRATCH_CPUS][4096];

/* Lookup current cpu index without pulling smp.h. Returns 0 (BSP) when
 * SMP didn't bring up APs or when called pre-SMP-init. */
extern uint32_t lapic_id(void);
extern int      smp_cpu_count(void);
extern struct smp_cpu *smp_cpu_by_lapic(uint8_t lapic_id);
static int chain_this_cpu_idx(void)
{
    /* If SMP isn't up yet, single-CPU. Touching lapic_id pre-LAPIC-init
     * crashes; chain_resolve isn't called pre-init so this is safe in
     * the live path. */
    extern int smp_cpus_online(void);
    if (smp_cpus_online() <= 1) return 0;
    uint32_t my_lapic = lapic_id();
    /* Walk the SMP table by index. Avoids dragging smp_cpu_t layout
     * into chain.c. */
    for (int i = 0; i < CHAIN_SCRATCH_CPUS; i++) {
        extern struct smp_cpu *smp_cpu_by_index(int idx);
        struct smp_cpu *c = smp_cpu_by_index(i);
        if (!c) break;
        /* smp_cpu layout: first byte is lapic_id (see smp.h). */
        if (*((uint8_t *)c) == (uint8_t)my_lapic) return i;
    }
    return 0;
}

/* Release the per-chain lock for `id`. The scheduler's preempt ISR
 * calls this immediately before longjmping out of a hung resolve, so
 * the lock doesn't leak. Idempotent on already-unlocked. */
void chain_resolve_force_unlock(int id)
{
    if (id < 0 || id >= MAX_CHAINS) return;
    spin_unlock(&chain_locks[id]);
}

int chain_resolve(int id)
{
    chain_t *c;
    uint64_t t_start, t_end, freq;
    int i;
    void *input;
    void *output;

    if (id < 0 || id >= MAX_CHAINS || !registry_used[id])
        return -1;

    c = &registry[id];

    /* Per-chain try-lock: if another CPU is already resolving this
     * chain, skip this attempt. Returns -2 so the caller (BSP
     * scheduler / AP loop) can distinguish "contended" from "errored"
     * and not bump b3_beta on a benign skip. */
    if (!spin_trylock(&chain_locks[id])) {
        return -2;
    }

    if (c->status != CHAIN_LIVE) {
        spin_unlock(&chain_locks[id]);
        /* Demoted from console-noise to silence: APs would otherwise
         * spam this every tick for non-LIVE chains they own. */
        return -1;
    }

    if (c->node_count == 0) {
        spin_unlock(&chain_locks[id]);
        return 0;
    }

    t_start = timer_read_tsc();

    int cpu = chain_this_cpu_idx();
    if (cpu < 0 || cpu >= CHAIN_SCRATCH_CPUS) cpu = 0;
    uint8_t *scratch_a = s_scratch_a[cpu];
    uint8_t *scratch_b = s_scratch_b[cpu];

    /* Run nodes in sequence, alternating scratch buffers. Set the CFA
     * observer to this chain id so any cfa_resolve() inside a node body
     * gets MasQ-tier-checked against this chain's tier. cfa_observer is
     * scoped to the calling thread but cfa_set_observer is a static
     * variable today; on SMP we accept the observer-tracking races
     * (MasQ tier checks degrade to permissive, never to a security
     * regression — wrong observer logs a false-positive perceive and
     * permits, doesn't deny). Tightening cfa observer to per-CPU is
     * future work. */
    int prev_observer = cfa_get_observer();
    cfa_set_observer(id);

    input = (void *)scratch_a;
    for (i = 0; i < c->node_count; i++) {
        output = (i % 2 == 0) ? (void *)scratch_b : (void *)scratch_a;
        if (c->nodes[i].resolve)
            c->nodes[i].resolve(&c->nodes[i], input, output);
        input = output;
    }

    cfa_set_observer(prev_observer);

    t_end = timer_read_tsc();

    /* Update timing */
    c->last_resolve_tsc = t_end;
    freq = timer_tsc_freq();
    if (freq > 0) {
        c->last_resolve_ms = (float)(t_end - t_start) * 1000.0f / (float)freq;
    }

    /*
     * B3 belief update is now handled by MDE's routing layer
     * (mde_resolve_chain / mde_resolve_all), which observes both
     * success AND failure outcomes. Don't double-count here.
     */

    /* Persistence checkpoint hook: increments resolve counter and
     * snapshots the registry to VAULT every CHECKPOINT_EVERY resolves.
     * Cheap until the threshold trips, then a single vault_write +
     * vault_sync. */
    persistence_on_resolve_complete();

    spin_unlock(&chain_locks[id]);
    return 0;
}

/* ── Get ─────────────────────────────────────────────────────────── */

chain_t *chain_get(int id)
{
    if (id < 0 || id >= MAX_CHAINS || !registry_used[id])
        return (chain_t *)0;
    return &registry[id];
}

/* ── Find by Type ────────────────────────────────────────────────── */

int chain_find_by_type(const char *output_type)
{
    int i, j;
    chain_t *c;

    for (i = 0; i < MAX_CHAINS; i++) {
        if (!registry_used[i]) continue;
        c = &registry[i];
        for (j = 0; j < c->node_count; j++) {
            if (str_equal(c->nodes[j].output_type, output_type))
                return i;
        }
    }
    return -1;
}

/* ── MasQ Perception ─────────────────────────────────────────────── */

int chain_can_perceive(int observer_id, int target_id)
{
    chain_t *observer, *target;

    observer = chain_get(observer_id);
    target = chain_get(target_id);

    if (!observer || !target)
        return 0;

    /*
     * Perception rules:
     *   Sovereign  -> Sovereign + Internal + Reference
     *   Internal   -> Internal + Reference
     *   Reference  -> Reference only
     */
    switch (observer->tier) {
    case MASQ_SOVEREIGN:
        return 1;  /* Sovereign sees everything */
    case MASQ_INTERNAL:
        return (target->tier == MASQ_INTERNAL || target->tier == MASQ_REFERENCE);
    case MASQ_REFERENCE:
        return (target->tier == MASQ_REFERENCE);
    }

    return 0;
}

/* ── Count ───────────────────────────────────────────────────────── */

int chain_count(void)
{
    return chain_total;
}

/* ── Dump ────────────────────────────────────────────────────────── */

void chain_dump(int id)
{
    chain_t *c;
    int i;

    c = chain_get(id);
    if (!c) {
        kputs("[chain] dump: invalid id ");
        kput_dec((uint64_t)id);
        kputc('\n');
        return;
    }

    kputs("── chain ");
    kput_dec((uint64_t)c->id);
    kputs(": \"");
    kputs(c->name);
    kputs("\" ──\n");

    /* CFA address */
    kputs("  cfa: ");
    for (i = 0; i < (int)c->addr.depth; i++) {
        if (i > 0) kputc('.');
        kput_dec((uint64_t)c->addr.segments[i]);
    }
    kputs("  depth=");
    kput_dec((uint64_t)c->addr.depth);
    kputc('\n');

    /* MasQ */
    kputs("  masq: ");
    switch (c->tier) {
    case MASQ_SOVEREIGN:  kputs("SOVEREIGN");  break;
    case MASQ_INTERNAL:   kputs("INTERNAL");   break;
    case MASQ_REFERENCE:  kputs("REFERENCE");  break;
    }
    kputc('\n');

    /* Status */
    kputs("  status: ");
    switch (c->status) {
    case CHAIN_LIVE:      kputs("LIVE");      break;
    case CHAIN_PAUSED:    kputs("PAUSED");    break;
    case CHAIN_ERROR:     kputs("ERROR");     break;
    case CHAIN_DETACHED:  kputs("DETACHED");  break;
    }
    kputc('\n');

    /* Tree */
    kputs("  parent: ");
    if (c->parent_id >= 0)
        kput_dec((uint64_t)c->parent_id);
    else
        kputs("(root)");
    kputc('\n');

    /* Nodes */
    kputs("  nodes: ");
    kput_dec((uint64_t)c->node_count);
    kputc('\n');
    for (i = 0; i < c->node_count; i++) {
        kputs("    [");
        kput_dec((uint64_t)i);
        kputs("] ");
        kputs(c->nodes[i].name);
        kputs("  ");
        kputs(c->nodes[i].input_type);
        kputs(" -> ");
        kputs(c->nodes[i].output_type);
        kputc('\n');
    }

    /* B3 */
    kputs("  b3: alpha=");
    kput_dec((uint64_t)c->b3_alpha);
    kputs(" beta=");
    kput_dec((uint64_t)c->b3_beta);
    kputs(" n=");
    kput_dec((uint64_t)c->b3_observations);
    kputc('\n');

    /* Timing */
    kputs("  resolve_ms=");
    kput_dec((uint64_t)c->last_resolve_ms);
    kputs("  vault_v=");
    kput_dec((uint64_t)c->vault_version);
    kputc('\n');
}
