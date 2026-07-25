# Stickler Tracker

Master state for the enterprise QA sweep of the Z+ corpus. Updated every batch.
Ruler: `STICKLER_STANDARD.md`. Backlog: `CANDIDATE_PROGRAMS.md`. Reports: `reports/`.

## Corpus run-status (baseline)

`zeos/tools/zplus/target/release/zplus-run` over `zeos/programs/**.zp`:

- **85 programs · 82 PASS · 3 FAIL (96%)**
- **P0 status:** now **83 PASS / 2 FAIL** after B-P0.
  - `web-zeos.zp`  — ✅ **FIXED + verified** (comma-less array-of-structs — a real program syntax slip).
  - `chat-zeos.zp` — 🚫 **BLOCKED on parser gap** (bare `.` current-signal ref; see Language Gap Register). Decision needed, not a program hack.
  - `notes-zeos.zp` — 🚫 **BLOCKED on parser gap** (same bare `.` idiom).

Regenerate the manifest any time:
```
cd zeos/tools/zplus && for f in $(find ../../programs -name '*.zp' | sort); do \
  ./target/release/zplus-run "$f" >/dev/null 2>&1 && echo "PASS $f" || echo "FAIL $f"; done
```

## Status legend

`⬜ pending` · `🔍 judged` (report written) · `🔧 fixing` · `✅ fixed+verified` · `🚫 P0 broken`

## Batch plan (5 per batch; P0-broken pulled forward)

| Batch | Programs | State |
|-------|----------|-------|
| **B1** | 01_file_watcher · 02_log_monitor · 03_http_server · 04_key_value_store · 05_firewall | 🔍 judging |
| **B-P0** | chat-zeos · notes-zeos · web-zeos (the 3 failing — priority fix) | ⬜ pending |
| B2 | 06_message_queue · 07_chat_system · 08_cicd_pipeline · 09_anomaly_detector · 10_game_server | ⬜ pending |
| B3 | 11_home_automation · 12_search_engine · 13_payment_processor · 14_video_streaming · 15_trading_system | ⬜ pending |
| B4 | 16_scada_industrial · 17_ecommerce · 18_patient_monitor · 19_autonomous_vehicle · 20_power_grid | ⬜ pending |
| B5 | 21_load_balancer · 22_precision_agriculture · 23_supply_chain · 24_lms_education · 25_election_system | ⬜ pending |
| B6 | 30_chain_native · 31_compute_via_mde · 32_gpu_compute · 40_runtime_chain · 50_strings_demo | ⬜ pending |
| B7 | 51_struct_demo · 52_module_demo · 53_stdlib_demo · 54_stdlib_gaps_demo · zpm | ⬜ pending |
| B8 | build-zeos · chirp · goya_fleet · goya_fleet_t3 · kv-zeos | ⬜ pending |
| B9 | quill · shield · web-zeos* · chat-zeos* · notes-zeos* (*=P0, see B-P0) | ⬜ pending |
| B10+ | subdir corpora: `competition/` · `demos/` · `derez/` · `lib/` · `multimodal/` · `zeros/` | ⬜ pending |

(Subdir programs make up the balance to 85; enumerated into batches as we reach them.)

## Batch 1 detail

| Program | Runs | Report | Score | Verdict | Fixed |
|---------|------|--------|-------|---------|-------|
| 01_file_watcher   | PASS | 🔍 | 86 / B | FIX (2×P1, 2×P2) | ✅ P1s (concat→signal form; exec on_degrade path). P2s pending config_surface.zp |
| 02_log_monitor    | PASS | 🔍 | 95 / A | SHIP (1×P2) | ✅ no program fix (P2 pending config_surface.zp) |
| 03_http_server    | PASS | 🔍 | 82 / B | FIX (2×P1) | ✅ dup net.listen removed. Round-trip P1 = language decision (net_source.zp) |
| 04_key_value_store| PASS | 🔍 | 89 / B | FIX (1×P1, 1×P2) | ✅ \|-overload→any(). P2 write-failure fork pending |
| 05_firewall       | PASS | 🔍 | 81 / B | FIX (2×P1) | ✅ accept-then-continue→fork; port any(); concat→signal form |

**Batch 1 FIX phase: COMPLETE & VERIFIED.** All P1s fixed. Verification: every batch-1 program
re-runs PASS via `zplus-run`; full-corpus regression check holds at **82/85 (no regression)**.
Remaining P2s are config-surface items blocked on `config_surface.zp` (a CANDIDATE — do not build
without discussing) and one write-failure fork (04). Language-decision items stay in the Register.

**Next: B-P0 — fix the 3 failing programs (`chat/notes/web-zeos`) → target 85/85.**

**Batch-1 program-level fix goals (the "then fix" phase — all must verify via zplus-run):**
- 01: string-concat leak → signal form; add exec failure path.
- 03: **delete duplicate `net.listen` binding** (l.8), keep the v2 middleware chain.
- 04: `gate(command:"set"|"del")` → `any("set","del")`.
- 05: **fork before terminal `accept`** (l.68–73); string-concat leak → signal form; `port: any(80,443)`.
- (02 has no program-level fix — SHIP; only config-surface P2 pending config_surface.zp.)
- Language-gap items are NOT program fixes — they stay in the Register for the roadmap.

## Language Gap Register (shared, cross-program)

Constructs many programs depend on that the spec has **not** formally defined. These are
**language-surface decisions, not per-program bugs.** Do not "fix" a program by removing a
construct its whole design needs. Tally demand here; route to the language roadmap.

| Construct | Seen in | Kind |
|-----------|---------|------|
| `fs("path") -> events/lines` | 01, 02 | signal source (filesystem) |
| `exec("cmd")` | 01 | shell interop / side-effect sink |
| `net.listen(port,proto)` / `net.interface()` / `net.connect()` | 03, 04, 05 | network signal source/sink |
| `respond(code, body)` | 03, 04 | round-trip / return-path emit |
| `parse(fields…)` | 02, 04 | structured extraction from text |
| `validate(body, schema:)` | 03 | schema validation node |
| `vault.store/read/delete/scan/snapshot/append_log/ttl/entries/memory_usage` | 02,03,04 | signal-emitting data layer |
| `gate(not: …)` | 01 | negation gate |
| `gate(sustained: 5m)` | 02 | temporal gate (threshold held over duration) |
| `on_block` | 03, 05 | gate actively rejects (vs `on_silence` = nothing arrived) |
| `group(by:)` / `count(distinct:, within:)` | 05 | grouping + cardinality-in-window |
| `sort(by:)` / `take(N%)` / `ratio()` | 04 | ordering / subset / derived metric |
| `source.geo` / `blacklist.add(ttl:)` | 05 | geo lookup / dynamic list |
| `debounce(restart: true/false)` | 01 | debounce restart semantics (ambiguous) |
| terminal nodes `drop` / `accept` | 05 | signals that *end*, not flow onward |
| program-level `priority: reflex` | 05 | whole-graph wire-speed priority |
| round-trip / bidirectional wire (`request <-> handler`) | 03 | response routes back to its source |
| **bare `.` = current-signal reference** (`encode(.)`, `f(.)`, `bt.range(i, ., .)`) | chat-zeos, notes-zeos | **parser gap** — grammar accepts `.field` but not a lone `.`. **BLOCKS these 2 programs.** Decision needed: teach the parser bare-`.`, or ratify `input` as the canonical current-signal name. Do NOT rewrite the idiom without a ruling. |

## Log

- `2026` — Standard authored; folder + manifest created; ClaudeStartHere primer placed at
  zeos root. Batch 1 judging begun. No stopping.
