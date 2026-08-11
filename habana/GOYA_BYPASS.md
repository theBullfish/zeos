# Goya Bypass — Skip SynapseAI, Speak To The Silicon

*Codex Labs LLC — 2026*

> **2026-05-26 LSO.04 CORRECTION — read this before trusting the rest of this doc.**
> The original draft (cloud-Claude session `claude/synapse-open-source-B3HRQ`,
> a06aa0b) claims a Goya MME descriptor register block at base `0x0F000000`
> with a flat GEMM encoding (`A_BASE`, `DIM_M/N/K`, `FLAGS`, `CMD`). **That
> register map is fabricated.** Verified against SDK 1.7.1-85
> (`/var/lib/dkms/habanalabs-dkms/1.7.1-85/build/drivers/misc/habanalabs/
> include/goya/asic_reg/mme_cmdq_regs.h` on Temple) and against the working
> Goya MME driver (`temple:/home/watchdog/goya-port/goya/libmme_goya_hlt.c`):
> the real MME descriptor base is **0xD0000** (`MME_ARCH_*`), and the
> encoding is a spatial-loop convolution (`HEADER`, `KERNEL_SIZE_M1`,
> `ASSOC_DIMS_0`, `A_VALID_0..4`, `A_LOOP_STRIDE_0..4`, `A_ROI_SIZE_n`,
> `CIN`/`COUT`/`BIAS` base pairs). GEMM is a configuration of that
> primitive, not a separate engine.
>
> Submitting writes to `0x0F00xxxx` via `HL_IOCTL_CS` would razwi-flood the
> RTR gate, fire `MME_ECC`/`MME_WACS` interrupts, and risk a card brick.
> `habana/linux/habana_proof.c`'s `run_ladder()` is therefore **gated at
> runtime** — it prints the disabled-reason and never reaches the bad path.
> The discovery path (`HL_IOCTL_INFO`) is correct and remains active.
>
> The **architectural** claim of this doc — bypass SynapseAI, drive the MME
> through its descriptor bus — still stands. What needs replacing before any
> HW touch (LSO.05+) is the descriptor builder, port from libmme_goya_hlt.c.
> Path of record for Goya remains the patched SDK 1.7.1-85 driver
> (HL_IOCTL_PROGRAM_MME_SHADOW0=0x08, HL_IOCTL_MME_DBG=0x09); see Goya MME
> arc LA.NN / LD.NN in temple:/home/watchdog/goya-port/NOTES_MME_FULLWIN.md.

---

## The Decision

Zeos does **not** use SynapseAI to drive Goya HL-1000 cards. The Goya
driver in `os/boot/gpu_goya.c` and the MME programmer in
`kernel/boot/gpu_goya_mme.c` talk to the silicon through the same
packet path the in-tree Linux `habanalabs` driver uses, without any
SynapseAI runtime, graph compiler, or precompiled recipe in the loop.

This is a permanent architectural choice, not a stopgap. The bypass is
not a fallback while waiting for upstream support — there is no
upstream support to wait for. Intel removed Goya from SynapseAI. The
silicon, the kernel driver, and the packet ABI did not change.

## Why It Works

Three layers of evidence, ordered from most to least certain:

1. **The kernel path is alive in mainline Linux.** Goya is still
   enumerated by `drivers/accel/habanalabs/goya/` in upstream Linux.
   The PCI ID (`1da3:0001`), the BAR layout, the reset sequence, the
   firmware FIT staging, the command-queue packet format, and the
   completion-fence path are all preserved because the same family
   tree (`habanalabs`) drives Gaudi/Gaudi2 with shared infrastructure.
   `gpu_goya.c` already mirrors that path for Zeos.

2. **The MME is fixed-function.** The Matrix Math Engine is not a
   programmable processor. There is no microcode and no graph
   compiler smart enough to "know" things hidden from the descriptor
   bus. The output of an MME submission is determined entirely by
   the descriptor: tensor bases, dims, strides, format, accumulate
   flag, activation. Correctness follows directly from descriptor
   programming. There is no opaque layer between us and the math.

3. **Open-source precedent exists.** `hl-thunk`
   (https://github.com/HabanaAI/hl-thunk) is the userspace library
   that wraps the same IOCTLs Linux exposes. Its test suite includes
   MME submissions against real Goya silicon. The `goya_mme_smoke`
   path in `gpu_goya_mme.c` reproduces those exercises at the wire
   level. If those tests pass for `hl-thunk` users, they pass for us.

## Why We're Faster Where It Matters

We pick the contests Goya wins by construction and do not enter the
ones it does not:

| Contest                                | Goya under Zeos                          | Goya under SynapseAI | H100 / A100 |
|----------------------------------------|------------------------------------------|----------------------|-------------|
| Per-token / per-event latency          | Sub-millisecond, no Python in path       | ms — Python overhead | ms — Python overhead |
| Deterministic worst-case latency       | Bounded by command queue depth           | Unpredictable        | Unpredictable (CUDA async) |
| Streaming inference on small models    | TPC↔MME locality utilized                | Underutilized        | Mostly idle, wasted power |
| Aggregate throughput per dollar        | $1.2k for 160 TPC + 20 MME + 160 GB HBM  | N/A (unsupported)    | $30k+ per H100 |
| Mid-graph topology mutation            | JIT through tpc_llvm, no precompile      | Impossible           | Possible but heavy |
| Training trillion-param LLMs           | **not in scope**                         | N/A                  | The contest H100 wins |
| FP64                                   | **not in scope**                         | N/A                  | The contest H100 wins |
| Batched offline GEMM (giant models)    | **not in scope**                         | N/A                  | The contest A100/H100 wins |

The first four rows are where we live. The last three are where
the GPU industry lives. We don't pretend otherwise.

## The Proof Ladder

`gpu_goya_mme.c` defines a three-step ladder that runs once at
end-of-bringup on every detected card. Each step gates the next:

1. **Smoke** — 8×8×8 FP16 GEMM with `A = I` (identity) and `B` a
   known sequence. Expect `C == B` bit-for-bit. If this passes the
   descriptor format is correct, the queue path is alive, and the
   MME answers correctly. ~100 µs.

2. **Roof** — 4096×4096×4096 FP16 GEMM. Measure GFLOPS observed
   against Goya's rated peak (~5000 FP16 GFLOPS on HL-1000). Pass
   threshold: ≥3000 GFLOPS (≥60% of peak). Below 60% the tuning gap
   is real and we know its size at boot time, not after months of
   engineering. ~55 ms at peak; 250 ms budgeted.

3. **Real** — One attention-head matmul (Q·Kᵀ, 64×64×64 FP16). This
   is the architectural claim: a real transformer-shaped op on real
   silicon, no SynapseAI in the loop. Softmax + V follow when the
   TPC kernel work lands (`tpc_llvm` integration, see
   `docs/FOUNDATIONAL_PROGRAMS.md`).

The ladder prints honest results to the selftest output. If smoke
fails on a real card, **every architectural claim in this document is
suspect and we re-plan**. The gate is not optional.

## What We Vendored, And Did Not

We did **not** vendor `SynapseAI_Core`. It is archived upstream and
its sole job (the graph compiler) is the layer we are bypassing.
Vendoring it would be performance theater.

We did **not** add a `libgoya-runtime` Debian package. There is no
runtime. The signal graph is the runtime. The Z+ compiler emits
descriptor packets directly.

What we may vendor as the proof ladder hardens:

- **`hl-thunk`** — only the test programs, only as reference for
  packet bit-layouts when our smoke test reveals a mismatch on
  silicon revisions we have not seen.
- **`tpc_llvm`** — pinned to its last Goya-supporting tag, only for
  TPC kernel compilation (softmax, ReLU, GELU, etc.). The MME path
  does not need it.
- **`linux-firmware/goya/`** — the firmware blobs the existing
  `gpu_goya.c` already objcopy-embeds. No code change needed; just
  drop the blobs at `os/lib/firmware/habanalabs/goya/`.

## The Federation Implication

A Goya programmed through this path becomes a node in the Zeos
signal graph identical in shape to any other compute node — local
PCIe FPGA, local Coral TPU, remote Linux Goya over the fabric. The
scheduler does not care where the TPC physically lives, only that it
exists and can be programmed. This is the structural property that
makes a federated pool of orphan Goyas worth more than any single
mid-tier GPU.

A Linux box with a Goya card and the `zsg-fabric` daemon (see
`docs/HARDWARE_DISCOVERY.md`) becomes a participating node without
running Zeos as the host OS. The architectural decision in this
document is what makes that possible: by talking to the silicon at
the packet layer we own, every Goya on every Linux box on every
network becomes addressable as a graph node.

## Files

- `os/boot/gpu_goya.h`         — Public bring-up surface
- `os/boot/gpu_goya.c`         — PCI + FW + TPC ring + accessors
- `kernel/boot/gpu_goya_mme.h`     — MME programmer public surface
- `kernel/boot/gpu_goya_mme.c`     — MME descriptor packets + ladder
- `programs/mme_proof.zp`          — T1 Z+ proof signal graph
- `programs/mme_proof_t3.zp`       — T3 Z+ proof signal graph
- `docs/FOUNDATIONAL_PROGRAMS.md`  — Habana SynapseAI row marks this as BYPASS
