# Stickler Report — `01_file_watcher.zp`

**Runs:** PASS (lex→parse→check→run clean) · **Score: 86 / B** · **Verdict: FIX**

Watches a filesystem tree, classifies/filters events, debounces rebuilds, detects change
bursts. Paradigm-strong: gate filtering, `rate()`+`gate(>20)` burst detection (delta thinking),
`debounce()`, telemetry taps (`~>`).

## Axis findings

| Axis | Score | Note |
|---|---|---|
| 1 Correctness & Verification | 5 | runs clean; no dead/duplicate bindings |
| 2 Paradigm Fidelity | 4 | signal-native throughout; **−1**: string concat `"deleted: " + path` (l.37) |
| 3 Language Surface Discipline | 4 | leans on `fs()`, `exec()`, `gate(not:)`, `debounce(restart?)` — all flagged honestly in its own FINDINGS |
| 4 Cleanliness & Structure | 5 | clean banners, honest names |
| 5 Efficiency | 5 | debounce, rate windows, burst backoff all right-sized |
| 6 Robustness | 3 | **no failure path** — `exec("make build")` (l.31) has no `on_degrade`/result handling; `fs()` source has no `on_silence` |
| 7 Provenance & Contract | 4 | telemetry taps present; no vault provenance (not required here) |
| 8 Documentation & Honesty | 5 | header + honest FINDINGS block |
| 9 Configurability | 3 | **hardcoded** `/home/z13/projects` (l.7) |
| 10 Consistency | 5 | matches corpus conventions |

## Fix plan

```
[P1] paradigm · l.37 · string concat in alert("deleted: " + path)
     → replace with signal-native form: alert(deleted, path:) or a templated emit
     → VERIFY: red-flag construct gone; zplus-run still PASS
[P1] robustness · l.31 · exec("make build") has no result/failure handling
     → add on_degrade / capture exec result → gate(failed) -> alert
     → VERIFY: zplus-run PASS; failure branch present
[P2] config · l.7 · hardcoded watch path
     → source from config("watch.root")  (see CANDIDATE: config_surface.zp)
     → VERIFY: zplus-run PASS with the config source
[P2] robustness · fs() source has no on_silence (a dead watcher looks identical to a quiet tree)
     → add fs(...) -> on_silence(> Nm) -> alert(warn:"watcher silent")
     → VERIFY: zplus-run PASS
```

**Language gaps depended on:** `fs()`, `exec()`, `gate(not:)`, `debounce(restart:)`.
Do not remove these — register them (already in Tracker). String-concat fix is program-level.
