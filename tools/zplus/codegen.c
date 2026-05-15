/* zplus — codegen: SFG → C source
 *
 * Output structure per Z+ program:
 *
 *   // ── generated header ──────────────────────────────────────────
 *   // struct typedefs from struct declarations
 *   void zp_<stem>_init(void);
 *
 *   // ── generated source ──────────────────────────────────────────
 *   #include "zp_runtime.h"   // thin shim over chain.h + vault.h + tap.h
 *
 *   // one resolve function per SFG node
 *   static void zp__<chain>__<name>__resolve(chain_node_t *self,
 *                                             void *input, void *output) { ... }
 *
 *   // init function: chain_create + chain_add_node calls
 *   void zp_<stem>_init(void) {
 *       int id_<chain> = chain_create("<chain>", -1, MASQ_INTERNAL);
 *       chain_add_node(id_<chain>, "<node>", "<sig_in>", "<sig_out>",
 *                      zp__<chain>__<name>__resolve);
 *       ...
 *   }
 */
#include "codegen.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ── safe C identifier from arbitrary string ─────────────────────── */
static void to_cident(char *dst, const char *src, int dstsz) {
    int j = 0;
    for (int i = 0; src[i] && j < dstsz-1; i++) {
        char c = src[i];
        if (isalnum((unsigned char)c) || c == '_') dst[j++] = c;
        else dst[j++] = '_';
    }
    dst[j] = '\0';
}

/* ── masq tier name ──────────────────────────────────────────────── */
static const char *masq_name(sfg_masq_t m) {
    switch (m) {
    case SFG_MASQ_SOVEREIGN:  return "MASQ_SOVEREIGN";
    case SFG_MASQ_INTERNAL:   return "MASQ_INTERNAL";
    case SFG_MASQ_REFERENCE:  return "MASQ_REFERENCE";
    default:                  return "MASQ_REFERENCE";
    }
}

/* ── type name mapping ───────────────────────────────────────────── */
static const char *zp_type_to_c(const char *t) {
    if (!t || !*t) return "void*";
    if (strcmp(t, "int")    == 0) return "long";
    if (strcmp(t, "float")  == 0) return "double";
    if (strcmp(t, "str")    == 0) return "const char*";
    if (strcmp(t, "bool")   == 0) return "int";
    if (strcmp(t, "void")   == 0) return "void";
    if (strcmp(t, "signal") == 0) return "void*";
    return "void*";
}

/* ── function name for a node's resolve fn ───────────────────────── */
static void resolve_fn_name(char *buf, int bufsz,
                             const char *chain_name, const char *node_name, int seq) {
    char cs[SFG_NAME_LEN], ns[SFG_NAME_LEN];
    to_cident(cs, chain_name, SFG_NAME_LEN);
    to_cident(ns, node_name,  SFG_NAME_LEN);
    snprintf(buf, (size_t)bufsz, "zp__%s__%s_%d__resolve", cs, ns, seq);
}

/* ── emit a single resolve function ─────────────────────────────── */
static void emit_resolve_fn(FILE *out, sfg_program_t *g, sfg_node_t *n) {
    /* find parent chain name */
    const char *cname = "unknown";
    sfg_chain_t *c = sfg_get_chain(g, n->chain_id);
    if (c) cname = c->name;

    char fname[128];
    resolve_fn_name(fname, sizeof(fname), cname, n->name, n->seq);

    fprintf(out,
        "static void %s(chain_node_t *self, void *input, void *output)\n"
        "{\n"
        "    (void)self; (void)input; (void)output;\n",
        fname);

    if (n->handler_body && n->handler_body[0]) {
        fprintf(out, "%s", n->handler_body);
    } else {
        /* default: pass-through for processors, NULL-output for sinks */
        switch (n->kind) {
        case SFG_SINK:
            fprintf(out, "    /* sink — no output */\n");
            break;
        case SFG_TAP:
            fprintf(out,
                "    zp_tap_log(\"%s\", input);\n"
                "    *(void**)output = input;\n", n->name);
            break;
        case SFG_GATE:
            fprintf(out,
                "    /* gate: condition not available — pass all */\n"
                "    *(void**)output = input;\n");
            break;
        default:
            fprintf(out, "    *(void**)output = input; /* pass-through */\n");
            break;
        }
    }

    fprintf(out, "}\n\n");
}

/* ── emit edge wiring comments ───────────────────────────────────── */
static void emit_edge_comments(FILE *out, sfg_program_t *g, int chain_id) {
    for (int i = 0; i < g->nedges; i++) {
        sfg_edge_t *e = &g->edges[i];
        sfg_node_t *fn = sfg_get_node(g, e->from_node);
        sfg_node_t *tn = sfg_get_node(g, e->to_node);
        if (!fn || !tn) continue;
        if (fn->chain_id != chain_id && tn->chain_id != chain_id) continue;
        const char *ek = "->";
        if (e->kind == SFG_EDGE_TAP)      ek = "~>";
        if (e->kind == SFG_EDGE_SEVER)    ek = "-x>";
        if (e->kind == SFG_EDGE_EXCHANGE) ek = "<->";
        fprintf(out, "    /* wire: %s %s %s (%s) */\n",
                fn->name, ek, tn->name, e->sig_type);
    }
}

/* ── emit init function ──────────────────────────────────────────── */
static void emit_init_fn(FILE *out, sfg_program_t *g) {
    fprintf(out, "void %s(void)\n{\n", g->init_fn_name ? g->init_fn_name : "zp_init");

    for (int ci = 0; ci < g->nchains; ci++) {
        sfg_chain_t *c = &g->chains[ci];
        if (c->nnodes == 0) continue;

        char cid_var[SFG_NAME_LEN];
        to_cident(cid_var, c->name, SFG_NAME_LEN);
        fprintf(out, "\n    /* ── chain: %s ──────────────────── */\n", c->name);
        fprintf(out, "    int id_%s = chain_create(\"%s\", %d, %s);\n",
                cid_var, c->name, c->parent_chain, masq_name(c->masq));
        fprintf(out, "    if (id_%s < 0) { /* chain table full */ return; }\n", cid_var);

        /* emit edge wiring comments */
        emit_edge_comments(out, g, ci);

        /* add nodes in sequence order */
        for (int ni = 0; ni < c->nnodes; ni++) {
            sfg_node_t *n = sfg_get_node(g, c->node_ids[ni]);
            if (!n) continue;

            char fname[128];
            resolve_fn_name(fname, sizeof(fname), c->name, n->name, n->seq);

            /* Sink nodes don't need a resolve function registered */
            const char *fn_arg = (n->kind == SFG_SINK) ? "NULL" : fname;

            fprintf(out,
                "    chain_add_node(id_%s, \"%s\", \"%s\", \"%s\", %s);\n",
                cid_var, n->name, n->sig_in, n->sig_out, fn_arg);
        }
    }

    fprintf(out, "}\n");
}

/* ── header emission ─────────────────────────────────────────────── */
void codegen_emit_header(sfg_program_t *g, FILE *out) {
    const char *sname = g->source_name ? g->source_name : "zp";
    /* guard */
    char guard[128]; to_cident(guard, sname, 128);
    for (int i = 0; guard[i]; i++) guard[i] = (char)toupper((unsigned char)guard[i]);

    fprintf(out,
        "/* zplus generated — %s */\n"
        "#ifndef ZP_%s_H\n"
        "#define ZP_%s_H\n\n"
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n",
        sname, guard, guard);

    /* struct typedefs */
    for (int i = 0; i < g->nstructs; i++) {
        sfg_struct_t *s = &g->structs[i];
        fprintf(out, "typedef struct {\n");
        sfg_struct_field_t *f = s->fields;
        while (f) {
            fprintf(out, "    %s %s;\n", zp_type_to_c(f->type), f->name);
            f = f->next;
        }
        fprintf(out, "} %s;\n\n", s->name);
    }

    /* init function declaration */
    fprintf(out, "void %s(void);\n\n",
            g->init_fn_name ? g->init_fn_name : "zp_init");

    fprintf(out,
        "#ifdef __cplusplus\n}\n#endif\n\n"
        "#endif /* ZP_%s_H */\n", guard);
}

/* ── main source emission ────────────────────────────────────────── */
void codegen_emit(sfg_program_t *g, FILE *out, cg_result_t *res) {
    (void)res;

    const char *sname = g->source_name ? g->source_name : "zp_program";

    fprintf(out,
        "/* zplus generated — %s\n"
        " * DO NOT EDIT — regenerate with: zplus %s\n"
        " * North Star: every chain is a typed signal graph, not a function call list.\n"
        " */\n\n"
        "#include \"zp_runtime.h\"\n"
        "#include <string.h>\n\n",
        sname, sname);

    /* struct typedefs — inline here so the .c is self-contained */
    for (int i = 0; i < g->nstructs; i++) {
        sfg_struct_t *s = &g->structs[i];
        fprintf(out, "typedef struct {\n");
        sfg_struct_field_t *f = s->fields;
        while (f) {
            fprintf(out, "    %s %s;\n", zp_type_to_c(f->type), f->name);
            f = f->next;
        }
        fprintf(out, "} %s;\n\n", s->name);
    }

    /* emit all resolve functions */
    fprintf(out, "/* ── signal node handlers ───────────────────────────────── */\n\n");
    for (int ci = 0; ci < g->nchains; ci++) {
        sfg_chain_t *c = &g->chains[ci];
        if (c->nnodes == 0) continue;
        fprintf(out, "/* chain: %s */\n", c->name);
        for (int ni = 0; ni < c->nnodes; ni++) {
            sfg_node_t *n = sfg_get_node(g, c->node_ids[ni]);
            if (n && n->kind != SFG_SINK) emit_resolve_fn(out, g, n);
        }
    }

    /* emit init function */
    fprintf(out,
        "/* ── init: wire the signal graph into the kernel ─────────── */\n\n");
    emit_init_fn(out, g);
}
