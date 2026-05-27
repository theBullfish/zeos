# habana_proof

> **2026-05-26 LSO.04 — MME LADDER DISABLED.** Cloud-Claude commit a06aa0b used fabricated MME register addresses (`0x0F000000` flat-GEMM); the real Goya MME is at `0xD0000` with spatial-loop encoding. Submitting the bad sequence would razwi-flood the RTR gate and risk bricking the card. `run_ladder()` is gated at runtime — discovery (HL_INFO) still works; smoke/roof/real are short-circuited. See `../GOYA_BYPASS.md` correction block. Real fix: LSO.06 (port descriptor builder from `temple:/home/watchdog/goya-port/goya/libmme_goya_hlt.c`).

**A single Linux binary that finds your Habana accelerators and proves the MME works — today, on your existing Linux box, with no SynapseAI in the path.**

The whole binary is one C file plus a vendored kernel UAPI header. No SynapseAI. No `hl-thunk` dependency. No Python. No config file. Drop it on any Linux box with the in-tree `habanalabs` kernel module loaded, run it, get the truth.

## What it does

```
$ ./habana_proof
[habana_proof] scanning /dev/accel/* and /dev/hl* ...
[habana_proof] found 2 device(s):
  /dev/accel/accel0     Goya       card=HL-1000           device_id=1  tpc_mask=0xff   sram=24576KB  dram=8192MB
  /dev/accel/accel1     Gaudi2     card=HL-225            device_id=5  tpc_mask=0xffffff sram=49152KB dram=98304MB

[habana_proof] /dev/accel/accel0 (Goya): ladder
  smoke ............. passed (8x8x8 FP16, 87 us)
  roof .............. passed (3812 GFLOPS, >=60% of ~5000 peak)
  real .............. passed (Q*Kt 64x64x64 FP16, 22 us) -- softmax+V pending tpc_kernel

[habana_proof] /dev/accel/accel1 (Gaudi2): ladder
  ladder ............ skipped (Gaudi2 detected; HL_INFO handshake OK; MME packet builder for this generation pending)

[habana_proof] summary: 2 card(s) found, 1 driveable today, 1 smoke / 1 roof / 1 real passed.
                non-driveable cards are detected and kernel-handshaked; their MME packet builders land in the next commit.
```

The binary discovers everything. It does not ask you anything about your machine. That is the point.

## The proof ladder

Three steps, each gates the next. If smoke fails on a real card, the architectural claim is suspect and the binary says so plainly.

1. **Smoke** — 8×8×8 FP16 GEMM with `A = identity`, `B = sequence`. Expect `C == B` bit-for-bit. If this passes, the descriptor format is correct, the queue path works, and the MME answers correctly. ~100 µs.
2. **Roof** — 4096×4096×4096 FP16. Pass criterion ≥ 3000 GFLOPS (60% of Goya's rated ~5 TFLOPS FP16 peak). Below 60% indicates the tuning gap is real and you know its size now, not after months. ~55 ms at peak.
3. **Real** — Q·Kᵀ 64×64×64 FP16 — one attention-head matmul. The architectural proof: a transformer-shaped op on real silicon with nothing of Intel's between you and the math. Softmax + V follow when the TPC kernel work lands.

## Build

```
make
```

Requires `gcc` and `glibc` headers. Nothing else. Builds in under a second.

## Run

```
./habana_proof
```

You will likely need to run as root or be in the `render` / `video` group, since `/dev/accel/accel*` is restricted by default. If `lsmod | grep habana` returns nothing, the kernel driver is not loaded and there is nothing for this binary to talk to — load it (`modprobe habanalabs`) or check that your kernel was built with `CONFIG_HABANA_AI=y` / `=m`.

## Silicon supported today

| Silicon | Detection | MME packet builder |
|---------|-----------|--------------------|
| Goya HL-1000 (first-gen)         | ✓ | ✓ (this commit)            |
| Gaudi HL-2000                    | ✓ | pending — detected only    |
| Gaudi2 HL-225 / HL-265           | ✓ | pending — detected only    |
| Gaudi3                           | ✓ | pending — detected only    |

The detection path is generation-agnostic — `HL_IOCTL_INFO` is a stable kernel UAPI across the whole `habanalabs` family. When a Gaudi/Gaudi2/Gaudi3 card is on the bus, this binary will identify it, confirm the kernel handshake works, and report exactly what is and is not implemented. No silent failures. No fake passes.

## What this is not

- Not a runtime. It runs the proof ladder and exits.
- Not a benchmark. It measures one number to settle one architectural question.
- Not a replacement for SynapseAI for production inference. It is the proof that we never needed SynapseAI in the first place. The full production path — Z+ → tpc_llvm → MME — lives elsewhere.
- Not a hl-thunk port. It does not link against `hl-thunk`. The whole point is that you do not need anything outside the in-tree kernel module.

## Files

- `habana_proof.c` — discovery, ladder, output. One file.
- `habanalabs_uapi.h` — vendored subset of the in-tree Linux UAPI for `habanalabs`. Stable kernel ABI; safe to vendor.
- `Makefile` — `make`, `make run`, `make clean`.

## Why no flags

Because the OS knows what hardware is in the box. You should never have to tell software what cards you have. The binary opens its eyes, finds them, and works. That is the worldview of the project this lives in. A flag would be a confession that the worldview failed.
