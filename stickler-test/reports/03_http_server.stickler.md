# Stickler Report — `03_http_server.zp`

**Runs:** PASS · **Score: 82 / B** · **Verdict: FIX**

Routing-as-`gate(method:, path:)`, middleware-as-signal-chain (`log → rate_limit → auth →
route`), schema-validation fork, telemetry taps. Conceptually excellent — but carries a real
structural defect the parser tolerates.

## Axis findings

| Axis | Score | Note |
|---|---|---|
| 1 Correctness | 4 | runs, but see P1 duplicate binding — latent ambiguity |
| 2 Paradigm Fidelity | 5 | routing IS filtering; middleware IS a chain — idiomatic |
| 3 Surface Discipline | 3 | heavy undefined surface (`respond`, `params`, `body`, `header()`, `validate()`, `on_block`, round-trip) — flagged, but dense |
| 4 Cleanliness | 3 | **duplicate `server : net.listen(...)` binding** (l.8 AND l.25); `requests` defined by two different chains — dead v1 or ambiguous active source |
| 5 Efficiency | 5 | rate-limit knee, single routing fan-out |
| 6 Robustness | 4 | `on_block` → 401/429, `gate(unmatched)` → 404, found/not_found forks |
| 7 Provenance | 4 | request taps (rps, latency, status breakdown) |
| 8 Docs | 5 | header + FINDINGS; raises the round-trip/return-path question well |
| 9 Config | 4 | port literal but conventional |
| 10 Consistency | 5 | matches corpus |

## Fix plan

```
[P1] cleanliness/correctness · l.8 & l.25 · duplicate net.listen("server") binding
     → collapse to ONE server source; keep the v2 middleware chain, delete the
       superseded v1 line (l.8) so `requests` has a single unambiguous origin
     → VERIFY: zplus-run PASS; exactly one `net.listen` binding remains
[P1] paradigm/spec · round-trip · respond() has no defined way to route back to its request
     → decide the return-path model (implicit wire-memory vs explicit request identity);
       until decided, document the assumption inline (do NOT silently rely on it)
     → VERIFY: assumption documented; zplus-run PASS  (language decision — see CANDIDATE: net_source.zp)
[P2] surface · consolidate the undefined HTTP verbs into the Language Gap Register (done)
```

**Language gaps depended on:** `net.listen`, `respond`, `params`, `body`, `header()`,
`validate()`, `on_block`, round-trip wire. The duplicate-binding fix is program-level and P1.
