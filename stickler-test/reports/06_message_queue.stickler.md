# Stickler Report — `06_message_queue.zp`

**Runs:** PASS · **Score: 88 / B** · **Verdict: FIX**

"A wire with memory." Topic-as-named-wire, consumer-groups-as-position-tracked-taps,
vault-append persistence with retention, knee-based backpressure on lag (delta thinking),
dead-letter via `on_fail(retries:)`, partition-as-deterministic-fork. Very clean paradigm.

## Axis findings

| Axis | Score | Note |
|---|---|---|
| 1 Correctness | 5 | runs clean |
| 2 Paradigm Fidelity | 5 | lag-as-delta, backpressure-knee, taps-as-consumer-groups — idiomatic |
| 3 Surface Discipline | 3 | leans on `topic()`, `consumer_group()`, `ack`/`advance`, `vault.append/replay`, `partition()`, `on_fail`, `throttle()` — flagged in FINDINGS |
| 4 Cleanliness | 4 | **partition fan-out is elided** (l.58–63): wires partitions 0, 1, 7 with `// ...` standing in for 2–6 — incomplete demonstration wiring |
| 5 Efficiency | 5 | knee backpressure, retention windows, fork partitioning |
| 6 Robustness | 5 | dead-letter + retry + throttle — genuine degrade path |
| 7 Provenance | 5 | append-log retention, disk-usage taps |
| 8 Docs | 5 | header + honest FINDINGS |
| 9 Config | 4 | port literal, conventional |
| 10 Consistency | 5 | matches corpus |

## Fix plan

```
[P2] cleanliness · l.58–63 · partition fan-out elided (// ... for partitions 2–6)
     → either wire all 8 explicitly, or express as a generated fan-out
       (partition(i) -> consumer(i) for i in 0..count) if the language supports it;
       otherwise annotate that only a sample is wired for illustration
     → VERIFY: zplus-run PASS; no silent gap between claimed count(8) and wired count
```

**Language gaps depended on:** `topic()`, `consumer_group()`, `ack`/upstream flow,
`vault.append/replay`, `partition(by:,count:)`, `on_fail(retries:)`, `producer.throttle()`.
`ack` = backward/upstream signal (register — same family as the 03 round-trip gap).
