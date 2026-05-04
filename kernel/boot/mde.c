/*
 * Zeos -- MDE (Mixing Desk Engine) implementation
 *
 * Type-based signal routing between chains.
 * Static arrays, no malloc, bare-metal.
 */

#include "mde.h"
#include "b3.h"
#include "kprint.h"
#include "scheduler.h"

/* ── Static state ───────────────────────────────────────────────── */

static mde_state_t mde;

/* ── B3 routing state ──────────────────────────────────────────── */

float mde_explore_factor = 0.1f;        /* 10% exploration by default */

static unsigned int mde_rr_counter = 0; /* round-robin tiebreaker */
static unsigned int mde_rng_state = 42; /* simple LCG for exploration */

static unsigned int mde_rand(void)
{
    /* Minimal LCG -- good enough for epsilon-greedy selection */
    mde_rng_state = mde_rng_state * 1103515245u + 12345u;
    return (mde_rng_state >> 16) & 0x7FFF;
}

/* ── Helpers ────────────────────────────────────────────────────── */

static void mde_str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int mde_str_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ── Init ───────────────────────────────────────────────────────── */

void mde_init(void)
{
    int i;

    for (i = 0; i < MDE_MAX_ROUTES; i++) {
        mde.routes[i].from_chain = -1;
        mde.routes[i].from_node  = -1;
        mde.routes[i].to_chain   = -1;
        mde.routes[i].to_node    = -1;
        mde.routes[i].signal_type[0] = '\0';
        mde.routes[i].weight     = 1.0f;
        mde.routes[i].active     = 0;
    }

    mde.route_count = 0;
    mde.auto_route  = 1;

    kputs("[mde] routing engine initialized (");
    kput_dec(MDE_MAX_ROUTES);
    kputs(" route slots)\n");
}

/* ── Add Route ──────────────────────────────────────────────────── */

int mde_add_route(int from_chain, int from_node,
                  int to_chain, int to_node, float weight)
{
    chain_t *fc, *tc;
    int slot = -1;
    int i;

    /* Validate chains exist */
    fc = chain_get(from_chain);
    tc = chain_get(to_chain);
    if (!fc || !tc)
        return -1;

    /* Validate node indices */
    if (from_node < 0 || from_node >= fc->node_count)
        return -1;
    if (to_node < 0 || to_node >= tc->node_count)
        return -1;

    /* Check type compatibility */
    if (!mde_str_equal(fc->nodes[from_node].output_type,
                       tc->nodes[to_node].input_type)) {
        kputs("[mde] WARNING: type mismatch on manual route ");
        kputs(fc->nodes[from_node].output_type);
        kputs(" -> ");
        kputs(tc->nodes[to_node].input_type);
        kputc('\n');
        /* Allow it anyway -- manual routes can override */
    }

    /* Check for duplicate */
    for (i = 0; i < MDE_MAX_ROUTES; i++) {
        if (mde.routes[i].active &&
            mde.routes[i].from_chain == from_chain &&
            mde.routes[i].from_node  == from_node &&
            mde.routes[i].to_chain   == to_chain &&
            mde.routes[i].to_node    == to_node) {
            /* Update weight on existing route */
            mde.routes[i].weight = weight;
            return i;
        }
    }

    /* Find free slot */
    for (i = 0; i < MDE_MAX_ROUTES; i++) {
        if (!mde.routes[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        kputs("[mde] ERROR: route table full\n");
        return -1;
    }

    mde.routes[slot].from_chain = from_chain;
    mde.routes[slot].from_node  = from_node;
    mde.routes[slot].to_chain   = to_chain;
    mde.routes[slot].to_node    = to_node;
    mde_str_copy(mde.routes[slot].signal_type,
                 fc->nodes[from_node].output_type, 32);
    mde.routes[slot].weight     = weight;
    mde.routes[slot].active     = 1;
    mde.route_count++;

    return slot;
}

/* ── Remove Route ───────────────────────────────────────────────── */

void mde_remove_route(int index)
{
    if (index < 0 || index >= MDE_MAX_ROUTES)
        return;
    if (!mde.routes[index].active)
        return;

    mde.routes[index].active = 0;
    mde.routes[index].from_chain = -1;
    mde.routes[index].from_node  = -1;
    mde.routes[index].to_chain   = -1;
    mde.routes[index].to_node    = -1;
    mde.routes[index].signal_type[0] = '\0';
    mde.route_count--;
}

/* ── Auto Route ─────────────────────────────────────────────────── */

int mde_auto_route(void)
{
    int created = 0;
    int i, j, k, m;
    chain_t *src, *dst;

    /*
     * For every chain's output node, find all chains with a matching
     * input node and create a route. This is the core of MDE:
     * type-based wiring with zero configuration.
     */
    for (i = 0; i < MAX_CHAINS; i++) {
        src = chain_get(i);
        if (!src) continue;

        for (j = 0; j < src->node_count; j++) {
            /* Skip nodes with empty output type */
            if (src->nodes[j].output_type[0] == '\0')
                continue;

            for (k = 0; k < MAX_CHAINS; k++) {
                if (k == i) continue;  /* Don't route to self */
                dst = chain_get(k);
                if (!dst) continue;

                for (m = 0; m < dst->node_count; m++) {
                    if (dst->nodes[m].input_type[0] == '\0')
                        continue;

                    if (mde_str_equal(src->nodes[j].output_type,
                                      dst->nodes[m].input_type)) {
                        int r = mde_add_route(i, j, k, m, 1.0f);
                        if (r >= 0)
                            created++;
                    }
                }
            }
        }
    }

    kputs("[mde] auto-route: ");
    kput_dec((uint64_t)created);
    kputs(" routes created\n");

    return created;
}

/* ── Find Routes From ───────────────────────────────────────────── */

int mde_find_routes_from(int chain_id, mde_route_t *out, int max)
{
    int count = 0;
    int i;

    for (i = 0; i < MDE_MAX_ROUTES && count < max; i++) {
        if (mde.routes[i].active && mde.routes[i].from_chain == chain_id) {
            out[count] = mde.routes[i];
            count++;
        }
    }
    return count;
}

/* ── Find Routes To ─────────────────────────────────────────────── */

int mde_find_routes_to(int chain_id, mde_route_t *out, int max)
{
    int count = 0;
    int i;

    for (i = 0; i < MDE_MAX_ROUTES && count < max; i++) {
        if (mde.routes[i].active && mde.routes[i].to_chain == chain_id) {
            out[count] = mde.routes[i];
            count++;
        }
    }
    return count;
}

/* ── Route Count ────────────────────────────────────────────────── */

int mde_route_count(void)
{
    return mde.route_count;
}

/* ── B3 Best Route ─────────────────────────────────────────────── */

/*
 * Build a temporary b3_state_t from a chain's inline B3 fields.
 * The chain stores alpha/beta/observations directly (not a b3_state_t),
 * so we bridge the gap here.
 */
static void mde_chain_to_b3(chain_t *c, b3_state_t *s)
{
    s->alpha        = c->b3_alpha;
    s->beta         = c->b3_beta;
    s->observations = c->b3_observations;
    s->successes    = 0;       /* not tracked separately in chain_t */
    s->prior_dr     = 0.5f;
    s->confidence_k = 2.0f;
}

int mde_best_route(const char *signal_type)
{
    int candidates[MAX_CHAINS];
    float predictions[MAX_CHAINS];
    int count = 0;
    int i, j;
    chain_t *c;
    b3_state_t tmp;

    /* 1. Find all chains that produce signal_type */
    for (i = 0; i < MAX_CHAINS; i++) {
        c = chain_get(i);
        if (!c) continue;

        for (j = 0; j < c->node_count; j++) {
            if (mde_str_equal(c->nodes[j].output_type, signal_type)) {
                mde_chain_to_b3(c, &tmp);
                candidates[count] = i;
                predictions[count] = b3_predict(&tmp);
                count++;
                break;  /* one match per chain is enough */
            }
        }
    }

    if (count == 0)
        return -1;

    if (count == 1)
        return candidates[0];

    /* 2. Epsilon-greedy exploration, scaled by confidence */
    {
        /*
         * Effective epsilon = explore_factor * (1 - avg_confidence)
         * As the system gets more confident, it explores less.
         */
        float avg_conf = 0.0f;
        float eff_epsilon;
        unsigned int roll;

        for (i = 0; i < count; i++) {
            c = chain_get(candidates[i]);
            mde_chain_to_b3(c, &tmp);
            avg_conf += b3_confidence(&tmp);
        }
        avg_conf /= (float)count;

        eff_epsilon = mde_explore_factor * (1.0f - avg_conf);
        if (eff_epsilon < 0.0f) eff_epsilon = 0.0f;
        if (eff_epsilon > 1.0f) eff_epsilon = 1.0f;

        /* Roll the dice: explore? */
        roll = mde_rand() % 1000;
        if (roll < (unsigned int)(eff_epsilon * 1000.0f)) {
            /* Explore: pick random candidate */
            int pick = (int)(mde_rand() % (unsigned int)count);

            kputs("[mde] B3 EXPLORE: ");
            kputs(signal_type);
            kputs(" -> chain ");
            kput_dec((uint64_t)candidates[pick]);
            kputs(" (random)\n");

            return candidates[pick];
        }
    }

    /* 3. Exploit: find the best prediction */
    {
        float best_pred = -1.0f;
        int   best_idx  = 0;
        int   tied      = 0;

        for (i = 0; i < count; i++) {
            if (predictions[i] > best_pred) {
                best_pred = predictions[i];
                best_idx  = i;
                tied      = 1;
            } else if (predictions[i] == best_pred) {
                tied++;
            }
        }

        /* If all tied (e.g. all at prior 0.5), round-robin */
        if (tied == count) {
            int pick = (int)(mde_rr_counter % (unsigned int)count);
            mde_rr_counter++;

            kputs("[mde] B3 round-robin: ");
            kputs(signal_type);
            kputs(" -> chain ");
            kput_dec((uint64_t)candidates[pick]);
            kputs(" (all equal)\n");

            return candidates[pick];
        }

        /* Log the B3 decision */
        {
            chain_t *winner = chain_get(candidates[best_idx]);
            int pct = (int)(best_pred * 100.0f);   /* E[f] as percentage */

            kputs("[mde] B3 routing ");
            kputs(signal_type);
            kputs(" -> ");
            if (winner) kputs(winner->name);
            kputs(" (E[f]=0.");
            if (pct < 10) kputc('0');
            kput_dec((uint64_t)pct);
            kputs(")\n");
        }

        return candidates[best_idx];
    }
}

/* ── Topological Sort (DFS-based) ───────────────────────────────── */

/*
 * Builds a resolve order so dependencies are resolved first.
 * If A -> B -> C, order is [A, B, C].
 * Detects cycles and skips them with a warning.
 */

#define TOPO_WHITE 0    /* Unvisited */
#define TOPO_GRAY  1    /* In progress (cycle detection) */
#define TOPO_BLACK 2    /* Finished */

static int  topo_color[MAX_CHAINS];
static int  topo_order[MAX_CHAINS];
static int  topo_count;
static int  topo_cycle;

static void topo_visit(int chain_id)
{
    int i;

    if (topo_color[chain_id] == TOPO_BLACK)
        return;

    if (topo_color[chain_id] == TOPO_GRAY) {
        /* Cycle detected. Only warn the first time we see this id; the
         * per-tick spam was burning serial bandwidth (each kputs cost
         * the budget overrun it claimed to be reporting). */
        static uint8_t s_cycle_warned[MAX_CHAINS];
        if (!topo_cycle) {
            if (!s_cycle_warned[chain_id]) {
                s_cycle_warned[chain_id] = 1;
                kputs("[mde] WARNING: cycle detected at chain ");
                kput_dec((uint64_t)chain_id);
                kputs(" (one-shot; further cycles silenced)\n");
            }
            topo_cycle = 1;
        }
        return;
    }

    topo_color[chain_id] = TOPO_GRAY;

    /* Visit all chains we route TO */
    for (i = 0; i < MDE_MAX_ROUTES; i++) {
        if (mde.routes[i].active && mde.routes[i].from_chain == chain_id) {
            topo_visit(mde.routes[i].to_chain);
        }
    }

    topo_color[chain_id] = TOPO_BLACK;
    topo_order[topo_count] = chain_id;
    topo_count++;
}

static int topo_sort(void)
{
    int i;

    topo_count = 0;
    topo_cycle = 0;

    for (i = 0; i < MAX_CHAINS; i++)
        topo_color[i] = TOPO_WHITE;

    /* Visit all chains that participate in routes */
    for (i = 0; i < MAX_CHAINS; i++) {
        if (!chain_get(i)) continue;
        if (topo_color[i] == TOPO_WHITE)
            topo_visit(i);
    }

    /*
     * topo_order is in reverse-finish order (post-order DFS).
     * Reverse it for correct dependency order.
     */
    {
        int lo = 0, hi = topo_count - 1;
        int tmp;
        while (lo < hi) {
            tmp = topo_order[lo];
            topo_order[lo] = topo_order[hi];
            topo_order[hi] = tmp;
            lo++;
            hi--;
        }
    }

    return topo_count;
}

/* ── Resolve All ────────────────────────────────────────────────── */

int mde_resolve_all(void)
{
    int count, i, id, err;
    int errors = 0;
    chain_t *c;
    b3_state_t tmp;

    count = topo_sort();

    /* Snapshot current scheduler tick once per pass for interval gating. */
    uint64_t now_tick = scheduler_tick_count();

    for (i = 0; i < count; i++) {
        id = topo_order[i];

        /*
         * Async / rate-limited resolution: if this chain has a non-zero
         * resolve_interval_ticks and not enough ticks have elapsed since
         * its last resolve, skip it this pass. This is what decouples
         * the compositor and per-display flush chains from the scheduler
         * tick cadence so cheap chains can resolve at full rate.
         */
        c = chain_get(id);
        if (c && c->resolve_interval_ticks > 0) {
            uint64_t since = now_tick - c->last_resolved_tick;
            if (since < (uint64_t)c->resolve_interval_ticks) {
                continue;
            }
        }

        /*
         * Before resolving: if multiple chains produce the same type
         * that feeds into this chain, B3 already selected which upstream
         * to use (via mde_resolve_chain propagation). Here we just
         * resolve in order and track B3 outcomes.
         */
        err = chain_resolve(id);
        if (c) c->last_resolved_tick = now_tick;

        /* Update B3 beliefs for this chain */
        c = chain_get(id);
        if (c) {
            mde_chain_to_b3(c, &tmp);
            if (err == 0) {
                b3_observe(&tmp, 1);
            } else {
                b3_observe(&tmp, 0);
            }
            c->b3_alpha        = tmp.alpha;
            c->b3_beta         = tmp.beta;
            c->b3_observations = tmp.observations;
        }

        if (err != 0)
            errors++;
    }

    return errors;
}

/* ── Resolve Chain (single + propagate) ─────────────────────────── */

int mde_resolve_chain(int chain_id)
{
    int err;
    int i;
    chain_t *c;
    b3_state_t tmp;

    err = chain_resolve(chain_id);

    /* Update B3 beliefs based on resolve outcome */
    c = chain_get(chain_id);
    if (c) {
        mde_chain_to_b3(c, &tmp);
        if (err == 0) {
            b3_observe(&tmp, 1);    /* success */
        } else {
            b3_observe(&tmp, 0);    /* failure */
        }
        /* Write back to chain */
        c->b3_alpha        = tmp.alpha;
        c->b3_beta         = tmp.beta;
        c->b3_observations = tmp.observations;
    }

    if (err != 0)
        return err;

    /*
     * After resolving, propagate: resolve all chains
     * that receive output from this chain.
     *
     * When multiple routes exist for the same signal type,
     * use B3 to pick the best downstream chain.
     */
    {
        /*
         * Collect unique signal types from this chain's outgoing routes,
         * then for each type, use mde_best_route to pick the winner.
         * Routes not selected by B3 are skipped this cycle.
         */
        char  seen_types[MDE_MAX_FUSE][32];
        int   seen_best[MDE_MAX_FUSE];     /* best chain_id per type */
        int   seen_count = 0;

        for (i = 0; i < MDE_MAX_ROUTES; i++) {
            if (!mde.routes[i].active)
                continue;
            if (mde.routes[i].from_chain != chain_id)
                continue;

            /* Check if we already handled this signal type */
            {
                int already = 0;
                int matched_idx = -1;
                int s;
                for (s = 0; s < seen_count; s++) {
                    if (mde_str_equal(seen_types[s],
                                       mde.routes[i].signal_type)) {
                        already = 1;
                        matched_idx = s;
                        break;
                    }
                }
                if (already) {
                    /* Only resolve if this route's target is the B3 winner */
                    if (matched_idx >= 0 &&
                        mde.routes[i].to_chain == seen_best[matched_idx])
                        chain_resolve(mde.routes[i].to_chain);
                    continue;
                }
            }

            /* New signal type -- count how many routes share it */
            {
                int same_type = 0;
                int j2;
                for (j2 = 0; j2 < MDE_MAX_ROUTES; j2++) {
                    if (mde.routes[j2].active &&
                        mde.routes[j2].from_chain == chain_id &&
                        mde_str_equal(mde.routes[j2].signal_type,
                                       mde.routes[i].signal_type)) {
                        same_type++;
                    }
                }

                if (same_type > 1 && seen_count < MDE_MAX_FUSE) {
                    /* Multiple routes for this type -- B3 decides */
                    int best = mde_best_route(mde.routes[i].signal_type);

                    mde_str_copy(seen_types[seen_count],
                                 mde.routes[i].signal_type, 32);
                    seen_best[seen_count] = best;
                    seen_count++;

                    if (best >= 0)
                        chain_resolve(best);
                } else {
                    /* Only one route -- just resolve it directly */
                    chain_resolve(mde.routes[i].to_chain);
                }
            }
        }
    }

    return 0;
}

/* ── Fuse ───────────────────────────────────────────────────────── */

int mde_fuse(int chain_ids[], int count, void *output)
{
    /*
     * Fusion: weighted average of outputs from multiple chains.
     * This is a placeholder -- real fusion depends on the signal type.
     * For now, we treat outputs as float arrays and do weighted average.
     *
     * The caller provides an output buffer. Each chain's last node
     * output is combined with the given weight from its routes.
     */
    float *out;
    int i, j;
    float total_weight;
    int valid;

    if (!output || count <= 0 || count > MDE_MAX_FUSE)
        return -1;

    out = (float *)output;
    total_weight = 0.0f;
    valid = 0;

    /* Zero the output (assume float-sized result for now) */
    out[0] = 0.0f;

    for (i = 0; i < count; i++) {
        chain_t *c = chain_get(chain_ids[i]);
        if (!c || c->node_count == 0)
            continue;

        /* Find the weight -- look for any route from this chain */
        float w = 1.0f;
        for (j = 0; j < MDE_MAX_ROUTES; j++) {
            if (mde.routes[j].active &&
                mde.routes[j].from_chain == chain_ids[i]) {
                w = mde.routes[j].weight;
                break;
            }
        }

        /*
         * Placeholder: accumulate weight. Real implementation would
         * read the chain's output buffer and combine signal data.
         */
        total_weight += w;
        valid++;
    }

    if (valid == 0)
        return -1;

    kputs("[mde] fuse: ");
    kput_dec((uint64_t)valid);
    kputs(" chains, total_weight=");
    kput_dec((uint64_t)total_weight);
    kputc('\n');

    return 0;
}

/* ── Dump Graph ─────────────────────────────────────────────────── */

void mde_dump_graph(void)
{
    int i;
    int active = 0;
    chain_t *fc, *tc;

    kputs("── MDE Route Graph ──\n");
    kputs("  routes: ");
    kput_dec((uint64_t)mde.route_count);
    kputs("  auto_route: ");
    kputs(mde.auto_route ? "ON" : "OFF");
    kputc('\n');

    for (i = 0; i < MDE_MAX_ROUTES; i++) {
        if (!mde.routes[i].active)
            continue;

        active++;
        fc = chain_get(mde.routes[i].from_chain);
        tc = chain_get(mde.routes[i].to_chain);

        kputs("  [");
        kput_dec((uint64_t)i);
        kputs("] ");

        if (fc) {
            kputs(fc->name);
        } else {
            kputc('?');
        }
        kputs("[");
        kput_dec((uint64_t)mde.routes[i].from_node);
        kputs("] --{");
        kputs(mde.routes[i].signal_type);
        kputs("}--> ");

        if (tc) {
            kputs(tc->name);
        } else {
            kputc('?');
        }
        kputs("[");
        kput_dec((uint64_t)mde.routes[i].to_node);
        kputs("]");

        /* Print weight if not 1.0 */
        if (mde.routes[i].weight != 1.0f) {
            kputs(" w=");
            kput_dec((uint64_t)(mde.routes[i].weight * 100.0f));
            kputs("%");
        }

        kputc('\n');
    }

    if (active == 0)
        kputs("  (no active routes)\n");

    kputs("── end ──\n");
}
