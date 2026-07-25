# Stickler Report — `05_firewall.zp`

**Runs:** PASS · **Score: 81 / B** · **Verdict: FIX**

Program-level `priority: reflex`, stateful `gate(state:)` tracking, default-deny
(`gate(otherwise) -> drop`), SYN-flood rate knee, port-scan via `count(distinct:, within:)`
(confluence), and MDE deep-packet inspection in the path. Ambitious and paradigm-strong — but
has a real chain-structure defect around terminal nodes.

## Axis findings

| Axis | Score | Note |
|---|---|---|
| 1 Correctness | 4 | runs, but **accept-then-continue** structural defect (see P1) — `accept` is terminal yet the chain continues |
| 2 Paradigm Fidelity | 4 | reflex, stateful gates, confluence — strong; **−1**: string concat `"port scan from " + source` (l.58) |
| 3 Surface Discipline | 3 | dense undefined surface (`net.interface`, `drop`/`accept` terminals, `group/count distinct`, `source.geo`, `blacklist.add`, program-level reflex) — flagged |
| 4 Cleanliness | 4 | clear sections; `gate(port: 80 \| 443)` reuses the `|`-as-OR hazard (l.67) |
| 5 Efficiency | 5 | whole-graph reflex is correct for a wire-speed filter |
| 6 Robustness | 4 | default-deny, SYN limiting, scan blacklist with TTL |
| 7 Provenance | 4 | pps/drops/top-blocked/port-heatmap taps |
| 8 Docs | 5 | header + FINDINGS; raises terminal-nodes and program-level-priority well |
| 9 Config | 4 | interface/ports literal but conventional |
| 10 Consistency | 5 | matches corpus |

## Fix plan

```
[P1] correctness/paradigm · l.68–73 · chain continues after a terminal `accept`:
     `-> gate(threat < 0.3) -> accept -> gate(threat >= 0.3) -> {drop, alert}`
     accept terminates the signal; you cannot gate it further on the same wire.
     → fork BEFORE terminating: threat -> { gate(< 0.3) -> accept, gate(>= 0.3) -> {drop, alert} }
     → VERIFY: no terminal node has downstream gates; zplus-run PASS
[P1] paradigm · l.58 · string concat in alert("port scan from " + source)
     → signal-native form: alert(port_scan, source:)
     → VERIFY: red-flag gone; zplus-run PASS
[P2] idiom · l.67 · gate(port: 80 | 443) → gate(port: any(80,443))
     → VERIFY: no `|`-as-OR in gate; zplus-run PASS
```

**Language gaps depended on:** `net.interface`, terminal `drop`/`accept`, program-level
`priority: reflex`, `group(by:)`, `count(distinct:, within:)`, `source.geo`,
`blacklist.add(ttl:)`. The accept-then-continue and string-concat fixes are program-level (P1);
terminal-node semantics are also a language decision — see CANDIDATE: terminal_nodes_demo.zp.
