/*
 * Zeos -- GPU compute backend abstraction.
 *
 * GPU compute backend abstraction. Today: CPU (real) + virtio-virgl
 * (registered when feature is available, but executes on CPU fallback --
 * shader compiler integration is queued in GPU_HOLES.md sections D1-D3).
 * Real Intel/AMD/NVIDIA backends register the same way once those drivers
 * land.
 *
 * Multi-backend selection: round-robin across same-class backends,
 * least-loaded with latency tie-break, shape-aware (stub).
 * Cooldown after 3+ consecutive errors. CPU is never cooldown'd.
 *
 * CHAIN_MDE.device_select calls gpu_compute_pick_with(policy, args) to
 * choose a backend. Per-backend live stats (inflight_count, EWMA dispatch
 * latency, consecutive_errors) are kept in gpu_compute_backend_t and
 * updated by the dispatch wrappers.
 */
#ifndef ZEOS_GPU_COMPUTE_H
#define ZEOS_GPU_COMPUTE_H

#include <stdint.h>

/* Capability bits. Backends advertise what they CAN run. The shader
 * compiler that turns these into actual GPU work is a separate concern
 * (D1/D2 in GPU_HOLES.md). */
#define GPU_CAP_INT    (1u << 0)
#define GPU_CAP_FLOAT  (1u << 1)
#define GPU_CAP_FP16   (1u << 2)
#define GPU_CAP_FP64   (1u << 3)
/* Shape capability hints (advisory; SHAPE_AWARE selector consults these). */
#define GPU_CAP_MATRIX (1u << 8)
#define GPU_CAP_CONV   (1u << 9)

#define GPU_COMPUTE_MAX_BACKENDS 32  /* Brad has 20 Goyas going in. */

/* Selection policies. */
#define MDE_SELECT_FIRST        0  /* legacy: first non-CPU that can_dispatch */
#define MDE_SELECT_ROUND_ROBIN  1  /* rotate within same name_prefix class */
#define MDE_SELECT_LEAST_LOADED 2  /* lowest inflight, tie-break on EWMA us */
#define MDE_SELECT_SHAPE_AWARE  3  /* stub: match shape_hints->capabilities,
                                    * falls back to LEAST_LOADED */

/* Cooldown thresholds. */
#define GPU_COOLDOWN_ERROR_THRESHOLD 3
#define GPU_COOLDOWN_DURATION_NS     (60ull * 1000ull * 1000ull * 1000ull)

typedef struct gpu_compute_backend {
    const char *name;          /* "cpu" / "goya-0" / "intel-iris" / etc. */
    int  (*can_dispatch)(void *args);             /* return 1 if this backend can handle the request */
    int  (*dispatch)(int (*kernel_fn)(void*), void *args, uint64_t *elapsed_tsc);
    int  capabilities;         /* bit flags: GPU_CAP_FLOAT, GPU_CAP_INT, GPU_CAP_FP16, etc. */
    int  device_id;            /* PCI device id or -1 for CPU */

    /* Live stats. Updated by gpu_compute dispatch wrappers, not by the
     * raw ->dispatch handler. Reads are racy but monotonic enough for
     * selection. */
    uint64_t inflight_count;       /* currently queued/dispatching */
    uint64_t total_dispatched;     /* lifetime completion count */
    uint64_t total_dispatch_tsc;   /* sum of elapsed_tsc, for moving avg */
    uint64_t last_dispatch_tsc;    /* TSC at last dispatch start */
    uint64_t last_completion_tsc;  /* TSC at last dispatch end */
    uint64_t ewma_dispatch_tsc;    /* EWMA of elapsed_tsc, alpha=1/8 */
    uint32_t consecutive_errors;   /* reset on first success */
    uint64_t cooldown_until_tsc;   /* TSC; 0 = not in cooldown */
} gpu_compute_backend_t;

/* Register a backend. Pointer must remain live -- the registry stores
 * the pointer, not a copy. CPU backend is registered automatically by
 * gpu_compute_init(). */
void gpu_compute_register(gpu_compute_backend_t *backend);

/* Pick a backend for a request using the global policy (back-compat
 * wrapper for callers that don't care about per-request overrides).
 * Returns the CPU backend when no other backend can_dispatch. May
 * return NULL only if the registry is empty. */
gpu_compute_backend_t *gpu_compute_pick(void *args);

/* Pick a backend using a specific policy. Same semantics as above. */
gpu_compute_backend_t *gpu_compute_pick_with(int policy, void *args);

/* Set / get the global selector policy. Default after init:
 * MDE_SELECT_LEAST_LOADED. Returns 0 on success, -1 on bad policy. */
int gpu_compute_set_policy(int policy);
int gpu_compute_get_policy(void);
const char *gpu_compute_policy_label(int policy);

/* Number of registered backends and indexed access for selftest. */
int gpu_compute_count(void);
gpu_compute_backend_t *gpu_compute_get(int idx);

/* Number of backends currently in cooldown (TSC <= cooldown_until_tsc). */
int gpu_compute_cooldown_count(void);

/* Wrap a dispatch with stats accounting + cooldown updates. CHAIN_MDE
 * uses this so inflight_count, EWMA latency, and consecutive_errors
 * stay accurate without every backend duplicating the bookkeeping.
 * Returns the kernel_fn rc. elapsed_out (optional) is filled with the
 * real elapsed_tsc the underlying dispatch reported. */
int gpu_compute_dispatch_tracked(gpu_compute_backend_t *b,
                                 int (*kernel_fn)(void *),
                                 void *args,
                                 uint64_t *elapsed_out);

/* Initialize the registry and register the always-present CPU backend.
 * Idempotent. Called from chain_registry before mde_chain_register. */
void gpu_compute_init(void);

/* Selftest line: "MDE selector .......... policy=LEAST_LOADED, N backend(s),
 * 0 in cooldown". */
void gpu_compute_print_selftest_line(void);

/* `gpu-load` shell command body. */
void gpu_compute_dump_load(void);

#endif /* ZEOS_GPU_COMPUTE_H */
