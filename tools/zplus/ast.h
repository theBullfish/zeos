/* zplus — AST node types
 *
 * The AST is a signal-flow graph shaped tree, not a statement/expression tree.
 * Top-level constructs: import, struct, chain, signal_decl, pipeline.
 * Within pipelines: flow stages, taps, forks, merges, gates, calls, literals.
 *
 * All nodes are allocated from the same arena (parser.arena).
 */
#pragma once
#include "arena.h"

/* ── node kinds ─────────────────────────────────────────────────── */
typedef enum {
    AST_PROGRAM,       /* root: list of top-level decls              */

    /* ── declarations ──────────────────────────────────────────── */
    AST_IMPORT,        /* import mod.path as alias                   */
    AST_STRUCT,        /* struct Name { fields... }                  */
    AST_FIELD,         /* name : type [optional/required]            */
    AST_CHAIN,         /* chain name { items... }                    */
    AST_CHAIN_ITEM,    /* label : pipeline  (inside a chain block)   */
    AST_SIGNAL_DECL,   /* name : pipeline   (at top level)           */

    /* ── pipeline structure ────────────────────────────────────── */
    AST_PIPELINE,      /* ordered list of stages (a -> b -> c)       */
    AST_FORK,          /* { branch, branch, ... }                    */
    AST_MERGE,         /* | [policy] |  (merge node)                 */

    /* ── flow edges (wrap the *source* of the edge) ────────────── */
    AST_FLOW_EDGE,     /* src ->  dst  (forward signal)              */
    AST_TAP_EDGE,      /* src ~>  dst  (read-only tap)               */
    AST_SEVER_EDGE,    /* src -x> dst  (cut the wire)                */
    AST_EXCHANGE_EDGE, /* src <-> dst  (bidirectional)               */

    /* ── expressions / atoms ───────────────────────────────────── */
    AST_CALL,          /* func(args...)  or  obj.func(args...)       */
    AST_FIELD_ACCESS,  /* obj.field                                  */
    AST_STRUCT_INIT,   /* Type { field = val, ... }                  */
    AST_IDENT,         /* bare identifier                            */
    AST_DOTTED,        /* a.b.c  (qualified name, e.g. std.btree)    */

    /* ── literals ──────────────────────────────────────────────── */
    AST_INT_LIT,
    AST_FLOAT_LIT,
    AST_STR_LIT,
    AST_DURATION_LIT,  /* val.ival = milliseconds                    */
    AST_MULTIPLIER,    /* val.ival = integer multiplier              */

    /* ── built-in signal operators ─────────────────────────────── */
    AST_GATE,          /* gate(condition)                            */
    AST_KNEE,          /* knee: value                                */
    AST_DELTA,         /* delta(signal)                              */
    AST_EMIT,          /* emit(value)                                */
    AST_ON_SILENCE,    /* on_silence(duration)                       */
    AST_ON_BLOCK,      /* on_block (bare)                            */
    AST_SUSTAINED,     /* sustained(cond, for: duration)             */
    AST_WITHIN,        /* within(duration)                           */
    AST_DEBOUNCE,      /* debounce(duration)                         */
    AST_THROTTLE,      /* throttle(rate)                             */
    AST_DECAY,         /* decay(half_life: duration)                 */
    AST_TICK,          /* tick(interval)                             */
    AST_RATE,          /* rate(per: duration)                        */
    AST_DELTA_EXPR,    /* delta()                                    */

    /* ── type references ───────────────────────────────────────── */
    AST_TYPEREF,       /* type annotation (int, float, str, ident)   */

    /* ── binary / unary expressions ────────────────────────────── */
    AST_BINARY,        /* left op right                              */
    AST_UNARY,         /* op operand                                 */

    /* ── argument in a call ────────────────────────────────────── */
    AST_ARG,           /* [label:] expr                              */

    /* ── list (generic sibling list, used by program/chain/fork) ── */
    AST_LIST,
} ast_kind_t;

/* ── binary operator kinds ──────────────────────────────────────── */
typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV,
    BINOP_EQ, BINOP_NEQ, BINOP_LT, BINOP_GT, BINOP_LE, BINOP_GE,
    BINOP_RANGE,   /* ~ (0.6 ~ 0.9)       */
    BINOP_AND,
    BINOP_OR,
} binop_t;

typedef enum { UNOP_NEG, UNOP_NOT } unop_t;

/* ── flow edge type (for pipeline stages) ───────────────────────── */
typedef enum { EDGE_FLOW, EDGE_TAP, EDGE_SEVER, EDGE_EXCHANGE } edge_t;

/* ── AST node ───────────────────────────────────────────────────── */
typedef struct ast_node ast_node_t;

struct ast_node {
    ast_kind_t kind;
    int        line;
    int        col;

    union {
        /* AST_PROGRAM, AST_LIST */
        struct { ast_node_t *items; int count; } list;

        /* AST_IMPORT */
        struct { char *module; char *alias; } import;

        /* AST_STRUCT */
        struct { char *name; ast_node_t *fields; int nfields; } strct;

        /* AST_FIELD */
        struct { char *name; ast_node_t *type; int optional; } field;

        /* AST_CHAIN */
        struct { char *name; ast_node_t *items; int nitems; } chain;

        /* AST_CHAIN_ITEM */
        struct { char *label; ast_node_t *pipeline; } chain_item;

        /* AST_SIGNAL_DECL */
        struct { char *name; ast_node_t *rhs; } sig_decl;

        /* AST_PIPELINE: ordered array of stages connected by edges */
        struct {
            ast_node_t **stages;   /* [stage0, stage1, ...] */
            edge_t      *edges;    /* edge[i] connects stages[i] -> stages[i+1] */
            int          count;
        } pipeline;

        /* AST_FORK */
        struct { ast_node_t **branches; int count; } fork;

        /* AST_MERGE */
        struct {
            ast_node_t *policy;    /* NULL for bare |, else ident/call */
            ast_node_t **sources;  /* explicit source list, may be empty */
            int          nsources;
        } merge;

        /* AST_CALL */
        struct {
            ast_node_t *func;      /* AST_IDENT or AST_DOTTED or AST_FIELD_ACCESS */
            ast_node_t *args;      /* AST_LIST of AST_ARG */
            int         nargs;
        } call;

        /* AST_FIELD_ACCESS */
        struct { ast_node_t *obj; char *field; } fld;

        /* AST_STRUCT_INIT */
        struct { char *type_name; ast_node_t *fields; int nfields; } sinit;

        /* AST_IDENT */
        struct { char *name; } ident;

        /* AST_DOTTED  (e.g. std.btree, vault.put) */
        struct { char **parts; int nparts; } dotted;

        /* AST_INT_LIT, AST_DURATION_LIT, AST_MULTIPLIER */
        struct { long ival; } ival;

        /* AST_FLOAT_LIT */
        struct { double fval; } fval;

        /* AST_STR_LIT */
        struct { char *s; int len; } sval;

        /* AST_GATE, AST_DELTA, AST_EMIT, AST_ON_SILENCE, AST_SUSTAINED,
           AST_WITHIN, AST_DEBOUNCE, AST_THROTTLE, AST_DECAY, AST_TICK,
           AST_RATE, AST_KNEE — all share: primary expr + optional list of
           labeled args */
        struct { ast_node_t *expr; ast_node_t *args; int nargs; } op;

        /* AST_BINARY */
        struct { binop_t op; ast_node_t *left; ast_node_t *right; } binary;

        /* AST_UNARY */
        struct { unop_t op; ast_node_t *operand; } unary;

        /* AST_ARG */
        struct { char *label; ast_node_t *val; } arg;

        /* AST_TYPEREF */
        struct { char *name; int is_optional; } typeref;
    } u;

    ast_node_t *next;   /* sibling list link (used by list nodes) */
};

/* ── helpers ────────────────────────────────────────────────────── */
static inline ast_node_t *ast_new(arena_t *a, ast_kind_t k, int line, int col) {
    ast_node_t *n = (ast_node_t*)arena_alloc(a, sizeof(ast_node_t));
    if (n) { n->kind = k; n->line = line; n->col = col; }
    return n;
}

const char *ast_kind_name(ast_kind_t k);
void        ast_dump(ast_node_t *n, int indent);
