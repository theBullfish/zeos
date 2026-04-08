/*
 * Zeos -- MDE (Mixing Desk Engine) implementation
 *
 * Type-based signal routing between chains.
 * Static arrays, no malloc, bare-metal.
 */

#include "mde.h"
#include "kprint.h"

/* ── Static state ───────────────────────────────────────────────── */

static mde_state_t mde;

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
        /* Cycle detected */
        if (!topo_cycle) {
            kputs("[mde] WARNING: cycle detected at chain ");
            kput_dec((uint64_t)chain_id);
            kputc('\n');
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

    count = topo_sort();

    for (i = 0; i < count; i++) {
        id = topo_order[i];
        err = chain_resolve(id);
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

    err = chain_resolve(chain_id);
    if (err != 0)
        return err;

    /*
     * After resolving, propagate: resolve all chains
     * that receive output from this chain.
     */
    for (i = 0; i < MDE_MAX_ROUTES; i++) {
        if (mde.routes[i].active &&
            mde.routes[i].from_chain == chain_id) {
            chain_resolve(mde.routes[i].to_chain);
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
