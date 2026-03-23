# Signal Logic — New Primitives for a New Paradigm

**When the execution model changes, the logic changes.**

*Codex Labs LLC — 2026*

---

## The Old Logic and What Replaces It

The old forms still have analogs in Z+, but they're structural now, not procedural:

| Old World | Z+ Equivalent | What Changed |
|-----------|---------------|--------------|
| `if/else` | `gate` | Not a branch in a road. A valve in a pipe. Signal flows or doesn't. |
| `while/for` | *doesn't exist* | Signal chains resolve continuously. There's nothing to repeat. |
| `switch/case` | Route by property | Signal finds its path by what it IS, not by being inspected. |
| `try/catch` | `on_silence`, `on_degrade` | Not an afterthought. A structural property of the connection. |
| `function(x) → y` | Node (continuous) | Doesn't run once. Exists always. Emits whenever input changes. |
| `variable = value` | Signal at time `t` | State isn't stored. It flows. History isn't a variable — it's `t-1`. |

These aren't new. They're just the old forms reshaped for signal flow. The real innovation is the logic that **has no old-world equivalent** because the old world couldn't think it.

---

## 1. Chord Logic

In sequential code, you check conditions one at a time:

```python
# Old world: sequential inspection
if sonar_clear and camera_safe and battery_ok:
    proceed()
```

Three checks. One after another. If the first is false, you never look at the others. And the values might change between checks — the sonar was clear when you checked, but by the time you got to the camera, it wasn't anymore. Time-of-check vs time-of-use. A fundamental bug class in sequential logic.

In Z+, signals arrive together and resolve as a **chord**:

```zplus
sonar.clear  -> |
camera.safe  -> | -> proceed -> motors
battery.ok   -> |
```

The merge operator `|` doesn't check sequentially. It waits for all three signals and resolves them **as one simultaneous moment**. Like a musical chord — the notes don't happen one after another. They ring together. The harmony IS the decision.

There is no time-of-check vs time-of-use because there is no gap between the checks. The chord resolves atomically. This logic form **cannot be expressed in sequential code without locks, mutexes, and careful synchronization**. In Z+, it's three arrows and a pipe.

### Partial Chords

Not all signals arrive at the same rate. A chord can resolve with policy:

```zplus
// Resolve when all arrive (strict chord)
a -> |all| -> output

// Resolve when any arrives (first signal wins)
a -> |any| -> output

// Resolve when 2 of 3 arrive (quorum — like distributed consensus, but in wiring)
a -> |
b -> | 2 of 3 | -> output
c -> |

// Resolve when the fastest N arrive, ignore stragglers
a -> |
b -> | fastest(2) | -> output
c -> |
```

Quorum logic. In distributed systems, this is Raft or Paxos — thousands of lines. In Z+, it's `2 of 3`. Because it's not an algorithm. It's a property of the merge point.

---

## 2. Knee Logic (Soft Decisions)

Every `if` statement in every language ever written is a cliff edge. True. False. One side or the other. The real world doesn't work like that.

A thermostat set to 72°F kicks on at 71.9°F and off at 72.1°F. The system oscillates. Bangs on and off. Because boolean logic has no middle ground.

Z+ has the **knee**:

```zplus
node comfort {
    in: temperature
    out: heat_level : f32

    gate: temperature < 72F
    knee: 3F
}
```

The knee creates a **smooth transition zone**. At 69°F, the gate is fully open (heat at 100%). At 72°F, the gate is fully closed (heat at 0%). Between 69°F and 72°F, the heat **fades smoothly** from full to zero.

No oscillation. No bang-bang. No hysteresis hack. The logic itself is analog.

This comes from audio engineering — a compressor's knee is the smooth curve between uncompressed and compressed signal. Brad designed this into the language because he knows what a soft knee does to signal quality. It's the same principle. Hard cutoffs create artifacts. Smooth transitions preserve the signal.

### Knee Shapes

```zplus
// Linear knee — straight ramp
gate: temp < 72F
knee: 3F

// Soft knee — S-curve (like a compressor)
gate: temp < 72F
knee: 3F, curve: soft

// Hard knee with dead zone — holds until clearly past threshold
gate: temp < 72F
knee: 3F, curve: hard, dead: 0.5F

// Asymmetric knee — enters slowly, exits fast (different behavior warming vs cooling)
gate: temp < 72F
knee_in: 3F
knee_out: 1F
```

Every piece of control logic that's ever been written with hysteresis bands, PID tuning, exponential smoothing, or moving averages was working around the fact that `if/else` is a cliff. The knee makes the cliff a slope. At the language level.

---

## 3. Silence as Signal

In every existing language, the absence of data is an error. A timeout. An exception. Something went wrong.

In Z+, silence is information:

```zplus
node heartbeat {
    in: sensor
    out: status

    resolve: "alive"
    on_silence(> 100ms): "quiet"
    on_silence(> 1s): "gone"
    on_silence(> 10s): "dead"
}
```

The node doesn't just detect silence — it **differentiates grades of silence**. Quiet is different from gone. Gone is different from dead. Each triggers different downstream behavior.

```zplus
sensor -> heartbeat -> {
    "quiet" -> wait,                        // Patience
    "gone"  -> failover -> backup_sensor,   // Concern
    "dead"  -> alert + decommission         // Acceptance
}
```

This is **negative-space logic**. Decisions based on what DIDN'T happen. Sequential languages can only do this with timers, polling loops, and timeout handlers — bolted-on mechanisms that approximate the concept. In Z+, silence is a first-class signal with its own temporal gradations.

### Silence Patterns

```zplus
// Intermittent silence — the sensor flickers
sensor -> on_pattern(silence: intermittent, window: 10s) -> "degrading"

// Correlated silence — two sensors go quiet together (probably same cause)
sensor_a -> |
             | on_silence(correlated, > 500ms) -> "shared_failure"
sensor_b -> |

// Silence after spike — it screamed, then went quiet (that's bad)
sensor -> on_pattern(spike then silence, within: 200ms) -> "catastrophic"
```

The spike-then-silence pattern is something every embedded engineer knows intuitively. A sensor that spikes and then goes quiet is fundamentally different from one that slowly fades. The old world has no way to express this distinction structurally. It's buried in custom state machines. In Z+, it's one line.

---

## 4. Delta Logic (Reasoning About Change)

Sequential code reasons about **state** — what IS the value right now?

Z+ reasons about **change** — what is the value DOING right now?

```zplus
// State logic (old world thinking ported to Z+)
temp -> gate(> 80C) -> alarm

// Delta logic (signal thinking)
delta(temp) -> gate(> 2C/s) -> alarm
```

The first triggers when temperature crosses a threshold. The second triggers when temperature is **rising fast** — regardless of the current value. 40°C and climbing at 3°C/s is more dangerous than 79°C and stable. Delta logic knows this. State logic doesn't.

### Higher-Order Deltas

```zplus
// First delta: rate of change (velocity)
delta(temp)             // How fast is it changing?

// Second delta: rate of rate of change (acceleration)
delta(delta(temp))      // Is the change speeding up?

// Third delta: jerk (is the acceleration itself unstable?)
delta(delta(delta(temp)))  // Is this system losing control?
```

A temperature climbing at a steady rate is predictable. One where the rate is accelerating is concerning. One where the acceleration is jerking is a system on the edge of chaotic failure.

Three levels of `delta()`. No calculus library. No numerical differentiation. No buffer management. The OS maintains the temporal window. You just ask.

```zplus
// Real-world application: predict failure before it happens
node failure_horizon {
    in: metric
    out: time_remaining : time @ s

    resolve:
        rate = delta(metric)
        accel = delta(rate)

        // If acceleration is positive, extrapolate to threshold
        accel > 0 -> (threshold - metric) / rate
                      adjusted_by: accel    // Not linear extrapolation — curved

        // If acceleration is zero, simple linear
        accel == 0 -> (threshold - metric) / rate

        // If acceleration is negative, it's self-correcting
        accel < 0 -> infinity               // It's healing
}

// "How long until this card overheats?" — answered continuously, in real time
goya_3.thermal -> failure_horizon(threshold: 85C) -> {
    gate(< 30s) -> emergency_migrate,
    gate(< 5m)  -> gentle_migrate,
    gate(< 1h)  -> note_it
}
```

That's predictive logic. Not machine learning. Not statistics. Just calculus — first, second, third derivatives — expressed as language primitives on a signal that already has temporal history.

---

## 5. Confluence (Temporal AND)

Boolean AND asks: "Are both of these true right now?"

Confluence asks: "Have both of these spoken recently?"

```zplus
// Boolean AND (old thinking — are both true at this instant?)
a AND b -> output

// Confluence (signal thinking — have both contributed within a window?)
a -> |
     | within(100ms) | -> output
b -> |
```

The difference matters. Two sensors might never fire at the exact same nanosecond. Boolean AND would miss every coincidence. Confluence says "these two events are related if they happen within 100ms of each other."

This is how the brain works. Neurons don't fire simultaneously. They fire within a temporal window, and the postsynaptic neuron treats them as coincident if they're close enough. Hebb's rule. Cells that fire together wire together. Confluence is Hebbian logic at the language level.

### Confluence Patterns

```zplus
// Tight confluence — signals must nearly coincide (sensor fusion)
lidar -> |
          | within(1ms) | -> fused_position
gps   -> |

// Loose confluence — signals relate over longer timescales (behavioral)
login_attempt -> |
                  | within(5m) | -> suspicious
password_reset -> |

// Sequential confluence — A then B (order matters)
door_open -> |
              | then, within(30s) | -> expected_entry
badge_scan -> |

// Reverse is different:
badge_scan -> |
              | then, within(30s) | -> normal_entry
door_open  -> |
```

`badge_scan then door_open within 30s` = normal entry.
`door_open then badge_scan within 30s` = someone entered before scanning. Different signal. Different meaning. Same events, different temporal order.

Sequential code needs timestamps, queues, state machines, and careful ordering logic to distinguish these. Z+ expresses the distinction in the wiring.

---

## 6. Proportional Logic (The Grade)

Boolean: working or broken. 1 or 0.

Grade: **how much**?

```zplus
node card_health {
    in: thermal, timing, bandwidth, aging, errors
    out: grade : f32    // 0.0 to 1.0

    resolve: weighted(
        thermal:   0.3,    // 30% of the grade
        timing:    0.25,
        bandwidth: 0.25,
        aging:     0.1,
        errors:    0.1
    )
}
```

The grade isn't boolean. It's a continuous value. And everything downstream reacts proportionally:

```zplus
// Work assignment scales with grade
card -> route.weight(card.grade)

// A card at 0.8 gets 80% of the load a perfect card gets.
// Not "working" or "benched." A smooth spectrum of contribution.
// The fleet doesn't have outages. It has gradients.
```

In the old world, a server is either in the load balancer pool or out. Health check passes or fails. Here, every device contributes proportional to its actual measured capability at this specific moment. The fleet doesn't have cliff edges where a device falls out. It has a continuous surface where devices contribute more or less based on reality.

### Grade Propagation

Grades compose through the signal graph:

```zplus
// A chain is only as healthy as its weakest link
camera(grade: 0.95) -> detect(grade: 0.7) -> decide(grade: 0.99)

// The chain's effective grade: min(0.95, 0.7, 0.99) = 0.7
// OR: product(0.95, 0.7, 0.99) = 0.658
// Policy is configurable per chain.

// MDE sees the chain grades and routes accordingly:
// High-value inference → high-grade chains
// Bulk inference → any chain above minimum
// The fleet self-organizes by quality.
```

---

## 7. Resonance (Convergent Evidence)

Multiple independent signal chains arriving at the same conclusion:

```zplus
// Three independent ways to detect intrusion
network_anomaly  -> |
file_tampering   -> | resonance(3 of 3, within: 10s) | -> confirmed_breach
process_spawn    -> |

// Two is suspicious. Three is certain.
network_anomaly  -> |
file_tampering   -> | resonance(2 of 3, within: 10s) | -> probable_breach
process_spawn    -> |
```

Resonance is different from a chord. A chord waits for signals to arrive. Resonance asks whether **independent chains converge** on the same conclusion. It's the signal-graph equivalent of convergent evidence.

One anomaly is noise. Two anomalies that correlate in time are suspicious. Three independent anomalies within ten seconds of each other is a pattern that demands a response.

No rule engine. No correlation database. No SIEM with a 30-second aggregation window. Just wiring that says "if these three independent observations agree within this window, that means something."

---

## 8. Reflex and Deliberation (Two-Speed Logic)

Some decisions must be instant. Some should be thoughtful.

```zplus
// Reflex: thermal spike → cut power. No thinking. Now.
thermal -> gate(> 95C) -> kill_power
    priority: reflex    // Bypasses normal resolution. Hardware-fast.

// Deliberation: thermal trending up → consider migration
thermal -> failure_horizon(threshold: 85C) -> {
    gate(< 5m) -> gentle_migrate
    priority: deliberate    // Takes its time. Considers the fleet.
}
```

The `reflex` priority tells the OS this chain resolves at interrupt speed. No queueing. No scheduling. The signal hits the gate and the action fires within microseconds. Like a spinal reflex — the signal doesn't go to the brain.

The `deliberate` priority says this chain can take its time, aggregate information, and make a nuanced decision. Like the prefrontal cortex — slow, but wise.

Same language. Same wiring syntax. Different temporal urgency. The OS handles the scheduling difference.

```zplus
// A robot with reflexes AND deliberation
sonar -> gate(< 5cm) -> emergency_stop
    priority: reflex                        // Don't think. Stop.

sonar -> |
          | -> navigate -> motors
camera -> eyes -> |
    priority: deliberate                    // Think. Plan. Move wisely.
```

The robot stops instantly when something is 5cm away. But its navigation — the complex path planning with vision — takes the deliberate path. Both run simultaneously. The reflex overrides the deliberation when it fires. Just like biology.

---

## Summary: The New Logic

| Form | What It Does | Old World Equivalent |
|------|-------------|---------------------|
| **Chord** | Multiple signals resolve as one moment | Locks + mutexes + careful synchronization |
| **Knee** | Smooth transition instead of binary switch | Hysteresis hacks, PID controllers, smoothing |
| **Silence** | Absence of signal carries meaning | Timers + polling + timeout handlers |
| **Delta** | Reason about change, not state | Numerical differentiation + buffer management |
| **Confluence** | Temporal coincidence detection | Timestamp queues + state machines |
| **Grade** | Proportional contribution, not binary | Custom weighted scoring bolted onto health checks |
| **Resonance** | Independent chains converging = certainty | Correlation engines + rule databases |
| **Reflex/Deliberation** | Two-speed decision making | Interrupt handlers vs. main loop (never unified) |

These aren't features bolted onto a sequential language. They emerge naturally from a paradigm where:
- Everything resolves simultaneously
- Time is a first-class dimension
- Connections are the program
- The OS knows the hardware

**The old logic was shaped by the old machine.** `if/else` exists because the CPU has one instruction pointer that goes one way or the other. `while` exists because the CPU can jump backward. `try/catch` exists because errors need to unwind a call stack.

**The new logic is shaped by signal flow.** Chords exist because signals arrive together. Knees exist because real signals aren't binary. Silence exists because absence is data. Delta exists because the OS remembers. Confluence exists because coincidence in time means something.

The logic changed because the machine changed.

---

*The old world asks: what is true?*
*The new world asks: what is happening?*

*Those are different questions. They need different logic.*

**Codex Labs LLC — 2026**
