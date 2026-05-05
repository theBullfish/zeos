# Zeos opt-in test reporting

**Status:** spec sketch. Not implemented. Captured 2026-05-05 from a Brad directive.

Zeos ships with a **deep self-test suite** that runs across every layer of the stack and, **with explicit user opt-in**, sends test reports back to **Codex Labs** (us). This is how we learn what's actually breaking on real hardware in real environments. Without it we're flying blind.

This is opt-in by default. Off until the user says yes. Off again if the user says no.

---

## What runs

The test suite is **deep** — it exercises every shipped subsystem the OS uses to operate, not just unit tests of individual functions. Categories (one per Zeos layer per `docs/TRISAVERSE_STACK.md`):

| Layer | What gets tested |
|---|---|
| L0 Silicon Awareness | Zixel timing-delta capture across every computational path; thermal / aging signatures match measured-vs-predicted curves |
| L1 Signal Chain | Every registered chain in `chain_registry_init` resolves; `b3_alpha`/`b3_beta` belief tracking advances correctly under fault injection |
| L2 Fractal Addressing | CFA derivation round-trips; buffer-overflow probes land in unmapped territory; cache-line timing leaks are bounded |
| L3 Temporal Storage | VAULT block round-trips; rollback to N versions back; provenance chain integrity |
| L4 Temporal Wayfinding | MasQ index lookups; replay determinism; branch-and-merge of timelines |
| L5 Compute Orchestration | MDE routes match learned profiles; failure-horizon prediction accuracy on synthetic workloads |
| L6 Z+ Language | Full corpus lex/parse/typecheck (already covered by the test crate) plus runtime exercises |
| Hardware drivers | Per-driver chain pipelines from `docs/CHAIN_CONTRACT.md` — audio, net (TX+RX), block, USB, display |
| Compat layer | POSIX shim for syscalls; pip/cargo/npm package operations |

Each test produces a structured result: pass / fail / degraded (continued under reduced capability) plus optional context (timing histogram, thermal trace, provenance excerpt).

---

## The opt-in

**Default:** off. Tests still RUN (silently — they're how the OS knows it's healthy), but reports are local-only.

**Opt-in flow:**

1. First boot, or a periodic prompt from `zeos-shell` / setup wizard, presents:

   > Zeos can send anonymized test reports back to Codex Labs to help us improve the OS for everyone. This includes:
   > - what subsystems passed / failed
   > - hardware fingerprint (no serial numbers)
   > - timing distributions
   > - thermal envelope
   >
   > NOT included:
   > - any user data
   > - file contents
   > - network destinations
   > - identity beyond device fingerprint
   >
   > [ ] Yes, share reports     [ ] No, keep local
   >
   > You can change this anytime via `zeos privacy`.

2. The choice is persisted in the **sovereign tier** of VAULT (encrypted, never leaves the device's signing-key control). The opt-in record itself is the source of truth — even Codex Labs can't fake or override it.

3. Reports send only when the opt-in record reads `share`. Every send is logged in MasQ so the user can see exactly what was sent and when.

**Opt-out flow:** `zeos privacy off` writes the new sovereign record. The **next** test cycle stops sending. Already-sent reports CANNOT be recalled (they were sent), but no new ones go out.

---

## What goes in a report

The full measurement set is in `specs/MEASUREMENT_TARGETS.md` — 10 categories from language drift to thermal envelope. Each report is a self-contained signal-graph snapshot (`.sgs` per `docs/FOUNDATIONAL_PROGRAMS.md`) with:

- Test ID (which suite, which version)
- Pass/fail/degraded results per layer
- Hardware fingerprint (Zixel-derived, NOT serial-based — different installs of the same model have different fingerprints)
- Timing histogram — Layer 0 telemetry summary
- Thermal envelope — min / median / max during the run
- MasQ excerpt — the last N system-state changes leading up to the test run

**Explicitly NOT in a report:**

- File contents from VAULT
- Process names beyond the test runner's own
- Network destinations the user has visited
- Any sovereign-tier data
- Anything readable to the user that the user wouldn't expect to share

The test runner has read-only access to the layers it tests; it CANNOT read user data even by mistake. CFA capability enforcement (Layer 2) makes this structural, not policy-based.

---

## How reports send

- Encrypted via Codex Labs' published public key (key rotation cadence TBD)
- Sent over the user's chosen network
- Failure is silent (a failed-to-send report is just dropped — we don't retry indefinitely)
- Rate-limited to one report per test cycle per device per day, max

If the user is offline, the report is queued in VAULT (sovereign tier) and sent on the next online window. Queue depth is capped — old reports drop oldest-first.

---

## Why we need this

Zeos is heterogeneous-hardware-first. We have NO way to characterize how a Habana Goya behaves on machine X vs machine Y vs in a thermal-stressed colo without observation. Static benchmarks lie. Opt-in real-world test reports are how:

- MDE learns what hardware combinations actually work well
- Failure-horizon prediction gets calibrated against real aging curves
- We catch driver regressions before they hit a user as a real outage
- We know which programs from the corpus run, and which break, on which hardware

Without this we ship blind. With it we ship on a feedback loop.

---

## Implementation order (when we get to it)

1. **Test-runner crate** — runs the full battery, produces `.sgs` reports. Lives in `tools/zeos-test/` (currently no such dir).
2. **Sovereign opt-in record** — VAULT format for the privacy preference. Co-designed with the VAULT crate.
3. **Report transport** — encryption + transmission over user's network. Co-designed with the network chain.
4. **`zeos privacy` CLI** — view/set/audit the opt-in record. Lives in `tools/zeos-shell/`.
5. **Setup-wizard prompt** — first-boot UI for the opt-in. Co-designed with `zeos-shell`.

Each is a separate work unit. Sequence: 1 first (we need the data flowing internally before we ship outwards), then 2, then 4, then 3, then 5.

---

## Open questions

- Key rotation cadence and revocation strategy for Codex Labs' public key
- Hardware-fingerprint stability across firmware updates (if firmware changes the Zixel signature, do we treat it as the same device or new?)
- Aggregate vs per-device retention on Codex Labs' side
- GDPR / regional regulatory compliance — opt-in language and data retention
- Whether to support per-layer opt-in (e.g., share thermal but not timing) vs global on/off

---

*Captured 2026-05-05. Brad's wording: "ship with a very deep testing suite that can send reports to **us** if they opt in."*
