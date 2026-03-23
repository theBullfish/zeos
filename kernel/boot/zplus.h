/*
 * Zeos — Z+ Interpreter (Minimal)
 *
 * Parses and executes Z+ Tier 3 syntax by mapping to the
 * signal chain engine. This is the bridge between the language
 * and the kernel.
 *
 * Supports:
 *   name : emit(N)                    — source node
 *   name : input -> * N -> output     — multiply transform
 *   name : input -> + N -> output     — add transform
 *   name : input -> print("...")      — display sink
 *   name : input -> gate(> N) -> out  — pass if greater than N
 *   name : input -> gate(< N) -> out  — pass if less than N
 *   name : input -> gate(== N) -> out — pass if equal to N
 *   name : input -> delta -> output   — change detection (t - t-1)
 *   a -> b -> c                       — wiring
 *   a -> {b, c}                       — fork (one source, two sinks)
 *   a ~> b                            — tap (read-only observation)
 *   // comments
 *
 * Not yet supported:
 *   knee, temporal t-2+, hardware sense/act, vault, MDE
 *
 * v0.2: gate + fork + tap + delta added.
 */

#ifndef ZEOS_ZPLUS_H
#define ZEOS_ZPLUS_H

#include <stdint.h>

/* Maximum limits for the interpreter */
#define ZP_MAX_NODES     32
#define ZP_MAX_EDGES     64
#define ZP_MAX_NAME      32
#define ZP_MAX_STRING    128
#define ZP_MAX_SOURCE    2048

/* Node types the interpreter can create */
enum zp_node_type {
    ZP_EMIT,        /* Produces a constant value */
    ZP_MULTIPLY,    /* Multiplies input by a constant */
    ZP_ADD,         /* Adds a constant to input */
    ZP_SUBTRACT,    /* Subtracts a constant from input */
    ZP_PRINT,       /* Displays input with a format string */
    ZP_NEGATE,      /* Multiplies input by -1 */
    ZP_GATE_GT,     /* Pass if input > threshold */
    ZP_GATE_LT,     /* Pass if input < threshold */
    ZP_GATE_EQ,     /* Pass if input == threshold */
    ZP_GATE_GTE,    /* Pass if input >= threshold */
    ZP_GATE_LTE,    /* Pass if input <= threshold */
    ZP_DELTA,       /* Output = input - previous input (change detection) */
    ZP_PASSTHROUGH, /* Pass input to output unchanged (for fork/tap wiring) */
};

/* A parsed node declaration */
struct zp_node_decl {
    char            name[ZP_MAX_NAME];
    enum zp_node_type type;
    int32_t         int_val;        /* Constant for emit/multiply/add */
    char            fmt[ZP_MAX_STRING]; /* Format string for print */
    int             sig_idx;        /* Index in the signal chain (-1 = unassigned) */
};

/* A parsed edge (wiring) */
struct zp_edge {
    char            src[ZP_MAX_NAME];
    char            dst[ZP_MAX_NAME];
};

/* The parsed program */
struct zp_program {
    struct zp_node_decl nodes[ZP_MAX_NODES];
    int                 node_count;
    struct zp_edge      edges[ZP_MAX_EDGES];
    int                 edge_count;
    int                 chain_id;       /* Signal chain ID after compilation */
};

/*
 * Parse a Z+ source string into a program structure.
 * Returns 0 on success, -1 on parse error.
 */
int zp_parse(const char *source, struct zp_program *prog);

/*
 * Compile a parsed program into a signal chain.
 * Creates nodes and edges in the signal chain engine.
 * Returns the chain ID, or -1 on error.
 */
int zp_compile(struct zp_program *prog);

/*
 * Execute a compiled program (resolve the signal chain).
 * Returns number of nodes fired.
 */
int zp_execute(struct zp_program *prog);

/*
 * Parse, compile, and execute a Z+ source string in one call.
 * Returns number of nodes fired, or -1 on error.
 */
int zp_run(const char *source);

#endif /* ZEOS_ZPLUS_H */
