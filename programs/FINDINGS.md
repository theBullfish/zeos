# Z+ Language Findings — 27 Programs Deep

**What works. What's missing. What emerged.**

*Codex Labs LLC — 2026*

---

## Programs Built

Line counts are measured from the files as they exist in this directory
(`wc -l`, and the same with blank and comment-only lines removed). Each
program is a Z+ *wiring specification* for the kind of system named in the
"Modeled on" column: it declares sources, derivations, gates, routing and
taps. It does not implement the verbs it calls (`detect(qrs_complex)`,
`score_resp()`, `weighted()`, `parse()`, `vault.nearest`, …). Those verbs are
where the bulk of a real system's code lives, and they are not yet written.
A line-count ratio against a shipping product would compare a wiring diagram
to a finished product, so none is given.

| # | Program | File lines | Code lines | Modeled on |
|---|---------|-----------:|-----------:|------------|
| 01 | File Watcher | 65 | 17 | inotify-style watcher |
| 02 | Log Monitor | 96 | 30 | ELK-style log pipeline |
| 03 | HTTP Server | 118 | 40 | small HTTP server |
| 04 | Key-Value Store | 114 | 30 | Redis-style KV store |
| 05 | Firewall | 117 | 38 | packet-filter firewall |
| 06 | Message Queue | 127 | 34 | Kafka-style queue |
| 07 | Chat System | 154 | 49 | Matrix-style chat |
| 08 | CI/CD Pipeline | 143 | 53 | Jenkins-style pipeline |
| 09 | Anomaly Detector | 161 | 55 | metrics anomaly detector |
| 10 | Game Server | 182 | 59 | authoritative game server |
| 11 | Home Automation | 181 | 80 | Home Assistant-style hub |
| 12 | Search Engine | 145 | 46 | Elasticsearch-style search |
| 13 | Payment Processor | 175 | 82 | payment gateway |
| 14 | Video Streaming | 140 | 60 | streaming backend |
| 15 | Trading System | 178 | 85 | exchange matching + risk |
| 16 | SCADA / Industrial | 190 | 84 | SCADA control |
| 17 | E-Commerce | 212 | 110 | storefront + orders |
| 18 | Patient Monitor | 198 | 75 | ICU bedside monitor |
| 19 | Autonomous Vehicle | 213 | 95 | AV control stack |
| 20 | Power Grid | 209 | 90 | grid control |
| 21 | Load Balancer | 119 | 49 | HAProxy-style LB |
| 22 | Precision Agriculture | 169 | 70 | field sensing + irrigation |
| 23 | Supply Chain | 194 | 84 | supply-chain tracking |
| 24 | LMS Education | 204 | 97 | Moodle-style LMS |
| 25 | Election System | 164 | 61 | election tabulation |
| — | Twitter Clone (`chirp.zp`) | 169 | 57 | Twitter-style feed |
| — | Goya Fleet (`goya_fleet_t3.zp`) | 73 | 36 | Goya card fleet control |

**This set: 27 programs, 4,210 file lines, 1,666 code lines.**
**Whole `programs/` corpus at time of writing: 85 `.zp` files, 12,092 file lines, 5,549 code lines.**

What the table does show: the wiring layer of each of these systems fits in
tens of lines of Z+, and the same handful of operators carries every domain.
What it does not show: how big the systems become once the verbs are real.
That number does not exist yet.

### The Universal Pattern (confirmed across all 27)
```
signal in → preprocess → score/filter → route → signal out
```
This is TRISA. Every program. Every industry. Every domain.

---

## What Works (Confirmed Across All Programs)

### Core operators — solid everywhere
- `->` (flow) — used in every single program. never ambiguous.
- `~>` (tap) — telemetry in every program. always read-only.
- `|` (merge) — timelines, rooms, topics, search fusion, chords. universal.
- `{}` (fork) — parallel paths in every program. clean.
- `gate()` — the universal filter. routing, security, logic, everything.
- `knee` — thermostat, rate limiting, eviction, deploy ramp. kills bang-bang.
- `delta()` — change detection, drift, anomaly, physics. the language's soul.
- `t, t-1, t-2` — temporal access in game server, anomaly detector, everywhere.
- `on_silence` — presence, heartbeat, failure detection. silence IS signal.
- `@` — hardware pinning (fleet), device routing (search NPU). clean.

### Patterns that repeated across domains
- **gate as router** — HTTP routing, command dispatch, packet filtering, mode switching. same construct.
- **tap as telemetry** — every program ends with `~>` for monitoring. zero overhead.
- **debounce** — file watcher, autocomplete, typing indicators. always useful.
- **rate()** — throughput measurement in every program. universal.
- **baseline + deviation** — anomaly detection, energy monitoring, seasonal awareness.
- **resonance** — anomaly correlation, anti-cheat, trending. independent signals converging.
- **reflex vs deliberate** — firewall (reflex), navigation (deliberate), smoke alarm (reflex).

---

## What's Missing (Needs Addition to Spec)

### Signal Sources
| Source | Programs That Need It |
|--------|----------------------|
| `fs()` — filesystem as signal source | file watcher, log monitor |
| `net.listen()` — network listener | HTTP, KV store, message queue, chat, search |
| `net.connect()` — outbound connection | replication, crawling |
| `net.discover()` — protocol discovery | home automation |
| `net.interface()` — raw network | firewall |
| `git.watch()` — repository events | CI/CD |
| `tick(rate:)` — clock signal | game server |
| `time` — calendar/clock as signal | home automation, CI/CD |

### Signal Terminals
| Terminal | Purpose |
|----------|---------|
| `respond()` — send reply back through wire | HTTP, KV store, search |
| `drop` — silently terminate signal | firewall |
| `accept` — pass signal forward and terminate chain | firewall |
| `abort()` — terminate chain with reason | CI/CD |
| `disconnect()` — sever connection | game server, chat |

### Vault Operations
| Operation | Purpose |
|-----------|---------|
| `vault.store` | basic storage |
| `vault.read` | basic retrieval |
| `vault.delete` | removal |
| `vault.append` | append-only log (message queue, WAL) |
| `vault.snapshot` | point-in-time persistence |
| `vault.replay` | read stored signals as stream |
| `vault.search` | keyword search |
| `vault.nearest` | vector similarity search |
| `vault.prefix` | prefix matching |
| `vault.index` | add to inverted/vector index |
| `vault.scan` | pattern-matching iteration |
| `vault.memory_usage` | storage pressure signal |

### New Operators
| Operator | Syntax | Purpose |
|----------|--------|---------|
| Range | `0.6 ~ 0.9` | between two values (gate ranges) |
| Negation | `gate(not: pattern)` | exclude matches |
| Fuzzy match | `~ "pattern"` | substring/fuzzy match in gates |
| OR in gate | `gate(command: any("set", "del"))` | match multiple values |
| Sever | `-x>` | cut a wire (unfollow, block, disconnect) |

### New Constructs
| Construct | Syntax | Discovered In |
|-----------|--------|---------------|
| `sustained(for:)` | gate must hold for duration | log monitor, CI/CD canary |
| `on_block` | gate rejected a signal | HTTP rate limiter, firewall |
| `baseline(window:)` | rolling learned normal | anomaly detector, home auto |
| `deviation(from:)` | distance from baseline | anomaly detector |
| `σ` (sigma) | standard deviation unit | anomaly detector |
| `then` | ordered temporal sequence | anomaly patterns |
| `queue(until:)` | deferred delivery | chat quiet hours |
| `decay(half_life:)` | temporal signal degradation | search ranking, trending |
| `partition(by:, count:)` | deterministic distribution | message queue |
| `consumer_group()` | stateful tap with position | message queue |
| `parse()` | structured extraction from text | log monitor, KV store |
| `group(by:)` | signal grouping | search facets, firewall |
| `normalize` | scale to 0-1 | search ranking |
| `rewind(by:)` | read world at t-N | game server |
| `random(interval:)` | randomized signal | home auto presence sim |
| `defer(until:)` | postpone action | home auto energy |

---

## Architectural Discoveries

### 1. Bidirectional Flow Needed
HTTP request/response, message queue ack, game client/server reconciliation — all need signals to flow BACK upstream. Current spec is one-directional (`->`). Need either:
- `<->` bidirectional operator
- Implicit return path (respond flows back through the wire it arrived on)
- Named return channels

**Recommendation:** implicit return. `respond()` sends back through the originating wire. The wire remembers where the signal came from. This matches how the OS already works — every signal has provenance.

### 2. Vault Is a First-Class Subsystem
Vault appeared in every single program. It's not just "storage." It's:
- Signal-aware (emits on_change)
- Temporally aware (TTL, retention, decay)
- Queryable in multiple modes (KV, search, vector, prefix, scan)
- Pressure-aware (memory_usage as continuous signal)
- Append-capable (logs, message queues)
- Replayable (read history as signal stream)

Vault is the fourth pillar alongside Zixel, MasQ, and MDE.

### 3. Named Merge Points Are Universal
Rooms (chat), topics (message queue), timelines (social), and channels all share one structure: a named wire where multiple sources merge and multiple consumers tap. This should be a first-class primitive:

```
channel("orders") : sources -> | merge | -> vault.append -> consumers
```

### 4. Every Program Is TRISA
Every single program follows the same pattern:
```
signal in → preprocess → score/filter → route → signal out
```
This is not a coincidence. This is the architecture. Z+ can't express anything else because the signal graph IS a TRISA pipeline. The language and the preprocessing engine are the same thing.

### 5. What The Line Counts Do And Don't Say
A Z+ program here is the wiring: sources, derivations, gates, routes, taps.
It is short because the language expresses only that layer, and because the
verbs it calls (`detect()`, `score_*()`, `parse()`, `vault.*`, `weighted()`)
are named, not implemented. Three things are true at once:

1. The wiring layer of every one of these systems fits in tens of lines, and
   the same operators carry every domain. That is the finding.
2. A conventional codebase for the same system also contains the verbs, the
   infrastructure that simulates signal flow, and the glue between components.
   Z+ removes the need to hand-write the second and third of those. It does
   not remove the first.
3. How much code the verbs will take is unknown until they exist. Any ratio
   quoted before then is a guess, so this document no longer quotes one.

---

## Spec Changes Required

### Must Add
1. Signal sources: `fs()`, `net.*`, `git.watch()`, `tick()`, `time`
2. Signal terminals: `respond()`, `drop`, `accept`, `abort()`
3. Vault as full subsystem with all discovered operations
4. Range operator: `~`
5. Sever operator: `-x>`
6. `sustained(for:)` temporal gate
7. `on_block` handler for gate rejection
8. `baseline()` + `deviation()` for learned normals
9. `decay(half_life:)` for temporal degradation
10. `channel()` as named merge+tap+store primitive
11. Bidirectional flow / return path semantics
12. `then` ordered temporal operator
13. `parse()` structured extraction

### Must Remove
1. `node { in: out: resolve: }` wrapper — replaced by bare wiring
2. `.where()` / `.count()` / `.avg()` method syntax — replaced by gates and named aggregates
3. `list<type>` angle bracket syntax — plurality is implicit
4. `per` iterator — replaced by fork fan-out

### Must Clarify
1. `debounce` — restart on new signal or ignore during window?
2. `|` operator — merge (wiring) vs OR (inside gate) — disambiguate
3. Nested nodes — allowed? scoping rules?
4. `<-` operator — is it real? define or remove
5. `exec()` — shell interop needed but feels like escape hatch. formalize.

---

*27 programs in the table above; 85 `.zp` files in the corpus at time of writing.*
*The wiring layer works. The gaps are at the edges, not the core.*
*The core — arrows, gates, knees, deltas, temporal access — is exercised by every program; "proven" waits on native code-gen running them on hardware.*

**Codex Labs LLC — 2026**

---

## Update — corpus expansion + working toolchain (2026-05-05)

The "WORKED / NEEDED / AMBIGUOUS" notes above are from the original 12-program
write-up and were later extended to 27. The `programs/` corpus has since grown
further; the table at the top was re-counted from the files on 2026-10-01.

A working bootstrap toolchain now exists in `tools/zplus/`:

- **Lexer** — all 68 files tokenize cleanly
- **Parser** — all 68 files parse to AST
- **Type checker** — three structural ratchets all at zero across
  the corpus (merge arity / chord rule, Flow connectivity, named-arg
  types)
- **Runtime** — tree-walking interpreter; tick model, sources,
  transformers (real `delta`, real `gate`), forks, merges with
  chord-policy resolution, stateful per-call-site state
- **CLIs** — `zplus-lex`, `zplus-parse`, `zplus-check`, `zplus-run`

The "WORKED / NEEDED / AMBIGUOUS" notes above are mostly resolved by
the parser + checker landings. Native code-gen is the remaining big
piece. See `STATE.md` at repo root.
