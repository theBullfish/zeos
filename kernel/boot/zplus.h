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
 *   chain name { n1 -> n2 -> n3 }     — chain definition (chain.h)
 *   // comments
 *
 * Not yet supported:
 *   knee, temporal t-2+, hardware sense/act, vault, MDE
 *
 * v0.2: gate + fork + tap + delta added.
 * v0.3: chain definitions added.
 * v0.4: live kernel chain binding. Z+ now binds directly to the
 *       registered chain graph. audio.* -> CHAIN_AUDIO,
 *       net.*  -> CHAIN_NET_TX/RX, fs.* -> CHAIN_BLOCK,
 *       vault.* -> vault config store. Adds knee + sustained
 *       continuous operators. Programs are first-class chain
 *       consumers — not a generic DSL but the surface that
 *       talks to the actual kernel chain graph.
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
#define ZP_MAX_CHAINS    16
#define ZP_MAX_CHAIN_NODES 16

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
    ZP_KNEE,        /* Smooth transition: 0 below low, 100 above high, lerp between */
    ZP_SUSTAINED,   /* Fires after input has held condition for N consecutive ticks */
    ZP_AUDIO_PLAY,  /* audio.play  -> chain_resolve(CHAIN_AUDIO) with staged pcm_request */
    ZP_NET_SEND,    /* net.send    -> chain_resolve(CHAIN_NET_TX) */
    ZP_NET_RECV,    /* net.recv    -> chain_resolve(CHAIN_NET_RX) */
    ZP_FS_READ,     /* fs.read     -> CHAIN_BLOCK with op=READ */
    ZP_FS_WRITE,    /* fs.write    -> CHAIN_BLOCK with op=WRITE */
    ZP_VAULT_PUT,   /* vault.put   -> vault_save_config */
    ZP_VAULT_GET,   /* vault.get   -> vault_load_config */
    ZP_TAP_LOG,     /* tap.log     -> kprint side-channel observation */
    ZP_COMPUTE_RUN, /* compute.run(node) -> CHAIN_MDE submit, kernel_fn = node's resolve */
};

/* A parsed node declaration */
struct zp_node_decl {
    char            name[ZP_MAX_NAME];
    enum zp_node_type type;
    int32_t         int_val;        /* Constant for emit/multiply/add/threshold */
    int32_t         int_val2;       /* Second constant (knee high, sustained count, fs lba/count) */
    int32_t         int_val3;       /* Third constant (fs count) */
    char            fmt[ZP_MAX_STRING]; /* Format string for print, or vault key */
    int             sig_idx;        /* Index in the signal chain (-1 = unassigned) */
    int             chain_bind_id;  /* >=0 if this node bridges to a kernel chain */
};

/* A parsed edge (wiring) */
struct zp_edge {
    char            src[ZP_MAX_NAME];
    char            dst[ZP_MAX_NAME];
};

/* A parsed chain definition (chain name { node1 -> node2 -> ... }) */
struct zp_chain_def {
    char    name[ZP_MAX_NAME];
    char    node_names[ZP_MAX_CHAIN_NODES][ZP_MAX_NAME];
    int     node_count;
    int     chain_id;           /* chain.h chain ID after compilation */
};

/* The parsed program */
struct zp_program {
    struct zp_node_decl nodes[ZP_MAX_NODES];
    int                 node_count;
    struct zp_edge      edges[ZP_MAX_EDGES];
    int                 edge_count;
    int                 chain_id;       /* Signal chain ID after compilation */
    struct zp_chain_def chain_defs[ZP_MAX_CHAINS];
    int                 chain_def_count;
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

/*
 * List all chain definitions created by the Z+ interpreter.
 * Prints name, node count, and chain ID for each.
 */
void zp_list_chains(void);

/*
 * Inspect a chain definition by chain_id (from chain.h).
 * Prints detailed info via chain_dump().
 */
void zp_inspect_chain(int chain_id);

#endif /* ZEOS_ZPLUS_H */
