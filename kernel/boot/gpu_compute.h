/*
 * Zeos -- GPU compute backend abstraction.
 *
 * GPU compute backend abstraction. Today: CPU (real) + virtio-virgl
 * (registered when feature is available, but executes on CPU fallback --
 * shader compiler integration is queued in GPU_HOLES.md sections D1-D3).
 * Real Intel/AMD/NVIDIA backends register the same way once those drivers
 * land.
 *
 * CHAIN_MDE.device_select calls gpu_compute_pick(args) to choose a
 * backend. Today the CPU backend always wins unless the request opts
 * in via mde_compute_request_t.prefer_gpu AND a non-CPU backend reports
 * can_dispatch == 1. This is the wiring for GPU_HOLES.md N3 / D3.
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

#define GPU_COMPUTE_MAX_BACKENDS 8

typedef struct gpu_compute_backend {
    const char *name;          /* "cpu" / "virtio-virgl" / "intel-iris" / etc. */
    int  (*can_dispatch)(void *args);             /* return 1 if this backend can handle the request */
    int  (*dispatch)(int (*kernel_fn)(void*), void *args, uint64_t *elapsed_tsc);
    int  capabilities;         /* bit flags: GPU_CAP_FLOAT, GPU_CAP_INT, GPU_CAP_FP16, etc. */
    int  device_id;            /* PCI device id or -1 for CPU */
} gpu_compute_backend_t;

/* Register a backend. Pointer must remain live -- the registry stores
 * the pointer, not a copy. CPU backend is registered automatically by
 * gpu_compute_init(). */
void gpu_compute_register(gpu_compute_backend_t *backend);

/* Pick a backend for a request. Returns the CPU backend when no GPU
 * backend can_dispatch. May return NULL only if the registry is empty
 * (init was never called). */
gpu_compute_backend_t *gpu_compute_pick(void *args);

/* Number of registered backends and indexed access for selftest. */
int gpu_compute_count(void);
gpu_compute_backend_t *gpu_compute_get(int idx);

/* Initialize the registry and register the always-present CPU backend.
 * Idempotent. Called from chain_registry before mde_chain_register. */
void gpu_compute_init(void);

#endif /* ZEOS_GPU_COMPUTE_H */
