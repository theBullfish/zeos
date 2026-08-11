# Zeos paradigm conversion (queued work)

After the POSIX compat layer is done, convert each driver to its native
signal-chain form. This is the *make Zeos actually be Zeos* pass.

## Honest current state (2026-05-03)

The kernel boots, drives modern and old hardware, talks TLS, mounts
filesystems, plays audio, drives USB. **Every driver in the tree right
now is shaped like a Linux driver:** imperative function calls
(`e1000_send`, `nvme_read`, `hda_play_pcm`, `https_get`), flat-pointer
DMA, no provenance, no chain integration. That's correct for the **POSIX
compat layer** in the README architecture — but the **Zeos Native API**
above it (signal chains, CFA, MasQ, Zixel) hasn't been touched.

This document is the queued plan to fix that.

## Architecture refresher (from /README.md)

```
┌─────────────────────────────────────────────────┐
│                 Developer Surface                │
│   (familiar tools: git, docker, python, etc.)   │
├─────────────────────────────────────────────────┤
│              POSIX Compat Layer                  │  ← we just shipped this
│      (lightweight Linux guest runtime)          │
├─────────────────────────────────────────────────┤
│              Zeos Native API                     │  ← not yet touched
│    (direct signal chain access for opt-in)      │
├──────────┬──────────┬───────────┬───────────────┤
│   MDE    │  VAULT   │   MasQ    │    Barca      │
│ Runtime  │ Storage  │ Temporal  │  Buffer Mgmt  │
│          │          │ Wayfinder │               │
├──────────┴──────────┴───────────┴───────────────┤
│              Zeos Kernel                         │
│  Signal Chain Scheduler │ CFA Memory Model      │
│  Device Signal Graph    │ Zero-Copy I/O         │
├─────────────────────────────────────────────────┤
│          Zixel + NTS Hardware Layer              │
│  Proprioception │ Timing Correction │ Telemetry │
├─────────────────────────────────────────────────┤
│            Commodity x86 Hardware                │
└─────────────────────────────────────────────────┘
```

## What needs to happen

### 1. Drivers register as chain nodes
- Each device declares input/output signal ports + a contract
- `os/boot/chain_registry.c` is the entry point; nodes get IDs that
  match the existing `CHAIN_*` constants
- Example contract for a NIC:
  - `in: frame_to_send`
  - `out: received_frame`
  - `out.zixel: tx_completion_timing`
  - `out.health: link_state`

### 2. CFA (Codex Fractal Addressing)
- Patent filed 2026-03-15. Rust lib at `~/cfa-lib` (oracle eliminated, TPM sealed).
- Replace direct phys/virt pointers with CFA addresses for any
  security-relevant memory (TLS keys, VAULT data, kernel structs)
- Boundary: hardware DMA still needs flat pointers (the chip can't
  dereference a fractal address); CFA wraps the kernel-side handle

### 3. MasQ temporal wayfinding
- Spec phase per README. No code yet in kernel.
- Every state change records: who, what, when, why-prior-state
- Initial scope: VAULT writes already version internally; MasQ extends
  this to **all** kernel state — network config, TLS sessions, mounted
  filesystems, USB device attach/detach

### 4. Zixel proprioception
- DARPA white paper exists. Sensing layer below kernel in hardware.
- First kernel-side step: capture timing deltas on every I/O completion
  (we already use TSC for benchmarks; promote it to a global awareness
  signal that the scheduler can read)

### 5. Z+ as the native development language
- Current interpreter: 4 builtins (`emit`, `print`, `gate`, `delta`),
  structural ops only. README implies the full language: `knee`,
  `sustained`, `vault.*`, `net.*`, `fs.*`.
- Need parser + runtime extensions for the missing constructs
  (`programs/FINDINGS.md` tracks gaps)

### 6. MDE Runtime integration
- 928 tests passing per README. Lives in `/home/z13/mde`.
- Workloads route to silicon (CPU/GPU/Goya/FPGA) via MDE.
- Kernel should expose MDE as a signal-chain node that takes compute
  requests and emits results.

### 7. Signal chain scheduler
- Currently the kernel runs a polling main loop in `shell.c`.
- Native model: chain resolution — every connected node fires when its
  inputs are ready, the runtime resolves the whole graph each "tick".
- Replaces time-slicing entirely.

### 8. Pick ONE driver as the proof
- **HDA audio** is the cleanest candidate:
  `pcm_source → volume_filter → hda_pin → hardware_dma`
- Already polling-driven, smallest API surface, no concurrency complexity.
- Once HDA proves the pattern, replicate for: NIC TX/RX, NVMe block I/O,
  USB transfers, file I/O.

### 9. Native developer surface
- Shell commands:
  - `zeos status` — chain graph view
  - `masq inspect <node>`
  - `mde route <workload>`
  - `chain trace <id>` (already exists — extend with MasQ history)

## Why this was queued

Brad's framing: a real OS is judged by what it can do (boot on hardware,
talk to gear, run programs). The compat layer lets it do that. The native
layer gives the OS its identity but isn't visible to a user just trying
to get on WiFi. Compat first; native pass after.

## Order of operations when we come back

1. HDA conversion (smallest, cleanest demo)
2. `chain_registry` surface design — what every device contract looks like
3. NIC drivers (4 of them) get the same treatment
4. NVMe + AHCI block layer goes through MasQ for write-provenance
5. CFA wrappers around TLS state + VAULT internals
6. Z+ extends to express the registered chains
7. MDE plugged in as a chain node
8. Scheduler swap
