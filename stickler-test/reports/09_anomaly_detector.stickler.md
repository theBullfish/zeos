# Stickler Report — `09_anomaly_detector.zp`

**Runs:** PASS · **Score: 90 / A− · Verdict: SHIP**

Batch exemplar #2 (alongside 02). Learned rolling `baseline` → `deviation` (delta, not fixed
threshold) → `σ` statistical gates → **resonance** severity scaling (2/5 warn, 3/5 high, 4/5
critical) → seasonal baselines → temporal `then` patterns → auto-remediation wired straight to
patterns. This is Datadog-plus-ML-pipeline in 65 lines. Almost no program-level fault.

## Axis findings

| Axis | Score | Note |
|---|---|---|
| 1 Correctness | 5 | runs clean |
| 2 Paradigm Fidelity | 5 | resonance, baseline/deviation, σ-gates, seasonal context, ordered `then` — masterclass |
| 3 Surface Discipline | 4 | `baseline`, `deviation`, `σ`, `sustained`, `pattern`, `then`, `auto_remediate` — flagged; heavy but coherent |
| 4 Cleanliness | 5 | the three resonance tiers read beautifully |
| 5 Efficiency | 5 | rolling windows, knee on σ-gates |
| 6 Robustness | 5 | severity scaling + auto-remediation + seasonal awareness |
| 7 Provenance | 5 | anomaly/incident/baseline-drift taps |
| 8 Docs | 5 | header + FINDINGS; σ-as-unit and `then`-operator proposals are excellent |
| 9 Config | 4 | one `/var/log/app/*.log` (l.13), conventional |
| 10 Consistency | 5 | reference-quality |

## Fix plan

```
[P2] idiom · l.92, l.94 · alert(channel: slack + pager + phone) uses + as set-union
     → confirm/define `+` as channel-union (vs arithmetic/string); if ambiguous, use
       alert(channels: [slack, pager, phone])
     → VERIFY: zplus-run PASS
[P2] robustness · l.99–100 · auto-remediate exec() has no result/failure path
     → fork on exec result: gate(ok) -> remediated, on_degrade -> escalate
     → VERIFY: zplus-run PASS
```

No P0/P1 — **ship it.** Cite as the exemplar for resonance + statistical gating.

**Language gaps depended on:** `baseline(window:, by:)`, `deviation(from:)`, `σ` unit,
`sustained(for:)`, `pattern()`, `then` ordered-confluence, `auto_remediate`. `then` and `σ` are
top-priority language proposals (register).
