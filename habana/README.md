# habana — Goya & Gaudi bypass

> **2026-05-26 LSO.04 CORRECTION:** the original cloud-Claude commits in this folder use fabricated Goya MME register addresses (`0x0F000000` base, flat GEMM layout). Real Goya MME is at `0xD0000` with spatial-loop encoding (see `GOYA_BYPASS.md` correction block + `temple:/home/watchdog/goya-port/goya/libmme_goya_hlt.c`). The Linux binary's MME ladder is gated off at runtime; discovery path remains live. Architectural bypass-vs-SynapseAI argument still holds — implementation needs the real descriptor builder (LSO.06).

**Start here. This folder is the entry point for everything related to driving Habana accelerators (Goya HL-1000, Gaudi, Gaudi2, Gaudi3) directly, with no SynapseAI in the path.**

If you are a person, a future Claude session, or anything in between, you can land here cold and find every part of the work in two clicks.

---

## What's here

```
habana/
├── README.md            ← you are here
├── GOYA_BYPASS.md       ← architectural choice — read this first
├── linux/               ← Linux side: a single binary you can build and run TODAY on any Linux box
└── zeos/                ← Zeos side: Z+ proof + pointer to kernel-C driver
```

## The two halves

There are two halves to the work, and they are deliberately separate:

### `habana/linux/` — Run on any Linux box today

A single C binary, no external dependencies beyond glibc. Drop it on a Linux machine with the in-tree `habanalabs` kernel module loaded, run it, and it self-discovers every Habana card on the bus, identifies the silicon generation, and runs the smoke / roof / real ladder. No flags, no config, no "tell me what you have."

```
cd habana/linux
make
./habana_proof
```

That's it. The full story is in `habana/linux/README.md`.

### `habana/zeos/` — Run inside Zeos when it boots

The Zeos kernel-side bypass. Same wire format as the Linux version, but built into Zeos's kernel so the cards come up natively when a Zeos box boots.

The kernel C lives at `kernel/boot/gpu_goya.c` and `kernel/boot/gpu_goya_mme.c` (kernel convention — every Zeos driver lives in `kernel/boot/`). The Z+ proof programs live in this folder. The full story is in `habana/zeos/README.md`.

## The architectural choice

`habana/GOYA_BYPASS.md` is the one document that explains *why* this work exists at all: Intel removed Goya from SynapseAI's graph compiler. The silicon, the in-tree Linux `habanalabs` driver, and the packet ABI did not change. SynapseAI was a one-shot graph compiler sitting between the application and a fixed-function descriptor bus; Zeos's signal graph and the Linux userspace binary in this folder both occupy the same role natively, without needing it.

Read that document before any other if you're new to this work.

## What's not here (yet)

- **Gaudi / Gaudi2 / Gaudi3 MME packet builders.** Detection of those generations works today (`HL_IOCTL_INFO` is generation-agnostic, the Linux binary reports them honestly), but the packet builders themselves land when we have one to validate against.
- **TPC kernel work** (`tpc_llvm` integration). Once that lands, the "real" step of the ladder extends from "Q·Kᵀ alone" to the full attention head (Q·Kᵀ → softmax → ·V).
- **Federation daemon** (`zsg-fabric`). The story where Goyas on multiple Linux boxes federate over the network. Not in this commit; sketched in conversation, pending.

## The proof gate

Everything in this folder is gated on one measurement: does the smoke step pass on real Habana silicon? If yes, the architectural claim becomes measured fact. If no, every assertion in `GOYA_BYPASS.md` is suspect and we re-plan with a root cause. The ladder is not optional. It is the contract.
