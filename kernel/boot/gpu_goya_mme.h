/*
 * Zeos -- Habana Goya HL-1000 MME (Matrix Math Engine) driver surface.
 *
 * Goya's MME is a fixed-function GEMM engine sitting next to the eight
 * TPCs on every HL-1000 die. SynapseAI's graph compiler used to program
 * it for us; Intel dropped Goya from that compiler. The engine itself
 * never left the silicon. The kernel/firmware path stays alive (mainline
 * `drivers/accel/habanalabs/goya/` still owns it). We just program it
 * directly: build descriptor packets, push them on the command queue,
 * fence the completion. No SynapseAI runtime in the path. No graph
 * compile step. No precompiled recipe.
 *
 * Why we know this works (in evidence order, see docs/GOYA_BYPASS.md):
 *   1) The kernel driver in upstream Linux still talks to Goya's MME.
 *      Same IOCTL family, same packet layout, same fence path. We
 *      mirror only the on-the-wire packet bytes and register offsets
 *      — both are hardware-defined and non-copyrightable, same as
 *      the BAR0 layout gpu_goya.c already uses.
 *   2) The MME is not a programmable processor. It is a deterministic
 *      tensor unit whose output is determined entirely by descriptor
 *      fields. Correctness follows from descriptor programming, not
 *      from any opaque scheduling layer.
 *   3) hl-thunk's open-source test suite exercises the same packet
 *      path on real silicon. Where we have it, the smoke test below
 *      reproduces those exercises bit-for-bit.
 *
 * The smoke/roof/real test ladder defined here gates the architectural
 * claim. If smoke fails on a real card, every assumption above is
 * suspect and we re-plan.
 */

#ifndef ZEOS_GPU_GOYA_MME_H
#define ZEOS_GPU_GOYA_MME_H

#include <stdint.h>

/* The MME path addresses cards by index — same numbering as
 * gpu_goya_device(idx). This keeps gpu_goya_mme.c independent of the
 * internal goya_dev_t struct in gpu_goya.c; the accessor functions
 * goya_dev_*() bridge state across the two files. */

/* Numeric format for an MME GEMM. Goya supports FP32 accumulate over
 * FP16/BF16/INT16/INT8 operands. FP16 is the default for first-pass
 * validation — it is the format the silicon hits peak throughput on
 * and the format every published Goya benchmark uses. */
typedef enum {
    GOYA_MME_FP16 = 0,
    GOYA_MME_BF16 = 1,
    GOYA_MME_INT16 = 2,
    GOYA_MME_INT8  = 3,
} goya_mme_fmt_t;

/* One GEMM operation:  C[M,N] = A[M,K] * B[K,N]  (+ C if accumulate).
 * Bases are device-side (HBM) addresses inside BAR2's aperture, in
 * bytes. Strides are in elements. If accumulate=0 the engine writes
 * C; if 1 it reads-modify-writes. activation is currently 0 (none) —
 * fused activations land once smoke + roof pass on a real card. */
typedef struct goya_mme_gemm {
    uint64_t       a_base;
    uint64_t       b_base;
    uint64_t       c_base;
    uint32_t       m;
    uint32_t       n;
    uint32_t       k;
    uint32_t       a_stride;
    uint32_t       b_stride;
    uint32_t       c_stride;
    goya_mme_fmt_t fmt;
    uint8_t        accumulate;
    uint8_t        activation;     /* 0 = none. Reserved. */
} goya_mme_gemm_t;

/* Result of one submission. elapsed_ns is the chip-side fence delta
 * measured against the MME completion counter; latency_host_ns is the
 * round-trip we observed from the host (includes doorbell + PCIe). */
typedef struct goya_mme_result {
    int      ok;             /* 1 if the completion fence advanced */
    uint32_t fence_before;
    uint32_t fence_after;
    uint64_t elapsed_ns;     /* chip-measured */
    uint64_t latency_host_ns;/* host round-trip */
    uint64_t gflops_observed;/* 2*M*N*K / elapsed_ns, integer */
} goya_mme_result_t;

/* Build the packet sequence for a GEMM into `buf`, returning the byte
 * count written. Returns 0 if buf is too small or the descriptor is
 * malformed. This is pure (no MMIO); callers may use it to inspect
 * what would be sent without actually submitting. Useful for unit
 * tests on machines without a card. */
uint32_t goya_mme_pack_gemm(const goya_mme_gemm_t *gemm,
                            uint8_t *buf, uint32_t buf_len);

/* Submit one GEMM and wait for completion. Returns 0 on success, -1
 * on timeout, -2 on malformed descriptor. The chip's MME fence counter
 * is mirrored into res->fence_after on completion. Bounded by the
 * timeout argument; passing 0 uses a sane default (100 ms). */
int goya_mme_submit_gemm(int card_idx,
                         const goya_mme_gemm_t *gemm,
                         goya_mme_result_t *res,
                         uint32_t timeout_ms);

/* The three-step proof ladder. Each step gates the next: if smoke
 * fails the rest are skipped. Each step writes one line of selftest
 * output. Safe to call on hosts with no Goya cards — they print
 * "skipped" and return 0. Compute-not-ready cards print "skipped
 * (no fw)" and return 0. The ladder runs once at end-of-bringup from
 * gpu_goya_init. */

/* Step 1 — Smoke: tiny 8x8x8 FP16 GEMM with known operands; verify
 * C bit-for-bit against the host reference. If this passes, the
 * descriptor format is right, the queue path works, and the MME
 * answers correctly. Everything else follows. */
int goya_mme_smoke(int card_idx);

/* Step 2 — Roof: large square GEMM (default 4096x4096x4096 FP16) to
 * measure throughput. Compares to Goya's rated peak (~5 TFLOPS FP16
 * on HL-1000). Pass criterion: >= 60% of peak. Below 60% indicates
 * the tuning gap is real and we know its size on day one. */
int goya_mme_roof(int card_idx, goya_mme_result_t *out);

/* Step 3 — Real: a single attention head -- Q*K^T, softmax (on TPC),
 * *V -- end-to-end on one card with no SynapseAI in the loop. Output
 * verified bit-for-bit against a host reference at low precision.
 * This is the architectural proof: if it passes, the bypass works for
 * a real shape, not just a synthetic GEMM. */
int goya_mme_real_attention_head(int card_idx);

/* Run smoke -> roof -> real and print one selftest line summarizing
 * the ladder state. Called by gpu_goya_init after every card finishes
 * bring-up. Returns the number of cards whose smoke passed. */
int goya_mme_run_ladder_all(void);

/* `goya-mme` shell command: dumps per-card MME ladder state, last
 * roof GFLOPS observation, last error if any. */
void goya_mme_dump_status(void);

#endif /* ZEOS_GPU_GOYA_MME_H */
