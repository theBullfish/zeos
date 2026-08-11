/*
 * Zeos -- GPU compute backend registry.
 *
 * Multi-backend selection: round-robin across same-class backends,
 * least-loaded with latency tie-break, shape-aware (stub).
 * Cooldown after 3+ consecutive errors. CPU is never cooldown'd.
 *
 * Tiny registry of compute backends. The CPU backend is registered
 * unconditionally by gpu_compute_init(); other backends register
 * themselves once their device probe succeeds (e.g. each Goya card
 * registers as "goya-N" once bring-up completes).
 *
 * Today only the CPU backend's dispatch actually executes user kernels.
 * The virtio-virgl backend is registered honestly: it tags the dispatch
 * as routed-to-GPU but falls back to CPU execution because no shader
 * compiler integrates yet. Goya backends round-trip a TPC NOP and run
 * the kernel_fn on the host (chip can't execute arbitrary host C).
 *
 * Selection happens in gpu_compute_pick_with(). Stats (inflight,
 * EWMA latency, consecutive_errors, cooldown) are updated by the
 * gpu_compute_dispatch_tracked() wrapper that CHAIN_MDE.execute calls.
 */

#include "gpu_compute.h"
#include "kprint.h"
#include "timer.h"
#include <stdint.h>
#include <stddef.h>

static gpu_compute_backend_t *s_backends[GPU_COMPUTE_MAX_BACKENDS];
static int s_count = 0;
static int s_inited = 0;
static int s_policy = MDE_SELECT_LEAST_LOADED;

/* Round-robin cursor per class. We don't persist a per-class table --
 * a single global cursor that scans within-class on each call is enough
 * for fair rotation across N goya-* backends. */
static uint32_t s_rr_cursor = 0;

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

/* ── Helpers ───────────────────────────────────────────────────── */

static int name_prefix_len(const char *n)
{
    /* Class prefix = up to and including the trailing '-' if present
     * (e.g. "goya-3" -> "goya-"); else the whole name. */
    if (!n) return 0;
    int i = 0;
    while (n[i]) {
        if (n[i] == '-') return i + 1;
        i++;
    }
    return i;
}

__attribute__((unused))
static int same_class(const gpu_compute_backend_t *a,
                      const gpu_compute_backend_t *b)
{
    if (!a || !b || !a->name || !b->name) return 0;
    int la = name_prefix_len(a->name);
    int lb = name_prefix_len(b->name);
    if (la != lb) return 0;
    for (int i = 0; i < la; i++) {
        if (a->name[i] != b->name[i]) return 0;
    }
    return 1;
}

static int is_cpu(const gpu_compute_backend_t *b)
{
    return b && b->device_id < 0;
}

static int in_cooldown(const gpu_compute_backend_t *b, uint64_t now)
{
    if (!b) return 0;
    if (is_cpu(b)) return 0;  /* CPU is never cooldown'd. */
    return b->cooldown_until_tsc != 0 && now < b->cooldown_until_tsc;
}

/* ── Registry ──────────────────────────────────────────────────── */

void gpu_compute_init(void)
{
    if (s_inited) return;
    s_inited = 1;
    s_count  = 0;
    s_policy = MDE_SELECT_LEAST_LOADED;
    s_rr_cursor = 0;
    /* CPU backend is always first so the legacy fallback path can find
     * it at index 0. */
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
    /* Reject duplicate registration by pointer. */
    for (int i = 0; i < s_count; i++) {
        if (s_backends[i] == backend) return;
    }
    /* Zero the live-stats fields. The caller may have set name/dispatch/
     * capabilities; the stat block belongs to the registry. */
    backend->inflight_count       = 0;
    backend->total_dispatched     = 0;
    backend->total_dispatch_tsc   = 0;
    backend->last_dispatch_tsc    = 0;
    backend->last_completion_tsc  = 0;
    backend->ewma_dispatch_tsc    = 0;
    backend->consecutive_errors   = 0;
    backend->cooldown_until_tsc   = 0;

    s_backends[s_count++] = backend;
    kputs("[gpu_compute] registered backend: ");
    kputs(backend->name ? backend->name : "(unnamed)");
    kputs("\n");
}

int gpu_compute_count(void) { return s_count; }

gpu_compute_backend_t *gpu_compute_get(int idx)
{
    if (idx < 0 || idx >= s_count) return 0;
    return s_backends[idx];
}

int gpu_compute_set_policy(int policy)
{
    if (policy != MDE_SELECT_FIRST &&
        policy != MDE_SELECT_ROUND_ROBIN &&
        policy != MDE_SELECT_LEAST_LOADED &&
        policy != MDE_SELECT_SHAPE_AWARE) {
        return -1;
    }
    s_policy = policy;
    return 0;
}

int gpu_compute_get_policy(void) { return s_policy; }

const char *gpu_compute_policy_label(int policy)
{
    switch (policy) {
        case MDE_SELECT_FIRST:        return "FIRST";
        case MDE_SELECT_ROUND_ROBIN:  return "ROUND_ROBIN";
        case MDE_SELECT_LEAST_LOADED: return "LEAST_LOADED";
        case MDE_SELECT_SHAPE_AWARE:  return "SHAPE_AWARE";
        default: return "?";
    }
}

int gpu_compute_cooldown_count(void)
{
    uint64_t now = timer_read_tsc();
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        if (in_cooldown(s_backends[i], now)) n++;
    }
    return n;
}

/* ── Selection policies ────────────────────────────────────────── */

/* Find CPU fallback (last-resort). */
static gpu_compute_backend_t *find_cpu(void)
{
    for (int i = 0; i < s_count; i++) {
        if (is_cpu(s_backends[i])) return s_backends[i];
    }
    return s_count > 0 ? s_backends[0] : 0;
}

/* MDE_SELECT_FIRST: legacy behavior. First non-CPU backend that
 * can_dispatch and isn't in cooldown. CPU fallback. */
static gpu_compute_backend_t *pick_first(void *args)
{
    uint64_t now = timer_read_tsc();
    for (int i = 0; i < s_count; i++) {
        gpu_compute_backend_t *b = s_backends[i];
        if (!b || is_cpu(b)) continue;
        if (in_cooldown(b, now)) continue;
        if (b->can_dispatch && b->can_dispatch(args)) return b;
    }
    return find_cpu();
}

/* MDE_SELECT_ROUND_ROBIN: rotate across all non-CPU eligible backends,
 * preferring the same class as the previously-picked one. The simple
 * implementation rotates a global cursor across the full list and
 * returns the first eligible non-CPU hit; this gives fair rotation
 * across N goya-* cards. CPU fallback when nothing else is eligible. */
static gpu_compute_backend_t *pick_round_robin(void *args)
{
    if (s_count <= 0) return 0;
    uint64_t now = timer_read_tsc();
    int seen = 0;
    int idx  = (int)(s_rr_cursor % (uint32_t)s_count);
    for (int step = 0; step < s_count; step++) {
        idx = (idx + 1) % s_count;
        gpu_compute_backend_t *b = s_backends[idx];
        seen++;
        if (!b || is_cpu(b)) continue;
        if (in_cooldown(b, now)) continue;
        if (!b->can_dispatch || !b->can_dispatch(args)) continue;
        s_rr_cursor = (uint32_t)idx;
        return b;
    }
    (void)seen;
    return find_cpu();
}

/* MDE_SELECT_LEAST_LOADED: pick eligible non-CPU backend with lowest
 * inflight_count; tie-break on lowest EWMA dispatch latency. CPU
 * fallback. */
static gpu_compute_backend_t *pick_least_loaded(void *args)
{
    uint64_t now = timer_read_tsc();
    gpu_compute_backend_t *best = 0;
    for (int i = 0; i < s_count; i++) {
        gpu_compute_backend_t *b = s_backends[i];
        if (!b || is_cpu(b)) continue;
        if (in_cooldown(b, now)) continue;
        if (!b->can_dispatch || !b->can_dispatch(args)) continue;
        if (!best) { best = b; continue; }
        if (b->inflight_count < best->inflight_count) {
            best = b;
        } else if (b->inflight_count == best->inflight_count &&
                   b->ewma_dispatch_tsc != 0 &&
                   (best->ewma_dispatch_tsc == 0 ||
                    b->ewma_dispatch_tsc < best->ewma_dispatch_tsc)) {
            best = b;
        }
    }
    return best ? best : find_cpu();
}

/* MDE_SELECT_SHAPE_AWARE: honestly-stubbed. We try to match the
 * caller's shape_hints to backends advertising matching capability
 * bits (matrix/conv) and otherwise fall back to LEAST_LOADED. This
 * is wired now so the API is stable; the real shape inference (per
 * GPU_HOLES.md D3) lands when MDE workloads are typed. */
static gpu_compute_backend_t *pick_shape_aware(void *args)
{
    /* Without a typed request struct we can't read shape_hints from
     * the void* args here -- CHAIN_MDE.device_select reads them from
     * mde_compute_request_t and uses the can_dispatch callback to
     * communicate eligibility. For now: defer to LEAST_LOADED. */
    return pick_least_loaded(args);
}

gpu_compute_backend_t *gpu_compute_pick_with(int policy, void *args)
{
    if (s_count <= 0) return 0;
    switch (policy) {
        case MDE_SELECT_FIRST:        return pick_first(args);
        case MDE_SELECT_ROUND_ROBIN:  return pick_round_robin(args);
        case MDE_SELECT_LEAST_LOADED: return pick_least_loaded(args);
        case MDE_SELECT_SHAPE_AWARE:  return pick_shape_aware(args);
        default:                      return pick_least_loaded(args);
    }
}

gpu_compute_backend_t *gpu_compute_pick(void *args)
{
    return gpu_compute_pick_with(s_policy, args);
}

/* ── Tracked dispatch (stats + cooldown) ───────────────────────── */

int gpu_compute_dispatch_tracked(gpu_compute_backend_t *b,
                                 int (*kernel_fn)(void *),
                                 void *args,
                                 uint64_t *elapsed_out)
{
    if (!b || !b->dispatch || !kernel_fn) return -1;

    b->inflight_count++;
    b->last_dispatch_tsc = timer_read_tsc();

    uint64_t elapsed = 0;
    int rc = b->dispatch(kernel_fn, args, &elapsed);

    /* Decrement inflight, update completion stats. */
    if (b->inflight_count > 0) b->inflight_count--;
    b->last_completion_tsc = timer_read_tsc();
    b->total_dispatched++;
    b->total_dispatch_tsc += elapsed;

    /* EWMA with alpha=1/8: ewma = ewma + (sample - ewma) / 8. */
    if (b->ewma_dispatch_tsc == 0) {
        b->ewma_dispatch_tsc = elapsed;
    } else if (elapsed > b->ewma_dispatch_tsc) {
        b->ewma_dispatch_tsc += (elapsed - b->ewma_dispatch_tsc) >> 3;
    } else {
        b->ewma_dispatch_tsc -= (b->ewma_dispatch_tsc - elapsed) >> 3;
    }

    /* Cooldown bookkeeping. CPU never cooldown'd. */
    if (rc < 0 && !is_cpu(b)) {
        b->consecutive_errors++;
        if (b->consecutive_errors >= GPU_COOLDOWN_ERROR_THRESHOLD) {
            uint64_t freq = timer_tsc_freq();
            uint64_t ticks_for_60s = freq;       /* default 1s if no calib */
            if (freq > 0) {
                /* 60 seconds in TSC ticks. */
                ticks_for_60s = freq * 60ull;
            }
            b->cooldown_until_tsc = b->last_completion_tsc + ticks_for_60s;
            kputs("[gpu_compute] backend ");
            kputs(b->name ? b->name : "(unnamed)");
            kputs(" entering 60s cooldown after ");
            kput_dec((uint64_t)b->consecutive_errors);
            kputs(" consecutive errors\n");
        }
    } else if (rc >= 0) {
        if (b->consecutive_errors > 0 || b->cooldown_until_tsc != 0) {
            kputs("[gpu_compute] backend ");
            kputs(b->name ? b->name : "(unnamed)");
            kputs(" recovered, clearing cooldown\n");
        }
        b->consecutive_errors = 0;
        b->cooldown_until_tsc = 0;
    }

    if (elapsed_out) *elapsed_out = elapsed;
    return rc;
}

/* ── Reporting ─────────────────────────────────────────────────── */

/* Convert TSC ticks to microseconds. timer_tsc_freq() returns Hz. */
static uint64_t tsc_to_us(uint64_t tsc)
{
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) return 0;
    /* Avoid overflow: tsc * 1e6 can blow uint64 for large tsc, so
     * divide first when tsc is large. */
    if (tsc < (uint64_t)1ull << 44) {
        return (tsc * 1000000ull) / freq;
    }
    return (tsc / freq) * 1000000ull;
}

void gpu_compute_dump_load(void)
{
    kputs("\nGPU compute load (policy: ");
    kputs(gpu_compute_policy_label(s_policy));
    kputs(")\n");
    kputs("idx  name              inflight  total   avg_us    cooldown\n");
    uint64_t now = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    for (int i = 0; i < s_count; i++) {
        gpu_compute_backend_t *b = s_backends[i];
        if (!b) continue;
        kput_dec((uint64_t)i);
        kputs("    ");
        const char *nm = b->name ? b->name : "(unnamed)";
        kputs(nm);
        /* Pad name to 14. */
        int nlen = 0; while (nm[nlen]) nlen++;
        for (int p = nlen; p < 14; p++) kputc(' ');
        kputs("  ");
        kput_dec(b->inflight_count);
        kputs("         ");
        kput_dec(b->total_dispatched);
        kputs("       ");
        if (b->total_dispatched == 0) {
            kputs("--");
        } else {
            uint64_t avg_us = tsc_to_us(b->ewma_dispatch_tsc);
            kput_dec(avg_us);
        }
        kputs("        ");
        if (in_cooldown(b, now)) {
            uint64_t left = b->cooldown_until_tsc - now;
            uint64_t left_s = freq ? (left / freq) : 0;
            kputs("yes (");
            kput_dec(left_s);
            kputs("s left)");
        } else if (b->consecutive_errors > 0) {
            kputs("--  (errs=");
            kput_dec((uint64_t)b->consecutive_errors);
            kputs(")");
        } else {
            kputs("--");
        }
        kputc('\n');
    }
    kputc('\n');
}

void gpu_compute_print_selftest_line(void)
{
    kputs("  MDE selector .......... policy=");
    kputs(gpu_compute_policy_label(s_policy));
    kputs(", ");
    kput_dec((uint64_t)s_count);
    kputs(" backend(s), ");
    kput_dec((uint64_t)gpu_compute_cooldown_count());
    kputs(" in cooldown\n");
}
