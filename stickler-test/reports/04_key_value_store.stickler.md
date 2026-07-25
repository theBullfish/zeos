# Stickler Report — `04_key_value_store.zp`

**Runs:** PASS · **Score: 89 / B** · **Verdict: FIX**

`gate(command:)` op routing (same pattern as HTTP), TTL as a temporal gate (no background
sweeper), replication as a write-tap, LRU eviction via a memory-pressure `knee`. Very clean; one
idiom hazard.

## Axis findings

| Axis | Score | Note |
|---|---|---|
| 1 Correctness | 5 | runs clean |
| 2 Paradigm Fidelity | 5 | TTL-as-gate and pressure-knee eviction are model Z+ |
| 3 Surface Discipline | 3 | vault-heavy (`store/read/delete/scan/snapshot/append_log/ttl/entries/memory_usage`), plus `sort/take/ratio` — flagged in FINDINGS |
| 4 Cleanliness | 4 | **`|`-as-OR overload**: `gate(command: "set" \| "del")` (l.52, l.57) collides with `|` = merge elsewhere |
| 5 Efficiency | 5 | debounced snapshot, knee eviction, tap replication |
| 6 Robustness | 4 | found/expired/missing forks; **no failure path** if `vault.store` fails (l.20) |
| 7 Provenance | 5 | AOF append-log, snapshot, hit/miss ratio taps |
| 8 Docs | 5 | header + FINDINGS; names the Vault-as-signal-surface insight |
| 9 Config | 4 | port/paths literal but conventional |
| 10 Consistency | 5 | matches corpus |

## Fix plan

```
[P1] idiom/clarity · l.52,57 · gate(command: "set" | "del") overloads | (merge) as OR
     → replace with gate(command: any("set","del"))  (proposed in the program's own FINDINGS)
     → VERIFY: no bare `|`-as-OR inside a gate; zplus-run PASS
[P2] robustness · l.20 · set_op -> vault.store(...) -> respond("OK") assumes success
     → fork on store result: gate(stored) -> respond("OK"), gate(failed) -> respond(error)
     → VERIFY: failure branch present; zplus-run PASS
```

**Language gaps depended on:** the whole `vault.*` surface, `sort(by:)`, `take(N%)`, `ratio()`,
and the `any(...)` in-gate OR helper. Vault is emerging as a first-class signal-emitting data
layer — see CANDIDATE: vault_demo.zp. The `|`-overload fix is program-level and P1.
