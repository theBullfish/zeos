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

/* ── Pass 1: strings + structs ─────────────────
 *
 * Z+ values are now a tagged union. The wire still carries int32 for
 * speed, but the int32 either IS the literal int (kind=ZP_VAL_INT) OR
 * it's a handle into the string/struct/array pool maintained in zplus.c
 * (kind=ZP_VAL_STR, ZP_VAL_STRUCT, ZP_VAL_ARRAY). A node's declared
 * output_kind tells the next node how to read the int32.
 *
 * The pools are heap-allocated through kmalloc with a small ref-count.
 * Cyclic struct references currently leak; a sweep collector will land
 * in pass 2.  Max string length = 64 KiB.
 */
#define ZP_MAX_STRING_BYTES   65536
#define ZP_STRING_POOL_SLOTS  256
#define ZP_STRUCT_TYPE_SLOTS  16
#define ZP_STRUCT_INST_SLOTS  64
#define ZP_ARRAY_POOL_SLOTS   64
#define ZP_MAX_STRUCT_FIELDS  8

enum zp_val_kind {
    ZP_VAL_INT    = 0,   /* wire == raw int32 (default — preserves legacy) */
    ZP_VAL_STR    = 1,   /* wire == handle into string pool */
    ZP_VAL_STRUCT = 2,   /* wire == handle into struct instance pool */
    ZP_VAL_ARRAY  = 3,   /* wire == handle into array pool */
};

/* Tagged value used at chain edges where the type tag matters. */
struct zp_value {
    int32_t kind;        /* enum zp_val_kind */
    int32_t handle;      /* int payload, or pool handle */
};

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

    /* ── Pass 1: string verbs ───────────────── */
    ZP_STR_LEN,         /* str.len(s) -> int */
    ZP_STR_FIND,        /* str.find(haystack, needle) -> int */
    ZP_STR_SPLIT,       /* str.split(s, sep) -> array */
    ZP_STR_REPLACE,     /* str.replace(s, old, new) -> str */
    ZP_STR_CONCAT,      /* str.concat(a, b) -> str */
    ZP_STR_UPPER,       /* str.upper(s) -> str */
    ZP_STR_LOWER,       /* str.lower(s) -> str */
    ZP_STR_TRIM,        /* str.trim(s) -> str */
    ZP_STR_STARTS_WITH, /* str.starts_with(s, prefix) -> int (0|1) */
    ZP_STR_ENDS_WITH,   /* str.ends_with(s, suffix) -> int (0|1) */
    ZP_STR_FROM_INT,    /* str.from_int(n) -> str */
    ZP_STR_TO_INT,      /* str.to_int(s) -> int */
    ZP_STR_LEN_OF_ARRAY,/* str.len_of_array(arr) -> int (count of strings) */

    /* ── Pass 1: struct verbs ───────────────── */
    ZP_STRUCT_LITERAL,  /* point { x = 1, y = 2 } — emits a struct instance */
    ZP_FIELD_ACCESS,    /* p.kind — extracts a field from a struct */
    ZP_GATE_FIELD_EQ,   /* gate(.field == "literal") — string-eq gate on struct field */
    ZP_SUM,             /* sum — running accumulator (int) */
};

/* Per-field descriptor for a struct type. */
struct zp_struct_field {
    char    name[ZP_MAX_NAME];
    int     kind;         /* ZP_VAL_INT | ZP_VAL_STR | ZP_VAL_STRUCT */
    int     offset;       /* slot index within the instance (0..N-1) */
};

/* Compiled struct type definition. */
struct zp_struct_type {
    char    name[ZP_MAX_NAME];
    int     field_count;
    struct zp_struct_field fields[ZP_MAX_STRUCT_FIELDS];
    int     in_use;
};

/* A parsed node declaration */
struct zp_node_decl {
    char            name[ZP_MAX_NAME];
    enum zp_node_type type;
    int32_t         int_val;        /* Constant for emit/multiply/add/threshold */
    int32_t         int_val2;       /* Second constant (knee high, sustained count, fs lba/count) */
    int32_t         int_val3;       /* Third constant (fs count) */
    char            fmt[ZP_MAX_STRING]; /* Format string for print, or vault key, or string literal */
    char            fmt2[ZP_MAX_STRING];/* Pass-1 second-string slot (gate field name, replace target, etc.) */
    int             sig_idx;        /* Index in the signal chain (-1 = unassigned) */
    int             chain_bind_id;  /* >=0 if this node bridges to a kernel chain */
    int             out_kind;       /* enum zp_val_kind — what the wire carries on output */
    int             struct_type;    /* For STRUCT_LITERAL / FIELD_ACCESS: index in struct table */
    int32_t         emit_handle;    /* For string/struct emit nodes: pool handle */
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
    /* Per-node verb AST captured inline in the chain block. When
     * have_decl[i] is non-zero, decls[i] is the parsed verb for node i;
     * otherwise the chain block referenced an existing/external node by
     * name only and the runtime should treat it as a passthrough. */
    struct zp_node_decl decls[ZP_MAX_CHAIN_NODES];
    uint8_t have_decl[ZP_MAX_CHAIN_NODES];
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

/* ── Pass 1 public surface ────────────────────────────────────────── */

/* Initialize the string/struct/array pools. Idempotent. */
void zp_pools_init(void);

/* Intern a NUL-terminated string. Returns a non-negative handle, or -1 on
 * out-of-pool. Strings are ref-counted; identical literals dedupe. The
 * returned handle has its ref-count bumped — the caller owns it. */
int  zp_str_intern(const char *s, int len);

/* Resolve a handle to its byte buffer; returns NULL for invalid handle.
 * Length out via *out_len when non-NULL. */
const char *zp_str_get(int handle, int *out_len);

/* Ref-count nudges. Internal nodes use these to stage handles between
 * resolve calls without leaks. */
void zp_str_addref(int handle);
void zp_str_release(int handle);

/* Number of string verbs registered (for selftest line). */
int  zp_string_verb_count(void);

/* Number of user-defined struct types currently registered. */
int  zp_struct_type_count(void);

/* Look up a struct type by name. Returns table index or -1. */
int  zp_struct_find(const char *name);

/* Register a struct type definition (used by parser). */
int  zp_struct_register(const struct zp_struct_type *def);

/* Allocate a fresh struct instance of given type. Returns handle or -1. */
int  zp_struct_alloc(int type_idx);

/* Read/write fields by name. Returns 0 on success, -1 on miss. */
int  zp_struct_set_int(int handle, const char *field, int32_t v);
int  zp_struct_set_str(int handle, const char *field, int str_handle);
int  zp_struct_get(int handle, const char *field, struct zp_value *out);

/* Array helpers (used by str.split). */
int  zp_array_alloc(void);
int  zp_array_push_str(int arr_handle, int str_handle);
int  zp_array_len(int arr_handle);
int  zp_array_get_str(int arr_handle, int idx);

#endif /* ZEOS_ZPLUS_H */
