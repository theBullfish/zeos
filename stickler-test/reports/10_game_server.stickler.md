# Stickler Report — `10_game_server.zp`

**Runs:** PASS · **Score: 86 / B · Verdict: FIX**

Tick-clock world, delta physics, collision-as-spatial-chord, **delta state sync = TRISA on game
state**, lag-compensation via temporal rewind (`t-N` is free), anti-cheat via delta+variance+
resonance, skill-proximity matchmaking. Conceptually superb. One clear paradigm leak: direct
procedural assignment where the paradigm wants signal-at-`t`.

## Axis findings

| Axis | Score | Note |
|---|---|---|
| 1 Correctness | 5 | runs clean |
| 2 Paradigm Fidelity | 4 | tick/delta/rewind/resonance idiomatic; **−1**: `target.state = dead` (l.64) is old-world `var = value` — the exact form SIGNAL_LOGIC replaces with signal-at-`t`. `entity.vel.y + jump_force` (l.70) and the pos/vel feedback (l.43,51) read procedurally too |
| 3 Surface Discipline | 3 | `tick`, `vec2/3`, `raycast`, `spawn`, `broadcast`, `interpolate`, `rewind`, `variance`, `distance` — flagged |
| 4 Cleanliness | 5 | clean sections; `/dev/null` sink is idiomatic |
| 5 Efficiency | 5 | delta state sync, temporal rewind (no snapshot buffer), knee correction |
| 6 Robustness | 5 | reconciliation, lag comp, anti-cheat resonance |
| 7 Provenance | 4 | frame-time/player-count/bandwidth taps |
| 8 Docs | 5 | header + FINDINGS; tick-clock and delta-sync insights are strong |
| 9 Config | 5 | ports/rates as literals, appropriate |
| 10 Consistency | 5 | matches corpus |

## Fix plan

```
[P1] paradigm · l.64 · `target.state = dead` — procedural assignment (var = value)
     → express as a signal transition: target.state -> dead  (or kill(target) emits state:dead)
     → VERIFY: no bare `=` assignment to state; zplus-run PASS
[P2] paradigm · l.43, l.51, l.70 · pos/vel/jump updates read as mutation
     → confirm these are signal-feedback (x + d -> x is a feedback edge, acceptable) vs
       mutation; if mutation, express as feedback/next-value edges
     → VERIFY: zplus-run PASS; feedback form intended and documented
```

**Language gaps depended on:** `tick(rate:)`, `vec2/3`, `raycast`, `spawn`/`despawn`,
`broadcast`, `interpolate`, `rewind(by:)`, `variance(window:)`, `distance`. `tick(rate:)` is a
first-class clock source proposal — directly relevant to the Simulation section (register).
