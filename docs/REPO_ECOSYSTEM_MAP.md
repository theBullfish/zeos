# REPO ECOSYSTEM MAP

This document is the internal map of the broader Codex / Zeos / TRISA repo ecosystem.

It exists to answer five questions quickly:

1. What is this repo really for?
2. What larger system does it belong to?
3. Is it active, conceptual, experimental, or replacement-bound?
4. What other repos does it depend on or relate to?
5. What should future contributors or code assistants **not** misunderstand about it?

This is not marketing copy.
This is a truth map.

---

## Naming Rule

When reading this map, use the following hierarchy:

- **TRISA** = the broader architectural / compute paradigm
- **Zeos** = the operating-system project built in that paradigm
- **Z+** = the native language of Zeos for TRISA-style signal-chain execution
- **Blue** = the agent / assistant / orchestration lane
- **Codex** = the broader ecosystem / memory / decomposition / knowledge layer

---

## Core Systems Map

| Repo | Role | Parent System | Notes |
|---|---|---|---|
| `zeos` | Main operating-system architecture repo | Zeos / TRISA | Primary OS repo. Contains current executable core: signal runtime, minimal Z+ interpreter, and early chain primitive. |
| `zeos-playground` | Browser-based Z+ interpreter / demo surface | Zeos / Z+ | Public-facing playground for Z+ with WASM + JS fallback. Good demo surface for the language/runtime idea. |
| `zeos-types` | Type/support lane for Zeos | Zeos | Likely intended to carry shared type definitions or structural typing support. Keep aligned with Z+ and runtime terminology. |
| `zeos-spine` | Structural/core support lane | Zeos | Spine repo name suggests shared structural substrate. Should be kept conceptually aligned with core Zeos primitives. |
| `zeos-website` | Public website lane | Zeos | Public-facing site layer, distinct from core runtime repo. |
| `zeos-os.com` | Domain/site repo | Zeos | Site/domain repo for Zeos-facing web surface. |
| `trisa` | Broader architectural lane | TRISA | Broader paradigm repo. Should remain conceptually above Zeos, not confused with the OS repo itself. |
| `trisa-litefury` | TRISA hardware/application lane | TRISA / Hardware | Likely specific hardware-targeting lane for LiteFury-related work. |
| `trisa-service` | Service/API lane for TRISA ecosystem | TRISA | Service-facing repo for TRISA-facing system support. |
| `trisa_hardware` | Hardware architecture lane | TRISA / Hardware | Hardware-specific system repo. |
| `TRISA-BOX` | Box/appliance/system packaging lane | TRISA | Packaging or appliance form of the system. |

---

## Execution / Decomposition / Intelligence Map

| Repo | Role | Parent System | Notes |
|---|---|---|---|
| `mde` | Model Decomp Engine | Codex / Execution | Focused engine for decomposing AI models. Narrower than CDE. |
| `cde` | Codex Decomp Engine | Codex / Execution | Broader successor/generalization of MDE. Not limited to AI models; meant to operate on more diverse structured inputs. |
| `brain_mapper` | Brain/cognition mapping lane | Codex / Cognition | Cognitive mapping / analysis lane. |
| `nlc-mapper` | Nonlinear cognition mapping lane | Codex / Cognition | Likely tied to mapping nonlinear associative cognition structures. |
| `bear_poking` | Specific app / experiment lane | Blue / Cognition | Active applied project. Treat as product/app lane, not just theory. |
| `blue-observer` | Blue support / observer lane | Blue | Observation or monitoring organ in the Blue system. |
| `blue-watch` | Blue monitoring/control lane | Blue | Related to Blue monitoring/watch functions. |
| `knob-api` | Blue control/interface API | Blue | Control surface API for Blue. Not random utility plumbing. |

---

## Memory / Continuity / Knowledge Map

| Repo | Role | Parent System | Notes |
|---|---|---|---|
| `chaos-codex` | Long-term memory bank / timeline website | Codex / Memory | Special repo. This is the long-term memory bank and timeline website. Treat as continuity infrastructure, not side project fluff. |
| `memory-wiki` | Structured memory/reference store | Codex / Memory | Likely companion knowledge/reference system. |
| `claude-memory-mcp` | MCP memory integration lane | Codex / Memory | Connector/integration repo for memory tooling. |
| `claude-memory-hub` | Broader memory coordination lane | Codex / Memory | Hub layer for memory system coordination. |
| `zignal` | Signal/notation/meaning lane | Codex / Language | Public repo with language/signal implications. |
| `zignal-notation` | Notation-specific lane | Codex / Language | Likely narrower notation support around Zignal ideas. |

---

## Auth / Sovereignty / Platform Map

| Repo | Role | Parent System | Notes |
|---|---|---|---|
| `zauth` | Zeos-native auth layer | Zeos / Platform | Replaces Google OAuth. Treat as a sovereignty/auth subsystem of Zeos, not generic auth boilerplate. |
| `cfa-lib` | CFA-related library | Zeos / Memory / Security | Shared library lane for CFA-related primitives. |

---

## Hardware / Bringup / Recovery Map

| Repo | Role | Parent System | Notes |
|---|---|---|---|
| `goya-bringup` | Habana Goya bring-up lane | Hardware / Zeos / TRISA | Public bring-up and deployment lane. |
| `goya-on-windows` | Windows-specific Goya lane | Hardware | Public hardware-specific environment lane. |
| `goya-recovery` | Recovery / remediation lane | Hardware | Private recovery-focused repo. |
| `pcb-forge` | PCB / EDA / hardware design lane | Hardware / Codex | AI-native EDA / board-design lane. |
| `-fpga-admin` | FPGA admin / board control lane | Hardware / FPGA | Large private repo; operational/admin lane for FPGA work. |

---

## GritBox / Media / Operations Map

| Repo | Role | Parent System | Notes |
|---|---|---|---|
| `gritboxops` | Main GritBoxOPS repo | GritBoxOPS | Core streaming / ops / product lane. |
| `GritBoxOpsApp` | App implementation lane | GritBoxOPS | Product/application lane. |
| `GritBoxOps2nd` | Secondary app or iteration lane | GritBoxOPS | Likely duplicate/iteration lane. Clarify status when practical. |
| `GritBoxOPS-DupeSweep` | Public support/utility lane | GritBoxOPS | Utility/support repo. |
| `GritBoxOPS-ForgeBench` | Bench/test lane | GritBoxOPS | Likely sandbox / forge / bench environment. |
| `watchdogpodcast.com` | Public web/media repo | Media / Watchdog | Public site/domain repo. |
| `ice-gritboxops` | Public issue/event-specific lane | GritBoxOPS / Media | Public repo tied to ICE / GritBoxOPS context. |

---

## Legacy / Utility / Staging Repos

| Repo | Role | Parent System | Notes |
|---|---|---|---|
| `piaddons` | Older Pi support lane | Legacy / Utility | Legacy or support repo. |
| `PiInstallStuff` | Pi install/support lane | Legacy / Utility | Older setup repo. |
| `Pi-Install-Script` | Pi install script lane | Legacy / Utility | Older setup repo. |
| `Pi-Install-last-two-thirds` | Pi install continuation lane | Legacy / Utility | Likely transitional or partial repo. |
| `PrometeusClientPi` | Pi client lane | Legacy / Utility | Older device/client work. |
| `WastedSpace` | Placeholder / staging repo | Staging | Likely low-priority / placeholder. |
| `WatchdogVolatility` | Early/side media or analysis lane | Staging / Media | Needs explicit status if it becomes active again. |
| `zindex-studio` | Studio/site/product lane | Media / Codex | Likely tied to studio/site identity. |
| `seven-degrees-of-reagan` | Special concept repo | Concept | Public concept repo. |

---

## Relationship Notes

### `MDE` vs `CDE`

This distinction matters.

- **MDE** = **Model Decomp Engine**
  - focused on decomposing AI models
  - narrower and model-specific

- **CDE** = **Codex Decomp Engine**
  - broader generalization of the decomposition idea
  - intended to accept more diverse inputs
  - should be treated as bigger than MDE, not just renamed MDE

### `zauth`

`zauth` is not just generic auth.
It replaces Google OAuth and belongs to the Zeos sovereignty/platform story.
Treat it as core platform infrastructure.

### `chaos-codex`

`chaos-codex` is not a novelty repo.
It is the long-term memory bank and timeline website.
Treat it as continuity infrastructure for the ecosystem.

### `knob-api`

`knob-api` belongs with **Blue**.
It is part of the Blue control/interface layer.
Do not treat it as random API miscellany.

---

## Repo Status Language

Use the following status language when expanding this map later:

- **core** = central to system identity or architecture
- **active** = currently live work
- **experimental** = real work, unstable direction
- **support** = utility or adjacent helper repo
- **legacy** = older repo, retained for continuity/reference
- **staging** = placeholder, incubator, or undefined future role

---

## How To Use This File

Future contributors, assistants, and code tools should use this document to avoid five common mistakes:

1. confusing Zeos and TRISA
2. flattening Blue into generic app plumbing
3. mistaking `chaos-codex` for a side project
4. treating `MDE` and `CDE` as the same thing
5. reading repo names literally without ecosystem context

If a repo’s purpose changes, update this file.
If a repo is superseded, mark it here.
If a repo is folded into another lane, say so explicitly.

This file is the ecosystem memory map.
