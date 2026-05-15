/* zplus — AST → Signal Flow Graph lowering
 *
 * Strategy:
 *   - Each top-level signal_decl and each chain_item becomes a sequence of
 *     SFG nodes connected by edges.
 *   - chain blocks map to SFG chains (kernel chain_t).
 *   - Top-level signal decls are collected into an implicit "main" chain.
 *   - Forks: the upstream node connects to multiple downstream first-nodes.
 *   - Merges: multiple upstream last-nodes connect to a single merge node.
 *   - Taps: edges marked SFG_EDGE_TAP; nodes of kind SFG_TAP.
 *
 * Pipeline lowering returns a (first_node_id, last_node_id) pair so the
 * caller can wire edges between adjacent pipelines.
 */
#include "ir.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ── type environment: named signal → output type ───────────────── */
#define TYPE_ENV_MAX 256
typedef struct {
    char name[SFG_NAME_LEN];
    char type[SFG_NAME_LEN];
} type_env_entry_t;

typedef struct {
    type_env_entry_t e[TYPE_ENV_MAX];
    int n;
} type_env_t;

static void tenv_set(type_env_t *env, const char *name, const char *type) {
    if (!name || !type || !*type || strcmp(type, "signal") == 0) return;
    for (int i = 0; i < env->n; i++) {
        if (strcmp(env->e[i].name, name) == 0) {
            snprintf(env->e[i].type, SFG_NAME_LEN, "%s", type); return;
        }
    }
    if (env->n >= TYPE_ENV_MAX) return;
    snprintf(env->e[env->n].name, SFG_NAME_LEN, "%s", name);
    snprintf(env->e[env->n].type, SFG_NAME_LEN, "%s", type);
    env->n++;
}

static const char *tenv_get(type_env_t *env, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < env->n; i++)
        if (strcmp(env->e[i].name, name) == 0) return env->e[i].type;
    return NULL;
}

/* ── context ────────────────────────────────────────────────────── */
typedef struct {
    sfg_program_t *g;
    arena_t       *arena;
    ir_result_t   *res;
    int            cur_chain;    /* which chain we're emitting into */
    type_env_t    *tenv;         /* named signal → type mapping     */
} ir_ctx_t;

static void ir_error(ir_ctx_t *ctx, ast_node_t *n, const char *fmt, ...) {
    if (ctx->res->nerrors >= IR_MAX_ERRORS) return;
    ir_error_t *e = &ctx->res->errors[ctx->res->nerrors++];
    e->line = n ? n->line : 0; e->col = n ? n->col : 0;
    va_list ap;
    va_start(ap, fmt); vsnprintf(e->msg, sizeof(e->msg), fmt, ap); va_end(ap);
}

/* ── name helpers ────────────────────────────────────────────────── */

/* Flatten an AST_IDENT or AST_DOTTED to a string */
static const char *node_to_str(ast_node_t *n, char *buf, int bufsz) {
    if (!n) { snprintf(buf, (size_t)bufsz, "null"); return buf; }
    if (n->kind == AST_IDENT) {
        snprintf(buf, (size_t)bufsz, "%s", n->u.ident.name);
    } else if (n->kind == AST_DOTTED) {
        buf[0] = '\0';
        for (int i = 0; i < n->u.dotted.nparts; i++) {
            if (i) strncat(buf, ".", (size_t)(bufsz - (int)strlen(buf) - 1));
            strncat(buf, n->u.dotted.parts[i], (size_t)(bufsz - (int)strlen(buf) - 1));
        }
    } else {
        snprintf(buf, (size_t)bufsz, "<expr>");
    }
    return buf;
}

/* Sanitize a name for use as a C identifier */
static void sanitize(char *dst, const char *src, int dstsz) {
    int j = 0;
    for (int i = 0; src[i] && j < dstsz-1; i++) {
        char c = src[i];
        dst[j++] = (c == '.' || c == '-' || c == '/' || c == ' ') ? '_' : c;
    }
    dst[j] = '\0';
}

/* Build a unique node name: chain__label__seq */
static void make_node_name(char *buf, int bufsz,
                            const char *chain, const char *label, int seq) {
    snprintf(buf, (size_t)bufsz, "%s__%s__%d", chain, label, seq);
}

/* ── gate condition emitter ──────────────────────────────────────── */
/* Render a gate condition expression to a C boolean expression string */
static void render_cond(char *buf, int bufsz, ast_node_t *expr) {
    if (!expr) { snprintf(buf, (size_t)bufsz, "1"); return; }
    switch (expr->kind) {
    case AST_BINARY: {
        char L[128], R[128];
        render_cond(L, sizeof(L), expr->u.binary.left);
        render_cond(R, sizeof(R), expr->u.binary.right);
        const char *op = "==";
        switch (expr->u.binary.op) {
        case BINOP_EQ:  op = "=="; break; case BINOP_NEQ: op = "!="; break;
        case BINOP_LT:  op = "<";  break; case BINOP_GT:  op = ">";  break;
        case BINOP_LE:  op = "<="; break; case BINOP_GE:  op = ">="; break;
        case BINOP_ADD: op = "+";  break; case BINOP_SUB: op = "-";  break;
        case BINOP_RANGE:
            /* ~ operator: "field ~ pattern" = contains/matches */
            if (expr->u.binary.right &&
                expr->u.binary.right->kind == AST_STR_LIT) {
                snprintf(buf, (size_t)bufsz, "(strstr(%s, %s) != NULL)", L, R);
            } else {
                snprintf(buf, (size_t)bufsz, "(%s >= %s && %s <= %s)", L, R, L, R);
            }
            return;
        case BINOP_AND:
            snprintf(buf, (size_t)bufsz, "(%s && %s)", L, R); return;
        case BINOP_OR:
            snprintf(buf, (size_t)bufsz, "(%s || %s)", L, R); return;
        default: break;
        }
        /* String comparisons: use strcmp instead of == on char* */
        if (expr->u.binary.right && expr->u.binary.right->kind == AST_STR_LIT) {
            if (expr->u.binary.op == BINOP_EQ)
                snprintf(buf, (size_t)bufsz, "(strcmp(%s, %s) == 0)", L, R);
            else if (expr->u.binary.op == BINOP_NEQ)
                snprintf(buf, (size_t)bufsz, "(strcmp(%s, %s) != 0)", L, R);
            else
                snprintf(buf, (size_t)bufsz, "(%s %s %s)", L, op, R);
        } else {
            snprintf(buf, (size_t)bufsz, "(%s %s %s)", L, op, R);
        }
        break;
    }
    case AST_IDENT:
        /* bare ident in gate condition = field name on the signal (e.g. gate(level: error))
         * or the signal value itself (e.g. gate(> 20) emits _v on the left).
         * Use zp_field_str accessor so we don't need full type propagation. */
        if (strcmp(expr->u.ident.name, "_v") == 0)
            snprintf(buf, (size_t)bufsz, "zp_signal_dbl(input)");
        else
            snprintf(buf, (size_t)bufsz, "zp_field_str(input, \"%s\")", expr->u.ident.name);
        break;
    case AST_FIELD_ACCESS: {
        const char *fname = expr->u.fld.field ? expr->u.fld.field : "field";
        snprintf(buf, (size_t)bufsz, "zp_field_str(input, \"%s\")", fname);
        break;
    }
    case AST_INT_LIT:
        snprintf(buf, (size_t)bufsz, "%ld", expr->u.ival.ival);
        break;
    case AST_FLOAT_LIT:
        snprintf(buf, (size_t)bufsz, "%g", expr->u.fval.fval);
        break;
    case AST_STR_LIT:
        snprintf(buf, (size_t)bufsz, "\"%s\"", expr->u.sval.s ? expr->u.sval.s : "");
        break;
    case AST_DURATION_LIT:
        snprintf(buf, (size_t)bufsz, "%ldLL", expr->u.ival.ival);
        break;
    case AST_UNARY:
        if (expr->u.unary.op == UNOP_NOT) {
            char inner[128]; render_cond(inner, sizeof(inner), expr->u.unary.operand);
            snprintf(buf, (size_t)bufsz, "(!(%s))", inner);
        }
        break;
    default:
        snprintf(buf, (size_t)bufsz, "1 /*unsupported cond*/");
        break;
    }
}

/* ── emit value renderer ─────────────────────────────────────────── */
static void render_emit_val(char *buf, int bufsz, ast_node_t *expr) {
    if (!expr) { snprintf(buf, (size_t)bufsz, "0"); return; }
    switch (expr->kind) {
    case AST_INT_LIT:      snprintf(buf, (size_t)bufsz, "%ld", expr->u.ival.ival); break;
    case AST_FLOAT_LIT:    snprintf(buf, (size_t)bufsz, "%g",  expr->u.fval.fval); break;
    case AST_STR_LIT:      snprintf(buf, (size_t)bufsz, "\"%s\"",
                                    expr->u.sval.s ? expr->u.sval.s : ""); break;
    case AST_DURATION_LIT: snprintf(buf, (size_t)bufsz, "%ldLL", expr->u.ival.ival); break;
    case AST_STRUCT_INIT:
        /* emit type name only — handler body uses a zero-initialised static */
        snprintf(buf, (size_t)bufsz, "%s",
                 expr->u.sinit.type_name ? expr->u.sinit.type_name : "void");
        break;
    case AST_BINARY:
        /* comparison in emit context (e.g. on_silence(> 5m)) — extract RHS value */
        if (expr->u.binary.right)
            render_emit_val(buf, bufsz, expr->u.binary.right);
        else
            snprintf(buf, (size_t)bufsz, "0");
        break;
    default: snprintf(buf, (size_t)bufsz, "0"); break;
    }
}

/* ── typed condition renderer ────────────────────────────────────── */
/* Like render_cond but uses typed struct field access when sig_in is known. */
static void render_cond_typed(char *buf, int bufsz, ast_node_t *expr,
                               sfg_program_t *g, const char *sig_in);

static void render_cond_typed(char *buf, int bufsz, ast_node_t *expr,
                               sfg_program_t *g, const char *sig_in) {
    if (!expr) { snprintf(buf, (size_t)bufsz, "1"); return; }

    sfg_struct_t *st = (sig_in && *sig_in) ? sfg_find_struct(g, sig_in) : NULL;
    int is_num = sig_in && (strcmp(sig_in, "int")  == 0 ||
                             strcmp(sig_in, "long") == 0 ||
                             strcmp(sig_in, "float")== 0 ||
                             strcmp(sig_in, "double")== 0);

    switch (expr->kind) {
    case AST_BINARY: {
        char L[128], R[128];
        render_cond_typed(L, sizeof(L), expr->u.binary.left,  g, sig_in);
        render_cond_typed(R, sizeof(R), expr->u.binary.right, g, sig_in);
        switch (expr->u.binary.op) {
        case BINOP_RANGE:
            if (expr->u.binary.right && expr->u.binary.right->kind == AST_STR_LIT)
                snprintf(buf, (size_t)bufsz, "(strstr(%s, %s) != NULL)", L, R);
            else
                snprintf(buf, (size_t)bufsz, "(%s >= %s && %s <= %s)", L, R, L, R);
            return;
        case BINOP_AND: snprintf(buf,(size_t)bufsz,"(%s && %s)",L,R); return;
        case BINOP_OR:  snprintf(buf,(size_t)bufsz,"(%s || %s)",L,R); return;
        default: break;
        }
        const char *op = "==";
        switch (expr->u.binary.op) {
        case BINOP_EQ:  op="=="; break; case BINOP_NEQ: op="!="; break;
        case BINOP_LT:  op="<";  break; case BINOP_GT:  op=">";  break;
        case BINOP_LE:  op="<="; break; case BINOP_GE:  op=">="; break;
        case BINOP_ADD: op="+";  break; case BINOP_SUB: op="-";  break;
        default: break;
        }
        if (expr->u.binary.right && expr->u.binary.right->kind == AST_STR_LIT) {
            if (expr->u.binary.op == BINOP_EQ)
                snprintf(buf,(size_t)bufsz,"(strcmp(%s, %s) == 0)",L,R);
            else if (expr->u.binary.op == BINOP_NEQ)
                snprintf(buf,(size_t)bufsz,"(strcmp(%s, %s) != 0)",L,R);
            else
                snprintf(buf,(size_t)bufsz,"(%s %s %s)",L,op,R);
        } else {
            snprintf(buf,(size_t)bufsz,"(%s %s %s)",L,op,R);
        }
        break;
    }
    case AST_IDENT: {
        const char *iname = expr->u.ident.name;
        if (strcmp(iname, "_v") == 0) {
            /* implicit subject of bare comparison like gate(> 20) */
            if (is_num) snprintf(buf,(size_t)bufsz,"*(long*)input");
            else        snprintf(buf,(size_t)bufsz,"zp_signal_dbl(input)");
        } else if (st) {
            /* typed struct field access */
            snprintf(buf,(size_t)bufsz,"((%s*)input)->%s", sig_in, iname);
        } else {
            snprintf(buf,(size_t)bufsz,"zp_field_str(input, \"%s\")", iname);
        }
        break;
    }
    case AST_FIELD_ACCESS: {
        const char *fname = expr->u.fld.field ? expr->u.fld.field : "field";
        if (st) snprintf(buf,(size_t)bufsz,"((%s*)input)->%s", sig_in, fname);
        else    snprintf(buf,(size_t)bufsz,"zp_field_str(input, \"%s\")", fname);
        break;
    }
    case AST_INT_LIT:      snprintf(buf,(size_t)bufsz,"%ld",  expr->u.ival.ival); break;
    case AST_FLOAT_LIT:    snprintf(buf,(size_t)bufsz,"%g",   expr->u.fval.fval); break;
    case AST_STR_LIT:      snprintf(buf,(size_t)bufsz,"\"%s\"",
                                    expr->u.sval.s ? expr->u.sval.s : ""); break;
    case AST_DURATION_LIT: snprintf(buf,(size_t)bufsz,"%ldLL",expr->u.ival.ival); break;
    case AST_UNARY:
        if (expr->u.unary.op == UNOP_NOT) {
            char inner[128];
            render_cond_typed(inner,sizeof(inner),expr->u.unary.operand,g,sig_in);
            snprintf(buf,(size_t)bufsz,"(!(%s))",inner);
        }
        break;
    default:
        snprintf(buf,(size_t)bufsz,"1 /*unsupported cond*/");
        break;
    }
}

/* ── infer output type for parse() call ─────────────────────────── */
/* Match labeled arg names against declared struct field names.
 * Returns struct name if majority of args match a struct, else "signal". */
static const char *infer_parse_type(sfg_program_t *g, ast_node_t *call) {
    if (g->nstructs == 0) return "signal";
    char arg_names[32][SFG_NAME_LEN];
    int nargs = 0;
    ast_node_t *a = call->u.call.args;
    while (a && nargs < 32) {
        if (a->kind == AST_ARG) {
            /* labeled: level: ... */
            if (a->u.arg.label && a->u.arg.label[0])
                snprintf(arg_names[nargs++], SFG_NAME_LEN, "%s", a->u.arg.label);
            /* unlabeled with ident value: timestamp (field name is the ident) */
            else if (a->u.arg.val && a->u.arg.val->kind == AST_IDENT)
                snprintf(arg_names[nargs++], SFG_NAME_LEN, "%s", a->u.arg.val->u.ident.name);
        }
        a = a->next;
    }
    if (nargs == 0) return "signal";
    for (int si = 0; si < g->nstructs; si++) {
        sfg_struct_t *s = &g->structs[si];
        int matched = 0;
        for (int ai = 0; ai < nargs; ai++) {
            for (sfg_struct_field_t *f = s->fields; f; f = f->next)
                if (strcmp(f->name, arg_names[ai]) == 0) { matched++; break; }
        }
        if (matched > 0 && matched >= nargs / 2 + (nargs & 1))
            return s->name;
    }
    return "signal";
}

/* ── is the right-hand side of a comparison a string literal? ─────── */
static int rhs_is_string(ast_node_t *expr) {
    if (!expr) return 0;
    if (expr->kind == AST_BINARY)
        return rhs_is_string(expr->u.binary.right);
    return expr->kind == AST_STR_LIT;
}

/* ── stdlib node kind classifier ────────────────────────────────── */
typedef enum {
    STDLIB_UNKNOWN,
    STDLIB_TAP_LOG,       /* tap.log              */
    STDLIB_VAULT_PUT,     /* vault.put/store       */
    STDLIB_VAULT_GET,     /* vault.get/read        */
    STDLIB_BTREE_INSERT,  /* btree.insert          */
    STDLIB_BTREE_GET,     /* btree.get             */
    STDLIB_BTREE_DELETE,  /* btree.delete          */
    STDLIB_TIME_NOW,      /* time.now / time.now_ms*/
    STDLIB_HTTP_LISTEN,   /* http.listen           */
    STDLIB_STR_SPLIT,     /* str.split             */
    STDLIB_STR_LEN,       /* str.len_of_array      */
    STDLIB_SUM,           /* sum                   */
    STDLIB_COUNT,         /* count                 */
    STDLIB_RATE,          /* rate()                */
} stdlib_kind_t;

static stdlib_kind_t classify_stdlib(const char *name) {
    if (!name) return STDLIB_UNKNOWN;
    if (strcmp(name, "tap.log")        == 0) return STDLIB_TAP_LOG;
    if (strcmp(name, "vault.put")      == 0) return STDLIB_VAULT_PUT;
    if (strcmp(name, "vault.store")    == 0) return STDLIB_VAULT_PUT;
    if (strcmp(name, "vault.get")      == 0) return STDLIB_VAULT_GET;
    if (strcmp(name, "vault.read")     == 0) return STDLIB_VAULT_GET;
    if (strcmp(name, "btree.insert")   == 0) return STDLIB_BTREE_INSERT;
    if (strcmp(name, "btree.get")      == 0) return STDLIB_BTREE_GET;
    if (strcmp(name, "btree.delete")   == 0) return STDLIB_BTREE_DELETE;
    if (strcmp(name, "time.now")       == 0) return STDLIB_TIME_NOW;
    if (strcmp(name, "time.now_ms")    == 0) return STDLIB_TIME_NOW;
    if (strcmp(name, "http.listen")    == 0) return STDLIB_HTTP_LISTEN;
    if (strcmp(name, "str.split")      == 0) return STDLIB_STR_SPLIT;
    if (strcmp(name, "str.len_of_array")== 0) return STDLIB_STR_LEN;
    if (strcmp(name, "sum")            == 0) return STDLIB_SUM;
    if (strcmp(name, "count")          == 0) return STDLIB_COUNT;
    return STDLIB_UNKNOWN;
}

static const char *stdlib_body(stdlib_kind_t k) {
    switch (k) {
    case STDLIB_TAP_LOG:
        return "    zp_tap_log(self->name, input);\n"
               "    *(void**)output = input;\n";
    case STDLIB_VAULT_PUT:
        return "    zp_vault_put(self->name, input);\n"
               "    *(void**)output = input;\n";
    case STDLIB_VAULT_GET:
        return "    *(void**)output = zp_vault_get(self->name, input);\n";
    case STDLIB_BTREE_INSERT:
        return "    if (self->state) btree_insert((btree_t*)self->state, input);\n"
               "    *(void**)output = input;\n";
    case STDLIB_BTREE_GET:
        return "    *(void**)output = self->state ?\n"
               "        btree_get((btree_t*)self->state, input) : NULL;\n";
    case STDLIB_BTREE_DELETE:
        return "    if (self->state) btree_delete((btree_t*)self->state, input);\n"
               "    *(void**)output = input;\n";
    case STDLIB_TIME_NOW:
        return "    static uint64_t _t = 0; _t = zp_time_now_ms();\n"
               "    *(void**)output = &_t;\n";
    case STDLIB_HTTP_LISTEN:
        return "    zp_http_listen(self->state, input);\n"
               "    *(void**)output = input;\n";
    case STDLIB_STR_SPLIT:
        return "    *(void**)output = zp_str_split((const char*)input, \" \");\n";
    case STDLIB_STR_LEN:
        return "    static long _len = 0;\n"
               "    _len = zp_array_len(input);\n"
               "    *(void**)output = &_len;\n";
    case STDLIB_SUM:
        return "    static long _sum = 0;\n"
               "    _sum += *(long*)input;\n"
               "    *(void**)output = &_sum;\n";
    case STDLIB_COUNT:
        return "    static long _cnt = 0; _cnt++;\n"
               "    *(void**)output = &_cnt;\n";
    default:
        return "    /* TODO: stdlib stub */\n"
               "    *(void**)output = input;\n";
    }
}

/* ── pipeline lowering ───────────────────────────────────────────── */

typedef struct { int first; int last; } node_range_t;

static node_range_t lower_pipeline(ir_ctx_t *ctx, ast_node_t *pipe,
                                   const char *chain_name, const char *label);

/* Lower a single stage/atom into one SFG node.
 * Returns the node id. */
static int lower_stage(ir_ctx_t *ctx, ast_node_t *stage,
                       const char *chain_name, const char *label, int seq) {
    if (!stage) return -1;
    sfg_program_t *g = ctx->g;
    char nname[SFG_NAME_LEN];
    make_node_name(nname, SFG_NAME_LEN, chain_name, label, seq);

    switch (stage->kind) {

    /* ── input / output (source/sink keywords) ──────────────────── */
    case AST_IDENT: {
        const char *s = stage->u.ident.name;
        if (strcmp(s, "input") == 0)
            return sfg_add_node(g, ctx->cur_chain, SFG_SOURCE, "input", "signal", "signal");
        if (strcmp(s, "output") == 0)
            return sfg_add_node(g, ctx->cur_chain, SFG_SINK, "output", "signal", "void");
        /* look up named signal type in the type environment */
        const char *known = tenv_get(ctx->tenv, s);
        char sname[SFG_NAME_LEN]; sanitize(sname, s, SFG_NAME_LEN);
        if (known)
            return sfg_add_node(g, ctx->cur_chain, SFG_PROCESSOR, sname, known, known);
        return sfg_add_node(g, ctx->cur_chain, SFG_PROCESSOR, sname, "signal", "signal");
    }

    /* ── dotted stdlib calls ─────────────────────────────────────── */
    case AST_DOTTED: {
        char flat[SFG_NAME_LEN]; flat[0] = '\0';
        for (int i = 0; i < stage->u.dotted.nparts; i++) {
            if (i) strncat(flat, ".", SFG_NAME_LEN - strlen(flat) - 1);
            strncat(flat, stage->u.dotted.parts[i], SFG_NAME_LEN - strlen(flat) - 1);
        }
        char safe[SFG_NAME_LEN]; sanitize(safe, flat, SFG_NAME_LEN);
        stdlib_kind_t sk = classify_stdlib(flat);
        sfg_node_kind_t nk = (sk == STDLIB_TAP_LOG) ? SFG_TAP : SFG_PROCESSOR;
        int nid = sfg_add_node(g, ctx->cur_chain, nk, safe, "signal", "signal");
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) n->handler_body = (char*)arena_strdup(ctx->arena,
                                        stdlib_body(sk), (int)strlen(stdlib_body(sk)));
        }
        return nid;
    }

    /* ── function call ───────────────────────────────────────────── */
    case AST_CALL: {
        char fname[SFG_NAME_LEN];
        node_to_str(stage->u.call.func, fname, SFG_NAME_LEN);
        char safe[SFG_NAME_LEN]; sanitize(safe, fname, SFG_NAME_LEN);
        stdlib_kind_t sk = classify_stdlib(fname);
        /* parse() output type: match labeled args against declared structs */
        const char *out_type = "signal";
        if (strcmp(fname, "parse") == 0)
            out_type = infer_parse_type(g, stage);
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_PROCESSOR, safe,
                               "signal", out_type);
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n && sk != STDLIB_UNKNOWN)
                n->handler_body = (char*)arena_strdup(ctx->arena,
                                    stdlib_body(sk), (int)strlen(stdlib_body(sk)));
        }
        return nid;
    }

    /* ── gate ────────────────────────────────────────────────────── */
    case AST_GATE: {
        /* Store AST for the fixup pass that re-renders with typed access.
         * Initial body uses stub accessors; fixup replaces it once sig_in
         * is resolved by type propagation. */
        char cond[256] = "1";
        if (stage->u.op.expr) render_cond(cond, sizeof(cond), stage->u.op.expr);
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_GATE, nname, "signal", "signal");
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) {
                n->gate_cond_ast = stage;
                n->gate_cond = (char*)arena_strdup(ctx->arena, cond, (int)strlen(cond));
                char body[512];
                snprintf(body, sizeof(body),
                    "    if (!(%s)) { *(void**)output = NULL; return; }\n"
                    "    *(void**)output = input;\n", cond);
                n->handler_body = (char*)arena_strdup(ctx->arena, body, (int)strlen(body));
            }
        }
        return nid;
    }

    /* ── emit ────────────────────────────────────────────────────── */
    case AST_EMIT: {
        char val[256] = "0";
        if (stage->u.op.expr) render_emit_val(val, sizeof(val), stage->u.op.expr);
        /* sig_out = struct type name when emitting a struct, else "signal" */
        const char *emit_out = "signal";
        if (stage->u.op.expr && stage->u.op.expr->kind == AST_STRUCT_INIT &&
            stage->u.op.expr->u.sinit.type_name)
            emit_out = stage->u.op.expr->u.sinit.type_name;
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_SOURCE, nname, "void", emit_out);
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) {
                n->emit_value = (char*)arena_strdup(ctx->arena, val, (int)strlen(val));
                char body[256];
                if (stage->u.op.expr &&
                    stage->u.op.expr->kind == AST_STRUCT_INIT) {
                    /* struct emit: zero-init a static of the struct type */
                    snprintf(body, sizeof(body),
                        "    static %s _v;\n"
                        "    *(void**)output = &_v;\n", val);
                } else if (stage->u.op.expr &&
                           stage->u.op.expr->kind == AST_STR_LIT) {
                    /* string literal: emit const char* directly */
                    snprintf(body, sizeof(body),
                        "    *(void**)output = (void*)%s;\n", val);
                } else {
                    /* numeric / duration */
                    snprintf(body, sizeof(body),
                        "    static long _v = %s;\n"
                        "    *(void**)output = &_v;\n", val);
                }
                n->handler_body = (char*)arena_strdup(ctx->arena, body, (int)strlen(body));
            }
        }
        return nid;
    }

    /* ── on_silence ──────────────────────────────────────────────── */
    case AST_ON_SILENCE: {
        char ms_str[64] = "5000";
        if (stage->u.op.expr) render_emit_val(ms_str, sizeof(ms_str), stage->u.op.expr);
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_PROCESSOR, nname, "signal", "signal");
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) {
                char body[256];
                snprintf(body, sizeof(body),
                    "    // on_silence threshold: %s ms\n"
                    "    *(void**)output = input;\n", ms_str);
                n->handler_body = (char*)arena_strdup(ctx->arena, body, (int)strlen(body));
            }
        }
        return nid;
    }

    /* ── debounce ────────────────────────────────────────────────── */
    case AST_DEBOUNCE: {
        char ms_str[64] = "500";
        if (stage->u.op.expr) render_emit_val(ms_str, sizeof(ms_str), stage->u.op.expr);
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_PROCESSOR, nname, "signal", "signal");
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) {
                char body[512];
                snprintf(body, sizeof(body),
                    "    static uint64_t _last = 0;\n"
                    "    uint64_t _now = zp_time_now_ms();\n"
                    "    if (_now - _last < %s) { *(void**)output = NULL; return; }\n"
                    "    _last = _now;\n"
                    "    *(void**)output = input;\n", ms_str);
                n->handler_body = (char*)arena_strdup(ctx->arena, body, (int)strlen(body));
            }
        }
        return nid;
    }

    /* ── rate ────────────────────────────────────────────────────── */
    case AST_RATE: {
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_PROCESSOR, nname, "signal", "signal");
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) n->handler_body = (char*)
                "    static long _cnt = 0; _cnt++;\n"
                "    static long _rate = 0; _rate = _cnt; /* simplified rate */\n"
                "    *(void**)output = &_rate;\n";
        }
        return nid;
    }

    /* ── decay ───────────────────────────────────────────────────── */
    case AST_DECAY: {
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_PROCESSOR, nname, "signal", "signal");
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) n->handler_body = (char*)
                "    /* decay: attenuate signal over time */\n"
                "    *(void**)output = input;\n";
        }
        return nid;
    }

    /* ── tick ────────────────────────────────────────────────────── */
    case AST_TICK: {
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_SOURCE, nname, "void", "signal");
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) n->handler_body = (char*)
                "    static long _t = 1;\n"
                "    *(void**)output = &_t;\n";
        }
        return nid;
    }

    /* ── fork ────────────────────────────────────────────────────── */
    case AST_FORK: {
        /* Fork is handled at the pipeline level by wiring edges from the
         * upstream last-node to each branch's first-node. We still emit
         * a FORK node as the fan-out point. */
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_FORK, nname, "signal", "signal");
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) n->handler_body = (char*)
                "    /* fork: fan-out handled by edge wiring */\n"
                "    *(void**)output = input;\n";
        }
        /* Now lower each branch and connect fork-node -> branch-first */
        for (int i = 0; i < stage->u.fork.count && i < 64; i++) {
            ast_node_t *branch = stage->u.fork.branches[i];
            char blabel[SFG_NAME_LEN];
            snprintf(blabel, SFG_NAME_LEN, "%s_br%d", label, i);
            node_range_t br = lower_pipeline(ctx, branch, chain_name, blabel);
            if (br.first >= 0 && nid >= 0)
                sfg_add_edge(g, SFG_EDGE_FLOW, nid, br.first, "signal");
        }
        return nid;
    }

    /* ── merge ───────────────────────────────────────────────────── */
    case AST_MERGE: {
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_MERGE, nname, "signal", "signal");
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) n->handler_body = (char*)
                "    /* merge: collector — pass first non-null signal */\n"
                "    *(void**)output = input;\n";
        }
        return nid;
    }

    /* ── delta ───────────────────────────────────────────────────── */
    case AST_DELTA_EXPR: {
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_PROCESSOR, nname, "signal", "signal");
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) n->handler_body = (char*)
                "    static double _prev = 0.0;\n"
                "    double _cur = input ? *(double*)input : 0.0;\n"
                "    static double _delta = 0.0; _delta = _cur - _prev; _prev = _cur;\n"
                "    *(void**)output = &_delta;\n";
        }
        return nid;
    }

    /* ── struct init / int/float/str literals → source nodes ─────── */
    case AST_STRUCT_INIT: {
        const char *tname = stage->u.sinit.type_name ? stage->u.sinit.type_name : "void";
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_SOURCE, nname, "void", tname);
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) {
                n->emit_value = (char*)arena_strdup(ctx->arena, tname, (int)strlen(tname));
                char body[256];
                /* Zero-init a static instance of the struct and emit it */
                snprintf(body, sizeof(body),
                    "    static %s _v;\n"
                    "    *(void**)output = &_v;\n", tname);
                n->handler_body = (char*)arena_strdup(ctx->arena, body, (int)strlen(body));
            }
        }
        return nid;
    }
    case AST_INT_LIT:
    case AST_FLOAT_LIT:
    case AST_STR_LIT:
    case AST_DURATION_LIT: {
        char val[256]; render_emit_val(val, sizeof(val), stage);
        int nid = sfg_add_node(g, ctx->cur_chain, SFG_SOURCE, nname, "void", "signal");
        if (nid >= 0) {
            sfg_node_t *n = sfg_get_node(g, nid);
            if (n) {
                n->emit_value = (char*)arena_strdup(ctx->arena, val, (int)strlen(val));
                char body[256];
                if (stage->kind == AST_STR_LIT) {
                    snprintf(body, sizeof(body),
                        "    static const char *_v = %s;\n"
                        "    *(const void**)output = &_v;\n", val);
                } else {
                    snprintf(body, sizeof(body),
                        "    static long _v = %s;\n"
                        "    *(void**)output = &_v;\n", val);
                }
                n->handler_body = (char*)arena_strdup(ctx->arena, body, (int)strlen(body));
            }
        }
        return nid;
    }

    default:
        return sfg_add_node(g, ctx->cur_chain, SFG_PROCESSOR, nname, "signal", "signal");
    }
}

static node_range_t lower_pipeline(ir_ctx_t *ctx, ast_node_t *pipe,
                                   const char *chain_name, const char *label) {
    node_range_t r = {-1, -1};
    if (!pipe) return r;

    /* Unwrap single-stage pipelines */
    if (pipe->kind != AST_PIPELINE) {
        int nid = lower_stage(ctx, pipe, chain_name, label, 0);
        r.first = r.last = nid;
        return r;
    }

    int prev_nid = -1;
    edge_t prev_edge = EDGE_FLOW;

    for (int i = 0; i < pipe->u.pipeline.count; i++) {
        ast_node_t *stage = pipe->u.pipeline.stages[i];
        char slabel[SFG_NAME_LEN];
        snprintf(slabel, SFG_NAME_LEN, "%s", label);

        int nid = lower_stage(ctx, stage, chain_name, slabel, i);
        if (nid < 0) continue;

        if (i == 0) r.first = nid;
        r.last = nid;

        if (prev_nid >= 0) {
            sfg_edge_kind_t ek = SFG_EDGE_FLOW;
            switch (prev_edge) {
            case EDGE_TAP:      ek = SFG_EDGE_TAP;      break;
            case EDGE_SEVER:    ek = SFG_EDGE_SEVER;     break;
            case EDGE_EXCHANGE: ek = SFG_EDGE_EXCHANGE;  break;
            default:            ek = SFG_EDGE_FLOW;      break;
            }
            sfg_add_edge(ctx->g, ek, prev_nid, nid, "signal");
            /* propagate type from upstream to downstream */
            sfg_node_t *prev_n = sfg_get_node(ctx->g, prev_nid);
            sfg_node_t *cur_n  = sfg_get_node(ctx->g, nid);
            if (prev_n && cur_n && strcmp(prev_n->sig_out, "signal") != 0) {
                if (strcmp(cur_n->sig_in,  "signal") == 0)
                    snprintf(cur_n->sig_in,  SFG_NAME_LEN, "%s", prev_n->sig_out);
                if (cur_n->kind != SFG_SINK && strcmp(cur_n->sig_out, "signal") == 0)
                    snprintf(cur_n->sig_out, SFG_NAME_LEN, "%s", prev_n->sig_out);
            }
        }
        prev_nid  = nid;
        prev_edge = (i < pipe->u.pipeline.count-1) ? pipe->u.pipeline.edges[i] : EDGE_FLOW;
    }
    return r;
}

/* ── struct lowering ─────────────────────────────────────────────── */
static void lower_struct(ir_ctx_t *ctx, ast_node_t *s) {
    if (ctx->g->nstructs >= 64) return;
    sfg_struct_t *ss = &ctx->g->structs[ctx->g->nstructs++];
    snprintf(ss->name, SFG_NAME_LEN, "%s", s->u.strct.name ? s->u.strct.name : "");
    ss->fields = NULL; ss->nfields = 0;

    sfg_struct_field_t **tail = &ss->fields;
    ast_node_t *f = s->u.strct.fields;
    while (f) {
        sfg_struct_field_t *sf = (sfg_struct_field_t*)arena_alloc(ctx->arena, sizeof(*sf));
        if (sf) {
            snprintf(sf->name, SFG_NAME_LEN, "%s", f->u.field.name ? f->u.field.name : "");
            const char *tname = (f->u.field.type && f->u.field.type->kind == AST_TYPEREF)
                                ? f->u.field.type->u.typeref.name : "signal_t";
            snprintf(sf->type, SFG_NAME_LEN, "%s", tname ? tname : "signal_t");
            sf->optional = f->u.field.optional;
            sf->next = NULL;
            *tail = sf; tail = &sf->next; ss->nfields++;
        }
        f = f->next;
    }
}

/* ── chain lowering ──────────────────────────────────────────────── */
static void lower_chain(ir_ctx_t *ctx, ast_node_t *cn) {
    const char *cname = cn->u.chain.name ? cn->u.chain.name : "unnamed";
    int cid = sfg_add_chain(ctx->g, cname, SFG_MASQ_INTERNAL, -1);
    if (cid < 0) return;

    int saved_chain = ctx->cur_chain;
    ctx->cur_chain = cid;

    ast_node_t *item = cn->u.chain.items;
    while (item) {
        if (item->kind == AST_CHAIN_ITEM) {
            const char *label = item->u.chain_item.label ? item->u.chain_item.label : "item";
            lower_pipeline(ctx, item->u.chain_item.pipeline, cname, label);
        } else if (item->kind == AST_PIPELINE) {
            lower_pipeline(ctx, item, cname, "pipe");
        }
        item = item->next;
    }

    ctx->cur_chain = saved_chain;
}

/* ── main entry ──────────────────────────────────────────────────── */
sfg_program_t *ir_lower(ast_node_t *program, arena_t *arena,
                        const char *source_name, ir_result_t *res) {
    memset(res, 0, sizeof(*res));

    sfg_program_t *g = sfg_alloc(arena);
    if (!g) { return NULL; }

    /* extract basename for init function name */
    g->source_name = (char*)arena_strdup(arena, source_name, (int)strlen(source_name));

    /* derive init fn name: strip path + extension, prefix "zp_", suffix "_init" */
    const char *base = source_name;
    for (const char *p = source_name; *p; p++) if (*p == '/') base = p+1;
    char stem[256]; snprintf(stem, sizeof(stem), "%s", base);
    /* strip .zp */
    for (int i = (int)strlen(stem)-1; i >= 0; i--)
        if (stem[i] == '.') { stem[i] = '\0'; break; }
    /* sanitize dots/dashes */
    for (int i = 0; stem[i]; i++) if (stem[i] == '-' || stem[i] == '.') stem[i] = '_';

    char init_name[SFG_NAME_LEN];
    snprintf(init_name, SFG_NAME_LEN, "zp_%s_init", stem);
    g->init_fn_name = (char*)arena_strdup(arena, init_name, (int)strlen(init_name));

    type_env_t tenv; memset(&tenv, 0, sizeof(tenv));
    ir_ctx_t ctx = {g, arena, res, -1, &tenv};

    /* implicit "main" chain for top-level signal decls */
    int main_chain = sfg_add_chain(g, "main", SFG_MASQ_REFERENCE, -1);
    ctx.cur_chain = main_chain;

    /* Two-pass lowering: structs first so parse() type inference can see them. */

    /* Pass 1: lower struct declarations */
    ast_node_t *decl = program->u.list.items;
    while (decl) {
        if (decl->kind == AST_STRUCT) lower_struct(&ctx, decl);
        decl = decl->next;
    }

    /* Pass 2: lower everything else */
    decl = program->u.list.items;
    while (decl) {
        switch (decl->kind) {
        case AST_IMPORT:
            break;
        case AST_STRUCT:
            break; /* already done */
        case AST_CHAIN:
            lower_chain(&ctx, decl);
            break;
        case AST_SIGNAL_DECL: {
            const char *nm = decl->u.sig_decl.name ? decl->u.sig_decl.name : "sig";
            ctx.cur_chain = main_chain;
            node_range_t nr = lower_pipeline(&ctx, decl->u.sig_decl.rhs, "main", nm);
            /* record output type so downstream references can use it */
            if (nr.last >= 0) {
                sfg_node_t *last_n = sfg_get_node(g, nr.last);
                if (last_n && strcmp(last_n->sig_out, "signal") != 0)
                    tenv_set(&tenv, nm, last_n->sig_out);
            }
            break;
        }
        case AST_PIPELINE:
            ctx.cur_chain = main_chain;
            lower_pipeline(&ctx, decl, "main", "pipe");
            break;
        default:
            break;
        }
        decl = decl->next;
    }

    /* ── type propagation pass (3 iterations covers most chains) ── */
    for (int pass = 0; pass < 3; pass++) {
        for (int ei = 0; ei < g->nedges; ei++) {
            sfg_edge_t *e = &g->edges[ei];
            if (e->kind != SFG_EDGE_FLOW && e->kind != SFG_EDGE_TAP) continue;
            sfg_node_t *from = sfg_get_node(g, e->from_node);
            sfg_node_t *to   = sfg_get_node(g, e->to_node);
            if (!from || !to) continue;
            if (strcmp(from->sig_out, "signal") == 0) continue;
            if (strcmp(to->sig_in, "signal") == 0)
                snprintf(to->sig_in, SFG_NAME_LEN, "%s", from->sig_out);
            if (to->kind != SFG_SINK && strcmp(to->sig_out, "signal") == 0)
                snprintf(to->sig_out, SFG_NAME_LEN, "%s", from->sig_out);
        }
    }

    /* ── gate body fixup: re-render with typed field access ──────── */
    for (int i = 0; i < g->nnodes; i++) {
        sfg_node_t *n = &g->nodes[i];
        if (n->kind != SFG_GATE || !n->gate_cond_ast) continue;

        /* If sig_in is still unresolved, scan same-chain peers for a known
         * struct type (handles the case where a named signal uses 'input'
         * as its source and the type flows in via the connecting pipeline). */
        if (strcmp(n->sig_in, "signal") == 0) {
            sfg_chain_t *ch = sfg_get_chain(g, n->chain_id);
            if (ch) {
                for (int j = 0; j < ch->nnodes; j++) {
                    sfg_node_t *peer = sfg_get_node(g, ch->node_ids[j]);
                    if (!peer || peer->id == n->id) continue;
                    if (strcmp(peer->sig_in, "signal") != 0 &&
                        sfg_find_struct(g, peer->sig_in)) {
                        snprintf(n->sig_in, SFG_NAME_LEN, "%s", peer->sig_in);
                        break;
                    }
                }
            }
        }
        if (strcmp(n->sig_in, "signal") == 0) continue; /* still unknown */

        ast_node_t *gate_ast = (ast_node_t*)n->gate_cond_ast;
        ast_node_t *cond_expr = gate_ast->u.op.expr;

        char cond[512] = "1";
        if (cond_expr)
            render_cond_typed(cond, sizeof(cond), cond_expr, g, n->sig_in);

        n->gate_cond = (char*)arena_strdup(arena, cond, (int)strlen(cond));
        char body[640];
        snprintf(body, sizeof(body),
            "    if (!(%s)) { *(void**)output = NULL; return; }\n"
            "    *(void**)output = input;\n", cond);
        n->handler_body = (char*)arena_strdup(arena, body, (int)strlen(body));
    }

    return g;
}
