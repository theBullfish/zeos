/* zplus — Signal Flow Graph IR
 *
 * The SFG is the compiler's native representation. A Z+ program IS a
 * signal flow graph; we lower the AST directly into this form. Each
 * SFG maps 1:1 to a set of kernel chain_create / chain_add_node calls.
 *
 * Topology: directed graph.
 *   - Nodes are signal processors (sources, gates, transforms, sinks).
 *   - Edges are typed signal wires (flow, tap, sever, exchange).
 *   - Forks are nodes with multiple out-edges.
 *   - Merges are nodes with multiple in-edges.
 *
 * Every SFG node has an associated handler_body (C source fragment)
 * that gets emitted by codegen. Built-in nodes (gate, emit, tap.log,
 * vault.put, etc.) have canned bodies. User-defined nodes get a stub.
 */
#pragma once
#include <stdint.h>
#include "arena.h"

#define SFG_MAX_NODES  1024
#define SFG_MAX_EDGES  2048
#define SFG_MAX_CHAINS  128
#define SFG_NAME_LEN    64

/* ── node kinds ─────────────────────────────────────────────────── */
typedef enum {
    SFG_SOURCE,      /* emits signals into the chain (input, emit(), fs(), ...) */
    SFG_PROCESSOR,   /* transforms signals (stdlib call, user node)             */
    SFG_GATE,        /* passes or blocks signals based on condition             */
    SFG_FORK,        /* fans one signal out to N downstream nodes               */
    SFG_MERGE,       /* collects N upstream signals into one                    */
    SFG_TAP,         /* read-only observer (does not block flow)                */
    SFG_SINK,        /* terminal node (no output signal)                        */
} sfg_node_kind_t;

/* ── edge kinds ─────────────────────────────────────────────────── */
typedef enum {
    SFG_EDGE_FLOW,     /* -> normal forward signal  */
    SFG_EDGE_TAP,      /* ~> read-only tap           */
    SFG_EDGE_SEVER,    /* -x> cut / disable wire     */
    SFG_EDGE_EXCHANGE, /* <-> bidirectional           */
} sfg_edge_kind_t;

/* ── masq tier (mirrors chain.h, but we don't include it here) ──── */
typedef enum { SFG_MASQ_SOVEREIGN=0, SFG_MASQ_INTERNAL=1, SFG_MASQ_REFERENCE=2 } sfg_masq_t;

/* ── node ───────────────────────────────────────────────────────── */
typedef struct sfg_node {
    int              id;
    sfg_node_kind_t  kind;
    char             name[SFG_NAME_LEN];
    char             sig_in[SFG_NAME_LEN];    /* input signal type  */
    char             sig_out[SFG_NAME_LEN];   /* output signal type */

    /* C source fragment for the resolve function body.
     * NULL = emit a pass-through stub.  */
    char            *handler_body;

    /* gate condition — for SFG_GATE nodes only */
    char            *gate_cond;

    /* emit value — for SFG_SOURCE/emit nodes */
    char            *emit_value;

    /* node-private state struct name, if any */
    char            *state_type;

    int              chain_id;   /* which SFG chain owns this node */
    int              seq;        /* position within the chain (0-based) */
} sfg_node_t;

/* ── edge ───────────────────────────────────────────────────────── */
typedef struct {
    int              id;
    sfg_edge_kind_t  kind;
    int              from_node;   /* sfg_node_t.id */
    int              to_node;
    char             sig_type[SFG_NAME_LEN];
} sfg_edge_t;

/* ── chain (maps to a kernel chain_t) ──────────────────────────── */
typedef struct {
    int       id;
    char      name[SFG_NAME_LEN];
    sfg_masq_t masq;
    int       parent_chain;    /* -1 for root */

    /* ordered node list (node ids in resolve order) */
    int       node_ids[SFG_MAX_NODES];
    int       nnodes;
} sfg_chain_t;

/* ── struct declaration (forwarded to generated header) ─────────── */
typedef struct sfg_struct_field {
    char name[SFG_NAME_LEN];
    char type[SFG_NAME_LEN];
    int  optional;
    struct sfg_struct_field *next;
} sfg_struct_field_t;

typedef struct {
    char               name[SFG_NAME_LEN];
    sfg_struct_field_t *fields;
    int                 nfields;
} sfg_struct_t;

/* ── the whole program ──────────────────────────────────────────── */
typedef struct {
    arena_t      *arena;   /* borrowed from parser */

    sfg_node_t    nodes[SFG_MAX_NODES];
    int           nnodes;

    sfg_edge_t    edges[SFG_MAX_EDGES];
    int           nedges;

    sfg_chain_t   chains[SFG_MAX_CHAINS];
    int           nchains;

    sfg_struct_t  structs[64];
    int           nstructs;

    char         *source_name;   /* basename of .zp file */
    char         *init_fn_name;  /* e.g. "zp_kv_zeos_init" */
} sfg_program_t;

/* ── API ────────────────────────────────────────────────────────── */
sfg_program_t *sfg_alloc(arena_t *a);

int  sfg_add_chain(sfg_program_t *g, const char *name, sfg_masq_t masq, int parent);
int  sfg_add_node (sfg_program_t *g, int chain_id, sfg_node_kind_t kind,
                   const char *name, const char *sig_in, const char *sig_out);
int  sfg_add_edge (sfg_program_t *g, sfg_edge_kind_t kind,
                   int from_node, int to_node, const char *sig_type);

sfg_chain_t *sfg_get_chain(sfg_program_t *g, int id);
sfg_node_t  *sfg_get_node (sfg_program_t *g, int id);

void sfg_dump(sfg_program_t *g);
