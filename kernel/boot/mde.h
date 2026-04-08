/*
 * Zeos -- MDE (Mixing Desk Engine)
 *
 * The kernel's routing layer. MDE routes signals between chains
 * by TYPE MATCHING: if chain A outputs "input_event" and chain B
 * accepts "input_event", MDE wires them automatically.
 *
 * Key concepts:
 *   - Type-based routing (output_type -> input_type matching)
 *   - Fusion (weighted combination of multiple same-type outputs)
 *   - Route graph (complete wiring diagram of chain connections)
 */

#ifndef ZEOS_MDE_H
#define ZEOS_MDE_H

#include "chain.h"

/* ── Limits ─────────────────────────────────────────────────────── */

#define MDE_MAX_ROUTES  256
#define MDE_MAX_FUSE    16      /* Max chains in a single fuse operation */

/* ── Route ──────────────────────────────────────────────────────── */

typedef struct {
    int   from_chain;
    int   from_node;
    int   to_chain;
    int   to_node;
    char  signal_type[32];
    float weight;               /* Fusion: contribution weight (0.0-1.0) */
    int   active;
} mde_route_t;

/* ── MDE State ──────────────────────────────────────────────────── */

typedef struct {
    mde_route_t routes[MDE_MAX_ROUTES];
    int         route_count;
    int         auto_route;     /* If true, MDE auto-discovers routes by type matching */
} mde_state_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize the routing engine */
void mde_init(void);

/* Scan all chains, create routes where output_type matches input_type */
int  mde_auto_route(void);

/* Add a manual route. Returns route index or -1. */
int  mde_add_route(int from_chain, int from_node,
                   int to_chain, int to_node, float weight);

/* Remove a route by index */
void mde_remove_route(int index);

/* Resolve all chains in dependency order (topological sort) */
int  mde_resolve_all(void);

/* Resolve one chain and propagate outputs to connected chains */
int  mde_resolve_chain(int chain_id);

/* Find all routes originating from a chain. Returns count.
   Results written to out[], up to max entries. */
int  mde_find_routes_from(int chain_id, mde_route_t *out, int max);

/* Find all routes targeting a chain. Returns count.
   Results written to out[], up to max entries. */
int  mde_find_routes_to(int chain_id, mde_route_t *out, int max);

/* Total active routes */
int  mde_route_count(void);

/* Print the full route graph to kprint */
void mde_dump_graph(void);

/* Fuse outputs from multiple chains (weighted combination).
   chain_ids: array of chain ids to fuse
   count: number of chains
   output: buffer for fused result
   Returns 0 on success, -1 on error. */
int  mde_fuse(int chain_ids[], int count, void *output);

/* ── B3-Informed Routing ───────────────────────────────────────── */

/*
 * Find the best chain that produces signal_type, using B3 beliefs.
 *
 * Evaluates E[f] = alpha / (alpha + beta) for each candidate.
 * Returns the chain_id with highest E[f] (most reliable).
 * If predictions are equal (both at prior), round-robins to explore.
 * With probability mde_explore_factor, picks a random candidate instead.
 *
 * Returns chain_id or -1 if no chain produces signal_type.
 */
int  mde_best_route(const char *signal_type);

/*
 * Exploration factor for B3 routing (epsilon-greedy).
 *
 * 0.0 = always pick the B3 best.
 * 1.0 = always pick random.
 * Default: 0.1 (10% exploration).
 *
 * Effective exploration decreases as B3 confidence increases:
 *   effective_epsilon = explore_factor * (1.0 - confidence)
 */
extern float mde_explore_factor;

#endif /* ZEOS_MDE_H */
