# Z+ — The TRISA Programming Language

**A language for wiring, not sequencing.**

*Codex Labs LLC — 2026*

---

## Why A New Language

Every programming language ever created describes **what to do, in what order.** Even "concurrent" languages just describe multiple sequences running in parallel. Even "reactive" frameworks are syntactic sugar on top of sequential event loops.

TRISA OS doesn't execute sequences. It resolves signal chains simultaneously. No existing language can express this naturally because every existing language assumes time flows in one direction through a list of instructions.

Z+ is a language where you describe **relationships between nodes.** The OS resolves them. You don't tell the machine what to do. You tell it what's connected to what, and how signals flow between them.

**Z+ is to TRISA OS what C was to Unix — the native tongue.**

---

## Design Principles

1. **The connection is the program.** `->` is the fundamental operator. Everything else serves it.
2. **Nodes, not functions.** Functions take input and return output once. Nodes exist continuously, emitting and receiving.
3. **Temporal by default.** Every node has access to t, t-1, t-2. History isn't an import. It's the language.
4. **No loops.** Signal chains resolve continuously. If you're writing a loop, you're thinking in the old world.
5. **No threads. No locks. No mutexes.** Concurrency is structural. Two signal chains running simultaneously aren't "parallel threads." They're two connections that both exist.
6. **Readable by a 12-year-old.** If a kid can't look at it and understand what's connected to what, we failed.
7. **The signal graph IS the program.** The visual representation and the code are interchangeable.

---

## Core Syntax

### Nodes

A node is anything that emits, receives, or transforms a signal. Nodes don't "run." They **exist.**

```zplus
// Declare a hardware node
node sonar : gpio(17) -> distance

// Declare a logic node
node wall_check {
    in: distance
    out: blocked : bool
    gate: distance < 20cm
}

// Declare an MDE node (AI module)
node eyes {
    in: frame
    out: label
    mde: "object-detect-v3.zdx"
}
```

### Connections

The program is wiring. `->` means "this signal flows there."

```zplus
// A robot that stops when it sees a wall
sonar -> wall_check -> motor

// That's the program.
// The sensor emits. The gate evaluates. The motor responds.
// Continuously. No loop. No polling. No sleep.
```

### Chains

Multiple nodes chain naturally:

```zplus
// Camera sees object, classifier identifies it, speaker announces it
camera -> eyes -> voice.speak

// Two sensors feeding one decision
sonar -> wall_check -> |
                        | -> navigate -> motors
lidar -> map_check  -> |
```

The `|` operator is a **merge point** — multiple signals resolve together as a chord before the chain continues.

### Temporal Access

Every node has implicit access to its own history:

```zplus
node speed_check {
    in: velocity
    out: acceleration

    // t is now, t-1 is previous, t-2 is before that
    // This is TRISA's native temporal window
    resolve: t - t-1
}
```

You don't import a history library. You don't manage a buffer. `t`, `t-1`, `t-2` are keywords. They exist because the OS thinks temporally.

### Delta — The Native Operator

The delta operator `Δ` (or `delta` for keyboard convenience) is first-class:

```zplus
// Rate of change of temperature
node warming {
    in: temp_sensor
    out: rate
    resolve: delta(temp_sensor)    // Equivalent to t - t-1
}

// Second derivative — acceleration of change
node alarm {
    in: rate
    out: urgent : bool
    resolve: delta(rate) > 5.0     // Change is accelerating
}

// Chain it naturally
temp_sensor -> warming -> alarm -> buzzer
```

### Gates

A gate passes or blocks a signal. It's the Z+ equivalent of `if` — but it's not branching. It's signal flow control.

```zplus
// Simple threshold gate
sonar -> gate(> 20cm) -> motor

// Named gate with hysteresis (the "soft knee")
node safe_distance {
    in: distance
    out: clear : bool
    gate: distance > 20cm
    knee: 2cm              // Soft transition zone — no hard cutoff
}
```

The `knee` parameter is TRISA's grit. Not sterile binary switching. Smooth analog-style transition. The signal doesn't snap on and off — it fades through a transition zone, just like a compressor.

### Forks

One signal feeds multiple paths simultaneously:

```zplus
// Camera feeds both display and AI classifier simultaneously
camera -> {
    display,
    eyes -> decision -> motors
}
```

No threads. No async. Both paths exist. Both resolve. The OS handles it because signal chains are structural, not scheduled.

### Taps

Observe a signal without altering the flow:

```zplus
// Monitor motor speed without affecting the control chain
controller -> motor
controller ~> dashboard    // ~ means tap (read-only observation)
```

Taps are how telemetry works in Z+. The MDE Flywheel — anonymous inference telemetry — is just a tap on every signal chain. Computation IS telemetry because every chain can be tapped.

---

## Type System

Z+ has a minimal, signal-oriented type system:

```zplus
// Primitive signal types
distance    : f32 @ cm | m | in | ft
temperature : f32 @ C | F | K
angle       : f32 @ deg | rad
speed       : f32 @ m/s | rpm | mph
voltage     : f32 @ V | mV
time        : f64 @ s | ms | us | ns
frame       : image @ rgb | gray | depth
audio       : stream @ pcm | delta
label       : string
trigger     : bool
raw         : bytes

// The @ operator declares units
// The compiler ENFORCES unit consistency
// You cannot connect a distance output to a temperature input
// You cannot accidentally mix cm and inches
```

Unit enforcement at the language level. The Mars Climate Orbiter crashed because of a unit mismatch. In Z+, that's a compile error.

---

## MDE Integration

AI modules are just nodes:

```zplus
// Load an MDE expert module
node detect {
    in: frame
    out: objects : list<label>
    mde: "yolo-v8-lite.zdx"
    device: goya(0)           // Run on first Goya card
}

// Hot-swap a module at runtime
detect.mde = "yolo-v9.zdx"   // MDE handles the swap. No restart.

// Module with MasQ provenance check
node secure_detect {
    in: frame
    out: objects : list<label>
    mde: "yolo-v8-lite.zdx"
    require_masq: true         // Reject modules with no history
    min_history: 30d           // Must have 30 days of provenance
}
```

### MDE Chains (Expert Routing)

```zplus
// Cascading experts — like the PNE tube compressor
// Each stage refines the signal
camera -> preprocess -> detect -> classify -> decide -> act

// With explicit device routing
camera -> preprocess @ fpga(0)
       -> detect     @ goya(0)
       -> classify   @ goya(1)
       -> decide     @ cpu
       -> act        @ gpio
```

The `@` operator pins a node to specific hardware. Without it, MDE routes automatically based on real-time capability (Zixel-informed). With it, you override.

---

## Zixel Access

The machine's proprioception is accessible as signal sources:

```zplus
// Read the chip's own state
node health {
    in: zixel.core(0).thermal
    out: warning : bool
    gate: thermal > 80C
}

// React to hardware degradation
zixel.core(2).aging -> gate(> threshold) -> migrate_workload(core: 2, to: 3)

// The machine heals itself — written in three lines of Z+
```

---

## MasQ Integration

Temporal wayfinding is native:

```zplus
// Recall a module's history
masq.recall("detect.zdx") -> history_view

// Navigate to a previous known-good state
masq.goto("detect.zdx", when: "2026-03-15T14:00:00")

// Compare two points in time
masq.diff("detect.zdx", from: "tuesday", to: "now") -> changes

// Dependency block inspection
masq.deps("detect.zdx") -> {
    current_deps -> display,
    historical_deps -> timeline_view
}
```

---

## Robotics Examples

### Line Follower (Traditional vs Z+)

**Python on Linux (the old way):**
```python
import RPi.GPIO as GPIO
import time

GPIO.setmode(GPIO.BCM)
GPIO.setup(14, GPIO.IN)   # left sensor
GPIO.setup(15, GPIO.IN)   # right sensor
GPIO.setup(18, GPIO.OUT)  # left motor
GPIO.setup(19, GPIO.OUT)  # right motor

left_pwm = GPIO.PWM(18, 100)
right_pwm = GPIO.PWM(19, 100)
left_pwm.start(0)
right_pwm.start(0)

try:
    while True:
        left = GPIO.input(14)
        right = GPIO.input(15)
        if left and right:
            left_pwm.ChangeDutyCycle(50)
            right_pwm.ChangeDutyCycle(50)
        elif left:
            left_pwm.ChangeDutyCycle(20)
            right_pwm.ChangeDutyCycle(50)
        elif right:
            left_pwm.ChangeDutyCycle(50)
            right_pwm.ChangeDutyCycle(20)
        else:
            left_pwm.ChangeDutyCycle(0)
            right_pwm.ChangeDutyCycle(0)
        time.sleep(0.01)
except KeyboardInterrupt:
    GPIO.cleanup()
```

**Z+ on TRISA OS:**
```zplus
node left_eye   : gpio(14) -> line : bool
node right_eye  : gpio(15) -> line : bool
node left_wheel : pwm(18) <- speed
node right_wheel: pwm(19) <- speed

node steer {
    in: left_eye, right_eye
    out: left_speed, right_speed

    resolve:
        both   -> 50, 50
        left   -> 20, 50
        right  -> 50, 20
        neither -> 0, 0
}

left_eye  -> |
              | -> steer -> left_wheel
right_eye -> |          -> right_wheel
```

Twelve lines. No imports. No setup. No cleanup. No loop. No sleep. No try/except. The robot follows the line because the signal chain says it does.

### Obstacle-Avoiding Robot with AI Vision

```zplus
// Hardware
node sonar    : gpio(17) -> distance
node camera   : csi(0)   -> frame
node left_m   : pwm(12)  <- speed
node right_m  : pwm(13)  <- speed
node speaker  : audio(0) <- voice

// AI
node eyes {
    in: frame
    out: objects : list<label>
    mde: "yolo-lite.zdx"
    device: coral(0)            // Run on Coral TPU
}

// Logic
node navigate {
    in: distance, objects
    out: left_speed, right_speed

    resolve:
        distance < 30cm           -> slow, slow
        distance < 15cm           -> 0, 0
        "person" in objects       -> 0, 0
        "cat" in objects          -> slow, turn_right   // don't scare the cat
        otherwise                 -> cruise, cruise
}

node announce {
    in: objects
    out: speech
    resolve: "I see " + objects.first
    throttle: 3s                 // Don't spam
}

// Wiring — this IS the program
sonar  -> |
           | -> navigate -> left_m
camera -> eyes -> |      -> right_m
               |
               -> announce -> speaker
```

A seeing, speaking, obstacle-avoiding robot. In 35 lines. A 12-year-old can read every line and understand what's connected to what.

### Swarm (Multiple Robots)

```zplus
// Each robot is a node on the mesh
mesh robot_swarm : lora(915MHz) {
    self -> broadcast(position, status)
    peers -> collect(positions)
}

node flock {
    in: self.position, peers.positions
    out: heading

    resolve:
        separation: avoid(peers, min: 30cm)
        alignment:  match(peers.heading)
        cohesion:   toward(peers.center)
}

robot_swarm.peers -> flock -> navigate -> motors
```

Swarm behavior. Boids algorithm. Expressed as signal wiring. Running on LoRa mesh. The Enigma provides the encrypted coordination channel.

---

## Visual Editor

Z+ is designed to be **bidirectional** — the text and the visual graph are the same thing. Edit either one. The other updates.

```
┌──────────┐    ┌───────┐    ┌──────────┐    ┌──────────┐
│  sonar   │───→│ gate  │───→│ navigate │───→│ left_m   │
│ gpio(17) │    │ >20cm │    │          │ ├─→│ right_m  │
└──────────┘    └───────┘    └──────────┘    └──────────┘
                                  ↑
┌──────────┐    ┌───────┐    ┌───┘
│  camera  │───→│ eyes  │───→┘
│  csi(0)  │    │ coral │
└──────────┘    └───────┘
```

Kids who can't type yet can drag and drop nodes and draw connections. The Z+ code generates automatically. Kids who can type see the code. Both are the same program. Always in sync.

---

## File Extension

`.zp`

```bash
$ ls my-robot/
main.zp
sensors.zp
behaviors.zp
masq/           # Temporal history lives here
```

---

## Compilation Targets

Z+ compiles to:

| Target | Use Case |
|--------|----------|
| TRISA Kernel native | Direct signal graph on TRISA OS |
| ARM bare-metal | Raspberry Pi, no OS overhead |
| FPGA bitstream | Direct hardware synthesis for signal chains |
| TRISA compat (Linux) | Development and testing on conventional OS |
| WebAssembly | Simulator in browser for classrooms |

The **WebAssembly target** means kids can learn Z+ in a browser before they ever touch a Pi. The simulator shows the signal graph resolving in real time. When they're ready, the same `.zp` file is intended to run on real hardware with zero translation — the toolchain targets it, but running Z+ on a physical board has not been demonstrated yet.

---

## Error Model

Z+ doesn't crash. Signal chains degrade.

```zplus
// A broken sensor doesn't crash the program
// The signal chain sees the node go silent
// The gate holds its last known state
// The robot stops gracefully

node sonar : gpio(17) -> distance
    on_silence: hold(t-1)         // Keep last value if sensor dies
    on_silence(> 500ms): emit(0)  // If dead too long, signal stop
```

No try/catch. No exceptions. No stack traces. A node goes quiet and the chain responds according to its declared behavior. Graceful degradation is structural, not defensive programming.

---

## FAFO At The Language Level

```zplus
// The Z+ compiler warns you about consequence chains
node heater : gpio(4) <- power

zixel.board.thermal -> gate(> 70C) -> heater.kill

// If you wire a heater with no thermal protection:
// COMPILER WARNING: node 'heater' has no zixel constraint
// "You sure about that?"
```

The language itself embodies FAFO. The compiler knows about consequences because the OS knows about consequences. Wire something dangerous without a safety chain and the compiler asks if you've thought about it.

Not a hard stop. Not a nanny. Just: "hey, you sure?"

---

## Pedagogy

### What Kids Learn With Python
- Sequential thinking
- Syntax memorization
- Exception handling
- Library imports
- Debugging print statements
- The concept that computers do one thing at a time

### What Kids Learn With Z+
- Systems thinking
- Signal flow
- Temporal relationships (change, rate of change, history)
- Graceful degradation
- Consequence chains (FAFO)
- The concept that everything happens simultaneously and relationships matter more than instructions

**The second list is how the actual world works.** Physics, biology, ecology, music, electronics — none of them are sequential. Z+ teaches kids to think in the shape of reality, not in the shape of a 1970s mainframe.

---

## Reserved Keywords

```
node, in, out, resolve, gate, knee, delta, merge, fork, tap,
t, t-1, t-2, mde, masq, zixel, vault,
on_silence, on_error, on_degrade,
hold, emit, kill, migrate,
mesh, broadcast, collect,
require_masq, min_history,
throttle, debounce,
device, gpio, pwm, csi, i2c, spi, uart,
goya, fpga, coral, cpu,
cm, m, in, ft, C, F, K, deg, rad, s, ms, us, ns,
rpm, mph, V, mV, Hz, kHz, MHz
```

Every keyword is either a signal concept, a hardware interface, or a unit of measurement. No `class`. No `function`. No `return`. No `while`. No `for`. No `try`. No `catch`. Those words belong to the old world.

---

## Summary

| Property | Z+ | Every Other Language |
|----------|-----|---------------------|
| Fundamental unit | Connection (`->`) | Instruction |
| Execution model | Simultaneous resolution | Sequential execution |
| Concurrency | Structural | Manual (threads/async) |
| Time | Native (`t, t-1, t-2`) | Library import |
| Change detection | Native (`delta`) | Manual diffing |
| Hardware awareness | Native (`zixel`) | Impossible |
| Provenance | Native (`masq`) | External tooling |
| Error handling | Graceful degradation | Exceptions |
| AI integration | Nodes (`mde`) | SDK imports |
| Visual/text parity | Bidirectional | Separate tools |
| Readable by kids | By design | By accident (sometimes) |

---

*Z+ is the native language of TRISA OS.*
*It speaks in connections because the OS thinks in signal chains.*
*It has history because the OS has memory.*
*It feels hardware because the OS has proprioception.*
*It degrades gracefully because the OS has consequences.*

**Codex Labs LLC — 2026**
