# Zeos Doctrine

*The canonical state of the Zeos project. Authored 2026-05-22 after PR #2 merged. Updated when major work lands. Source of truth for "where are we" — when this disagrees with `ROADMAP.md` or `STATE.md`, this wins.*

`STATE.md` is session-scale ("what I did, what's next this week"). This is project-scale ("where we are, in the big shape").

---

## I. What Is (Built and Running)

### Z+ — running language
Full toolchain shipped to `main` as of PR #2 (final commit `39eea16`).

- Lexer / parser / typed AST / 3-layer type checker / tree-walking runtime
- All 68 `.zp` programs in `programs/` parse cleanly
- 168 tests green across 7 test files
- **Three corpus ratchets locked at zero:** merge arity (chord rule), Flow connectivity (output(a) ↔ input(b)), named-arg types
- CLI: `zplus lex|parse|check|run|tune` (umbrella binary + 5 sub-binaries)
- Demo programs in `programs/demos/` execute end-to-end via `zplus run`
- Observer / aggregator / tuner loop wired for ML-driven config tuning later

**Paths:**

| Path | Purpose |
|---|---|
| `tools/zplus/src/` | language stack source |
| `tools/zplus/README.md` | user-facing quickstart |
| `tools/zplus/TOKEN_TAXONOMY.md` | lexer spec |
| `tools/zplus/SEMANTIC_CONTRACTS.md` | 28 synthetic op contracts |
| `tools/zplus/tests/` | test fixtures + ratchets |
| `programs/demos/` | runnable Z+ demos |
| `programs/*.zp` (68 files) | the corpus (de-facto language spec) |

### Specs (foundation written, code pending)

| Path | Status |
|---|---|
| `specs/OPT_IN_TEST_REPORTING.md` | opt-in telemetry contract — spec done, code not started |
| `specs/MEASUREMENT_TARGETS.md` | 10 categories of what to measure — spec done |
| `docs/PRIMITIVES.md` | investigative primitives / Truth-Pry chain |
| `BRAD.md` | cognitive profile working description |

### Bookkeeping

| Path | Purpose |
|---|---|
| `STATE.md` | session-level running notes (read first each session) |
| `ADDED_FEATURES.md` | landing log (one row per merged change) |
| `DOCTRINE.md` | this doc — canonical project state |
| `ROADMAP.md` | feature-tracker checklist (substantial, partially stale on Z+ section) |

### Kernel
Substantive; details in `ROADMAP.md` and `docs/AUDIT_2026_03_27.md`. Includes:

- Boot, SMP audit, AP partition framework
- USB Bluetooth HCI foundation
- quick_look PNG decode via lodepng
- Multi-GPU virtio-gpu paths
- HDA audio (real codec walk)

---

## II. What's Queued (Incremental Z+ Work)

Session-sized, not architectural. Each is one focused session:

- Real semantics for transformers: `rate`, `baseline`, `deviation`, `decay`, `count`, `on_silence`, `weighted`, `sort`, `normalize` (same pattern as the shipped `delta` semantic)
- Real merge timing windows: `Within(30s)` buffering with sliding ticks
- Real-time pacing: runtime sleeps based on `RuntimeConfig::ticks_per_real_second`
- Field projection on records: so `gate(level: error)` checks the field
- `gate(message ~ "pat")` with real fuzzy match against upstream
- File I/O: `fs("...")` actually reads a file
- Network I/O: `net.listen`, `net.connect`
- Hardware-pin annotations: `@ goya(0)` runtime device routing (stub form)

Each lands with new tests; the three corpus ratchets catch regressions.

---

## III. What's Architectural (Big, Open Decisions)

These need a thinking session before code:

- **Z+ IR / `.zpc` bytecode.** Programs ship as compiled artifacts, not source.
- **Native codegen.** **Open question:** LLVM via `inkwell` vs custom backend. Chord rule pulls toward custom (LLVM has no Merge primitive).
- **Target backends per `docs/FOUNDATIONAL_PROGRAMS.md`:** Zeos kernel native, ARM bare-metal, FPGA bitstream (via Yosys), WASM.
- **Type system v2:** generics, subtyping policy, inference. v1 is "explicit annotations at chain boundaries, structural for most, nominal for hardware classes."

---

## IV. What's Untouched (Subsystems Not Started)

Each is roughly the same scale as the Z+ stack just shipped. All P0 BUILD per `docs/FOUNDATIONAL_PROGRAMS.md`.

| Subsystem | Path | What it is |
|---|---|---|
| **VAULT** | n/a | 3-tier sovereign storage; replaces filesystem for native Zeos |
| **MasQ + MasQi** | n/a | Temporal record format + navigable index ("system has memory") |
| **CFA** | n/a | Codex Fractal Addressing (Layer 2) |
| **zeos-build** | `tools/zeos-build/` empty | Hardware-aware build system |
| **zeos-pkg** | `tools/zeos-pkg/` empty | MasQ-backed package manager |
| **zeos-vc** | `tools/zeos-vc/` empty | Delta-native version control |
| **zeos-test** | n/a | Per-layer self-test + opt-in reporter (spec exists) |
| **native runtime** | `runtime/` empty | Native signal-chain executor (replaces tree-walker eventually) |
| **MDE backend** | external | Heterogeneous compute orchestration (Layer 5) |
| **Goya signal contract** | n/a | Habana SynapseAI native driver |
| **PyTorch MDE backend** | n/a | `torch.device("mde")` routing |
| **Python native extensions** | n/a | Zixel / MasQ bindings |
| **Zixel L0 telemetry** | partial | Real silicon awareness; currently conceptual |
| **Fractal Addressing implementation** | partial | Layer 2 conceptual; implementation pending |
| **Temporal Wayfinding** | n/a | Layer 4 (above storage) |

---

## V. External Integrations

### Nexus (paused on source-surfacing)

Brad's personal AI assistant. HDA is its gauntlet. Integration paused — waiting on source-surfacing (widen MCP scope OR get files onto local disk).

Three layers when source available:
1. **POSIX compat** (`compat/`) — runs today, no native benefit
2. **Native bindings** — MDE / Zixel / MasQ hooks for the heavy parts
3. **Signal-graph port** — orchestration loop as Z+ chains (months)

### Sister repos (per `CODE-PHONE-BRIEF.md`)

| Repo | Why it matters |
|---|---|
| `b3` | Bayesian Balance — decision engine MDE consults |
| `mde` | Model Decomposition Engine — Layer 5; chord-resolution primitive lives in `src/mde/fusion/engine.py` |
| `cde` | Codex Decomposition Engine — universal stream decomp |
| `zignal` | Signal-graph notation + knowledge graph |

---

## VI. Metrics (Ratchets)

Test-suite lock-ins that catch regression:

| Ratchet | Value | Where |
|---|---|---|
| Corpus parse coverage | 68/68 | `tools/zplus/tests/corpus.rs::parser_minimum_corpus_coverage` |
| Corpus lex coverage (Error tokens) | 0 across all files | `tools/zplus/tests/corpus.rs::error_counts_match_allowlist` |
| Chord-rule violations (merge arity) | 0 | `tools/zplus/tests/check_log_monitor.rs::checker_corpus_violations_are_capped` |
| Flow connectivity violations | 0 | `tools/zplus/tests/check_log_monitor.rs::flow_connectivity_corpus_ratchet` |
| Call-arg violations (named-arg types) | 0 | `tools/zplus/tests/check_log_monitor.rs::call_args_corpus_ratchet` |
| Total tests green | 168 | `cargo test` from `tools/zplus/` |

Raise floors as new work earns them. Don't lower without explicit reason.

---

## VII. Major Commits (PR #2)

For navigability when this doc gets old:

- `1d149fb` — TOKEN_TAXONOMY.md (lexer spec)
- `9b506b6` — v1 lexer + Cargo skeleton
- `b630c5d` — AST type with chord rule structural
- `2f8ac9b` — Parser skeleton with chord-rule coalescing
- `ca4f57a` — Parser corpus coverage 2 → 68/68
- `85e49d6` — Type-shape enum + SEMANTIC_CONTRACTS
- `502bd37` — Type checker first cut (merge arity)
- `13f3b76` — Chord-rule violations 17 → 0
- `0cbb1f9` — TypeEnv + infer_chain
- `5b66da6` — Flow connectivity check
- `cd0b5f0` — Named-arg type checking
- `ffafa62` — Runtime v1 (Z+ programs RUN)
- `4a41139` — Runtime v1.5 (two time knobs)
- `2b79948` — Runtime v2 (forks + delta)
- `4e226a5` — Runtime v2.1 (real gate predicates)
- `85f8a58` — Cleanup pass
- `c3ecd32` — zplus umbrella + user README
- `8614cce` — Runtime hooks (Observer + RuntimeEvent)
- `cc7a0ad` — Aggregator + Report
- `ec7427a` — Heuristic tuner
- `0b566fc` — docs/PRIMITIVES.md
- `39eea16` — BRAD.md

---

## VIII. Update Protocol

This doc gets edited when:

- A major subsystem ships → move it from §III/§IV to §I
- An architectural decision resolves → move it from §III to §I
- A new subsystem enters scope → add to §IV
- A ratchet floor changes → update §VI
- An external integration completes or starts → update §V
- A new session lands a meaningful commit → add to §VII

Don't let this doc drift. When `STATE.md` says something different about a major component, **this wins**; reconcile by updating STATE.md to match. When `ROADMAP.md` shows old checklist items as not-done that this shows as done, refresh ROADMAP.md.

`STATE.md` is session-scale (read first each session). This is project-scale (read for "where are we"). Use both.

---

*Authored 2026-05-22 after PR #2 (52 commits, +9904/−42, 44 files) merged.*
