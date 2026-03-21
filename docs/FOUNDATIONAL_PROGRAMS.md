# Zeos — Foundational Programs & File Formats

**Target users: developers and AI developers.**

This document maps every foundational program needed for Zeos to be a viable development platform. For each, we assess: can it run in the POSIX compat layer, does it need a native port, or do we build our own? We also identify new file formats Zeos requires.

The philosophy: **compat first, native when it matters.** Developers start in familiar territory. They notice the speed. They go native one tool at a time.

---

## Priority Tiers

- **P0** — Zeos doesn't boot or develop without this
- **P1** — Core dev/AI workflow blocked without this
- **P2** — Important but workarounds exist
- **P3** — Nice to have, can wait

---

## 1. Language Runtimes & Compilers

| Program | Status | Path | Notes |
|---------|--------|------|-------|
| **Z+ compiler** | BUILD | Native | Our language. Targets: Zeos kernel, ARM bare-metal, FPGA bitstream, WASM. P0. |
| **LLVM/Clang** | PORT | Compat → Native backend | Z+ likely compiles through LLVM. Need a Zeos backend for native codegen. P0. |
| **GCC** | COMPAT | POSIX layer | Runs in compat for C/C++ legacy code. Not priority for native port. P2. |
| **Python 3.x** | COMPAT → OPTIMIZE | POSIX layer + native hooks | AI devs live in Python. Runs in compat, but MDE/Zixel/MasQ bindings need native Python extensions. P0. |
| **Rust toolchain** | COMPAT | POSIX layer | Kernel and systems work. Add Zeos target triple to rustc eventually. P1. |
| **Node.js** | COMPAT | POSIX layer | Web tooling, dashboard UIs. P2. |
| **Go** | COMPAT | POSIX layer | Infrastructure tooling. P2. |
| **Julia** | COMPAT | POSIX layer | Scientific/AI compute. P3. |

### Decisions needed:
- Z+ compiler architecture: custom frontend → LLVM IR → Zeos backend? Or fully custom?
- Do we need a Zeos-native Python runtime long-term, or is compat + native extensions sufficient?

---

## 2. Build Systems

| Program | Status | Path | Notes |
|---------|--------|------|-------|
| **zeos-build** | BUILD | Native | Hardware-aware build system. Knows signal graph, routes compilation to available silicon. P0. |
| **CMake** | COMPAT | POSIX layer | Needed for building LLVM, most C/C++ projects. P1. |
| **Make** | COMPAT | POSIX layer | Universal dependency. P1. |
| **Ninja** | COMPAT | POSIX layer | Fast builds. P2. |
| **Meson** | COMPAT | POSIX layer | P3. |
| **Cargo** | COMPAT | POSIX layer | Rust builds. P1. |
| **pip / setuptools** | COMPAT | POSIX layer | Python packaging. P0 for AI devs. |

### Decisions needed:
- zeos-build needs to understand `.zp` files, signal contracts, and MDE routing. Define the build graph format.
- Should zeos-build also handle C/Rust compilation with hardware awareness, or only Z+ projects?

---

## 3. Version Control

| Program | Status | Path | Notes |
|---------|--------|------|-------|
| **Git** | COMPAT → BRIDGE | POSIX layer + zeos-vc bridge | Git runs in compat. zeos-vc adds delta-native temporal history on top. Devs use git commands, zeos-vc enriches with MasQ provenance. P0. |
| **zeos-vc** | BUILD | Native | Delta-native version control. Every commit carries MasQ temporal state. Not a git replacement — a git enhancement that adds what git can't do. P1. |

### Decisions needed:
- zeos-vc format: extend git objects or parallel metadata store?
- How does MasQ provenance attach to git commits?

---

## 4. AI / ML Frameworks

| Program | Status | Path | Notes |
|---------|--------|------|-------|
| **PyTorch** | COMPAT → NATIVE BACKEND | POSIX layer + MDE device backend | Runs in compat. Needs a Zeos device backend so `torch.device("mde")` routes through the signal graph to Goya/FPGA/Coral. This is where "10x faster and you can't figure out why" comes from. P0. |
| **TensorFlow** | COMPAT | POSIX layer | Lower priority than PyTorch. Compat is fine. P2. |
| **JAX** | COMPAT → NATIVE BACKEND | POSIX layer + XLA Zeos plugin | JAX's XLA compiler could target MDE directly. High-value native integration. P1. |
| **ONNX Runtime** | COMPAT → NATIVE | POSIX layer → native signal graph execution | ONNX graphs map naturally to signal chains. A native ONNX executor on the signal graph could be transformative. P1. |
| **Hugging Face (transformers, datasets, tokenizers)** | COMPAT | POSIX layer | Pure Python + compiled extensions. Runs in compat. P1. |
| **vLLM / TGI** | COMPAT → OPTIMIZE | POSIX layer + MDE scheduling | LLM serving. MDE-aware scheduling for KV cache placement across heterogeneous memory. P1. |
| **Jupyter** | COMPAT + NATIVE KERNEL | POSIX layer + Z+ kernel | Jupyter runs in compat. Add a Z+ kernel so notebooks can mix Python and Z+ cells. P1. |
| **MLflow / Weights & Biases** | COMPAT | POSIX layer | Experiment tracking. MasQ could eventually replace these natively. P2. |
| **Triton (OpenAI)** | COMPAT → NATIVE | POSIX layer → Zeos GPU kernel language | Triton compiles Python to GPU kernels. A Zeos backend could target any signal graph node. P2. |

### Decisions needed:
- PyTorch MDE backend: implement as a custom device extension or fork torch?
- ONNX-to-signal-chain compiler: new tool or part of zeos-build?
- MasQ as experiment tracker: replaces MLflow/W&B or complements?

---

## 5. Data & Storage

| Program | Status | Path | Notes |
|---------|--------|------|-------|
| **VAULT** | BUILD | Native | 3-tier sovereign storage. Replaces filesystem for native Zeos. P0. |
| **SQLite** | COMPAT | POSIX layer | Embedded DB. Too useful to replace. P1. |
| **PostgreSQL** | COMPAT | POSIX layer | Production DB for web apps. P2. |
| **Redis** | COMPAT → NATIVE | POSIX layer → signal graph cache | Redis as a signal graph node with CFA-addressed keys could be very fast. P2. |
| **DuckDB** | COMPAT | POSIX layer | Analytics. AI devs use this heavily. P1. |
| **Arrow / Parquet** | COMPAT + NATIVE READERS | POSIX layer + native I/O | Columnar data formats. Native zero-copy readers through the signal graph. P1. |
| **HDF5** | COMPAT | POSIX layer | Scientific data. P2. |

### Decisions needed:
- VAULT API: does it expose a POSIX-like file interface for compat, or is it purely native?
- Do we need a native columnar format, or is Arrow/Parquet + native readers sufficient?

---

## 6. Containers & Virtualization

| Program | Status | Path | Notes |
|---------|--------|------|-------|
| **Docker / Podman** | COMPAT | POSIX layer | Devs expect `docker compose up` to work. Runs in the Linux compat guest. P1. |
| **QEMU** | COMPAT | POSIX layer | Testing, cross-platform dev. P2. |
| **Zeos isolation** | BUILD | Native | Signal graph isolation with CFA. Not containers — structural isolation. Replaces containers for native workloads. P1. |

### Decisions needed:
- Docker in compat is fine, but native Zeos workloads need CFA-based isolation. Define the isolation primitive.
- Is the POSIX compat layer itself a lightweight VM, a container, or a translation layer?

---

## 7. Networking & Communication

| Program | Status | Path | Notes |
|---------|--------|------|-------|
| **curl / wget** | COMPAT | POSIX layer | P1. |
| **OpenSSH** | COMPAT | POSIX layer | Remote access. P0. |
| **nginx / Caddy** | COMPAT | POSIX layer | Web serving. P2. |
| **gRPC** | COMPAT → NATIVE | POSIX layer → signal contract RPC | Signal contracts are already a form of RPC. Native gRPC-like protocol over signal graph. P2. |
| **ZeroMQ / NATS** | COMPAT | POSIX layer | Message passing. Signal graph may replace these natively. P2. |

### Decisions needed:
- Native networking: does network traffic enter the signal graph as delta streams (per the Thirteen Problems)?
- Signal contract RPC: define the wire format.

---

## 8. Developer Tools

| Program | Status | Path | Notes |
|---------|--------|------|-------|
| **Shell (bash/zsh)** | COMPAT | POSIX layer | Devs need a shell. Runs in compat. P0. |
| **zeos shell** | BUILD | Native | Native shell that speaks signal graph. `zeos status`, `zeos recall`, etc. Complements bash, doesn't replace it. P1. |
| **vim / neovim** | COMPAT | POSIX layer | P1. |
| **VS Code (remote)** | COMPAT + EXTENSION | POSIX layer + Zeos extension | VS Code runs remote. Build a Zeos extension for Z+ syntax, signal graph visualization, MasQ timeline. P1. |
| **GDB / LLDB** | COMPAT → EXTEND | POSIX layer + signal chain debugger | Traditional debuggers for compat code. Need a native signal chain debugger for Z+. P1. |
| **zeos-debug** | BUILD | Native | Signal chain debugger. Visualize live signal flow, freeze chains, inspect temporal state. P1. |
| **strace / perf** | COMPAT | POSIX layer | For compat code. Native Zeos doesn't need these — computation IS telemetry. P2. |
| **tmux / screen** | COMPAT | POSIX layer | P2. |
| **ripgrep / fd / fzf** | COMPAT | POSIX layer | Search tools. P1. |

### Decisions needed:
- Z+ language server protocol (LSP) for VS Code / editors
- Signal chain debugger UX: CLI, TUI, or web-based?

---

## 9. Hardware & FPGA Tools

| Program | Status | Path | Notes |
|---------|--------|------|-------|
| **Vivado / Quartus** | COMPAT | POSIX layer | FPGA synthesis. Heavy, proprietary. Compat only path. P1. |
| **Yosys + nextpnr** | COMPAT → INTEGRATE | POSIX layer → zeos-build pipeline | Open-source FPGA toolchain. Integrate into zeos-build for Z+ → bitstream compilation. P1. |
| **OpenOCD** | COMPAT | POSIX layer | Hardware debugging. P2. |
| **Habana SynapseAI** | COMPAT → NATIVE DRIVER | POSIX layer → signal contract | Goya SDK. Need signal contract + native driver for Goya as signal graph citizen. P0. |
| **PCB FORGE** | COMPAT → NATIVE | Own repo | AI-native EDA. Should eventually be a Zeos-native tool. P2. |

### Decisions needed:
- Z+ → FPGA bitstream: through Yosys, or custom synthesis?
- Goya signal contract: what's the interface between MDE and SynapseAI?

---

## 10. Package Management

| Program | Status | Path | Notes |
|---------|--------|------|-------|
| **zeos-pkg** | BUILD | Native | MasQ-backed package management. Every package has temporal provenance. No dependency hell — MasQ tracks what worked when. P0. |
| **apt / dnf** | COMPAT | POSIX layer | For compat layer system packages. P1. |
| **pip** | COMPAT | POSIX layer | Python packages. P0. |
| **npm** | COMPAT | POSIX layer | JS packages. P2. |
| **conda** | COMPAT | POSIX layer | AI/ML environments. P1. |

### Decisions needed:
- zeos-pkg format: what does a native package look like? `.zdx` modules with signal contracts + MasQ history?
- Can zeos-pkg wrap pip/conda for Python packages with added provenance?

---

## 11. Observability & Monitoring

| Program | Status | Path | Notes |
|---------|--------|------|-------|
| **Prometheus / Grafana** | COMPAT | POSIX layer | For compat workloads. P2. |
| **Zixel telemetry** | BUILD | Native | Computation IS telemetry. No instrumentation needed. The signal graph emits its own observability data. P0. |
| **MasQ timeline** | BUILD | Native | Temporal navigation of system state. Replaces log aggregation. P1. |

### Decisions needed:
- Zixel telemetry export format: for bridging to Prometheus/Grafana in compat
- MasQ timeline UI: CLI, TUI, web dashboard?

---

## New File Formats Required

These don't exist yet. Zeos needs them.

### P0 — Must have

| Format | Extension | Purpose | Notes |
|--------|-----------|---------|-------|
| **Z+ source** | `.zp` | Signal-flow programs | Defined in Z+ spec. Human-readable. |
| **Z+ compiled** | `.zpc` | Compiled signal chain bytecode | For Zeos kernel execution. Needs definition. |
| **Signal contract** | `.sigc` | Device capability declarations | Lives in `/specs/`. Declares what a class of device can do. JSON/TOML-like? |
| **MasQ record** | `.masq` | Temporal state snapshot | Image-based provenance record. Binary with navigable index. |
| **MasQ index** | `.masqi` | Temporal navigation index | Fast lookup across MasQ history. |
| **Zeos module** | `.zdx` | Self-contained deployable module | Module + dependencies + signal contract + MasQ history. The native "package." |
| **VAULT block** | `.vlt` | Sovereign storage unit | 3-tier storage format. CFA-addressed. |
| **CFA address map** | `.cfa` | Fractal address space layout | Defines the memory topology for a running system. |

### P1 — Important

| Format | Extension | Purpose | Notes |
|--------|-----------|---------|-------|
| **Zixel profile** | `.zxl` | Hardware telemetry snapshot | Timing deltas, thermal map, aging signature for a device. |
| **Device MasQ** | `.dmasq` | Hardware-specific learned profile | What a specific piece of silicon actually does (vs spec sheet). |
| **Signal graph snapshot** | `.sgs` | Frozen signal graph state | For debugging, replay, sharing system configurations. |
| **Z+ visual graph** | `.zpv` | Visual editor format | Bidirectional with `.zp` source. Node positions + layout. |
| **MDE routing table** | `.mrt` | Workload-to-silicon mapping | Current and historical routing decisions. |
| **TriDelta token stream** | `.tdt` | Temporal validation data | 8-bit stability tokens across pipeline stages. |

### P2 — Can derive later

| Format | Extension | Purpose | Notes |
|--------|-----------|---------|-------|
| **Zeos image** | `.zimg` | Bootable OS image | Single image for any hardware. |
| **Z+ WASM output** | `.zpw` | Browser-compiled Z+ | For classroom simulators. |
| **Zeos core dump** | `.zcd` | Signal graph crash state | Full temporal context, not just memory. |
| **NTS correction log** | `.nts` | Timing correction history | Self-healing event record. |

---

## Conversion Strategy

### Phase 1: Boot & Build (P0)
Get the POSIX compat layer running. Python, git, bash, SSH, curl work. Devs can log in and do familiar things. Z+ compiler produces first `.zp` → kernel native output. VAULT stores first data. zeos-pkg installs first `.zdx`.

### Phase 2: AI Workflow (P0-P1)
PyTorch MDE backend. `torch.device("mde")` routes to Goya/FPGA/Coral through the signal graph. Jupyter gets a Z+ kernel. Hugging Face models load and run. This is where AI devs see the speed and get curious.

### Phase 3: Native Tools (P1)
zeos-debug, zeos-vc, VS Code extension, Z+ LSP. The native toolchain becomes productive. Devs can build, debug, and version-control Z+ projects end to end.

### Phase 4: Deep Integration (P1-P2)
ONNX-to-signal-chain compiler. Native Arrow/Parquet readers. CFA-based isolation replaces containers for native workloads. Zixel telemetry exports to Grafana. MasQ replaces MLflow.

### Phase 5: Full Native (P2-P3)
Everything that wants to go native can. Compat layer is still there but fewer devs need it. Zeos-native apps outperform compat equivalents because they speak signal graph natively.

---

## Summary: Build vs Port vs Compat

| Category | Build from scratch | Port / Native backend | Compat layer only |
|----------|-------------------|----------------------|-------------------|
| **Languages** | Z+ compiler | LLVM (Zeos backend), Python (native extensions) | GCC, Rust, Node, Go, Julia |
| **Build** | zeos-build | — | CMake, Make, Ninja, Cargo, pip |
| **VCS** | zeos-vc | Git (bridge to MasQ) | — |
| **AI/ML** | — | PyTorch (MDE backend), JAX (XLA plugin), ONNX (signal chain executor) | TensorFlow, HF, Jupyter (+ Z+ kernel), MLflow |
| **Storage** | VAULT | Redis (signal graph cache), Arrow (native readers) | SQLite, Postgres, DuckDB, HDF5 |
| **Containers** | Zeos isolation (CFA) | — | Docker, QEMU |
| **Network** | Signal contract RPC | — | curl, SSH, nginx, gRPC, ZeroMQ |
| **Dev tools** | zeos shell, zeos-debug, Z+ LSP | VS Code (extension) | bash, vim, GDB, tmux, ripgrep |
| **Hardware** | Goya signal contract | Yosys (zeos-build integration) | Vivado, Quartus, OpenOCD |
| **Packages** | zeos-pkg | — | apt, pip, npm, conda |
| **Observability** | Zixel telemetry, MasQ timeline | — | Prometheus, Grafana |

**Total: ~12 things to build, ~10 to port/extend, ~30+ run in compat unchanged.**

---

**Codex Labs LLC** — Minneapolis, MN — 2026
