# Z+ Language Specification v2

**Signal flow. Nothing else.**

*Codex Labs LLC — 2026*

---

## 1. What Z+ Is

Z+ describes connections between signals. The OS resolves them. You don't tell the machine what to do. You tell it what's connected to what.

```
sensor -> gate(> 20cm) -> motor
```

That's a program. The sensor emits. The gate evaluates. The motor responds. Continuously. No loop. No polling. No sleep.

**Z+ is to TRISA OS what C was to Unix — the native tongue.**

---

## 2. Operators

The entire language.

### Flow

| Operator | Name | Meaning |
|----------|------|---------|
| `->` | flow | signal flows from left to right |
| `~>` | tap | observe without affecting (read-only) |
| `-x>` | sever | cut a wire (disconnect, unfollow, block) |
| `<->` | exchange | bidirectional — request/response, ack |
| `@` | pin | run on specific hardware |

### Structure

| Operator | Name | Meaning |
|----------|------|---------|
| `\|` | merge | multiple signals converge into one point |
| `{}` | fork | one signal splits to multiple paths simultaneously |
| `->` chain | chain | A -> B -> C — signal flows through a pipeline |

### Logic

| Operator | Name | Meaning |
|----------|------|---------|
| `gate()` | gate | signal passes or doesn't |
| `knee:` | knee | smooth transition zone (no hard cutoff) |
| `delta()` | delta | rate of change: `t - t-1` |
| `Δ()` | delta (alt) | same as `delta()` |

### Temporal

| Operator | Name | Meaning |
|----------|------|---------|
| `t` | now | current signal value |
| `t-1` | previous | one step back |
| `t-N` | history | N steps back |
| `on_silence()` | silence | nothing arrived for duration |
| `on_block` | blocked | gate rejected the signal |
| `sustained:` | sustained | gate must hold for duration |
| `within()` | confluence | signals must arrive within time window |
| `then` | sequence | A then B — ordered temporal coincidence |
| `debounce()` | debounce | collapse rapid signals into one |
| `throttle()` | throttle | limit signal rate |
| `decay()` | decay | signal strength decreases over time |
| `tick()` | clock | emit signal at fixed rate |

### Value

| Operator | Name | Meaning |
|----------|------|---------|
| `~` | range | between two values: `0.6 ~ 0.9` |
| `any()` | or-match | match any of several values |
| `not:` | negate | exclude matches |
| `σ` | sigma | standard deviations from baseline |

---

## 3. Signal Sources

Signals originate from the world. The OS discovers them.

```
// hardware — discovered at boot
sensor    : gpio(17)            -> distance @ cm
camera    : csi(0)              -> frame
fleet     : pcie(vendor: habana) -> cards

// filesystem
changes   : fs("/home/z13/projects") -> events

// network
server    : net.listen(port: 8080) -> requests
feed      : net.connect(host, port) -> data
devices   : net.discover(proto: zigbee | modbus) -> found

// time
heartbeat : tick(rate: 60Hz)    -> frame
daily     : tick(rate: daily, at: "00:00 UTC") -> trigger

// OS internals
cores     : zixel.cores         -> telemetry
memory    : zixel.memory        -> usage
```

Signal sources emit continuously. They don't "run." They exist.

---

## 4. Gates

The gate is the universal logic construct. Signal passes or doesn't.

### Basic gate

```
// threshold
signal -> gate(> 80C) -> alarm

// range
signal -> gate(0.6 ~ 0.9) -> tier_general

// match
signal -> gate(type: error) -> errors

// negation
signal -> gate(not: "*.tmp", ".git/*") -> clean

// multi-match
signal -> gate(command: any("set", "del")) -> writes
```

### Knee (smooth transition)

Every gate can have a knee. The knee creates a smooth transition zone instead of a hard cutoff.

```
// hard gate: off at 71F, on at 72F (oscillates)
temp -> gate(< 72F) -> heater

// knee gate: fades smoothly from 69F to 72F (no oscillation)
temp -> gate(< 72F, knee: 3F) -> heater
```

Knee shapes:

```
gate(< 72F, knee: 3F)                    // linear ramp (default)
gate(< 72F, knee: 3F, curve: soft)       // S-curve
gate(< 72F, knee_in: 3F, knee_out: 1F)   // asymmetric
```

The knee is how Z+ does proportional control. A gate with a knee doesn't output boolean. It outputs a value between 0.0 and 1.0 based on where the signal sits in the transition zone.

### Temporal gates

```
// sustained — signal must hold above threshold for duration
errors -> rate(per: 1m) -> gate(> 10, sustained: 5m) -> alarm
// won't fire on a momentary spike. must be sustained.

// silence — nothing arrived
sensor -> on_silence(> 100ms) -> failover
sensor -> on_silence(> 5s) -> decommission
// graded silence. quiet is different from gone.

// blocked — gate rejected a signal
rate_limit -> on_block -> respond(429)
// what happens to the signal that didn't pass.
```

### Composite gates

```
// outside range
ph -> gate(outside: 6.5 ~ 8.5) -> alarm

// crossing threshold (fires once on transition, not continuously)
temp -> gate(crossing: 80C) -> event

// rising / falling
grade -> gate(rising, > 0.3) -> recovery
grade -> gate(falling, > 10%, sustained: 30d) -> degradation
```

---

## 5. Merge

Multiple signals converge into one point.

```
// basic merge
sonar  -> |
camera -> | -> decision
lidar  -> |
```

### Merge policies

```
// all — wait for every signal (chord)
a -> |
b -> | all | -> output
c -> |

// any — first signal wins
a -> | any | -> output

// quorum
a -> |
b -> | 2 of 3 | -> output
c -> |

// sorted merge (order book, timeline)
bids -> | merge(sort: price descending, then: time ascending) | -> book

// temporal — signals within a time window
a -> |
b -> | within(100ms) | -> fused
c -> |

// ordered temporal — A then B
badge  -> |
door   -> | then, within(30s) | -> normal_entry
```

### Resonance

Independent signals converging on the same conclusion.

```
// 3 independent anomaly signals within 5 minutes = confirmed
network_anomaly  -> |
file_tampering   -> | resonance(2 of 3, within: 10s) | -> probable
process_spawn    -> |

network_anomaly  -> |
file_tampering   -> | resonance(3 of 3, within: 10s) | -> confirmed
process_spawn    -> |
```

One anomaly is noise. Two are suspicious. Three are certain. Resonance is convergent evidence.

---

## 6. Fork

One signal splits to multiple paths simultaneously.

```
camera -> {
    display,                        // path 1: show it
    detect -> classify -> act       // path 2: analyze it
}
```

Both paths exist. Both resolve. No threads. No async.

---

## 7. Tap

Observe a signal without affecting the flow.

```
controller -> motor           // signal drives the motor
controller ~> dashboard       // tap observes, doesn't affect

// telemetry is always taps
requests ~> rate(per: 1s) ~> rps
errors   ~> count(per: 1h) ~> error_rate
```

Taps are how monitoring works. The signal chain is the work. The taps are the observation. Zero overhead on the main flow.

---

## 8. Delta

Rate of change. The language's soul.

```
// first derivative — velocity
delta(temp)                  // how fast is it changing?

// second derivative — acceleration
delta(delta(temp))           // is the change speeding up?

// third derivative — jerk
delta(delta(delta(temp)))    // is this losing control?
```

Delta is not a library function. It's a keyword. Because the OS maintains temporal history on every signal. `t`, `t-1`, `t-2` exist. Delta is just `t - t-1`.

### Baseline + Deviation

```
// learn what's normal
signal -> baseline(window: 7d) -> normal

// measure deviation from normal
signal -> deviation(from: normal) -> dev

// gate on standard deviations
dev -> gate(> 2σ, knee: 1σ) -> anomaly
```

Seasonal baselines:

```
// Monday at 9am is different from Sunday at 3am
cpu -> baseline(window: 7d, by: day_of_week, hour) -> seasonal_normal
```

---

## 9. Grade

Continuous health score. Not boolean. Proportional.

```
grade : weighted(
    thermal:   0.3,
    timing:    0.25,
    bandwidth: 0.25,
    aging:     0.1,
    errors:    0.1
) // outputs 0.0 to 1.0
```

Everything downstream reacts proportionally:

```
// route by grade — healthiest get more work
cards -> route(proportional: grade)

// tier by grade
grade -> gate(> 0.9)       -> tier_prime
grade -> gate(0.6 ~ 0.9)   -> tier_general
grade -> gate(0.3 ~ 0.6)   -> tier_light
grade -> gate(<= 0.3)      -> tier_rest
```

---

## 10. Priority

Two speeds of resolution.

```
// reflex — interrupt speed, overrides everything
thermal -> gate(> 95C) -> emergency_shutdown
    priority: reflex

// deliberate — takes time, considers context (default)
thermal -> failure_horizon(85C) -> gentle_migrate
    priority: deliberate
```

Reflex is spinal. Firewall packet filtering. Emergency brake. Safety interlocks. Smoke alarm.

Deliberate is cortical. Path planning. Trend analysis. Optimization.

Program-level priority:

```
// entire program runs at reflex speed
priority: reflex    // at top of .zp file
```

---

## 11. Type System

Signal-oriented. Unit-enforced.

### Primitives

```
distance    : f32 @ cm | m | in | ft
temperature : f32 @ C | F | K
pressure    : f32 @ psi | bar | Pa
speed       : f32 @ m/s | rpm | mph
voltage     : f32 @ V | mV
current     : f32 @ A | mA
frequency   : f32 @ Hz | kHz | MHz
time        : f64 @ s | ms | us | ns
power       : f32 @ W | kW | MW
flow        : f32 @ gpm | lpm
angle       : f32 @ deg | rad
frame       : image @ rgb | gray | depth
audio       : stream @ pcm | delta
label       : string
trigger     : bool
raw         : bytes
vec2        : (f32, f32)
vec3        : (f32, f32, f32)
```

The `@` operator declares units. The compiler enforces consistency. You cannot connect a distance output to a temperature input. You cannot accidentally mix cm and inches.

### Structured signals

```
message : {
    author,
    content : text | image | file,
    timestamp: t
}
```

### No generics. No inheritance. No classes.

Signals have structure. That's it. If you need a collection, the signal carries multiple values. The OS handles plurality from discovery.

---

## 12. Vault

Signal-aware data surface. Not a database. A living store that emits, ages, and responds to queries.

### Storage

```
vault.store(collection, data)                   // store signal
vault.store(collection, data, ttl: 30d)         // with expiry
vault.append(collection, data)                  // append-only (logs, ledger)
vault.append(collection, data, immutable: true) // tamper-proof
```

### Retrieval

```
vault.read(collection, id)           // single record
vault.query(collection, filter)      // filtered query
vault.scan(collection, pattern)      // pattern match
vault.search(collection, text)       // full-text search
vault.nearest(collection, vector, k) // vector similarity
vault.prefix(collection, text)       // prefix match (autocomplete)
vault.replay(collection, from)       // read history as signal stream
```

### Signals from vault

```
vault.entries -> on_change -> ...        // emits when data changes
vault.memory_usage -> ...                // storage pressure signal
vault.count(collection) -> ...           // entry count
vault.size(collection) -> ...            // storage size
```

### Ledger (atomic multi-entry)

```
vault.ledger.atomic {
    debit(from: account_a, amount),
    credit(to: account_b, amount)
}
// all succeed or none do. money never vanishes.
```

### Properties

- Temporally aware — TTL, retention, decay are native
- Emits signals — on_change, memory pressure, counts
- Multi-modal — KV, text search, vector search, append-only, ledger
- Replayable — stored signals can be re-emitted as a stream

---

## 13. Channel

Named merge point with storage and taps. The universal communication primitive.

```
channel("orders") : producers -> | merge | -> vault.append -> consumers
```

Rooms (chat), topics (message queue), timelines (social), feeds (streaming) — all the same construct:

```
// chat room
channel("general") : members.voice -> | merge(by: time) | -> members.ears

// message queue topic
channel("orders") : producers -> | merge | -> vault.append -> consumer_groups

// social timeline
channel(user.timeline) : follows.voice -> | merge(by: time) | -> user.ears
```

Channels have:
- Named identity
- Merge policy (by time, by priority, by score)
- Optional storage (vault.append for persistence)
- Consumer tracking (position/offset for replay)

---

## 14. Exchange (Bidirectional Flow)

Request/response patterns. Signal goes out, answer comes back.

```
// HTTP: request flows through gates, response returns
request <-> { gate(path: "/api") -> handler } -> response

// message queue: consumer receives, ack flows back
message <-> consumer -> ack -> advance(offset)

// shorthand: respond() sends back through originating wire
request -> handler -> respond(200, data)
```

The wire remembers where the signal came from. `respond()` routes back to the source.

---

## 15. MDE Integration

AI models are nodes. Inference is signal flow.

```
// load a model, run on specific hardware
frame -> detect @ mde("yolo-v8.zdx") @ goya(0) -> objects

// hot-swap models at runtime
detect.mde = "yolo-v9.zdx"    // no restart

// cascading experts
camera -> preprocess @ fpga(0)
       -> detect     @ goya(0)
       -> classify   @ goya(1)
       -> decide     @ cpu
       -> act        @ gpio
```

MDE reads each device's MasQ and routes to optimal hardware automatically. The `@` pin overrides when you know better.

---

## 16. Zixel

The OS feels its own silicon. Zixel signals are always available.

```
zixel.cores            -> per-core telemetry
zixel.core(N).thermal  -> temperature of core N
zixel.core(N).timing   -> timing characteristics
zixel.memory           -> memory usage, bandwidth
zixel.storage          -> disk I/O, capacity
zixel.network          -> interface statistics
zixel.device(name)     -> device-specific telemetry
```

Every Zixel signal has temporal history. `zixel.core(0).thermal` at `t`, `t-1`, `t-2`.

---

## 17. MasQ

Temporal provenance. Every component carries its history.

```
// device history
masq.device(goya_0)         -> full performance profile
masq.device(goya_0).aging   -> aging trend
masq.device(goya_0).prefers -> what data formats it handles best

// module history
masq.recall("model.zdx")    -> version history
masq.goto("model.zdx", when: "2026-03-15") // time travel
masq.diff("model.zdx", from: "tuesday", to: "now") -> changes
```

MasQ is what makes the Ship of Theseus stay itself. Hardware changes. History persists.

---

## 18. Error Model

Z+ doesn't crash. Signal chains degrade.

```
// node goes silent — hold last value
sensor -> on_silence: hold(t-1)

// silent too long — emit safe default
sensor -> on_silence(> 500ms): emit(0)

// graded silence
sensor -> on_silence(> 100ms) -> "quiet"
sensor -> on_silence(> 1s) -> "gone"
sensor -> on_silence(> 10s) -> "dead"
```

No try/catch. No exceptions. No stack traces. A node goes quiet and the chain responds according to its declared behavior. Graceful degradation is structural.

### Retry

```
// simple retry
operation -> on_fail(retries: 3) -> dead_letter

// escalating retry
operation -> on_fail(schedule: [1s, 10s, 1m]) -> dead_letter

// retry with different behavior per attempt
retry -> {
    attempt(1) -> notify("retrying"),
    attempt(2) -> notify("second attempt"),
    attempt(3) -> escalate
}
```

---

## 19. FAFO — Consequence Awareness

The compiler knows about consequences.

```
// wire a heater with no thermal protection:
// COMPILER WARNING: node 'heater' has no zixel constraint
// "You sure about that?"

// wire a gate with no knee on a safety-critical path:
// COMPILER WARNING: gate has no knee — hard cutoff on safety path
// "Consider adding knee for smooth transition"
```

Not a hard stop. Not a nanny. A question: "hey, you sure?"

---

## 20. File Format

Extension: `.zp`

```bash
$ ls my-project/
main.zp          # primary wiring
sensors.zp       # sensor definitions
behaviors.zp     # behavior logic
masq/            # temporal history
```

### Program structure

```
// optional: program-level priority
priority: reflex

// signal sources
server : net.listen(port: 8080) -> requests

// wiring (this IS the program)
requests -> gate(path: "/api") -> handler -> respond
requests -> gate(path: "/") -> static -> respond

// telemetry (taps)
requests ~> rate(per: 1s) ~> rps
```

No imports. No main function. No boilerplate. The file is wiring. The wiring is the program.

---

## 21. Compilation Targets

| Target | Use Case |
|--------|----------|
| TRISA OS native | Direct signal graph resolution |
| ARM bare-metal | Raspberry Pi, embedded, no OS overhead |
| FPGA bitstream | Hardware synthesis for signal chains |
| x86 TRISA compat | Development and testing on Linux |
| Codex Box native | Graph cores + tensor cores |
| WebAssembly | Browser simulator for classrooms |

The WebAssembly target means kids can learn Z+ in a browser. When ready, the same `.zp` file is intended to run on real hardware with zero translation (target, not yet demonstrated on a physical board).

---

## 22. Visual Parity

Z+ is bidirectional — text and visual graph are the same thing. Edit either. The other updates.

```
// text:
sonar -> gate(> 20cm) -> motor

// visual:
┌────────┐    ┌────────┐    ┌───────┐
│ sonar  │───→│ > 20cm │───→│ motor │
│gpio(17)│    │  gate  │    │pwm(18)│
└────────┘    └────────┘    └───────┘
```

Kids who can't type drag and drop nodes. Kids who can type write code. Both are the same program. Always in sync.

---

## 23. Reserved Keywords

```
// flow
gate, knee, delta, merge, fork, tap, channel

// temporal
t, t-1, t-2, on_silence, on_block, sustained, within,
then, debounce, throttle, decay, tick, baseline, deviation

// priority
reflex, deliberate

// system
zixel, masq, mde, vault

// lifecycle
on_fail, retry, hold, emit, halt, drop, accept, abort, respond

// structure
weighted, grade, resonance, priority

// hardware
device, gpio, pwm, csi, i2c, spi, uart, pcie,
goya, fpga, coral, npu, cpu

// network
net, listen, connect, discover, broadcast

// storage
fs, store, read, delete, append, query, search,
nearest, replay, scan, prefix, ledger, atomic,
immutable, retention

// units
cm, m, in, ft, mm,
C, F, K,
s, ms, us, ns,
Hz, kHz, MHz, GHz,
V, mV, A, mA,
W, kW, MW,
psi, bar, Pa,
deg, rad,
m/s, rpm, mph, gpm,
percent, ppm, bpm,
σ
```

Every keyword is a signal concept, a hardware interface, a unit, or a system subsystem. No `class`. No `function`. No `return`. No `while`. No `for`. No `try`. No `catch`.

---

## 24. What Doesn't Exist

| Concept | Why Not |
|---------|---------|
| Variables | Signals flow. State is `t`. History is `t-1`. |
| Functions | Nodes exist continuously. They don't "run once." |
| Loops | Signal chains resolve continuously. Nothing to repeat. |
| Threads | Concurrency is structural. Two chains both exist. |
| Locks/Mutexes | No shared mutable state. Signals flow through wires. |
| Exceptions | Chains degrade gracefully. `on_silence`, not `try/catch`. |
| Imports | The OS knows everything at boot. No libraries to load. |
| Classes/Objects | Signals have structure. That's it. |
| Null | Silence is the absence of signal. It's data, not error. |
| Garbage collection | Signals flow. What's past is `t-1`. Vault manages storage. |
| Package manager | `.zp` files are wiring. They compose by connecting, not importing. |
| Build system | The compiler reads `.zp` files. No makefile. No config. |

---

## 25. Summary

| Property | Z+ |
|----------|-----|
| Fundamental unit | Connection (`->`) |
| Execution model | Simultaneous resolution |
| Concurrency | Structural |
| Time | Native (`t, t-1, t-2`) |
| Change detection | Native (`delta`) |
| Smooth control | Native (`knee`) |
| Hardware awareness | Native (`zixel`) |
| Provenance | Native (`masq`) |
| AI integration | Native (`mde`) |
| Storage | Native (`vault`) |
| Communication | Native (`channel`) |
| Error handling | Graceful degradation |
| Visual/text parity | Bidirectional |

---

*The old world asks: what is true?*
*The new world asks: what is happening?*

*Those are different questions. They need different logic.*
*Z+ speaks the new one.*

**Codex Labs LLC — 2026**
