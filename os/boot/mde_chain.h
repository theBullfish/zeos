/*
 * Zeos -- MDE exposed AS a chain (header)
 *
 * mde.c remains the chain-graph router (the routing infrastructure
 * every chain runs through). mde_chain.c is the *new* chain that lets
 * any caller submit a generic compute_request through the same
 * pipeline contract every other Zeos device chain follows.
 *
 * Pipeline:
 *   compute_request -> device_select -> schedule -> execute -> result_emit
 *
 * Backends today: CPU. The device_select node is where GPU/Goya/FPGA
 * selection lives once those drivers expose backends.
 *
 * MasQ tier: INTERNAL. Parent: CHAIN_CPU.
 */
#ifndef ZEOS_MDE_CHAIN_H
#define ZEOS_MDE_CHAIN_H

#include <stdint.h>

/* Backend identifiers. CPU is the only one wired today; the others are
 * reserved so the device_select decision tree has stable IDs as drivers
 * land. */
#define MDE_BACKEND_NONE  0
#define MDE_BACKEND_CPU   1
#define MDE_BACKEND_GPU   2
#define MDE_BACKEND_GOYA  3
#define MDE_BACKEND_FPGA  4

typedef struct {
    int      (*kernel_fn)(void *args);  /* required: the work to run */
    void      *args;                    /* opaque payload */
    int        rc;                      /* filled on completion */
    uint64_t   elapsed_tsc;             /* filled on completion */
    int        prefer_gpu;              /* opt-in: try GPU backend first */
    int        backend_used;            /* filled on completion: MDE_BACKEND_* */

    /* -1 means use the global policy; else MDE_SELECT_* from gpu_compute.h. */
    int        policy_override;
    /* Per-request hints for SHAPE_AWARE selection (matrix dims, conv params).
     * Today consulted by the stub policy, kept for the typed-workloads path
     * (GPU_HOLES.md D3). All zeros = "no hints". */
    uint32_t   shape_hints[4];
    /* -1 means free-pick; else pin to gpu_compute_get(idx). The selector
     * still verifies can_dispatch + cooldown. */
    int        affinity_backend_idx;
    /* Filled on completion: index into gpu_compute registry of the backend
     * that actually ran the request. -1 if the legacy direct CPU path
     * fired (registry empty). */
    int        backend_idx_used;
} mde_compute_request_t;

/* Register CHAIN_MDE under parent_id (CHAIN_CPU). Returns 0 on success. */
int  mde_chain_register(int parent_id);

/* Submit a compute request through CHAIN_MDE. Synchronous: returns when
 * the pipeline has run end-to-end. Returns the kernel_fn's rc on
 * success, or -1 if the pipeline rejected the request (full schedule,
 * unregistered chain, no kernel_fn). */
int  mde_chain_submit(mde_compute_request_t *req);

/* Number of requests currently in-flight in the schedule (selftest
 * affordance). */
int  mde_chain_inflight(void);

/* Public chain id, set by mde_chain_register(). -1 until registered. */
extern int CHAIN_MDE;

#endif /* ZEOS_MDE_CHAIN_H */
