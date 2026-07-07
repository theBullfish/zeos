/*
 * Zeos — ZIR (Z+ Interchange Representation) loader.
 *
 * Consumes ZIR v1 JSON produced by a Z+ front-end (the Rust `zplus-emit`
 * codegen backend, or the C transpiler's `sfg_to_zir`) and populates a
 * `struct zp_program` — the same structure the in-kernel parser builds.
 *
 * This is the convergence point described in tools/zplus/ZIR.md: the kernel
 * no longer needs its own Z+ source parser as the ingestion path. A front-end
 * lowers .zp -> ZIR; the kernel loads ZIR -> zp_program -> signal chain.
 *
 * Deliberately dependency-free: no libc, no kmalloc. It uses only the small
 * static helpers in zplus_zir.c and writes into caller-provided fixed storage,
 * so the exact same translation unit compiles in the kernel AND in a host
 * unit test (gcc). See zplus_zir_test.c.
 */
#ifndef ZEOS_ZPLUS_ZIR_H
#define ZEOS_ZPLUS_ZIR_H

#include "zplus.h"

#define ZIR_SUPPORTED_VERSION 1

/* Outcome of a load. Even on success, `unmapped` / `truncated` may be non-zero
 * — the loader degrades honestly (records the count) rather than silently
 * dropping nodes it doesn't recognize or can't fit. */
typedef struct {
    int ok;             /* 1 = a program was produced, 0 = fatal parse error */
    int version;        /* the "zir" version seen in the document */
    int nodes_loaded;   /* nodes written into prog->nodes */
    int edges_loaded;   /* edges written into prog->edges */
    int unmapped;       /* nodes whose verb had no kernel node-type (-> passthrough) */
    int truncated;      /* nodes/edges dropped because a ZP_MAX_* cap was hit */
    char error[128];    /* human-readable reason when ok == 0 */
} zir_load_result_t;

/*
 * Load ZIR JSON text into `prog`. `prog` is zeroed first. Returns 0 on success
 * (a program was produced), -1 on fatal error (res->error explains). Partial
 * degradation (unmapped verbs, truncation) is NOT fatal and is reported in res.
 */
int zir_load(const char *json, struct zp_program *prog, zir_load_result_t *res);

/* Map a ZIR verb (+ optional gate op) to a kernel node type. Returns the type,
 * or ZP_PASSTHROUGH for anything unknown (and sets *known = 0). Exposed for the
 * unit test. `gate_op` may be NULL for non-gate verbs. */
enum zp_node_type zir_verb_to_type(const char *verb, const char *gate_op, int *known);

#endif /* ZEOS_ZPLUS_ZIR_H */
