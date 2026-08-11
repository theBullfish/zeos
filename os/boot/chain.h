/*
 * Zeos -- Chain Primitive
 *
 * The ONE primitive of Z-OS. Everything is a chain.
 * A chain unifies four aspects: CFA addressing, MasQ perception,
 * MDE processing, and B3 belief -- into a single composable unit.
 */

#ifndef ZEOS_CHAIN_H
#define ZEOS_CHAIN_H

#include <stdint.h>

/* ── CFA Address ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t segments[8];   /* Fractal address segments (parent.child.child...) */
    uint8_t  depth;         /* How deep in the graph */
    uint64_t birth_tsc;     /* When this chain was created (temporal component) */
} cfa_addr_t;

/* ── MasQ Tier ───────────────────────────────────────────────────── */

typedef enum {
    MASQ_SOVEREIGN,         /* Private, sacred -- only sovereign peers can perceive */
    MASQ_INTERNAL,          /* Your data -- internal and sovereign can perceive */
    MASQ_REFERENCE,         /* Public, shared -- everyone can perceive */
} masq_tier_t;

/* ── Chain Status ────────────────────────────────────────────────── */

typedef enum {
    CHAIN_LIVE,             /* Actively resolving */
    CHAIN_PAUSED,           /* Suspended, state preserved */
    CHAIN_ERROR,            /* Resolution failed */
    CHAIN_DETACHED,         /* Not rendering output, still exists */
} chain_status_t;

/* ── Chain Node (a processing step within a chain) ───────────────── */

#define MAX_CHAIN_NODES 16

typedef struct chain_node {
    char name[32];
    char input_type[32];    /* Type this node accepts */
    char output_type[32];   /* Type this node produces */
    void (*resolve)(struct chain_node *self, void *input, void *output);
    void *state;            /* Node-private state */
} chain_node_t;

/* ── Route (connection between chains) ───────────────────────────── */

#define MAX_ROUTES 32

typedef struct {
    int  from_chain_id;
    int  from_node_idx;
    int  to_chain_id;
    int  to_node_idx;
    char signal_type[32];   /* What type flows on this route */
} route_t;

/* ── The Chain ───────────────────────────────────────────────────── */

#define MAX_CHAINS 128

typedef struct chain {
    int             id;
    char            name[64];
    cfa_addr_t      addr;
    masq_tier_t     tier;
    chain_status_t  status;

    /* MDE -- processing */
    chain_node_t    nodes[MAX_CHAIN_NODES];
    int             node_count;

    /* B3 -- belief */
    float           b3_alpha;
    float           b3_beta;
    int             b3_observations;

    /* Timing */
    uint64_t        created_tsc;
    uint64_t        last_resolve_tsc;
    float           last_resolve_ms;

    /* Tree */
    int             parent_id;      /* -1 if root */

    /* VAULT */
    int             vault_version;  /* Temporal state version */

    /* Scheduler watchdog -- post-hoc hang detection.
     * watchdog_deadline_tsc: armed before chain_resolve(); 0 = inactive.
     * watchdog_timeout_us:   per-chain timeout, default 500000 (500ms).
     * Preemption uses the LAPIC timer (vector 0xEF) to longjmp out of a
     * hung resolve; the post-hoc deadline sweep at start-of-tick is the
     * defensive secondary path for the case where longjmp itself faults. */
    uint64_t        watchdog_deadline_tsc;
    uint32_t        watchdog_timeout_us;

    /* Scheduler B3 backoff -- tunable per chain.
     * backoff_skip_threshold: failure ratio (b3_beta / (alpha+beta)) at
     *                         which to start skipping. Default 0.5.
     * backoff_skip_every:     skip period when ratio is at the threshold
     *                         floor. Default 4. Higher ratios scale up. */
    float           backoff_skip_threshold;
    uint32_t        backoff_skip_every;

    /* Async resolution: rate-limit how often this chain is resolved.
     *   resolve_interval_ticks = 0  -> resolve every scheduler tick
     *                                  (default; matches old behavior).
     *   resolve_interval_ticks = N  -> only resolve once every N ticks.
     * The scheduler skips chains where (current_tick - last_resolved_tick)
     * < resolve_interval_ticks. Used to decouple expensive draw chains
     * (CHAIN_COMPOSITOR, CHAIN_DISPLAY_<n>) from the per-tick budget
     * so cheap chains can resolve at full speed. */
    uint32_t        resolve_interval_ticks;
    uint64_t        last_resolved_tick;

    /* SMP affinity. -1 = any CPU may resolve (default; partition is by
     * id-modulo). Otherwise pinned to a specific cpu_idx — used to keep
     * display/compositor chains on the BSP because the GOP framebuffer
     * blit path is single-threaded. */
    int             affinity;
} chain_t;

#define CHAIN_AFFINITY_ANY (-1)

/* ── API ─────────────────────────────────────────────────────────── */

/* Initialize the chain registry */
void     chain_init(void);

/* Create a chain, derive CFA from parent + position. Returns id or -1. */
int      chain_create(const char *name, int parent_id, masq_tier_t tier);

/* Remove chain from registry */
void     chain_destroy(int id);

/* Add a processing node to a chain. Returns node index or -1. */
int      chain_add_node(int chain_id, const char *name,
                        const char *input_type, const char *output_type,
                        void (*resolve_fn)(chain_node_t *self, void *input, void *output));

/* Run all nodes in sequence, measure time, update B3.
 * Returns:
 *    0 on success
 *   -1 on per-chain error (not LIVE, invalid id, etc.)
 *   -2 if another CPU is already resolving this chain (try-lock fail);
 *      callers should treat as a benign skip, not a B3 failure. */
int      chain_resolve(int id);

/* Release the per-chain SMP lock for `id`. ONLY for the scheduler's
 * preempt ISR before longjmping out of a hung resolve — never call
 * directly from chain code. Idempotent if the lock isn't held. */
void     chain_resolve_force_unlock(int id);

/* Get pointer to chain by id. Returns NULL if invalid. */
chain_t *chain_get(int id);

/* Find first chain that produces the given output type. Returns id or -1. */
int      chain_find_by_type(const char *output_type);

/* MasQ check: can observer perceive target? */
int      chain_can_perceive(int observer_id, int target_id);

/* How many chains exist */
int      chain_count(void);

/* Print chain info via kprint */
void     chain_dump(int id);

/* Derive a child CFA address from a parent address */
cfa_addr_t cfa_derive(const cfa_addr_t *parent, uint32_t child_index);

#endif /* ZEOS_CHAIN_H */
