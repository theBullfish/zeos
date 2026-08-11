/*
 * Zeos — Activity Monitor (chain-shaped)
 *
 * The Linux/macOS "Activity Monitor" is a top-style imperative tool
 * that polls the kernel for stats. The Zeos shape is different:
 *
 *   CHAIN_SYSTEM_STATE  — a chain whose pipeline samples the chain
 *                         registry, the per-CPU table, the page
 *                         allocator, and per-chain counters, then
 *                         emits a typed `system_state` signal at
 *                         resolve_interval_ticks=1200 (~1Hz).
 *
 *   CHAIN_ACTIVITY_ANOMALY — a subscriber chain (input_type=
 *                         system_state) that fires notify_send on
 *                         four conditions (B3 ratio, tps drop, low
 *                         memory, AP heartbeat stall) with a 60s
 *                         per-anomaly-type throttle.
 *
 * The UI in activity.c is a stream renderer over the most-recent
 * snapshot + a 600-sample history ring (10 minutes at 1Hz). It does
 * not poll. It reads the cached snapshot the chain produced.
 *
 * Public surface:
 *   activity_open / close / active / tick / draw / key* / mouse*
 *   activity_chain_register(parent)         -- registers both chains
 *   activity_persist_save / restore         -- last window pos
 *   activity_print_selftest_line            -- selftest banner
 *   activity_cmd_top(args)                  -- "top" shell command
 *
 * History persists to VAULT every 60s under /activity/history.
 */

#ifndef ZEOS_ACTIVITY_H
#define ZEOS_ACTIVITY_H

#include <stdint.h>

#define ACTIVITY_WIN_ID            5153

#define ACT_MAX_CHAINS_SAMPLED     64
#define ACT_MAX_CPUS_SAMPLED        8
#define ACT_HISTORY_SAMPLES       600   /* 10 minutes at 1 Hz */
#define ACT_HISTORY_PERSIST_EVERY  60   /* seconds */

/* ── Per-chain sample (compact) ──────────────────────────────────── */
typedef struct {
    int      chain_id;
    int      parent_id;
    int      tier;                  /* masq_tier_t */
    int      status;                /* chain_status_t */
    int      affinity;              /* -1 or cpu_idx */
    int      vault_version;
    float    b3_ratio;              /* beta/(alpha+beta) — failure rate */
    float    last_resolve_ms;
    uint32_t resolve_interval_ticks;
    uint32_t tps_estimate;          /* ~ chain_resolves / sec */
    uint64_t last_resolved_tick;
    char     name[32];
} act_chain_sample_t;

/* ── Per-CPU sample (compact) ────────────────────────────────────── */
typedef struct {
    int      cpu_idx;
    int      lapic_id;
    int      is_bsp;
    int      alive;
    uint64_t tick_count;
    uint64_t chains_resolved;
    uint64_t preempt_kills;
    uint64_t tlb_shootdowns_received;
    uint64_t heartbeat_tsc;          /* TSC at last AP loop iteration */
    uint64_t heartbeat_age_ms;       /* now - heartbeat, in ms (0 for BSP) */
    uint32_t tps;                    /* per-CPU rolling TPS */
} act_cpu_sample_t;

/* ── system_state typed signal ───────────────────────────────────── */
typedef struct {
    uint64_t  timestamp_tsc;
    uint64_t  timestamp_unix;        /* 0 if tod not ready */
    uint32_t  seq;                   /* monotonic per snapshot */
    int       valid;                 /* 0 = sentinel between resolves */

    int                  n_chains;
    act_chain_sample_t   chains[ACT_MAX_CHAINS_SAMPLED];

    int                  n_cpus;
    act_cpu_sample_t     cpus[ACT_MAX_CPUS_SAMPLED];

    uint64_t  free_pages;
    uint64_t  total_pages;
    uint64_t  used_pages;

    /* System aggregates */
    uint32_t  scheduler_tps;
    uint32_t  scheduler_tick_avg_us;
    uint32_t  scheduler_chains_resolved_last;
    uint32_t  scheduler_errors_last;
    uint32_t  scheduler_slow_resolves_total;
    uint32_t  scheduler_aggregate_slow_total;
    uint32_t  scheduler_watchdog_kills;
    uint32_t  scheduler_preempt_kills;

    /* Storage / FS */
    uint64_t  fs_events_total;
    uint64_t  block_journal_total;

    /* Notifications passing through CHAIN_NOTIFY (provenance proxy) */
    uint32_t  notify_chain_emitted_total;
} system_state_t;

/* ── Compact history sample (kept small to fit 600) ──────────────── */
typedef struct {
    uint64_t  timestamp_unix;
    uint32_t  scheduler_tps;
    uint32_t  scheduler_tick_avg_us;
    uint32_t  scheduler_chains_resolved_last;
    uint32_t  scheduler_errors_last;
    uint32_t  scheduler_watchdog_kills;
    uint32_t  scheduler_preempt_kills;
    uint32_t  free_pages_kib;
    uint32_t  used_pages_kib;
    uint32_t  n_chains;
    uint32_t  fs_events_total;
} act_history_sample_t;

/* ── Public API ──────────────────────────────────────────────────── */

int  activity_open(void);
void activity_close(void);
int  activity_active(void);

void activity_tick(void);
void activity_draw(void);

int  activity_key(int ascii);
int  activity_key_arrow(int dir);
int  activity_mouse_down(int x, int y, int button);
int  activity_mouse_move(int x, int y);
int  activity_mouse_up(int x, int y, int button);

/* Register CHAIN_SYSTEM_STATE + CHAIN_ACTIVITY_ANOMALY under parent.
 * Returns total chains registered (1..2), -1 on failure. Idempotent. */
int  activity_chain_register(int parent_chain_id);

/* Read the most recent snapshot (chain-cached). Returns 0 if no
 * snapshot has been emitted yet. The caller does NOT own the buffer. */
const system_state_t *activity_latest_snapshot(void);

/* Read history. idx=0 is newest. Returns 0 on success, -1 OOB. */
int  activity_history_count(void);
int  activity_history_get(int idx, act_history_sample_t *out);

/* Persistence */
void activity_persist_save(void);
void activity_persist_restore(void);

/* Selftest line printer. */
void activity_print_selftest_line(void);

/* `top` shell command — opens UI; `top --json` prints snapshot JSON. */
void activity_cmd_top(const char *args);

/* Exported chain ids (set by activity_chain_register, -1 otherwise). */
extern int CHAIN_SYSTEM_STATE;
extern int CHAIN_ACTIVITY_ANOMALY;

#endif /* ZEOS_ACTIVITY_H */
