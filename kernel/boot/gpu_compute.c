/*
 * Zeos -- GPU compute backend registry.
 *
 * Tiny registry of compute backends. The CPU backend is registered
 * unconditionally by gpu_compute_init(); other backends register
 * themselves once their device probe succeeds (e.g. gpu_virtio_init
 * registers "virtio-virgl" when VIRTIO_GPU_F_VIRGL is offered by the
 * host).
 *
 * Today only the CPU backend's dispatch actually executes user kernels.
 * The virtio-virgl backend is registered honestly: it tags the dispatch
 * as routed-to-GPU but falls back to CPU execution because no shader
 * compiler integrates yet. This is the wiring described in
 * docs/GPU_HOLES.md sections D1-D3 and N3.
 */

#include "gpu_compute.h"
#include "kprint.h"
#include "timer.h"
#include <stdint.h>

static gpu_compute_backend_t *s_backends[GPU_COMPUTE_MAX_BACKENDS];
static int s_count = 0;
static int s_inited = 0;

/* ── CPU backend ───────────────────────────────────────────────── */

static int cpu_can_dispatch(void *args)
{
    (void)args;
    /* CPU runs everything. Last-resort backend. */
    return 1;
}

static int cpu_dispatch(int (*kernel_fn)(void *), void *args,
                        uint64_t *elapsed_tsc)
{
    if (!kernel_fn) return -1;
    uint64_t t0 = timer_read_tsc();
    int rc = kernel_fn(args);
    uint64_t t1 = timer_read_tsc();
    if (elapsed_tsc) *elapsed_tsc = t1 - t0;
    return rc;
}

static gpu_compute_backend_t s_cpu_backend = {
    .name         = "cpu",
    .can_dispatch = cpu_can_dispatch,
    .dispatch     = cpu_dispatch,
    .capabilities = GPU_CAP_INT | GPU_CAP_FLOAT | GPU_CAP_FP64,
    .device_id    = -1,
};

/* ── Registry ──────────────────────────────────────────────────── */

void gpu_compute_init(void)
{
    if (s_inited) return;
    s_inited = 1;
    s_count  = 0;
    /* CPU backend is always first so gpu_compute_pick falls back to
     * it when no GPU backend wants the work. */
    gpu_compute_register(&s_cpu_backend);
}

void gpu_compute_register(gpu_compute_backend_t *backend)
{
    if (!backend) return;
    if (s_count >= GPU_COMPUTE_MAX_BACKENDS) {
        kputs("[gpu_compute] WARN: backend registry full, dropping ");
        kputs(backend->name ? backend->name : "(unnamed)");
        kputs("\n");
        return;
    }
    /* Reject duplicate registration by name. */
    for (int i = 0; i < s_count; i++) {
        if (s_backends[i] == backend) return;
    }
    s_backends[s_count++] = backend;
    kputs("[gpu_compute] registered backend: ");
    kputs(backend->name ? backend->name : "(unnamed)");
    kputs("\n");
}

gpu_compute_backend_t *gpu_compute_pick(void *args)
{
    /* Prefer non-CPU backends when they can_dispatch. CPU is always at
     * index 0 (registered first by gpu_compute_init). */
    for (int i = 1; i < s_count; i++) {
        gpu_compute_backend_t *b = s_backends[i];
        if (!b) continue;
        if (b->can_dispatch && b->can_dispatch(args)) return b;
    }
    /* Fall back to CPU (index 0) if registered, else NULL. */
    if (s_count > 0) return s_backends[0];
    return 0;
}

int gpu_compute_count(void)
{
    return s_count;
}

gpu_compute_backend_t *gpu_compute_get(int idx)
{
    if (idx < 0 || idx >= s_count) return 0;
    return s_backends[idx];
}
