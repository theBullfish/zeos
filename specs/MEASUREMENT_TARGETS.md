# Measurement targets for future Zeos iterations

**Status:** brainstorm. 2026-05-05. Companion to `specs/OPT_IN_TEST_REPORTING.md`.

What signals we want flowing back to us so future iterations get smarter. Each item: **what we measure → what decision it informs**. Items are not equally important — priority comes from how often the decision needs to be made and how wrong we are without the data.

The reports go through the opt-in pipeline (per `OPT_IN_TEST_REPORTING.md` §"How reports send"); none of these items is collected without explicit user consent.

---

## 1. Z+ language drift — what people actually write

How the corpus evolves once users write their own programs:

- **New token kinds** the lexer didn't anticipate. Each one is a parser-extension target. Track: lexeme, file/line, surrounding context.
- **New chain operators** beyond `-> ~> -x> <-`. Possible chord-rule extensions.
- **New `__op__` patterns** the parser synthesizes. Each grows `SEMANTIC_CONTRACTS.md`.
- **Compile time per program** (lex / parse / typecheck phases broken out). Performance regressions surface here.
- **Type-check failure categories** — what gets rejected, what the user does next (fix vs give up).
- **Workaround patterns** — code that's clearly fighting the language. These are language-design feedback.

→ **Decisions informed:** when to add new tokens, when to add chain ops, what the compiler should optimize, what error messages to improve, what language features are missing.

## 2. Hardware behavior — the MasQ feed

Continuous per-device measurements that build the device's MasQ profile (`docs/COMPONENT_AS_MODULE.md`):

- **Throughput at thermal sweet spot vs degraded.** Curve, not point.
- **Aging signature** — timing drift over weeks/months. Predicts end-of-life.
- **Power draw at idle / load / burst** — when measurable.
- **Warm-up time** — cold start to peak.
- **Recovery times** — warm shutdown vs hard kill.
- **Synergy effects** — paired with which peers does this device run hotter or cooler? Faster or slower? These are the combinations MDE doesn't predict from spec sheets.

→ **Decisions informed:** MDE routing weights, predictive failure migration, whether a heterogeneous pairing should be encouraged or avoided.

## 3. Signal-graph runtime — does the chord rule hold in practice?

Does what we shipped actually behave like signal flow at runtime?

- **Chord resolution latency** — target vs actual. Wide tail = bad chord.
- **Fastest-N straggler statistics** — how many stragglers per merge, how often.
- **Quorum miss rate** — how often did the merge time out? What's the recovery path?
- **Tap overhead** — `~>` is supposed to be zero-impact. Verify by comparing chains with and without taps.
- **Reflex priority hit rate** — did fast paths actually beat deliberate ones in the cases where it mattered?
- **Chord-vs-serialized fallback rate** — if the runtime ever serializes a merge under load, count it. This is the chord rule's only failure mode, and we want to know.

→ **Decisions informed:** runtime tuning, scheduler design, when to escalate priority.

## 4. Memory pressure — CFA / VAULT health

The Layer-2 / Layer-3 invariants need ongoing proof:

- **VAULT block utilization** per tier (sovereign / internal / reference).
- **VAULT version explosion** — writes per second, retention pressure.
- **CFA collision rate** — should be zero by design. Measure to verify.
- **Cache-line timing leak measurements** — the security claim needs ongoing evidence.
- **MasQ index lookup latency at scale** — does temporal navigation stay fast as history grows?

→ **Decisions informed:** retention policies, VAULT GC tuning, when to add a tier.

## 5. MDE routing — is the orchestrator actually optimizing?

Self-test for the L5 layer (`docs/TRISAVERSE_STACK.md`):

- **Routing decisions per workload class** — distribution by device, not just totals.
- **Routing changes over time** as device MasQ profiles refine.
- **Prediction-vs-actual on failure-horizon timing** — accuracy trend.
- **Stranded capacity** — devices underutilized after routing decisions.
- **Cross-device data movement overhead** — how much does routing cost vs save?

→ **Decisions informed:** MDE algorithm refinements, when to override routing manually, when to add a new device class.

## 6. Compat layer overhead

POSIX is a bridge, not a destination. Quantify the cost:

- **Syscall translation latency** per syscall class (file I/O / network / IPC / process / mem).
- **Compat memory overhead** vs native equivalent.
- **Workload categories where compat is "good enough"** vs where native pays off.

→ **Decisions informed:** which workloads to prioritize for native ports, when "compat forever" is fine.

## 7. Network / inter-node

Once Zeos clusters:

- **Signal contract RPC round-trip times** by contract class.
- **Cross-node chain resolution latency** — how much does the network add?
- **Cluster failover behavior** — node loss recovery time, data integrity.
- **Network-as-signal delta-stream effectiveness** — does treating network traffic as deltas actually save bandwidth?

→ **Decisions informed:** clustering protocols, failover thresholds, when to add a DPU.

## 8. Developer experience

The least-quantified, most-important category. Without UX data the language stalls:

- **Time-to-first-running-program** for new users.
- **Compile error clarity** — do users iterate to a fix, or give up? Sessions that abort after N errors are red flags.
- **Common mistake patterns** — these become language / compiler improvement targets.
- **Error message text that confuses users** — collected as a category, not as text (no user data leaves).
- **Tooling friction** — how often do users drop to compat for things they should be able to do native?

→ **Decisions informed:** error message rewrites, syntactic sugar additions, doc gaps, missing builtins.

## 9. Security invariants

The structural-security claims need ongoing proof:

- **Buffer-overflow probe results** — overflow attempts must land in unmapped territory.
- **Side-channel timing leak measurements** — fractal addressing should make these noise.
- **Capability enforcement violations** — should be zero. Track to verify.
- **Sovereign-tier read attempts** by non-owner — should be zero.

→ **Decisions informed:** when the security model needs hardening, where the threat model has gaps.

## 10. Power / thermal envelope

Edge deployments need this:

- **Per-workload thermal trace** — heat curve over the workload's lifetime.
- **Per-workload power draw.**
- **Cooling response curve** — how fast does the system shed heat after load drops?
- **Thermal-induced perf cliffs** — where does performance degrade sharply with temperature?

→ **Decisions informed:** edge deployment recommendations, MDE thermal-aware routing weights.

---

## What goes in the report (mapped to OPT_IN_TEST_REPORTING.md §"What goes in a report")

| Category | Dimensions | Granularity |
|---|---|---|
| 1 — Language drift | tokens / ops / synthetic-callees / compile-time / errors | per program, aggregated |
| 2 — Hardware MasQ | throughput / aging / power / warm-up / synergy | per device, time-windowed |
| 3 — Runtime chord rule | latency / stragglers / quorum / tap / reflex / fallback | per chain, distribution |
| 4 — Memory | VAULT / CFA / MasQ | per tier, time-windowed |
| 5 — MDE | routing distribution / prediction accuracy | per device class |
| 6 — Compat | syscall latency / overhead / workload class | per syscall family |
| 7 — Network | RPC / failover / cross-node | per contract |
| 8 — DX | session shape / error categories | per user (anonymized) |
| 9 — Security | invariant counters | per invariant |
| 10 — Power/thermal | curves | per workload |

Each report element has the **anonymization filter from OPT_IN_TEST_REPORTING.md** applied — no file paths leak, no user data, no network destinations. Hardware fingerprint is Zixel-derived, not serial-based.

---

## Sequencing

We don't build all 10 categories at once. Suggested order, easiest-first AND highest-leverage-first:

1. **#1 (language drift)** — already partially landed via `tools/zplus`'s test ratchet. Need a runtime hook to capture user-program statistics.
2. **#3 (chord rule runtime)** — once the runtime exists, instrument the `Merge` resolver. Without these we don't know if the chord rule holds.
3. **#2 (hardware MasQ)** — once Layer 0 silicon awareness is hooked into the test runner.
4. **#9 (security invariants)** — cheap to add once CFA is live.
5. **#4 (memory pressure)** — once VAULT writes through the `.vlt` format.
6. Everything else **as the corresponding subsystem ships**.

---

## Open questions

- Aggregation level. Per-event (high signal, high cost) vs per-window (lossy, cheap). Probably per-window for most, per-event for security violations.
- How to prevent measurement from itself becoming a bottleneck. Layer 0 is "computation IS telemetry" — but we still need to be careful about pull-cost when reading.
- Per-category opt-in vs global. Some users may want to share thermal data but not session-shape data. (Cross-link: `OPT_IN_TEST_REPORTING.md` open questions.)
- How long do we retain raw vs aggregated data on Codex Labs' side? Affects what's safe to send.

---

*Captured 2026-05-05. The list is not exhaustive — it's the ones obvious from this session's vantage point. Append as new measurement needs surface.*
