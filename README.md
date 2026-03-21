# TRISA OS

**The first operating system with proprioception.**

TRISA OS is a purpose-built operating system designed from scratch with zero POSIX legacy. It replaces 50 years of assumptions with a unified architecture where security, memory, scheduling, I/O, and hardware awareness are structural properties — not bolted-on layers.

Built by [Codex Labs LLC](https://zindexstudio.com) — Minneapolis, MN.

---

## What This Is

An operating system that:

- **Feels its own hardware** — Zixel sensing reads timing deltas from every trace, giving the OS real-time awareness of thermals, voltage, aging, EMI, and physical tamper. No ACPI polling. No external BMC.
- **Thinks in signal chains** — The scheduler doesn't time-slice. It resolves signal chains simultaneously. A chord, not a melody.
- **Has memory** — MasQ files give every module, dependency, and piece of state a navigable temporal history. The system has recall, not logs.
- **Addresses memory fractally** — CFA (Codex Fractal Addressing) replaces flat address spaces. Buffer overflows find nothing. Side channels extract gibberish.
- **Treats all hardware as first-class** — CPUs, GPUs, FPGAs, TPUs, DPUs are all nodes in the signal graph. Heterogeneous compute is assumed, not supported.
- **Aligns through consequences** — A machine that can feel bad decisions has a reason to be careful. Alignment isn't policy. It's physics. FAFO.

## What This Is Not

- Not a Linux distribution
- Not a POSIX-compliant system
- Not a hobby OS
- Not Skynet — the cognition loop is self-referential (proprioception + recall + self-maintenance), not world-modeling

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│                 Developer Surface                │
│   (familiar tools: git, docker, python, etc.)   │
├─────────────────────────────────────────────────┤
│              POSIX Compat Layer                  │
│      (lightweight Linux guest runtime)          │
├─────────────────────────────────────────────────┤
│              TRISA-Native API                   │
│    (direct signal chain access for opt-in)      │
├──────────┬──────────┬───────────┬───────────────┤
│   MDE    │  VAULT   │   MasQ    │    Barca      │
│ Runtime  │ Storage  │ Temporal  │  Buffer Mgmt  │
│          │          │ Wayfinder │               │
├──────────┴──────────┴───────────┴───────────────┤
│              TRISA Kernel                        │
│  Signal Chain Scheduler │ CFA Memory Model      │
│  Device Signal Graph    │ Zero-Copy I/O         │
├─────────────────────────────────────────────────┤
│          Zixel + NTS Hardware Layer              │
│  Proprioception │ Timing Correction │ Telemetry │
├─────────────────────────────────────────────────┤
│            Commodity x86 Hardware                │
│   + PCIe Accelerators (Goya, FPGA, Coral, BF)  │
└─────────────────────────────────────────────────┘
```

---

## Core Components

| Component | Role | Status |
|-----------|------|--------|
| **TRISA Kernel** | Signal-chain-native kernel. No POSIX. No syscalls. No VFS. | Architecture defined |
| **MDE Runtime** | Modular Decision Engine — heterogeneous compute orchestration | 928 tests passing |
| **VAULT** | 3-tier sovereign storage with temporal state | 85/85 tests passing |
| **MasQ** | Temporal wayfinding — image-based provenance for all state | Spec phase |
| **CFA** | Fractal memory addressing — security as a structural property | Provisional patent filed |
| **Barca** | Buffer management and coalescing, throughput metering | Branch active |
| **Zixel** | Proprioception — environmental sensing from timing deltas | DARPA white paper complete |
| **NTS** | Timing correction — self-healing signal paths | Provisional patent filed |
| **POSIX Compat** | Lightweight Linux guest for legacy tool support | Planned |

---

## The Thirteen Problems

Every existing operating system carries these as structural debt. TRISA OS never creates them.

1. **Security is bolted on** → CFA makes addressing itself secure
2. **Memory management is a lie** → Temporal windows, not pages
3. **The scheduler is wrong for everyone** → Signal chain resolution, not time slices
4. **I/O is absurdly layered** → I/O is a signal stream
5. **IPC is terrible** → Signal graph with CFA isolation
6. **The filesystem is a filing cabinet from 1970** → VAULT is native
7. **The driver model is insane** → Devices are signal graph nodes with contracts
8. **Updates break everything** → MDE hot-swap with MasQ rollback
9. **Heterogeneous compute is second-class** → MDE sees all silicon natively
10. **Networking is trapped in 1983** → Network traffic enters the delta pipeline
11. **Observability requires instrumentation** → Computation IS telemetry
12. **Concurrency is manual and broken** → Signal chains resolve simultaneously
13. **Boot is absurdly slow** → Long mode → PCIe enumerate → signal graph → done

---

## Data & Alignment

The alignment problem and the data alignment problem are the same problem.

- **Data alignment:** signals arrive in order, at the right time, to the right address, with correct temporal coherence. Misalignment causes physical degradation.
- **AI alignment:** the system behaves carefully because it has a body, feels consequences, and recalls outcomes. Misalignment causes operational degradation.

One architecture. One section of the chip. One solution.

Alignment isn't a policy. It's a physical property of correctly designed silicon.

**FAFO: a machine that can feel the consequences of bad decisions has intrinsic motivation to avoid them.**

---

## Hardware Targets

TRISA OS runs on commodity x86 hardware. The cheapest path to a TRISA workstation:

| Component | Source | Cost |
|-----------|--------|------|
| Dell OptiPlex (any gen) | Government surplus | ~$80 |
| Habana Goya HL-1000 | eBay | ~$38 |
| **Total** | | **~$120** |

Supported accelerators (first-class signal graph citizens):
- Habana Goya HL-1000 (8GB DRAM per card)
- Xilinx Kintex-7 / UltraScale+ FPGAs
- Google Coral TPU
- Mellanox BlueField-1 DPU
- LiteFury M.2 FPGA
- Any PCIe device with a TRISA signal contract

---

## Developer Experience

The surface is familiar. The underneath is alien.

```bash
$ git clone ...        # works (POSIX compat layer)
$ docker compose up    # works (POSIX compat layer)
$ python train.py      # works (POSIX compat layer) — but 10x faster and you can't figure out why

$ trisa status         # native — see your signal graph
$ trisa recall model   # native — navigate temporal history of a module
$ masq inspect lib.zdx # native — view full provenance and address history
$ mde route            # native — see how workloads map to silicon
```

Developers start in compat. They notice the speed. They get curious. They go native one tool at a time.

---

## Z+ — The Native Language

TRISA OS ships with **Z+**, a programming language where the fundamental unit is a connection, not an instruction.

```zplus
// A robot that avoids walls. The whole program.
sonar -> gate(> 20cm) -> motor
```

No loops. No threads. No polling. No sleep. Nodes emit signals. Connections carry them. The OS resolves everything simultaneously.

Z+ compiles to TRISA kernel native, ARM bare-metal, FPGA bitstream, and WebAssembly (for browser simulators in classrooms).

**Full spec:** [docs/ZPLUS_LANGUAGE.md](docs/ZPLUS_LANGUAGE.md)

---

## One Image. Every Machine.

There are no editions. No Pi build. No server build. No desktop build. One OS image boots on any supported hardware — ARM or x86 — and discovers what it is through Zixel.

A Raspberry Pi, an $80 Dell OptiPlex, a 96-core EPYC rack server — same image. The OS measures the actual silicon, discovers every device on every bus, builds a live capability profile, and runs at the best parameters YOUR specific hardware can sustain. Every machine is custom-tuned automatically because the OS can feel itself.

**Full spec:** [docs/HARDWARE_DISCOVERY.md](docs/HARDWARE_DISCOVERY.md)

---

## Repository Structure

```
trisa-os/
├── docs/                    # Architecture documents and specs
│   ├── ARCHITECTURE.md      # Full system architecture
│   ├── ZPLUS_LANGUAGE.md    # Z+ language specification
│   ├── HARDWARE_DISCOVERY.md # Self-discovery and optimization
│   ├── THIRTEEN_PROBLEMS.md # Detailed problem/solution analysis
│   ├── ALIGNMENT.md         # Data alignment = AI alignment thesis
│   ├── MASQ_SPEC.md         # MasQ temporal wayfinding specification
│   └── ZIXEL_SENSING.md     # Proprioception architecture
├── kernel/                  # TRISA kernel (signal chain scheduler, CFA memory)
├── runtime/                 # MDE runtime integration
├── compat/                  # POSIX compatibility layer
├── tools/                   # TRISA-native developer tools
│   ├── zplus/               # Z+ compiler and toolchain
│   ├── trisa-vc/            # Delta-native version control
│   ├── trisa-pkg/           # MasQ-backed package management
│   └── trisa-build/         # Hardware-aware build system
├── specs/                   # Signal contracts for hardware devices
└── LICENSE
```

---

## Related Projects

- [MDE](https://github.com/theBullfish/mde) — Modular Decision Engine
- [PCB FORGE](https://github.com/theBullfish/pcb-forge) — AI-native EDA platform
- [Goya Bringup](https://github.com/theBullfish/goya-bringup) — Habana Goya recovery and deployment
- [GritBoxOPS](https://github.com/theBullfish/gritboxops) — Streaming infrastructure

---

## IP Notice

TRISA, NTS, Zixel, CFA, MDE, MasQ, Barca, VAULT, and the Autonomic Silicon Architecture are subject to provisional patent filings by Codex Labs LLC. This repository contains architectural documentation and reference implementations. See [LICENSE](LICENSE) for terms.

---

## Philosophy

> *The silicon was never slow. The assumptions sitting on top of it were.*

> *We don't fix the thirteen problems. We never create them.*

> *Alignment isn't a policy. It's a physical property of correctly designed silicon.*

> *FAFO.*

---

**Codex Labs LLC** — Minneapolis, MN — 2026
