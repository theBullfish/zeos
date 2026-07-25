# The Stickler Standard — Z+ Program Review Criteria

**Enterprise-grade check-down for every `.zp` program in the corpus.**
Codex Labs LLC — the Bible Protocol.

This is the ruler. It is grounded in Z+'s *own* laws (`docs/SIGNAL_LOGIC.md`,
`docs/CHAIN_CONTRACT.md`, `docs/ZPLUS_SPEC_V2.md`) plus general software
engineering discipline — never generic-checklist fluff. A program is measured
against the paradigm it was written in, not against C or Python habits.

## How a review runs

1. **Judge a batch of 5–10 programs** — one report per program under
   `reports/NN_name.stickler.md`, scored against every axis below, ending in a
   prioritized fix plan. No fixing during the judging pass.
2. **Then stop judging and fix that batch** — accomplish every fix goal with
   **verified internal verification** (the fix must re-pass the real toolchain:
   `zplus-lex → zplus-parse → zplus-check → zplus-run`, and behavior must not
   regress) before moving to the next batch.
3. Keep `CANDIDATE_PROGRAMS.md` **append-only** as new-program ideas surface.
   Never build a *new* program without discussing it first.

## The axes (each scored 0–5; weighted)

| # | Axis | Weight | What earns a 5 |
|---|------|--------|----------------|
| 1 | **Correctness & Verification** | ×3 | lex→parse→check→run clean on the real toolchain; deterministic; no dead or duplicate bindings; no unreachable chains |
| 2 | **Paradigm Fidelity** | ×3 | signal-native throughout — uses the eight forms (Chord, Knee, Silence, Delta, Confluence, Grade, Resonance, Reflex/Deliberation) where apt; **zero** ported `if/else`, `while/for`, `try/catch`, or `var = value` state-thinking |
| 3 | **Language Surface Discipline** | ×2 | every construct is either defined in the spec **or** explicitly flagged as speculative in FINDINGS and registered in the Language Gap Register — no silent assumption that undefined calls exist |
| 4 | **Cleanliness & Structure** | ×2 | clear section banners, honest names, no duplication/dead code, idiom consistent with the corpus |
| 5 | **Efficiency** | ×1 | signal-graph economy — debounce/rate windows sized right, no redundant chains, `reflex` where wire-speed is required, knees instead of oscillation |
| 6 | **Robustness (Silence/Degrade/Failure)** | ×2 | `on_silence`/`on_degrade`/`on_block` where the real world goes quiet or rejects; failure paths for stateful ops; graceful, not optimistic |
| 7 | **Provenance & Contract** | ×1 | telemetry taps (`~>`), vault provenance, B3/grade where relevant; state-mutating nodes bump the temporal record (CHAIN_CONTRACT ethos) |
| 8 | **Documentation & Honesty** | ×1 | header (what it is + honest conventional-LOC comparison); FINDINGS present and *honest*; comments idiomatic, not noise |
| 9 | **Configurability & Portability** | ×1 | no hardcoded machine paths/hosts; a config surface for anything environment-specific |
| 10 | **Consistency** | ×1 | matches corpus conventions (banner style, findings block, naming) |

**Score** = Σ(axis × weight) / Σ(weight) × 20 → 0–100.
**Grade**: A ≥ 90 · B ≥ 80 · C ≥ 70 · D ≥ 60 · F < 60.
**Verdict**: `SHIP` (A/B, no P0/P1) · `FIX` (has P1s, no P0) · `REWORK` (any P0).

## Paradigm red flags (auto-deductions on Axis 2)

- `if`, `else`, `while`, `for`, `switch`, `try/catch` used as control flow → **−2**
- string concatenation for messages (`"x: " + y`) where a signal-native form is
  intended → **−1** (a known corpus leak; see FINDINGS across programs)
- state stored/mutated procedurally instead of flowing (`t-1` / delta) → **−1**
- boolean cliff where a `knee` belongs (control loops, thresholds) → **−1**

## Fix-plan format (every report ends with this)

Each issue is one row:

```
[P0|P1|P2] axis · location · problem → fix action → VERIFY: how we confirm
```

- **P0** — broken (won't lex/parse/check/run) or a correctness defect. Must fix.
- **P1** — paradigm violation, dead/duplicate code, missing failure path. Should fix.
- **P2** — polish: config surface, doc, idiom, naming. Nice to fix.

Verification is never "looks right." It is: re-run the toolchain stage that was
failing; for behavioral fixes, diff the run output before/after; for paradigm
fixes, confirm the red-flag construct is gone and the program still runs clean.

## Language Gap Register (shared, cross-program)

Many programs lean on constructs the spec has not yet defined (`fs()`, `exec()`,
`net.listen()`, `respond()`, `parse()`, `vault.*`, `gate(not:)`,
`gate(sustained:)`, `on_block`, `group(by:)`, `count(distinct:)`, `sort(by:)`,
`source.geo`, …). These are **not per-program bugs** — a program must not be
"fixed" by gutting a construct its whole design needs. They are language-surface
decisions. The register lives in `STICKLER_TRACKER.md`; each report tags which
gaps it depends on so the language roadmap sees the aggregate demand.
