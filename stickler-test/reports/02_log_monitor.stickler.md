# Stickler Report — `02_log_monitor.zp`

**Runs:** PASS · **Score: 95 / A** · **Verdict: SHIP**

The strongest in the batch. Textbook Signal Logic: multi-source merge chord (`| -> all_lines`),
`gate(level:)` severity, `rate()`+`delta()`+`knee` spike detection, `sustained` gate, confluence
(`within(30s)` crash-loop correlation), `on_silence` (a quiet log is scarier than a loud one),
`vault.store(ttl:)` retention, telemetry taps. Uses six of the eight forms correctly.

## Axis findings

| Axis | Score | Note |
|---|---|---|
| 1 Correctness | 5 | runs clean |
| 2 Paradigm Fidelity | 5 | exemplary — chord, delta, knee, confluence, silence all idiomatic |
| 3 Surface Discipline | 4 | leans on `parse()`, `gate(~ pattern)`, `gate(sustained:)`, `vault.store(ttl:)` — all flagged in FINDINGS |
| 4 Cleanliness | 5 | clean, well-sectioned |
| 5 Efficiency | 5 | rate/delta windows sized right; knee prevents false spikes |
| 6 Robustness | 5 | `on_silence` + sustained + correlation — genuine negative-space robustness |
| 7 Provenance | 5 | vault retention (differential TTL), taps, heatmaps |
| 8 Docs | 5 | header + honest FINDINGS; documents the proposed `sustained` construct well |
| 9 Config | 3 | hardcoded `/var/log/*` paths (l.8–10) |
| 10 Consistency | 5 | reference-quality |

## Fix plan

```
[P2] config · l.8–10 · hardcoded log paths
     → source from config("logs.paths") when config_surface lands
     → VERIFY: zplus-run PASS
```

No P0/P1. **Ship it.** This is the paradigm reference program for the batch — cite it in the
Standard as an exemplar.

**Language gaps depended on:** `parse()`, `gate(message ~ …)`, `gate(sustained:)`,
`vault.store(ttl:)`. The `sustained` temporal gate is a strong, well-motivated language
proposal (Tracker + CANDIDATE: sustained_gate_demo.zp).
