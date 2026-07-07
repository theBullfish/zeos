/* zplus — SFG -> ZIR serialization. See sfg_serialize.h and ZIR.md. */
#include "sfg_serialize.h"
#include <string.h>

static const char *kind_str(sfg_node_kind_t k) {
    switch (k) {
        case SFG_SOURCE:    return "source";
        case SFG_PROCESSOR: return "processor";
        case SFG_GATE:      return "gate";
        case SFG_FORK:      return "fork";
        case SFG_MERGE:     return "merge";
        case SFG_TAP:       return "tap";
        case SFG_SINK:      return "sink";
    }
    return "processor";
}

static const char *edge_kind_str(sfg_edge_kind_t k) {
    switch (k) {
        case SFG_EDGE_FLOW:     return "flow";
        case SFG_EDGE_TAP:      return "tap";
        case SFG_EDGE_SEVER:    return "sever";
        case SFG_EDGE_EXCHANGE: return "exchange";
    }
    return "flow";
}

static const char *masq_str(sfg_masq_t m) {
    switch (m) {
        case SFG_MASQ_SOVEREIGN: return "sovereign";
        case SFG_MASQ_INTERNAL:  return "internal";
        case SFG_MASQ_REFERENCE: return "reference";
    }
    return "internal";
}

/* Print a JSON-escaped string (with surrounding quotes). */
static void put_json_str(FILE *out, const char *s) {
    fputc('"', out);
    if (s) {
        for (const char *p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            switch (c) {
                case '"':  fputs("\\\"", out); break;
                case '\\': fputs("\\\\", out); break;
                case '\n': fputs("\\n", out);  break;
                case '\r': fputs("\\r", out);  break;
                case '\t': fputs("\\t", out);  break;
                default:
                    if (c < 0x20) fprintf(out, "\\u%04x", c);
                    else fputc(c, out);
            }
        }
    }
    fputc('"', out);
}

void sfg_to_zir(sfg_program_t *g, FILE *out) {
    fputs("{\n", out);
    fputs("  \"zir\": 1,\n", out);
    fputs("  \"source\": ", out);
    put_json_str(out, g->source_name ? g->source_name : "");
    fputs(",\n", out);

    /* chains */
    fputs("  \"chains\": [\n", out);
    for (int i = 0; i < g->nchains; i++) {
        sfg_chain_t *ch = &g->chains[i];
        fprintf(out, "    { \"id\": %d, \"name\": ", ch->id);
        put_json_str(out, ch->name);
        fprintf(out, ", \"masq\": \"%s\", \"parent\": %d, \"nodes\": [",
                masq_str(ch->masq), ch->parent_chain);
        for (int j = 0; j < ch->nnodes; j++)
            fprintf(out, "%s%d", j ? ", " : "", ch->node_ids[j]);
        fprintf(out, "] }%s\n", i + 1 < g->nchains ? "," : "");
    }
    fputs("  ],\n", out);

    /* nodes */
    fputs("  \"nodes\": [\n", out);
    for (int i = 0; i < g->nnodes; i++) {
        sfg_node_t *n = &g->nodes[i];
        fprintf(out, "    { \"id\": %d, \"chain\": %d, \"seq\": %d, \"kind\": \"%s\", \"verb\": ",
                n->id, n->chain_id, n->seq, kind_str(n->kind));
        /* The C SFG stores the operation in the node name. */
        put_json_str(out, n->name);
        fprintf(out, ", \"name\": ");
        put_json_str(out, n->name);
        fprintf(out, ", \"sig_in\": ");
        put_json_str(out, n->sig_in);
        fprintf(out, ", \"sig_out\": ");
        put_json_str(out, n->sig_out);
        fputs(", \"args\": []", out);

        if (n->kind == SFG_SOURCE && n->emit_value) {
            fputs(", \"emit\": {\"str\": ", out);
            put_json_str(out, n->emit_value);
            fputs("}", out);
        }
        if (n->kind == SFG_GATE && n->gate_cond) {
            /* The transpiler renders the gate condition as a C expression
             * string, not a structured op — carry it as an expr. The kernel
             * loader treats an op-less gate as an honest passthrough. */
            fputs(", \"gate\": {\"expr\": ", out);
            put_json_str(out, n->gate_cond);
            fputs("}", out);
        }
        if (n->kind == SFG_MERGE) {
            /* NOTE: the C SFG node does not yet carry the chord policy
             * (sfg_node_t has no policy field) — a real divergence from the
             * Rust AST, which preserves it. Emit the default so the document
             * is valid; fixing the SFG to carry policy is tracked in ZIR.md. */
            fputs(", \"merge\": {\"policy\": \"all\"}", out);
        }
        fprintf(out, " }%s\n", i + 1 < g->nnodes ? "," : "");
    }
    fputs("  ],\n", out);

    /* edges */
    fputs("  \"edges\": [\n", out);
    for (int i = 0; i < g->nedges; i++) {
        sfg_edge_t *e = &g->edges[i];
        fprintf(out, "    { \"id\": %d, \"kind\": \"%s\", \"from\": %d, \"to\": %d, \"sig\": \"signal\" }%s\n",
                e->id, edge_kind_str(e->kind), e->from_node, e->to_node,
                i + 1 < g->nedges ? "," : "");
    }
    fputs("  ]\n", out);
    fputs("}\n", out);
}
