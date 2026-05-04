/*
 * Zeos -- MDE exposed as a chain.
 *
 * MDE exposed as a chain. Pipeline:
 *   compute_request -> device_select -> schedule -> execute -> result_emit
 * Backends: CPU now, GPU/Goya/FPGA when the device drivers expose them.
 * Z+ submits work via compute.run(); kernel callers via mde_chain_submit().
 * MasQ tier: INTERNAL. Parent: CHAIN_CPU.
 *
 * Per the chain contract: state-changing resolves bump
 * chain->vault_version. The "schedule" admit and the "result_emit"
 * completion are both state changes -- one says "we accepted work",
 * one says "we returned a result". Two bumps per submit on the happy
 * path. Reads of chain state (inflight count) do not bump.
 */

#include "mde_chain.h"
#include "chain.h"
#include "chain_registry.h"
#include "gpu_compute.h"
#include "kprint.h"
#include "timer.h"
#include <stdint.h>
#include <stddef.h>

int CHAIN_MDE = -1;

/* ── In-flight schedule ────────────────────────────────────────── */

#define MDE_CHAIN_INFLIGHT_CAP 8

static int s_inflight;          /* count of requests currently scheduled */

int mde_chain_inflight(void) { return s_inflight; }

/* ── Per-submit state ──────────────────────────────────────────── */
/*
 * One submit is in progress at a time from the chain's perspective:
 * mde_chain_submit() stages the request, calls chain_resolve(), then
 * unstages. The pipeline carries a small dispatch_token between nodes
 * (backend_id + request_ptr), then a compute_result at the tail.
 */
typedef struct {
    int                       backend_id;
    gpu_compute_backend_t    *backend;   /* non-NULL when a registry-backed
                                          * backend was picked (CPU or GPU);
                                          * NULL = legacy direct CPU path */
    mde_compute_request_t    *req;
} mde_dispatch_token_t;

typedef struct {
    int       rc;
    uint64_t  elapsed_tsc;
    void     *output_ptr;     /* future: backend-owned result buffer */
} mde_compute_result_t;

typedef struct {
    int                       valid;
    int                       error;
    int                       admitted;          /* bumped vault on schedule */
    mde_compute_request_t    *req;

    /* Filled across the pipeline. */
    mde_dispatch_token_t      tok;
    mde_compute_result_t      res;
    uint64_t                  exec_start_tsc;
} mde_chain_slot_t;

static mde_chain_slot_t s_slot;

/* ── Node resolves ─────────────────────────────────────────────── */

/*
 * compute_request: copy the caller's request pointer into chain-local
 * state. Earliest stage where we reject malformed submits (no
 * kernel_fn). Does NOT bump vault_version -- accepting at the door is
 * not a state change, scheduling is.
 */
static void compute_request_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    (void)output;

    if (!s_slot.valid) {
        /* Speculative resolve from chain_registry_tick with no submit
         * staged. Mark error so downstream nodes short-circuit. */
        s_slot.error = 1;
        return;
    }
    if (!s_slot.req || !s_slot.req->kernel_fn) {
        s_slot.error = 1;
        return;
    }
    s_slot.tok.backend_id = MDE_BACKEND_NONE;
    s_slot.tok.backend    = NULL;
    s_slot.tok.req        = s_slot.req;
    s_slot.res.rc          = -1;
    s_slot.res.elapsed_tsc = 0;
    s_slot.res.output_ptr  = 0;
    s_slot.exec_start_tsc  = 0;
}

/*
 * device_select: pick a backend for this request.
 *
 * Decision tree (extension points marked):
 *   1. If request has hinted backend (future field) and that driver
 *      exposes a backend -> use the hint.
 *   2. Else inspect compute shape (size, op class) -- future Goya/GPU
 *      eligibility checks live here.
 *   3. Else fall back to CPU (always available).
 *
 * Today only #3 fires because GPU/Goya/FPGA drivers haven't published
 * backends yet. The selection itself is read-only against chain state,
 * so this resolve does NOT bump vault_version.
 */
static void device_select_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    (void)output;

    if (!s_slot.valid || s_slot.error) { s_slot.error = 1; return; }

    /* Ask the gpu_compute registry for a backend. Today the registry
     * always has at least the CPU backend; GPU backends register from
     * gpu_virtio_init / future Intel/AMD/NVIDIA drivers.
     *
     * Selection policy:
     *   prefer_gpu=1 -> use whatever gpu_compute_pick returns (GPU first,
     *                   CPU fallback when no GPU backend can_dispatch).
     *   prefer_gpu=0 -> force CPU backend at index 0 for safety/determinism.
     *
     * The registry never returns NULL when init has run, so the
     * legacy MDE_BACKEND_CPU branch in execute_resolve only fires if
     * the registry was somehow empty (defense-in-depth). */
    gpu_compute_backend_t *picked = NULL;
    if (s_slot.req->prefer_gpu) {
        picked = gpu_compute_pick(s_slot.req->args);
    } else if (gpu_compute_count() > 0) {
        picked = gpu_compute_get(0);  /* CPU is always at index 0 */
    }

    if (picked) {
        s_slot.tok.backend    = picked;
        s_slot.tok.backend_id = (picked->device_id < 0)
                                ? MDE_BACKEND_CPU : MDE_BACKEND_GPU;
    } else {
        s_slot.tok.backend    = NULL;
        s_slot.tok.backend_id = MDE_BACKEND_CPU;
    }
}

/*
 * schedule: cap concurrent work. If the slot table is full, fail
 * CHAIN_ERROR-style (set error so the rest of the pipeline short-
 * circuits and result_emit reports rc=-1). Admission is a state
 * change -> bump vault_version exactly once.
 */
static void schedule_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    (void)output;

    if (!s_slot.valid || s_slot.error) { s_slot.error = 1; return; }

    if (s_inflight >= MDE_CHAIN_INFLIGHT_CAP) {
        s_slot.error = 1;
        return;
    }
    s_inflight++;
    s_slot.admitted = 1;

    chain_t *c = chain_get(CHAIN_MDE);
    if (c) c->vault_version++;
}

/*
 * execute: invoke the backend. CPU backend calls kernel_fn(args)
 * directly. The backend really runs -- no stub. Records elapsed TSC
 * on the request struct so the caller sees real timing.
 *
 * GPU/Goya/FPGA dispatch will branch on s_slot.tok.backend_id once
 * those drivers register.
 */
static void execute_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    (void)output;

    if (!s_slot.valid || s_slot.error) { s_slot.error = 1; return; }

    s_slot.exec_start_tsc = timer_read_tsc();

    int rc = -1;
    uint64_t elapsed = 0;

    if (s_slot.tok.backend && s_slot.tok.backend->dispatch) {
        /* Registry-backed dispatch. The CPU backend really runs the
         * kernel_fn. The virtio-virgl backend currently logs and falls
         * back to CPU execution -- honest about not having a shader
         * compiler yet. Real Intel/AMD/NVIDIA backends will execute on
         * device when they register. */
        rc = s_slot.tok.backend->dispatch(s_slot.tok.req->kernel_fn,
                                          s_slot.tok.req->args,
                                          &elapsed);
    } else if (s_slot.tok.backend_id == MDE_BACKEND_CPU) {
        /* Legacy direct path -- only reached if the registry is empty
         * (gpu_compute_init never ran). */
        rc = s_slot.tok.req->kernel_fn(s_slot.tok.req->args);
        elapsed = timer_read_tsc() - s_slot.exec_start_tsc;
    } else {
        s_slot.error = 1;
        return;
    }

    s_slot.res.rc          = rc;
    s_slot.res.elapsed_tsc = elapsed ? elapsed
                                     : (timer_read_tsc() - s_slot.exec_start_tsc);
    s_slot.req->rc           = rc;
    s_slot.req->elapsed_tsc  = s_slot.res.elapsed_tsc;
    s_slot.req->backend_used = s_slot.tok.backend_id;
}

/*
 * result_emit: package the return value, drop the schedule slot, bump
 * vault_version (the work has officially completed and the chain has
 * a new state). On error path we still drop the slot but only bump if
 * we previously admitted.
 */
static void result_emit_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    (void)output;

    if (!s_slot.valid) return;

    if (s_slot.admitted) {
        if (s_inflight > 0) s_inflight--;
        s_slot.admitted = 0;
        chain_t *c = chain_get(CHAIN_MDE);
        if (c) c->vault_version++;
    }
    /* On error, ensure the request struct reflects failure. */
    if (s_slot.error) {
        s_slot.req->rc          = -1;
        s_slot.req->elapsed_tsc = 0;
    }
}

/* ── Registration ──────────────────────────────────────────────── */

int mde_chain_register(int parent_id)
{
    CHAIN_MDE = chain_create("mde", parent_id, MASQ_INTERNAL);
    if (CHAIN_MDE < 0) {
        kputs("[mde_chain] failed to create mde chain\n");
        return -1;
    }

    chain_add_node(CHAIN_MDE, "compute_request",
                   "compute_request", "dispatch_token",
                   compute_request_resolve);
    chain_add_node(CHAIN_MDE, "device_select",
                   "dispatch_token", "dispatch_token",
                   device_select_resolve);
    chain_add_node(CHAIN_MDE, "schedule",
                   "dispatch_token", "dispatch_token",
                   schedule_resolve);
    chain_add_node(CHAIN_MDE, "execute",
                   "dispatch_token", "compute_result",
                   execute_resolve);
    chain_add_node(CHAIN_MDE, "result_emit",
                   "compute_result", "tx_completion",
                   result_emit_resolve);
    return 0;
}

/* ── Submit ────────────────────────────────────────────────────── */

int mde_chain_submit(mde_compute_request_t *req)
{
    if (CHAIN_MDE < 0)             return -1;
    if (!req || !req->kernel_fn)   return -1;

    s_slot.valid     = 1;
    s_slot.error     = 0;
    s_slot.admitted  = 0;
    s_slot.req       = req;
    req->rc           = -1;
    req->elapsed_tsc  = 0;
    req->backend_used = MDE_BACKEND_NONE;

    int rc = chain_resolve(CHAIN_MDE);

    int err   = s_slot.error;
    int kfrc  = s_slot.res.rc;
    s_slot.valid = 0;
    s_slot.req   = 0;

    if (rc != 0) return -1;
    if (err)     return -1;
    return kfrc;
}
