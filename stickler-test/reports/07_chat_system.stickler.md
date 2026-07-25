# Stickler Report — `07_chat_system.zp`

**Runs:** PASS · **Score: 84 / B** · **Verdict: FIX**

Room-as-merge-point, presence-as-temporal-signal (decays via `on_silence`), typing-as-keystroke-
debounce, threads-as-sub-wires, reactions-as-micro-signals, moderation via MDE fork, encrypted
DMs. Rich and paradigm-native. Two leaks and one dangling attachment.

## Axis findings

| Axis | Score | Note |
|---|---|---|
| 1 Correctness | 5 | runs clean |
| 2 Paradigm Fidelity | 4 | presence/typing/threads idiomatic; **−1**: string concat `"@" + user.name` (l.92) |
| 3 Surface Discipline | 3 | leans on `room()`, `reply(to:)`, `react()`, `queue(until:)`, `vault.search`, `private(encrypt:)` — flagged |
| 4 Cleanliness | 4 | **dangling modifiers on their own lines**: `gate(author: not self)` (l.70) trails the typing chain without a `->`; `no_tap: true` (l.76) likewise — ambiguous attachment |
| 5 Efficiency | 5 | debounce + silence for typing; taps for telemetry |
| 6 Robustness | 5 | presence decay, quiet-hours queueing, moderation hold |
| 7 Provenance | 4 | vault store (retention: forever), room-size taps |
| 8 Docs | 5 | header + FINDINGS; the "room=topic=timeline = one channel primitive" insight is strong |
| 9 Config | 4 | conventional |
| 10 Consistency | 5 | matches corpus (reuses chirp.zp DM pattern) |

## Fix plan

```
[P1] paradigm · l.92 · string concat in gate(content ~ "@" + user.name)
     → signal-native mention match: gate(content: mentions(user)) or gate(mention: user)
     → VERIFY: red-flag gone; zplus-run PASS
[P2] cleanliness · l.70, l.76 · modifiers on their own lines with no connector
     → attach explicitly to their chains (inline the gate; make no_tap a chain attribute)
     → VERIFY: zplus-run PASS; no free-floating modifier lines
```

**Language gaps depended on:** `room()`, `reply(to:)`, `react()`, `queue(until:)`,
`vault.search`, `private(encrypt:)`. The `queue(until:)` deferred-delivery and the
channel-primitive unification are strong language proposals (register / candidate).
