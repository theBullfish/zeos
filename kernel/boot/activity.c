/*
 * Activity Monitor: a chain that emits typed system_state snapshots.
 * Anomaly subscriber chain reacts via notify_send. UI is a stream
 * renderer that displays the most recent snapshot + history graph.
 * MDE auto-routes; the renderer just registers a system_state listener.
 *
 * Pipelines:
 *   CHAIN_SYSTEM_STATE:
 *       tick_request -> sample_chains -> sample_cpus -> sample_mem -> emit_state
 *       (output: system_state, resolve_interval_ticks=1200)
 *
 *   CHAIN_ACTIVITY_ANOMALY:
 *       input -> check -> fire_notify
 *       (input: system_state)
 *
 * MDE auto_route wires emitter -> subscriber via output_type=system_state
 * to input_type=system_state.
 */

#include "activity.h"
#include "chain.h"
#include "chain_registry.h"
#include "mde.h"
#include "scheduler.h"
#include "smp.h"
#include "pmm.h"
#include "notify.h"
#include "vault.h"
#include "fs_event.h"
#include "block_chain.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "compositor.h"
#include "ui_dirty.h"
#include "inspector.h"
#include "kprint.h"
#include "timer.h"
#include "timeofday.h"

/* ── Chain ids ──────────────────────────────────────────────────── */
int CHAIN_SYSTEM_STATE       = -1;
int CHAIN_ACTIVITY_ANOMALY   = -1;

/* ── Layout ─────────────────────────────────────────────────────── */
#define ACT_W            860
#define ACT_H            560
#define ACT_TITLE_H       32
#define ACT_TABS_H        28
#define ACT_STATUS_H      22
#define ACT_PAD            8
#define ACT_ROW_H         18
#define ACT_FONT_PX       13

#define ACT_TAB_CHAINS     0
#define ACT_TAB_CORES      1
#define ACT_TAB_MEMORY     2
#define ACT_TAB_NETWORK    3
#define ACT_TAB_STORAGE    4
#define ACT_TAB_GRAPH      5
#define ACT_TAB_COUNT      6

static const char *s_tab_names[ACT_TAB_COUNT] = {
    "Chains", "Cores", "Memory", "Network", "Storage", "Graph"
};

/* Graph metric selection (cycled with ',' '.' on Graph tab) */
#define ACT_GMETRIC_TPS              0
#define ACT_GMETRIC_TICK_AVG_US      1
#define ACT_GMETRIC_CHAINS_RESOLVED  2
#define ACT_GMETRIC_ERRORS           3
#define ACT_GMETRIC_FREE_PAGES       4
#define ACT_GMETRIC_FS_EVENTS        5
#define ACT_GMETRIC_COUNT            6
static const char *s_gmetric_names[ACT_GMETRIC_COUNT] = {
    "scheduler_tps",
    "tick_avg_us",
    "chains_resolved_last",
    "errors_last",
    "free_pages_kib",
    "fs_events_total",
};

/* ── Tiny helpers (no libc) ─────────────────────────────────────── */
static int  a_strlen(const char *s){int n=0; if(!s)return 0; while(s[n])n++; return n;}
static void a_strncpy(char *d,const char *s,int max){int i=0; if(!d||max<=0)return; if(s) while(i<max-1&&s[i]){d[i]=s[i];i++;} d[i]=0;}
static int  a_streq(const char *a,const char *b){if(!a||!b)return 0;while(*a&&*b){if(*a!=*b)return 0;a++;b++;}return *a==*b;}
static void a_strcat(char *d,const char *s,int max){int n=a_strlen(d); if(n>=max-1)return; int i=0; if(s) while(n<max-1&&s[i])d[n++]=s[i++]; d[n]=0;}
static void a_itoa(uint64_t v, char *out){
    char buf[24]; int n=0;
    if(v==0){out[0]='0';out[1]=0;return;}
    while(v && n<23){buf[n++]=(char)('0'+(v%10)); v/=10;}
    int k=0; while(n>0) out[k++]=buf[--n]; out[k]=0;
}
static void a_memset(void *d, int v, int n){
    uint8_t *dd=(uint8_t*)d; for(int i=0;i<n;i++) dd[i]=(uint8_t)v;
}
__attribute__((unused))
static void a_memcpy(void *d, const void *s, int n){
    uint8_t *dd=(uint8_t*)d; const uint8_t *ss=(const uint8_t*)s;
    for(int i=0;i<n;i++) dd[i]=ss[i];
}

/* TSC->ms conversion. We assume ~2GHz on modern x86 absent a probe API,
 * matching editor.c's calibration. Honest: this is approximate. */
static uint64_t s_tsc_per_sec = 2000000000ULL;
static uint64_t tsc_to_ms(uint64_t dt) {
    if (s_tsc_per_sec == 0) return 0;
    return (dt * 1000ULL) / s_tsc_per_sec;
}

/* ── Cached state (chain-emitted; UI renders this) ───────────────── */
static system_state_t s_latest;
static int            s_have_snapshot;
static uint32_t       s_seq;

/* History ring */
static act_history_sample_t s_history[ACT_HISTORY_SAMPLES];
static int                  s_history_count;     /* up to ACT_HISTORY_SAMPLES */
static int                  s_history_head;      /* index of oldest entry */

/* Last persistence time (TSC) */
static uint64_t s_last_persist_tsc;

/* ── Anomaly throttle ───────────────────────────────────────────── */
#define ACT_ANOMALY_KIND_B3        0
#define ACT_ANOMALY_KIND_TPS_DROP  1
#define ACT_ANOMALY_KIND_LOW_MEM   2
#define ACT_ANOMALY_KIND_AP_STALL  3
#define ACT_ANOMALY_KIND_COUNT     4

static uint64_t s_anomaly_last_tsc[ACT_ANOMALY_KIND_COUNT];

/* Rolling tps average (last 16 samples) */
#define ACT_TPS_RING 16
static uint32_t s_tps_ring[ACT_TPS_RING];
static int      s_tps_ring_count;
static int      s_tps_ring_head;

/* ── UI state ───────────────────────────────────────────────────── */
typedef struct {
    int active;
    int wx, wy;
    int tab;
    int scroll;             /* row scroll for current tab */
    int focus_idx;          /* highlighted row (chains / cores) */
    int graph_metric;
} act_ui_t;

static act_ui_t g_ui;

/* ── Counters ───────────────────────────────────────────────────── */
static uint32_t g_total_emits;
static uint32_t g_total_anomalies_fired;
static uint32_t g_total_anomalies_throttled;

/* ── Forward decls ──────────────────────────────────────────────── */
static void act_persist_save_internal(void);
static void act_history_push(const system_state_t *s);

/* ─────────────────────────────────────────────────────────────────
 * Sampler — runs inside CHAIN_SYSTEM_STATE pipeline. Walks the chain
 * registry, per-CPU table, page allocator. Each node fills its slice
 * of a shared scratch system_state_t and the terminal node copies it
 * into s_latest.
 * ───────────────────────────────────────────────────────────────── */

static system_state_t s_scratch;

static void node_tick_request(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    system_state_t *o = (system_state_t *)out;
    a_memset(o, 0, sizeof(*o));
    a_memset(&s_scratch, 0, sizeof(s_scratch));
    s_scratch.timestamp_tsc  = timer_read_tsc();
    s_scratch.timestamp_unix = tod_now_unix();
    s_scratch.seq            = ++s_seq;
    s_scratch.valid          = 1;
    *o = s_scratch;
}

/* Per-chain TPS estimator: track last seen tick + tsc per chain id. */
typedef struct {
    int       chain_id;
    uint64_t  last_tsc;
    uint64_t  last_resolved_tick;
} act_tps_track_t;
static act_tps_track_t s_tps_track[ACT_MAX_CHAINS_SAMPLED];
static int             s_tps_track_count;

static act_tps_track_t *track_for(int chain_id) {
    for (int i = 0; i < s_tps_track_count; i++)
        if (s_tps_track[i].chain_id == chain_id) return &s_tps_track[i];
    if (s_tps_track_count >= ACT_MAX_CHAINS_SAMPLED) return 0;
    act_tps_track_t *t = &s_tps_track[s_tps_track_count++];
    t->chain_id = chain_id;
    t->last_tsc = 0;
    t->last_resolved_tick = 0;
    return t;
}

static void node_sample_chains(chain_node_t *self, void *in, void *out) {
    (void)self;
    system_state_t *i = (system_state_t *)in;
    system_state_t *o = (system_state_t *)out;
    *o = *i;

    int total = chain_count();
    int n = 0;
    uint64_t now = o->timestamp_tsc;
    for (int id = 0; id < total && n < ACT_MAX_CHAINS_SAMPLED; id++) {
        chain_t *c = chain_get(id);
        if (!c) continue;
        act_chain_sample_t *s = &o->chains[n];
        s->chain_id  = c->id;
        s->parent_id = c->parent_id;
        s->tier      = (int)c->tier;
        s->status    = (int)c->status;
        s->affinity  = c->affinity;
        s->vault_version = c->vault_version;
        float ab = c->b3_alpha + c->b3_beta;
        s->b3_ratio = (ab > 0.0f) ? (c->b3_beta / ab) : 0.0f;
        s->last_resolve_ms = c->last_resolve_ms;
        s->resolve_interval_ticks = c->resolve_interval_ticks;
        s->last_resolved_tick = c->last_resolved_tick;

        /* Estimate per-chain tps: delta(last_resolved_tick) / delta(time). */
        act_tps_track_t *t = track_for(c->id);
        s->tps_estimate = 0;
        if (t) {
            if (t->last_tsc > 0 && now > t->last_tsc) {
                uint64_t dt_ms = tsc_to_ms(now - t->last_tsc);
                if (dt_ms > 0) {
                    uint64_t dticks = (c->last_resolved_tick > t->last_resolved_tick)
                                      ? (c->last_resolved_tick - t->last_resolved_tick) : 0;
                    /* dticks is scheduler ticks. tps_estimate = dticks * 1000 / dt_ms. */
                    s->tps_estimate = (uint32_t)((dticks * 1000ULL) / dt_ms);
                }
            }
            t->last_tsc = now;
            t->last_resolved_tick = c->last_resolved_tick;
        }
        a_strncpy(s->name, c->name, sizeof(s->name));
        n++;
    }
    o->n_chains = n;
    s_scratch = *o;
}

static void node_sample_cpus(chain_node_t *self, void *in, void *out) {
    (void)self;
    system_state_t *i = (system_state_t *)in;
    system_state_t *o = (system_state_t *)out;
    *o = *i;

    int n_cpus = smp_cpu_count();
    if (n_cpus > ACT_MAX_CPUS_SAMPLED) n_cpus = ACT_MAX_CPUS_SAMPLED;
    uint64_t now = o->timestamp_tsc;
    for (int idx = 0; idx < n_cpus; idx++) {
        smp_cpu_t *cp = smp_cpu_by_index(idx);
        if (!cp) continue;
        act_cpu_sample_t *cs = &o->cpus[o->n_cpus++];
        cs->cpu_idx                 = cp->cpu_idx;
        cs->lapic_id                = cp->lapic_id;
        cs->is_bsp                  = cp->is_bsp;
        cs->alive                   = cp->alive;
        cs->tick_count              = cp->tick_count;
        cs->chains_resolved         = cp->chains_resolved;
        cs->preempt_kills           = cp->preempt_kills;
        cs->tlb_shootdowns_received = cp->tlb_shootdowns_received;
        cs->heartbeat_tsc           = cp->heartbeat_tsc;
        if (cp->is_bsp || cp->heartbeat_tsc == 0) {
            cs->heartbeat_age_ms = 0;
        } else if (now > cp->heartbeat_tsc) {
            cs->heartbeat_age_ms = tsc_to_ms(now - cp->heartbeat_tsc);
        } else {
            cs->heartbeat_age_ms = 0;
        }
        cs->tps = smp_cpu_tps(cp->cpu_idx);
    }
    s_scratch = *o;
}

static void node_sample_mem(chain_node_t *self, void *in, void *out) {
    (void)self;
    system_state_t *i = (system_state_t *)in;
    system_state_t *o = (system_state_t *)out;
    *o = *i;

    o->total_pages = pmm_total_pages();
    o->free_pages  = pmm_free_pages();
    o->used_pages  = pmm_used_pages();

    o->scheduler_tps                    = scheduler_tps();
    o->scheduler_tick_avg_us            = scheduler_tick_avg_us();
    o->scheduler_chains_resolved_last   = scheduler_chains_resolved_last();
    o->scheduler_errors_last            = scheduler_errors_last();
    o->scheduler_slow_resolves_total    = scheduler_slow_resolves_total();
    o->scheduler_aggregate_slow_total   = scheduler_aggregate_slow_total();
    o->scheduler_watchdog_kills         = scheduler_watchdog_kills();
    o->scheduler_preempt_kills          = scheduler_preempt_kills();

    o->fs_events_total       = (uint64_t)fs_event_total_emits();
    o->block_journal_total   = block_chain_journal_total();
    o->notify_chain_emitted_total = notify_chain_emitted_total();

    s_scratch = *o;
}

static void node_emit_state(chain_node_t *self, void *in, void *out) {
    (void)self;
    system_state_t *i = (system_state_t *)in;
    system_state_t *o = (system_state_t *)out;
    *o = *i;
    if (!i->valid) return;

    /* Cache for UI + subscribers. */
    s_latest = *i;
    s_have_snapshot = 1;
    g_total_emits++;

    /* Update tps rolling avg */
    s_tps_ring[s_tps_ring_head] = i->scheduler_tps;
    s_tps_ring_head = (s_tps_ring_head + 1) % ACT_TPS_RING;
    if (s_tps_ring_count < ACT_TPS_RING) s_tps_ring_count++;

    /* Push history */
    act_history_push(i);

    /* Mark window dirty so the UI repaints on next compositor frame. */
    if (g_ui.active) {
        dirty_register(ACTIVITY_WIN_ID);
        compositor_dirty(g_ui.wx, g_ui.wy, ACT_W, ACT_H);
    }

    /* Persist history every ACT_HISTORY_PERSIST_EVERY seconds. */
    uint64_t now = i->timestamp_tsc;
    if (s_last_persist_tsc == 0 ||
        tsc_to_ms(now - s_last_persist_tsc) >=
            (uint64_t)ACT_HISTORY_PERSIST_EVERY * 1000ULL) {
        act_persist_save_internal();
        s_last_persist_tsc = now;
    }

    /* Bump emitter chain's vault_version per CHAIN_CONTRACT — every
     * snapshot is a state change recorded in MasQ. */
    if (CHAIN_SYSTEM_STATE >= 0) {
        chain_t *c = chain_get(CHAIN_SYSTEM_STATE);
        if (c) c->vault_version++;
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Anomaly subscriber chain
 *   input: system_state
 *   - check 1: any chain with b3_ratio > 0.7
 *   - check 2: scheduler_tps drops > 50% from rolling avg
 *   - check 3: free_pages < 5% of total
 *   - check 4: any AP heartbeat age > 5000ms
 * Throttled per-kind to once per 60s.
 * ───────────────────────────────────────────────────────────────── */

static int anomaly_should_fire(int kind, uint64_t now) {
    uint64_t last = s_anomaly_last_tsc[kind];
    if (last == 0) return 1;
    uint64_t dt_ms = tsc_to_ms(now - last);
    return dt_ms >= 60000;
}

static void anomaly_mark(int kind, uint64_t now) {
    s_anomaly_last_tsc[kind] = now;
    g_total_anomalies_fired++;
}

static void node_anomaly_input(chain_node_t *self, void *in, void *out) {
    (void)self; (void)in;
    system_state_t *o = (system_state_t *)out;
    if (s_have_snapshot) *o = s_latest;
    else { a_memset(o, 0, sizeof(*o)); o->valid = 0; }
}

static void node_anomaly_check(chain_node_t *self, void *in, void *out) {
    (void)self;
    system_state_t *i = (system_state_t *)in;
    system_state_t *o = (system_state_t *)out;
    *o = *i;
    /* Pass-through; fire_notify reads the snapshot and emits. */
}

static void node_anomaly_fire_notify(chain_node_t *self, void *in, void *out) {
    (void)self;
    system_state_t *i = (system_state_t *)in;
    system_state_t *o = (system_state_t *)out;
    *o = *i;
    if (!i->valid) return;
    uint64_t now = i->timestamp_tsc;

    /* check 1: B3 failure ratio per chain */
    for (int k = 0; k < i->n_chains; k++) {
        const act_chain_sample_t *cs = &i->chains[k];
        if (cs->b3_ratio > 0.7f) {
            if (anomaly_should_fire(ACT_ANOMALY_KIND_B3, now)) {
                char text[128];
                a_strncpy(text, "[anomaly] chain ", sizeof(text));
                a_strcat(text, cs->name, sizeof(text));
                a_strcat(text, " has 70%+ failure rate", sizeof(text));
                notify_send(text, "activity", NOTIFY_WARNING);
                anomaly_mark(ACT_ANOMALY_KIND_B3, now);
            } else {
                g_total_anomalies_throttled++;
            }
            break;  /* one alert per kind per cycle */
        }
    }

    /* check 2: scheduler tps drop >50% from rolling avg */
    if (s_tps_ring_count >= 4) {
        uint64_t sum = 0;
        for (int k = 0; k < s_tps_ring_count; k++) sum += s_tps_ring[k];
        uint32_t avg = (uint32_t)(sum / (uint64_t)s_tps_ring_count);
        if (avg > 0 && i->scheduler_tps < (avg / 2)) {
            if (anomaly_should_fire(ACT_ANOMALY_KIND_TPS_DROP, now)) {
                char text[128];
                a_strncpy(text, "[anomaly] scheduler tps dropped >50% (",
                          sizeof(text));
                char ns[16]; a_itoa(i->scheduler_tps, ns);
                a_strcat(text, ns, sizeof(text));
                a_strcat(text, " vs avg ", sizeof(text));
                a_itoa(avg, ns);
                a_strcat(text, ns, sizeof(text));
                a_strcat(text, ")", sizeof(text));
                notify_send(text, "activity", NOTIFY_WARNING);
                anomaly_mark(ACT_ANOMALY_KIND_TPS_DROP, now);
            } else {
                g_total_anomalies_throttled++;
            }
        }
    }

    /* check 3: low memory */
    if (i->total_pages > 0) {
        /* free < 5% */
        uint64_t threshold = (i->total_pages * 5ULL) / 100ULL;
        if (i->free_pages < threshold) {
            if (anomaly_should_fire(ACT_ANOMALY_KIND_LOW_MEM, now)) {
                notify_send("[anomaly] low memory: free pages < 5%",
                            "activity", NOTIFY_WARNING);
                anomaly_mark(ACT_ANOMALY_KIND_LOW_MEM, now);
            } else {
                g_total_anomalies_throttled++;
            }
        }
    }

    /* check 4: AP heartbeat stall (>5s, BSP excluded) */
    for (int k = 0; k < i->n_cpus; k++) {
        const act_cpu_sample_t *cp = &i->cpus[k];
        if (cp->is_bsp) continue;
        if (!cp->alive) continue;
        if (cp->heartbeat_age_ms > 5000) {
            if (anomaly_should_fire(ACT_ANOMALY_KIND_AP_STALL, now)) {
                char text[128];
                a_strncpy(text, "[anomaly] core ", sizeof(text));
                char ns[16]; a_itoa((uint64_t)cp->cpu_idx, ns);
                a_strcat(text, ns, sizeof(text));
                a_strcat(text, " stalled (no heartbeat >5s)", sizeof(text));
                notify_send(text, "activity", NOTIFY_ERROR);
                anomaly_mark(ACT_ANOMALY_KIND_AP_STALL, now);
            } else {
                g_total_anomalies_throttled++;
            }
            break;
        }
    }

    /* MasQ provenance: every check pass bumps subscriber's vault_version. */
    if (CHAIN_ACTIVITY_ANOMALY >= 0) {
        chain_t *c = chain_get(CHAIN_ACTIVITY_ANOMALY);
        if (c) c->vault_version++;
    }
}

/* ─────────────────────────────────────────────────────────────────
 * History ring + persistence
 * ───────────────────────────────────────────────────────────────── */

static void act_history_push(const system_state_t *s) {
    act_history_sample_t *h;
    if (s_history_count < ACT_HISTORY_SAMPLES) {
        h = &s_history[s_history_count++];
    } else {
        h = &s_history[s_history_head];
        s_history_head = (s_history_head + 1) % ACT_HISTORY_SAMPLES;
    }
    h->timestamp_unix              = s->timestamp_unix;
    h->scheduler_tps               = s->scheduler_tps;
    h->scheduler_tick_avg_us       = s->scheduler_tick_avg_us;
    h->scheduler_chains_resolved_last = s->scheduler_chains_resolved_last;
    h->scheduler_errors_last       = s->scheduler_errors_last;
    h->scheduler_watchdog_kills    = s->scheduler_watchdog_kills;
    h->scheduler_preempt_kills     = s->scheduler_preempt_kills;
    h->free_pages_kib              = (uint32_t)((s->free_pages * 4ULL));
    h->used_pages_kib              = (uint32_t)((s->used_pages * 4ULL));
    h->n_chains                    = (uint32_t)s->n_chains;
    h->fs_events_total             = (uint32_t)s->fs_events_total;
}

int activity_history_count(void) { return s_history_count; }

int activity_history_get(int idx, act_history_sample_t *out) {
    if (idx < 0 || idx >= s_history_count || !out) return -1;
    /* idx=0 is newest. Map to ring layout. */
    int phys;
    if (s_history_count < ACT_HISTORY_SAMPLES) {
        phys = (s_history_count - 1) - idx;
    } else {
        int newest = (s_history_head - 1 + ACT_HISTORY_SAMPLES) % ACT_HISTORY_SAMPLES;
        phys = (newest - idx + ACT_HISTORY_SAMPLES) % ACT_HISTORY_SAMPLES;
    }
    *out = s_history[phys];
    return 0;
}

const system_state_t *activity_latest_snapshot(void) {
    return s_have_snapshot ? &s_latest : 0;
}

/* ─────────────────────────────────────────────────────────────────
 * Persistence (window pos + history ring) under /activity/
 * ───────────────────────────────────────────────────────────────── */

#define ACT_VAULT_DIR      "/activity"
#define ACT_VAULT_STATE    "/activity/state"
#define ACT_VAULT_HISTORY  "/activity/history"

static int s_vault_dir_inited;

static void act_persist_save_internal(void) {
    if (!s_vault_dir_inited) {
        (void)vault_mkdir(ACT_VAULT_DIR, VAULT_TIER_INTERNAL);
        s_vault_dir_inited = 1;
    }
    /* Window state — small ascii blob */
    char buf[128];
    a_strncpy(buf, "", sizeof(buf));
    char ns[16];
    a_itoa((uint64_t)g_ui.wx, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "\t", sizeof(buf));
    a_itoa((uint64_t)g_ui.wy, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "\t", sizeof(buf));
    a_itoa((uint64_t)g_ui.tab, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "\n", sizeof(buf));
    (void)vault_write(ACT_VAULT_STATE, buf, (uint32_t)a_strlen(buf));

    /* History ring — write the binary ring as-is. The reader knows the
     * layout because it's the same struct. Total <= 600 * sizeof(sample). */
    if (s_history_count > 0) {
        (void)vault_write(ACT_VAULT_HISTORY, s_history,
                          (uint32_t)(s_history_count *
                                     (int)sizeof(act_history_sample_t)));
    }
}

void activity_persist_save(void) { act_persist_save_internal(); }

void activity_persist_restore(void) {
    char buf[128];
    int got = vault_read(ACT_VAULT_STATE, buf, sizeof(buf) - 1);
    if (got > 0) {
        buf[got] = 0;
        int i = 0;
        int wx = 0, wy = 0, tab = 0;
        while (i < got && buf[i] >= '0' && buf[i] <= '9') wx = wx*10 + (buf[i++]-'0');
        if (i < got && buf[i] == '\t') {
            i++;
            while (i < got && buf[i] >= '0' && buf[i] <= '9') wy = wy*10 + (buf[i++]-'0');
            if (i < got && buf[i] == '\t') {
                i++;
                while (i < got && buf[i] >= '0' && buf[i] <= '9') tab = tab*10 + (buf[i++]-'0');
            }
        }
        g_ui.wx = wx; g_ui.wy = wy;
        if (tab >= 0 && tab < ACT_TAB_COUNT) g_ui.tab = tab;
    }
    /* Restore history ring */
    int rc = vault_read(ACT_VAULT_HISTORY, s_history, (uint32_t)sizeof(s_history));
    if (rc > 0) {
        int n = rc / (int)sizeof(act_history_sample_t);
        if (n > ACT_HISTORY_SAMPLES) n = ACT_HISTORY_SAMPLES;
        s_history_count = n;
        s_history_head  = 0;
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Chain registration
 * ───────────────────────────────────────────────────────────────── */

int activity_chain_register(int parent_chain_id) {
    if (CHAIN_SYSTEM_STATE >= 0) return 2;

    int e = chain_create("system_state", parent_chain_id, MASQ_INTERNAL);
    if (e < 0) return -1;
    chain_add_node(e, "tick_request",   "void",         "system_state", node_tick_request);
    chain_add_node(e, "sample_chains",  "system_state", "system_state", node_sample_chains);
    chain_add_node(e, "sample_cpus",    "system_state", "system_state", node_sample_cpus);
    chain_add_node(e, "sample_mem",     "system_state", "system_state", node_sample_mem);
    chain_add_node(e, "emit_state",     "system_state", "system_state", node_emit_state);
    {
        chain_t *c = chain_get(e);
        if (c) c->resolve_interval_ticks = 1200;  /* ~1Hz */
    }
    CHAIN_SYSTEM_STATE = e;

    int a = chain_create("activity_anomaly", parent_chain_id, MASQ_INTERNAL);
    if (a >= 0) {
        chain_add_node(a, "input",       "system_state", "system_state", node_anomaly_input);
        chain_add_node(a, "check",       "system_state", "system_state", node_anomaly_check);
        chain_add_node(a, "fire_notify", "system_state", "system_state", node_anomaly_fire_notify);
        chain_t *c = chain_get(a);
        if (c) c->resolve_interval_ticks = 1200;
        CHAIN_ACTIVITY_ANOMALY = a;
    }

    int total = 0;
    if (CHAIN_SYSTEM_STATE     >= 0) total++;
    if (CHAIN_ACTIVITY_ANOMALY >= 0) total++;
    return total;
}

/* ─────────────────────────────────────────────────────────────────
 * UI — open/close/active/draw/tick/input
 * ───────────────────────────────────────────────────────────────── */

static void act_layout_origin(void) {
    int sw = (int)fb_width(), sh = (int)fb_height();
    if (g_ui.wx == 0 && g_ui.wy == 0) {
        g_ui.wx = (sw - ACT_W) / 2;
        g_ui.wy = (sh - ACT_H) / 2;
    }
    if (g_ui.wx < 0) g_ui.wx = 0;
    if (g_ui.wy < 0) g_ui.wy = 0;
    if (g_ui.wx + ACT_W > sw) g_ui.wx = sw - ACT_W;
    if (g_ui.wy + ACT_H > sh) g_ui.wy = sh - ACT_H;
}

int activity_open(void) {
    if (!g_ui.active) {
        g_ui.active = 1;
        if (g_ui.tab < 0 || g_ui.tab >= ACT_TAB_COUNT) g_ui.tab = ACT_TAB_CHAINS;
        if (g_ui.graph_metric < 0 || g_ui.graph_metric >= ACT_GMETRIC_COUNT)
            g_ui.graph_metric = ACT_GMETRIC_TPS;
        g_ui.scroll = 0;
        g_ui.focus_idx = -1;
    }
    act_layout_origin();
    /* Force an immediate snapshot so the UI is populated on first draw. */
    if (CHAIN_SYSTEM_STATE >= 0) (void)chain_resolve(CHAIN_SYSTEM_STATE);
    int sw = (int)fb_width(), sh = (int)fb_height();
    compositor_dirty(0, 0, sw, sh);
    return 0;
}

void activity_close(void) {
    if (!g_ui.active) return;
    g_ui.active = 0;
    act_persist_save_internal();
    int sw = (int)fb_width(), sh = (int)fb_height();
    compositor_dirty(0, 0, sw, sh);
}

int activity_active(void) { return g_ui.active; }

void activity_tick(void) {
    /* No-op — CHAIN_SYSTEM_STATE drives the data. The scheduler will
     * resolve it at resolve_interval_ticks=1200 (~1Hz). The UI repaints
     * only when emit_state pushes a fresh snapshot. */
}

/* ── Drawing helpers ────────────────────────────────────────────── */

static void draw_close_button(int wx, int wy) {
    int bx = wx + ACT_W - 28, by = wy + 6;
    fb_rect(bx, by, 20, 20, COLOR_SURFACE_HIGH);
    font_draw(bx + 6, by + 3, "x", FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE);
}

static void draw_tabs(int wx, int wy) {
    int ty = wy + ACT_TITLE_H;
    fb_rect(wx, ty, ACT_W, ACT_TABS_H, COLOR_SURFACE_HIGH);
    int bx = wx + ACT_PAD;
    for (int t = 0; t < ACT_TAB_COUNT; t++) {
        int active = (g_ui.tab == t);
        int bw = 110;
        if (active) fb_rect(bx, ty + 4, bw, 20, COLOR_PRIMARY);
        font_draw(bx + 8, ty + 7, s_tab_names[t], FONT_UI, ACT_FONT_PX,
                  active ? 0xFFFFFFFF : COLOR_ON_SURFACE);
        bx += bw + 6;
    }
}

static void draw_status(int wx, int wy) {
    int sy = wy + ACT_H - ACT_STATUS_H;
    fb_rect(wx, sy, ACT_W, ACT_STATUS_H, COLOR_SURFACE_HIGH);
    char buf[160];
    a_strncpy(buf, "snapshots: ", sizeof(buf));
    char ns[16]; a_itoa((uint64_t)g_total_emits, ns);
    a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "  |  history: ", sizeof(buf));
    a_itoa((uint64_t)s_history_count, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "/", sizeof(buf));
    a_itoa((uint64_t)ACT_HISTORY_SAMPLES, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "  |  anomalies: ", sizeof(buf));
    a_itoa((uint64_t)g_total_anomalies_fired, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, " (throttled ", sizeof(buf));
    a_itoa((uint64_t)g_total_anomalies_throttled, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, ")", sizeof(buf));
    font_draw(wx + ACT_PAD, sy + 4, buf, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_2);
}

/* Render a fixed-width-ish two-decimal float, e.g. 1.23 */
static void fmt_float2(float v, char *out, int max) {
    if (max < 8) { if (max>0) out[0]=0; return; }
    if (v < 0.0f) v = 0.0f;
    int whole = (int)v;
    int frac  = (int)((v - (float)whole) * 100.0f);
    if (frac < 0) frac = 0;
    if (frac > 99) frac = 99;
    char ns[16];
    a_itoa((uint64_t)whole, ns);
    a_strncpy(out, ns, max);
    a_strcat(out, ".", max);
    if (frac < 10) a_strcat(out, "0", max);
    a_itoa((uint64_t)frac, ns);
    a_strcat(out, ns, max);
}

static const char *status_name(int status) {
    switch (status) {
        case 0: return "LIVE";
        case 1: return "PAUSED";
        case 2: return "ERROR";
        case 3: return "DETACHED";
        default: return "?";
    }
}

static const char *tier_name(int tier) {
    switch (tier) {
        case 0: return "SOV";
        case 1: return "INT";
        case 2: return "REF";
        default: return "?";
    }
}

static void draw_tab_chains(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    fb_rect_outline(x, y, w, h, COLOR_SEPARATOR, 1);

    /* Header */
    int hx = x + 8, hy = y + 4;
    font_draw(hx,            hy, "id",   FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 30,       hy, "name", FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 220,      hy, "par",  FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 256,      hy, "tier", FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 296,      hy, "status",FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 360,      hy, "b3",   FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 410,      hy, "ms",   FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 460,      hy, "vver", FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 520,      hy, "aff",  FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 560,      hy, "tps",  FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);

    if (!s_have_snapshot) {
        font_draw(x + w/2 - 60, y + h/2, "(no snapshot yet)",
                  FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
        return;
    }

    int row_y = y + 22;
    int max_rows = (h - 28) / ACT_ROW_H;
    int n = s_latest.n_chains;
    int start = g_ui.scroll;
    if (start < 0) start = 0;
    if (start > n - 1 && n > 0) start = n - 1;
    if (g_ui.focus_idx >= n) g_ui.focus_idx = n - 1;

    for (int i = 0; i < max_rows && (start + i) < n; i++) {
        const act_chain_sample_t *cs = &s_latest.chains[start + i];
        int focused = (g_ui.focus_idx == start + i);
        if (focused) fb_rect(x + 4, row_y - 1, w - 8, ACT_ROW_H, COLOR_PRIMARY);
        uint32_t fg = focused ? 0xFFFFFFFF : COLOR_ON_SURFACE;
        char ns[16];
        a_itoa((uint64_t)cs->chain_id, ns);
        font_draw(hx,        row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        font_draw(hx + 30,   row_y + 2, cs->name, FONT_UI, ACT_FONT_PX, fg);
        if (cs->parent_id < 0) a_strncpy(ns, "-", sizeof(ns));
        else                    a_itoa((uint64_t)cs->parent_id, ns);
        font_draw(hx + 220,  row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        font_draw(hx + 256,  row_y + 2, tier_name(cs->tier),   FONT_UI, ACT_FONT_PX, fg);
        font_draw(hx + 296,  row_y + 2, status_name(cs->status),FONT_UI, ACT_FONT_PX,
                  cs->status == 2 ? 0xFFE06060 : fg);
        char fb_buf[12]; fmt_float2(cs->b3_ratio, fb_buf, sizeof(fb_buf));
        font_draw(hx + 360,  row_y + 2, fb_buf, FONT_UI, ACT_FONT_PX,
                  cs->b3_ratio > 0.7f ? 0xFFE06060 : fg);
        char ms_buf[12]; fmt_float2(cs->last_resolve_ms, ms_buf, sizeof(ms_buf));
        font_draw(hx + 410,  row_y + 2, ms_buf, FONT_UI, ACT_FONT_PX, fg);
        a_itoa((uint64_t)cs->vault_version, ns);
        font_draw(hx + 460,  row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        if (cs->affinity < 0) a_strncpy(ns, "any", sizeof(ns));
        else                   a_itoa((uint64_t)cs->affinity, ns);
        font_draw(hx + 520,  row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        a_itoa((uint64_t)cs->tps_estimate, ns);
        font_draw(hx + 560,  row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        row_y += ACT_ROW_H;
    }
}

static void draw_tab_cores(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    fb_rect_outline(x, y, w, h, COLOR_SEPARATOR, 1);

    int hx = x + 8, hy = y + 4;
    font_draw(hx,        hy, "idx",      FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 40,   hy, "lapic",    FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 90,   hy, "role",     FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 140,  hy, "alive",    FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 200,  hy, "tps",      FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 250,  hy, "resolved", FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 340,  hy, "ticks",    FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 420,  hy, "preempt",  FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 490,  hy, "tlb_rx",   FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    font_draw(hx + 560,  hy, "hb_age_ms",FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);

    if (!s_have_snapshot) return;

    int row_y = y + 22;
    for (int i = 0; i < s_latest.n_cpus; i++) {
        const act_cpu_sample_t *cp = &s_latest.cpus[i];
        int focused = (g_ui.focus_idx == i);
        if (focused) fb_rect(x + 4, row_y - 1, w - 8, ACT_ROW_H, COLOR_PRIMARY);
        uint32_t fg = focused ? 0xFFFFFFFF : COLOR_ON_SURFACE;
        char ns[16];
        a_itoa((uint64_t)cp->cpu_idx, ns); font_draw(hx, row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        a_itoa((uint64_t)cp->lapic_id, ns); font_draw(hx + 40, row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        font_draw(hx + 90, row_y + 2, cp->is_bsp ? "BSP" : "AP", FONT_UI, ACT_FONT_PX, fg);
        font_draw(hx + 140, row_y + 2, cp->alive ? "yes" : "no",
                  FONT_UI, ACT_FONT_PX, cp->alive ? fg : 0xFFE06060);
        a_itoa((uint64_t)cp->tps, ns);  font_draw(hx + 200, row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        a_itoa(cp->chains_resolved, ns); font_draw(hx + 250, row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        a_itoa(cp->tick_count, ns);     font_draw(hx + 340, row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        a_itoa(cp->preempt_kills, ns);  font_draw(hx + 420, row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        a_itoa(cp->tlb_shootdowns_received, ns);
                                          font_draw(hx + 490, row_y + 2, ns, FONT_UI, ACT_FONT_PX, fg);
        if (cp->is_bsp) a_strncpy(ns, "-", sizeof(ns));
        else            a_itoa(cp->heartbeat_age_ms, ns);
        font_draw(hx + 560, row_y + 2, ns, FONT_UI, ACT_FONT_PX,
                  cp->heartbeat_age_ms > 5000 ? 0xFFE06060 : fg);
        row_y += ACT_ROW_H;
    }

    /* Drill-down: when focused on a core row, list chains pinned to it. */
    if (g_ui.focus_idx >= 0 && g_ui.focus_idx < s_latest.n_cpus) {
        int focus_cpu = s_latest.cpus[g_ui.focus_idx].cpu_idx;
        int dy = row_y + 8;
        char header[64]; a_strncpy(header, "Chains assigned to core ", sizeof(header));
        char ns[16]; a_itoa((uint64_t)focus_cpu, ns);
        a_strcat(header, ns, sizeof(header));
        a_strcat(header, " (affinity == this core):", sizeof(header));
        font_draw(x + 8, dy, header, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
        dy += ACT_ROW_H;
        for (int k = 0; k < s_latest.n_chains && dy < y + h - 4; k++) {
            const act_chain_sample_t *cs = &s_latest.chains[k];
            if (cs->affinity != focus_cpu) continue;
            char line[80];
            a_strncpy(line, "  ", sizeof(line));
            a_itoa((uint64_t)cs->chain_id, ns); a_strcat(line, ns, sizeof(line));
            a_strcat(line, "  ", sizeof(line));
            a_strcat(line, cs->name, sizeof(line));
            font_draw(x + 8, dy, line, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_2);
            dy += ACT_ROW_H;
        }
    }
}

static void draw_tab_memory(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    fb_rect_outline(x, y, w, h, COLOR_SEPARATOR, 1);
    int dy = y + 8;
    char buf[160]; char ns[24];

    if (!s_have_snapshot) {
        font_draw(x + 8, dy, "(no snapshot yet)", FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
        return;
    }
    a_strncpy(buf, "total pages: ", sizeof(buf));
    a_itoa(s_latest.total_pages, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "   (", sizeof(buf));
    a_itoa(s_latest.total_pages * 4ULL, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, " KiB)", sizeof(buf));
    font_draw(x + 8, dy, buf, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE); dy += ACT_ROW_H;

    a_strncpy(buf, "free  pages: ", sizeof(buf));
    a_itoa(s_latest.free_pages, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "   (", sizeof(buf));
    a_itoa(s_latest.free_pages * 4ULL, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, " KiB)", sizeof(buf));
    font_draw(x + 8, dy, buf, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE); dy += ACT_ROW_H;

    a_strncpy(buf, "used  pages: ", sizeof(buf));
    a_itoa(s_latest.used_pages, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "   (", sizeof(buf));
    a_itoa(s_latest.used_pages * 4ULL, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, " KiB)", sizeof(buf));
    font_draw(x + 8, dy, buf, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE); dy += ACT_ROW_H;

    /* Percentage bar */
    int bar_x = x + 8, bar_y = dy + 4, bar_w = w - 16, bar_h = 10;
    fb_rect(bar_x, bar_y, bar_w, bar_h, COLOR_SURFACE_TOP);
    if (s_latest.total_pages > 0) {
        uint64_t fill = ((uint64_t)bar_w * s_latest.used_pages) / s_latest.total_pages;
        fb_rect(bar_x, bar_y, (int)fill, bar_h, COLOR_PRIMARY);
    }
    dy = bar_y + bar_h + 8;

    a_strncpy(buf, "scheduler tps: ", sizeof(buf));
    a_itoa((uint64_t)s_latest.scheduler_tps, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "   tick_avg_us: ", sizeof(buf));
    a_itoa((uint64_t)s_latest.scheduler_tick_avg_us, ns); a_strcat(buf, ns, sizeof(buf));
    font_draw(x + 8, dy, buf, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_2); dy += ACT_ROW_H;

    a_strncpy(buf, "watchdog kills: ", sizeof(buf));
    a_itoa((uint64_t)s_latest.scheduler_watchdog_kills, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "   preempt kills: ", sizeof(buf));
    a_itoa((uint64_t)s_latest.scheduler_preempt_kills, ns); a_strcat(buf, ns, sizeof(buf));
    a_strcat(buf, "   slow resolves: ", sizeof(buf));
    a_itoa((uint64_t)s_latest.scheduler_slow_resolves_total, ns); a_strcat(buf, ns, sizeof(buf));
    font_draw(x + 8, dy, buf, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_2); dy += ACT_ROW_H;

    /* Top consumers (top 8 chains by vault_version delta as a proxy for
     * activity — we don't track per-chain page allocation today, so we
     * report what we DO have honestly). */
    dy += 6;
    font_draw(x + 8, dy, "Most-active chains (by vault_version):",
              FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    dy += ACT_ROW_H;

    /* Simple selection: top-8 by vault_version */
    int top_idx[8]; int top_v[8];
    int chosen = 0;
    for (int i = 0; i < 8; i++) { top_idx[i] = -1; top_v[i] = -1; }
    for (int i = 0; i < s_latest.n_chains; i++) {
        int v = s_latest.chains[i].vault_version;
        for (int j = 0; j < 8; j++) {
            if (v > top_v[j]) {
                /* shift down */
                for (int k = 7; k > j; k--) { top_v[k] = top_v[k-1]; top_idx[k] = top_idx[k-1]; }
                top_v[j] = v; top_idx[j] = i;
                if (chosen < 8) chosen++;
                break;
            }
        }
    }
    for (int j = 0; j < chosen && dy < y + h - 4; j++) {
        if (top_idx[j] < 0) continue;
        const act_chain_sample_t *cs = &s_latest.chains[top_idx[j]];
        char line[96];
        a_strncpy(line, "  ", sizeof(line));
        a_strcat(line, cs->name, sizeof(line));
        a_strcat(line, "  vver=", sizeof(line));
        a_itoa((uint64_t)cs->vault_version, ns); a_strcat(line, ns, sizeof(line));
        font_draw(x + 8, dy, line, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_2);
        dy += ACT_ROW_H;
    }
}

static void draw_tab_network(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    fb_rect_outline(x, y, w, h, COLOR_SEPARATOR, 1);
    int dy = y + 8;
    char buf[160]; char ns[24];

    if (!s_have_snapshot) {
        font_draw(x + 8, dy, "(no snapshot yet)", FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
        return;
    }
    /* Per-chain net activity from chain registry. We surface
     * CHAIN_NET_TX, CHAIN_NET_RX, CHAIN_WIFI, CHAIN_BLUETOOTH. */
    font_draw(x + 8, dy, "Network chains (vver = MasQ provenance bumps):",
              FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    dy += ACT_ROW_H;

    const char *targets[] = { "net_tx", "net_rx", "wifi", "bluetooth" };
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < s_latest.n_chains; i++) {
            const act_chain_sample_t *cs = &s_latest.chains[i];
            if (!a_streq(cs->name, targets[t])) continue;
            a_strncpy(buf, "  ", sizeof(buf));
            a_strcat(buf, cs->name, sizeof(buf));
            a_strcat(buf, "  vver=", sizeof(buf));
            a_itoa((uint64_t)cs->vault_version, ns); a_strcat(buf, ns, sizeof(buf));
            a_strcat(buf, "  status=", sizeof(buf));
            a_strcat(buf, status_name(cs->status), sizeof(buf));
            a_strcat(buf, "  last_ms=", sizeof(buf));
            char ms_buf[12]; fmt_float2(cs->last_resolve_ms, ms_buf, sizeof(ms_buf));
            a_strcat(buf, ms_buf, sizeof(buf));
            font_draw(x + 8, dy, buf, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE);
            dy += ACT_ROW_H;
            break;
        }
    }
    dy += 6;
    /* Honest disclosure: per-chain bytes/sec counters do NOT exist on
     * the chain struct today. We surface vver / status / last_ms as
     * the truthful chain-shaped network telemetry. */
    font_draw(x + 8, dy,
              "(per-chain byte counters not yet wired; CHAIN_CONTRACT future work)",
              FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
}

static void draw_tab_storage(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    fb_rect_outline(x, y, w, h, COLOR_SEPARATOR, 1);
    int dy = y + 8;
    char buf[160]; char ns[24];

    if (!s_have_snapshot) {
        font_draw(x + 8, dy, "(no snapshot yet)", FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
        return;
    }
    a_strncpy(buf, "block journal entries (total): ", sizeof(buf));
    a_itoa(s_latest.block_journal_total, ns); a_strcat(buf, ns, sizeof(buf));
    font_draw(x + 8, dy, buf, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE);
    dy += ACT_ROW_H;

    a_strncpy(buf, "fs events (total emits): ", sizeof(buf));
    a_itoa(s_latest.fs_events_total, ns); a_strcat(buf, ns, sizeof(buf));
    font_draw(x + 8, dy, buf, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE);
    dy += ACT_ROW_H;

    a_strncpy(buf, "notifications emitted: ", sizeof(buf));
    a_itoa((uint64_t)s_latest.notify_chain_emitted_total, ns);
    a_strcat(buf, ns, sizeof(buf));
    font_draw(x + 8, dy, buf, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE);
    dy += ACT_ROW_H;

    /* Surface block / hda / nvme chains' status. */
    dy += 6;
    font_draw(x + 8, dy, "Storage chains:",
              FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    dy += ACT_ROW_H;

    const char *targets[] = { "block", "audio", "fs_event", "fs_trash_react",
                              "fs_index", "fs_undo", "fs_notify" };
    int nt = 7;
    for (int t = 0; t < nt; t++) {
        for (int i = 0; i < s_latest.n_chains; i++) {
            const act_chain_sample_t *cs = &s_latest.chains[i];
            if (!a_streq(cs->name, targets[t])) continue;
            a_strncpy(buf, "  ", sizeof(buf));
            a_strcat(buf, cs->name, sizeof(buf));
            a_strcat(buf, "  vver=", sizeof(buf));
            a_itoa((uint64_t)cs->vault_version, ns); a_strcat(buf, ns, sizeof(buf));
            a_strcat(buf, "  ", sizeof(buf));
            a_strcat(buf, status_name(cs->status), sizeof(buf));
            font_draw(x + 8, dy, buf, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE);
            dy += ACT_ROW_H;
            break;
        }
    }
}

static uint32_t hist_metric(const act_history_sample_t *h, int metric) {
    switch (metric) {
        case ACT_GMETRIC_TPS:             return h->scheduler_tps;
        case ACT_GMETRIC_TICK_AVG_US:     return h->scheduler_tick_avg_us;
        case ACT_GMETRIC_CHAINS_RESOLVED: return h->scheduler_chains_resolved_last;
        case ACT_GMETRIC_ERRORS:          return h->scheduler_errors_last;
        case ACT_GMETRIC_FREE_PAGES:      return h->free_pages_kib / 1024U;  /* MiB */
        case ACT_GMETRIC_FS_EVENTS:       return h->fs_events_total;
    }
    return 0;
}

static void draw_tab_graph(int x, int y, int w, int h) {
    fb_rect(x, y, w, h, COLOR_SURFACE);
    fb_rect_outline(x, y, w, h, COLOR_SEPARATOR, 1);

    char title[96];
    a_strncpy(title, "metric: ", sizeof(title));
    a_strcat(title, s_gmetric_names[g_ui.graph_metric], sizeof(title));
    a_strcat(title, "   (',' / '.' to cycle)", sizeof(title));
    font_draw(x + 8, y + 4, title, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_2);

    if (s_history_count <= 1) {
        font_draw(x + 8, y + h/2,
                  "(history empty — wait for samples)",
                  FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
        return;
    }

    int gx = x + 12, gy = y + 26;
    int gw = w - 24, gh = h - 36;
    fb_rect(gx, gy, gw, gh, COLOR_SURFACE_TOP);

    /* Find min/max */
    uint32_t maxv = 0, minv = 0xFFFFFFFFu;
    for (int i = 0; i < s_history_count; i++) {
        act_history_sample_t s; if (activity_history_get(i, &s) != 0) continue;
        uint32_t v = hist_metric(&s, g_ui.graph_metric);
        if (v > maxv) maxv = v;
        if (v < minv) minv = v;
    }
    if (maxv == 0) maxv = 1;
    if (minv == maxv) minv = 0;

    /* Draw line: oldest at left, newest at right. */
    int n = s_history_count;
    int prev_x = -1, prev_y = -1;
    for (int k = 0; k < n; k++) {
        /* idx = newest-first; walk from oldest -> newest */
        int idx = (n - 1) - k;
        act_history_sample_t s; if (activity_history_get(idx, &s) != 0) continue;
        uint32_t v = hist_metric(&s, g_ui.graph_metric);
        int px = gx + (k * gw) / (n > 1 ? (n - 1) : 1);
        int py;
        if (maxv == minv) py = gy + gh - 1;
        else py = gy + gh - 1 - (int)(((uint64_t)(v - minv) * (uint64_t)(gh - 2)) /
                                      (uint64_t)(maxv - minv));
        if (py < gy) py = gy;
        if (py >= gy + gh) py = gy + gh - 1;
        if (prev_x >= 0) {
            /* simple Bresenham-ish line: step in x */
            int x0 = prev_x, y0 = prev_y, x1 = px, y1 = py;
            int dx = x1 - x0; if (dx <= 0) dx = 1;
            for (int xx = x0; xx <= x1; xx++) {
                int yy = y0 + (int)(((int64_t)(xx - x0) * (int64_t)(y1 - y0)) / (int64_t)dx);
                fb_rect(xx, yy, 1, 1, COLOR_PRIMARY);
            }
        } else {
            fb_rect(px, py, 1, 1, COLOR_PRIMARY);
        }
        prev_x = px; prev_y = py;
    }

    /* Axes labels */
    char ns[16];
    a_itoa((uint64_t)maxv, ns);
    font_draw(gx + 2, gy + 2, ns, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
    a_itoa((uint64_t)minv, ns);
    font_draw(gx + 2, gy + gh - 14, ns, FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE_3);
}

void activity_draw(void) {
    if (!g_ui.active) return;
    act_layout_origin();
    int wx = g_ui.wx, wy = g_ui.wy;

    /* Background + chrome */
    fb_rect(wx, wy, ACT_W, ACT_H, COLOR_SURFACE);
    fb_rect_outline(wx, wy, ACT_W, ACT_H, COLOR_SURFACE_TOP, 1);
    fb_rect(wx, wy, ACT_W, ACT_TITLE_H, COLOR_SURFACE_HIGH);
    font_draw(wx + ACT_PAD, wy + 8, "Activity Monitor",
              FONT_UI, ACT_FONT_PX, COLOR_ON_SURFACE);
    draw_close_button(wx, wy);
    draw_tabs(wx, wy);

    int bx = wx;
    int by = wy + ACT_TITLE_H + ACT_TABS_H;
    int bw = ACT_W;
    int bh = ACT_H - ACT_TITLE_H - ACT_TABS_H - ACT_STATUS_H;

    switch (g_ui.tab) {
        case ACT_TAB_CHAINS:  draw_tab_chains (bx, by, bw, bh); break;
        case ACT_TAB_CORES:   draw_tab_cores  (bx, by, bw, bh); break;
        case ACT_TAB_MEMORY:  draw_tab_memory (bx, by, bw, bh); break;
        case ACT_TAB_NETWORK: draw_tab_network(bx, by, bw, bh); break;
        case ACT_TAB_STORAGE: draw_tab_storage(bx, by, bw, bh); break;
        case ACT_TAB_GRAPH:   draw_tab_graph  (bx, by, bw, bh); break;
    }

    draw_status(wx, wy);
}

/* ── Input ──────────────────────────────────────────────────────── */

static int act_in_window(int x, int y) {
    return (x >= g_ui.wx && y >= g_ui.wy &&
            x <  g_ui.wx + ACT_W && y <  g_ui.wy + ACT_H);
}

int activity_key(int ascii) {
    if (!g_ui.active) return 0;
    if (ascii == 27) { activity_close(); return 1; }
    if (ascii == 9)  {
        g_ui.tab = (g_ui.tab + 1) % ACT_TAB_COUNT;
        g_ui.scroll = 0; g_ui.focus_idx = -1;
        return 1;
    }
    if (g_ui.tab == ACT_TAB_GRAPH) {
        if (ascii == ',') {
            g_ui.graph_metric = (g_ui.graph_metric + ACT_GMETRIC_COUNT - 1) % ACT_GMETRIC_COUNT;
            return 1;
        }
        if (ascii == '.') {
            g_ui.graph_metric = (g_ui.graph_metric + 1) % ACT_GMETRIC_COUNT;
            return 1;
        }
    }
    /* Enter on Chains tab opens inspector for the focused chain. */
    if ((ascii == '\n' || ascii == '\r') && g_ui.tab == ACT_TAB_CHAINS) {
        if (g_ui.focus_idx >= 0 && g_ui.focus_idx < s_latest.n_chains) {
            int cid = s_latest.chains[g_ui.focus_idx].chain_id;
            inspector_show(cid);
        }
        return 1;
    }
    return 1;
}

int activity_key_arrow(int dir) {
    if (!g_ui.active) return 0;
    int n = 0;
    if      (g_ui.tab == ACT_TAB_CHAINS) n = s_have_snapshot ? s_latest.n_chains : 0;
    else if (g_ui.tab == ACT_TAB_CORES)  n = s_have_snapshot ? s_latest.n_cpus   : 0;
    if (n == 0) return 1;
    if (dir == 2) { /* up */
        if (g_ui.focus_idx > 0) g_ui.focus_idx--; else g_ui.focus_idx = 0;
    } else if (dir == 3) { /* down */
        if (g_ui.focus_idx < 0) g_ui.focus_idx = 0;
        else if (g_ui.focus_idx < n - 1) g_ui.focus_idx++;
    }
    /* Auto-scroll */
    int max_rows = (ACT_H - ACT_TITLE_H - ACT_TABS_H - ACT_STATUS_H - 28) / ACT_ROW_H;
    if (g_ui.focus_idx < g_ui.scroll) g_ui.scroll = g_ui.focus_idx;
    else if (g_ui.focus_idx >= g_ui.scroll + max_rows)
        g_ui.scroll = g_ui.focus_idx - max_rows + 1;
    return 1;
}

static int tab_hit(int x, int y) {
    int ty = g_ui.wy + ACT_TITLE_H;
    if (y < ty + 4 || y > ty + 24) return -1;
    int bx = g_ui.wx + ACT_PAD;
    for (int t = 0; t < ACT_TAB_COUNT; t++) {
        int bw = 110;
        if (x >= bx && x < bx + bw) return t;
        bx += bw + 6;
    }
    return -1;
}

static int row_hit(int x, int y, int *rel_idx) {
    int by = g_ui.wy + ACT_TITLE_H + ACT_TABS_H + 22;
    if (x < g_ui.wx + 4 || x >= g_ui.wx + ACT_W - 4) return 0;
    int rel = (y - by) / ACT_ROW_H;
    if (rel < 0) return 0;
    if (rel_idx) *rel_idx = rel;
    return 1;
}

int activity_mouse_down(int x, int y, int button) {
    if (!g_ui.active) return 0;
    (void)button;
    if (!act_in_window(x, y)) return 1;
    int wx = g_ui.wx, wy = g_ui.wy;
    /* Close */
    if (x >= wx + ACT_W - 28 && x <= wx + ACT_W - 8 &&
        y >= wy + 6 && y <= wy + 26) {
        activity_close(); return 1;
    }
    /* Tabs */
    int t = tab_hit(x, y);
    if (t >= 0) {
        g_ui.tab = t; g_ui.scroll = 0; g_ui.focus_idx = -1;
        return 1;
    }
    /* Row click */
    int rel = -1;
    if (row_hit(x, y, &rel)) {
        if (g_ui.tab == ACT_TAB_CHAINS && s_have_snapshot) {
            int idx = g_ui.scroll + rel;
            if (idx >= 0 && idx < s_latest.n_chains) {
                g_ui.focus_idx = idx;
                if (button == 0) {
                    /* left-click selects; second left-click on same row opens inspector */
                    static int s_last_clicked = -1;
                    if (s_last_clicked == idx) {
                        inspector_show(s_latest.chains[idx].chain_id);
                    }
                    s_last_clicked = idx;
                }
            }
        } else if (g_ui.tab == ACT_TAB_CORES && s_have_snapshot) {
            int idx = rel;
            if (idx >= 0 && idx < s_latest.n_cpus) g_ui.focus_idx = idx;
        }
        return 1;
    }
    return 1;
}

int activity_mouse_move(int x, int y) { (void)x;(void)y; return 0; }
int activity_mouse_up(int x, int y, int button) { (void)x;(void)y;(void)button; return 0; }

/* ─────────────────────────────────────────────────────────────────
 * Selftest line
 * ───────────────────────────────────────────────────────────────── */

void activity_print_selftest_line(void) {
    int armed = (CHAIN_ACTIVITY_ANOMALY >= 0) ? 1 : 0;
    kputs("Activity Monitor ..... CHAIN_SYSTEM_STATE @1Hz");
    if (CHAIN_SYSTEM_STATE < 0) kputs(" (NOT REGISTERED)");
    if (armed) kputs(", anomaly subscriber armed");
    else       kputs(", anomaly subscriber NOT REGISTERED");
    kputs(", history=");
    kput_dec((uint64_t)s_history_count);
    kputs(" samples\n");
}

/* ─────────────────────────────────────────────────────────────────
 * Shell command — `top` opens UI, `top --json` prints snapshot.
 * ───────────────────────────────────────────────────────────────── */

static void emit_json(const system_state_t *s) {
    char ns[24];
    kputs("{\"seq\":");        a_itoa((uint64_t)s->seq, ns); kputs(ns);
    kputs(",\"timestamp_unix\":"); a_itoa(s->timestamp_unix, ns); kputs(ns);
    kputs(",\"n_chains\":");   a_itoa((uint64_t)s->n_chains, ns); kputs(ns);
    kputs(",\"n_cpus\":");     a_itoa((uint64_t)s->n_cpus, ns); kputs(ns);
    kputs(",\"free_pages\":"); a_itoa(s->free_pages, ns); kputs(ns);
    kputs(",\"used_pages\":"); a_itoa(s->used_pages, ns); kputs(ns);
    kputs(",\"total_pages\":");a_itoa(s->total_pages, ns); kputs(ns);
    kputs(",\"scheduler_tps\":");          a_itoa((uint64_t)s->scheduler_tps, ns); kputs(ns);
    kputs(",\"scheduler_tick_avg_us\":");  a_itoa((uint64_t)s->scheduler_tick_avg_us, ns); kputs(ns);
    kputs(",\"watchdog_kills\":");         a_itoa((uint64_t)s->scheduler_watchdog_kills, ns); kputs(ns);
    kputs(",\"preempt_kills\":");          a_itoa((uint64_t)s->scheduler_preempt_kills, ns); kputs(ns);
    kputs(",\"fs_events_total\":");        a_itoa(s->fs_events_total, ns); kputs(ns);
    kputs(",\"block_journal_total\":");    a_itoa(s->block_journal_total, ns); kputs(ns);

    kputs(",\"chains\":[");
    for (int i = 0; i < s->n_chains; i++) {
        const act_chain_sample_t *cs = &s->chains[i];
        if (i > 0) kputs(",");
        kputs("{\"id\":");      a_itoa((uint64_t)cs->chain_id, ns); kputs(ns);
        kputs(",\"name\":\"");  kputs(cs->name); kputs("\"");
        kputs(",\"parent\":");  a_itoa((uint64_t)cs->parent_id, ns); kputs(ns);
        kputs(",\"status\":");  a_itoa((uint64_t)cs->status, ns); kputs(ns);
        kputs(",\"affinity\":");a_itoa((uint64_t)cs->affinity, ns); kputs(ns);
        kputs(",\"vault_version\":"); a_itoa((uint64_t)cs->vault_version, ns); kputs(ns);
        char fb_buf[12]; fmt_float2(cs->b3_ratio, fb_buf, sizeof(fb_buf));
        kputs(",\"b3_ratio\":"); kputs(fb_buf);
        char ms_buf[12]; fmt_float2(cs->last_resolve_ms, ms_buf, sizeof(ms_buf));
        kputs(",\"last_resolve_ms\":"); kputs(ms_buf);
        kputs(",\"tps\":");      a_itoa((uint64_t)cs->tps_estimate, ns); kputs(ns);
        kputs("}");
    }
    kputs("]");

    kputs(",\"cpus\":[");
    for (int i = 0; i < s->n_cpus; i++) {
        const act_cpu_sample_t *cp = &s->cpus[i];
        if (i > 0) kputs(",");
        kputs("{\"idx\":");      a_itoa((uint64_t)cp->cpu_idx, ns); kputs(ns);
        kputs(",\"lapic\":");    a_itoa((uint64_t)cp->lapic_id, ns); kputs(ns);
        kputs(",\"bsp\":");      a_itoa((uint64_t)cp->is_bsp, ns); kputs(ns);
        kputs(",\"alive\":");    a_itoa((uint64_t)cp->alive, ns); kputs(ns);
        kputs(",\"tps\":");      a_itoa((uint64_t)cp->tps, ns); kputs(ns);
        kputs(",\"chains_resolved\":"); a_itoa(cp->chains_resolved, ns); kputs(ns);
        kputs(",\"tick_count\":"); a_itoa(cp->tick_count, ns); kputs(ns);
        kputs(",\"tlb_rx\":");   a_itoa(cp->tlb_shootdowns_received, ns); kputs(ns);
        kputs(",\"hb_age_ms\":"); a_itoa(cp->heartbeat_age_ms, ns); kputs(ns);
        kputs("}");
    }
    kputs("]");

    kputs("}\n");
}

void activity_cmd_top(const char *args) {
    while (args && *args == ' ') args++;
    int json = (args && a_streq(args, "--json"));

    if (json) {
        /* Force a sample if we have no snapshot yet. */
        if (CHAIN_SYSTEM_STATE >= 0 && !s_have_snapshot)
            (void)chain_resolve(CHAIN_SYSTEM_STATE);
        if (!s_have_snapshot) {
            kputs("{\"error\":\"no snapshot yet — CHAIN_SYSTEM_STATE not registered or never resolved\"}\n");
            return;
        }
        emit_json(&s_latest);
        return;
    }
    (void)activity_open();
    kputs("  top: opened activity monitor (Tab to switch panels, Enter on a chain row to inspect, Esc to close)\n");
}
