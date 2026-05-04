/*
 * Zeos -- Z+ Runtime Chain Registry
 *
 * Glues the Z+ interpreter's parsed chain definitions to the live kernel
 * chain registry (chain.h). When a Z+ program declares
 *   `chain heartbeat_runtime { pulse : emit(1) -> ... -> tap.log }`
 * the runtime registers a real chain via chain_create(CHAIN_CPU, MASQ_INTERNAL)
 * and adds typed nodes via chain_add_node() with a single dispatch thunk
 * (zp_kernel_resolve_thunk) that walks the node's stored verb AST.
 *
 * Programs are loaded once and stay live. When a program is reloaded with
 * the same chain name, the old chain is torn down (chain_destroy) before the
 * new one is registered -- otherwise we leak.
 *
 * The runtime has its own persistent storage so that re-running zp_run()
 * (which uses a stack-local zp_program) does not strand pointers in the
 * chain.h registry.
 */

#ifndef ZEOS_ZP_RUNTIME_H
#define ZEOS_ZP_RUNTIME_H

#include <stdint.h>
#include "zplus.h"

/* Per-node verb AST kept in persistent runtime storage. Mirrors
 * zp_node_decl but is owned by the runtime, not the program. */
struct zp_runtime_node {
    char            name[ZP_MAX_NAME];
    enum zp_node_type type;
    int32_t         int_val;
    int32_t         int_val2;
    int32_t         int_val3;
    char            fmt[ZP_MAX_STRING];

    /* Mutable state for stateful verbs (sustained, delta). */
    int32_t         sustain_count;
    int32_t         delta_prev;
    uint8_t         has_delta_prev;
};

/* Initialize the runtime registry (called once, idempotent). */
void zp_runtime_init(void);

/*
 * Register a chain in the live kernel registry.
 *
 * If a runtime chain with `name` already exists, it is torn down first
 * (chain_destroy) so re-running a Z+ program does not leak.
 *
 * `nodes` is a sequential list -- node[0] feeds node[1] feeds node[2] etc.
 * input/output types are inferred from each node's verb (zp_runtime_typeof).
 *
 * Returns the chain.h chain id on success, -1 on failure.
 */
int zp_runtime_register_chain(const char *name,
                              const struct zp_runtime_node *nodes,
                              int node_count);

/* Number of live runtime chains in the kernel registry. */
int zp_runtime_count(void);

/* Inferred input/output types for a Z+ verb. Static strings; never NULL. */
const char *zp_runtime_input_type(enum zp_node_type t);
const char *zp_runtime_output_type(enum zp_node_type t);

#endif /* ZEOS_ZP_RUNTIME_H */
