/* zplus — SFG allocation and management */
#include "sfg.h"
#include <string.h>
#include <stdio.h>

sfg_program_t *sfg_alloc(arena_t *a) {
    sfg_program_t *g = (sfg_program_t*)arena_alloc(a, sizeof(sfg_program_t));
    if (g) { g->arena = a; g->nnodes = g->nedges = g->nchains = g->nstructs = 0; }
    return g;
}

int sfg_add_chain(sfg_program_t *g, const char *name, sfg_masq_t masq, int parent) {
    if (g->nchains >= SFG_MAX_CHAINS) return -1;
    int id = g->nchains++;
    sfg_chain_t *c = &g->chains[id];
    c->id = id; c->masq = masq; c->parent_chain = parent; c->nnodes = 0;
    snprintf(c->name, SFG_NAME_LEN, "%s", name);
    return id;
}

int sfg_add_node(sfg_program_t *g, int chain_id, sfg_node_kind_t kind,
                 const char *name, const char *sig_in, const char *sig_out) {
    if (g->nnodes >= SFG_MAX_NODES) return -1;
    int id = g->nnodes++;
    sfg_node_t *n = &g->nodes[id];
    n->id = id; n->kind = kind; n->chain_id = chain_id;
    snprintf(n->name,    SFG_NAME_LEN, "%s", name);
    snprintf(n->sig_in,  SFG_NAME_LEN, "%s", sig_in  ? sig_in  : "signal");
    snprintf(n->sig_out, SFG_NAME_LEN, "%s", sig_out ? sig_out : "signal");
    n->handler_body = NULL; n->gate_cond = NULL;
    n->emit_value = NULL;   n->state_type = NULL;

    /* attach to chain */
    sfg_chain_t *c = sfg_get_chain(g, chain_id);
    if (c && c->nnodes < SFG_MAX_NODES) {
        n->seq = c->nnodes;
        c->node_ids[c->nnodes++] = id;
    }
    return id;
}

int sfg_add_edge(sfg_program_t *g, sfg_edge_kind_t kind,
                 int from_node, int to_node, const char *sig_type) {
    if (g->nedges >= SFG_MAX_EDGES) return -1;
    int id = g->nedges++;
    sfg_edge_t *e = &g->edges[id];
    e->id = id; e->kind = kind; e->from_node = from_node; e->to_node = to_node;
    snprintf(e->sig_type, SFG_NAME_LEN, "%s", sig_type ? sig_type : "signal");
    return id;
}

sfg_chain_t *sfg_get_chain(sfg_program_t *g, int id) {
    if (id < 0 || id >= g->nchains) return NULL;
    return &g->chains[id];
}

sfg_node_t *sfg_get_node(sfg_program_t *g, int id) {
    if (id < 0 || id >= g->nnodes) return NULL;
    return &g->nodes[id];
}

static const char *node_kind_name(sfg_node_kind_t k) {
    switch (k) {
    case SFG_SOURCE:    return "SOURCE";
    case SFG_PROCESSOR: return "PROCESSOR";
    case SFG_GATE:      return "GATE";
    case SFG_FORK:      return "FORK";
    case SFG_MERGE:     return "MERGE";
    case SFG_TAP:       return "TAP";
    case SFG_SINK:      return "SINK";
    default:            return "?";
    }
}

void sfg_dump(sfg_program_t *g) {
    printf("=== SFG: %s ===\n", g->source_name ? g->source_name : "(unnamed)");
    printf("chains: %d  nodes: %d  edges: %d  structs: %d\n",
           g->nchains, g->nnodes, g->nedges, g->nstructs);
    for (int i = 0; i < g->nchains; i++) {
        sfg_chain_t *c = &g->chains[i];
        printf("  chain[%d] %s (masq=%d parent=%d)\n", i, c->name, c->masq, c->parent_chain);
        for (int j = 0; j < c->nnodes; j++) {
            sfg_node_t *n = sfg_get_node(g, c->node_ids[j]);
            if (n) printf("    [%d] %-12s %-10s %-12s -> %-12s%s\n",
                          n->seq, node_kind_name(n->kind), n->name,
                          n->sig_in, n->sig_out,
                          n->gate_cond ? " (gate)" : "");
        }
    }
    printf("  edges:\n");
    for (int i = 0; i < g->nedges; i++) {
        sfg_edge_t *e = &g->edges[i];
        const char *ek = "->";
        if (e->kind == SFG_EDGE_TAP)      ek = "~>";
        if (e->kind == SFG_EDGE_SEVER)    ek = "-x>";
        if (e->kind == SFG_EDGE_EXCHANGE) ek = "<->";
        printf("    node[%d] %s node[%d]  (%s)\n",
               e->from_node, ek, e->to_node, e->sig_type);
    }
}
