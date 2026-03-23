# zeos-os.com — Website Copy

> For the Vercel site. Fractal database front and center.
> March 23, 2026

---

## Hero Section

### Headline
**The first operating system that is also a database.**

### Subhead
Zeos doesn't run on top of a filesystem. It doesn't connect to a database.
Zeos IS the database. Every file, process, sensor reading, and screen pixel
lives in one fractal signal graph — addressed by CFA, persisted by VAULT,
queried by gates, connected by wires.

### CTA
[Download the Kernel] [Read the Architecture] [Try in Browser (WASM)]

---

## The Pitch (3 columns)

### One Architecture
Signal chains. From kernel boot to web browser to robot controller.
The same `->` operator that wires a sensor to a motor also wires
a spreadsheet cell to a chart and a CSS rule to a DOM node.

### One Database
Every cell in the spreadsheet. Every node in the DOM. Every sensor
reading from a robot. Every pixel on screen. One fractal graph.
CFA addresses it. VAULT persists it. Gates query it. MasQ controls
who sees what.

### One Language
Z+ describes what's connected to what. Not what to do in what order.
A 12-year-old can read it. A kernel runs it. Same `.zp` file on
screen or on hardware.

---

## The Fractal Database

### What makes it fractal?
Self-similar at every scale.

| Zoom | What You See |
|------|-------------|
| Byte | CFA-derived address (fractal key) |
| Value | Signal node (data + computation) |
| Chain | Graph of nodes (table with live relationships) |
| Program | Collection of chains (schema) |
| Application | Database with a UI (spreadsheet, browser, game) |
| OS | Database of databases |
| Fleet | Distributed fractal database |

Same structure. Every level. Zoom in, it looks the same.

### Database operations you already know

| SQL | Z+ | What it does |
|-----|-----|-------------|
| INSERT | `emit(42)` | Create a datum |
| SELECT WHERE | `gate(> 50)` | Filter signals |
| JOIN | `{a, b} -> fuse` | Merge signals |
| UPDATE | `source.set(43)` | Change propagates automatically |
| VIEW | `data ~> dashboard` | Read-only observation |
| TRIGGER | `a -> b` | Every wire is a trigger |
| TRANSACTION | `sig_resolve(chain)` | Atomic chain resolution |
| TEMPORAL QUERY | `vault.read(path, at: t-1)` | Read any point in time |

### Why this matters
Every database company sells: "We are the source of truth for your data."
Zeos is the source of truth for ALL data — because the OS IS the database.
No integration layer. No ETL pipeline. No data warehouse. One graph.

---

## For Students (Zeros & DereZ)

### Two squads. One team.

**Zeros** — Robotics. Hardware. Sensors. Motors. Build it with your hands.
**DereZ** — Code. AI. Debug. Signals. Build it with your brain.

Both on the same robotics team. Both using the same OS. Both writing
the same language. The game code IS the robot code.

### 38 premade projects
From "blink an LED" to "autonomous SLAM navigation."
From "hello signal chain" to "build a neural network."
Every project has a "TRY THIS" section. Every concept transfers
from screen to hardware.

### The curriculum

```
Zeros: Blink → Light Chaser → Line Follower → Sumo Bot
         → Maze Solver → Arm Controller → Swarm → Autonomous Nav

DereZ: Hello Chain → Signal Playground → Pixel Art → Music Tracker
         → Platform Run → Block Builder → Tower Line → Bot Trainer

Bridge: Shadow Bot → Sound Reactive → Digital Twin → Battle Arena
```

### Full capability behind the curtain
Not dumbed down. Not less capable. Type `raise` and you see everything.
The curtain is the student's choice to lift.

---

## The Kernel

Zeos boots from UEFI on any x86_64 machine. $120 surplus Dell OptiPlex.
Your laptop. A server.

**What's running today:**
- UEFI bootstrap + framebuffer console
- Physical memory manager (bitmap page frame allocator)
- Virtual memory manager
- Heap allocator
- IDT + keyboard IRQ handler
- PCIe bus enumeration
- Signal chain engine with TSC timing
- Z+ interpreter (emit, math, gate, fork, tap, delta)
- VAULT filesystem (temporal, versioned, append-only)
- Framebuffer drawing (rect, line, circle, blit, blend, text)
- Signal chain visualizer
- Shell with persona system (Zeros/DereZ/Full)

**Built with:**
- C (freestanding, no libc)
- GNU-EFI
- QEMU + OVMF for testing
- Zero external dependencies

---

## Applications

### Surf — Web Browser
`url -> fetch -> parse -> style -> layout -> paint`
Five signal chains. Not a monolith. Each stage is independent,
tappable, replaceable, timed.

### Quill — Office Suite
- **Write**: Word processor with block editing and markdown shortcuts
- **Calc**: Spreadsheet where every cell IS a signal node and every formula IS a wire
- **Present**: Presentations with live embedded signal graphs and charts
- **Draw**: Vector drawing with layers and export

### Forge — IDE
Code editor with live signal graph panel. Edit the code, graph updates.
Click the graph, cursor jumps. Debugger traces signal flow.
Live robot sensor values appear as ghost text while you edit.

### Shield — Security
Security is not a feature. It's the topology. No wire = no access.
Not "denied" — nonexistent. 10 layers, 300 lines of Z+.

---

## Hardware

**Primary target:** x86_64 (any UEFI machine)
**Reference machine:** Minisforum BD395i MAX — Ryzen AI MAX+ 395, 128GB unified
**Accelerator:** Habana Goya HL-1000 ($38/card, 20-card fleet)
**FPGA:** Xilinx K480T (TRISA-BOX), XC7A100T (LiteFury)
**Endgame:** RISC-V custom silicon

---

## Open Source

GitHub: [theBullfish/zeos](https://github.com/theBullfish/zeos)

The kernel, the language, the programs, the specs — all public.
Built by Codex Labs LLC.

---

## Footer

*The first operating system with proprioception.*
*Signal chains, not processes. CFA addressing, not flat memory.*
*TRISA decides. The machine feels.*

Codex Labs LLC — 2026
