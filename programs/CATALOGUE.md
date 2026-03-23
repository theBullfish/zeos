# Z+ Program Catalogue

**Three libraries. One language. Same signal chains everywhere.**

*Codex Labs LLC — 2026*

---

## Zeros (Robotics) — 8 programs

Programs that start with hardware. Sensors, motors, physical things.

| # | Program | Level | File | What It Teaches |
|---|---------|-------|------|-----------------|
| 1 | **Blink** | 0 — Reskin | `zeros/blink.zp` | LED on/off, first hardware signal |
| 2 | **Light Chaser** | 1 — Tweak | `zeros/light_chaser.zp` | Photo-sensors, differential steering, moth vs cockroach |
| 3 | **Line Follower** | 1 — Tweak | `zeros/line_follower.zp` | Sensor → decision → motor loop, PID concept |
| 4 | **Sensor Dashboard** | 2 — Extend | `zeros/sensor_dashboard.zp` | Read any sensor, display everything, recording |
| 5 | **Sumo Bot** | 2 — Extend | `zeros/sumo_bot.zp` | State machines, priority, edge detection |
| 6 | **Maze Solver** | 3 — Script | `zeros/maze_solver.zp` | Wall following, pathfinding, map building |
| 7 | **Arm Controller** | 3 — Script | `zeros/arm_controller.zp` | Servo kinematics, record/playback, presets |
| 8 | **Swarm** | 4 — Create | `zeros/swarm.zp` | Multi-robot coordination, emergent behavior, three rules |
| 9 | **Autonomous Nav** | 5 — Engine | `zeros/autonomous_nav.zp` | LIDAR SLAM, A* pathfinding, PID, the capstone |

---

## DereZ (Dev) — 9 programs

Programs that start on screen. Code, signals, creative tools, games.

| # | Program | Level | File | What It Teaches |
|---|---------|-------|------|-----------------|
| 1 | **Hello Chain** | 0 — Reskin | `derez/hello_chain.zp` | First signal chain, data flow |
| 2 | **Signal Playground** | 1 — Tweak | `derez/signal_playground.zp` | Build chains live, experiment |
| 3 | **Pixel Art Editor** | 1 — Tweak | `derez/pixel_art.zp` | Input, grid, tools, export, Pico-8 palette |
| 4 | **Music Tracker** | 2 — Extend | `derez/music_tracker.zp` | Step sequencer, audio, timing, beat |
| 5 | **Chat Room** | 2 — Extend | `derez/chat_room.zp` | Networking, state, presence, commands |
| 6 | **Platform Run** | 2 — Extend | `derez/platform_run.zp` | Physics, collision, coyote time, animation |
| 7 | **Block Builder** | 2 — Extend | `derez/block_builder.zp` | Voxel world, crafting, day/night, first-person |
| 8 | **Tower Line** | 3 — Script | `derez/tower_line.zp` | Pathfinding, economy, targeting, splash/slow |
| 9 | **Crew Suspect** | 3 — Script | `derez/crew_suspect.zp` | Networking, state machines, voting, vision |
| 10 | **Bot Trainer** | 3 — Script | `derez/bot_trainer.zp` | Neural network from scratch, backprop, ML concepts |
| 11 | **Dashboard Builder** | 3 — Script | `derez/dashboard_builder.zp` | Data viz, gauges, graphs, alerts, heatmaps |

---

## Multimodal (The Bridge) — 8 programs

**Same codebase, two targets.** Game code IS robot code.

| # | Program | Level | File | What It Teaches |
|---|---------|-------|------|-----------------|
| 1 | **Shadow Bot** | 2 | `multimodal/shadow_bot.zp` | One brain, screen or robot — switch with one line |
| 2 | **Sound Reactive** | 2 | `multimodal/sound_reactive.zp` | Audio FFT → screen visuals AND LED strips/servos |
| 3 | **Weather World** | 2 | `multimodal/weather_world.zp` | Real sensors drive a game world (snow, rain, night) |
| 4 | **Mission Control** | 3 | `multimodal/mission_control.zp` | DereZ builds dashboard, signal contract |
| 5 | **Rover** | 3 | `multimodal/rover.zp` | The robot half — broadcast + commands |
| 6 | **Digital Twin** | 3 | `multimodal/digital_twin.zp` | Physical robot + live virtual mirror |
| 7 | **Gesture Controller** | 3 | `multimodal/gesture_controller.zp` | Accelerometer glove → game OR robot |
| 8 | **Battle Arena** | 4 | `multimodal/battle_arena.zp` | Robot combat + live broadcast + instant replay |

---

## Competition — 2 programs

Tournament infrastructure for robotics events.

| # | Program | File | What It Does |
|---|---------|------|-------------|
| 1 | **FIRST Challenge** | `competition/first_challenge.zp` | Full match: autonomous + teleop + endgame |
| 2 | **Zeros Tournament** | `competition/zeros_tournament.zp` | Bracket, scheduling, scoring, leaderboard, replay |

---

## System Programs (Full Z+) — 8 programs

Language stress tests. Prove Z+ works at scale.

| Program | File | Conventional Equivalent |
|---------|------|------------------------|
| File Watcher | `01_file_watcher.zp` | ~500 LOC Python |
| Log Monitor | `02_log_monitor.zp` | ~400 LOC Python |
| HTTP Server | `03_http_server.zp` | ~800 LOC Python |
| Key-Value Store | `04_key_value_store.zp` | ~600 LOC Python |
| Firewall | `05_firewall.zp` | ~1000 LOC C |
| Chirp (Twitter) | `chirp.zp` | Thousands of microservices |
| Goya Fleet | `goya_fleet.zp` | Custom tooling |
| Goya Fleet T3 | `goya_fleet_t3.zp` | Tier 3 variant |

---

## Totals

| Category | Programs | Levels Covered |
|----------|----------|----------------|
| Zeros (Robotics) | 9 | L0 through L5 |
| DereZ (Dev) | 11 | L0 through L3 |
| Multimodal (Bridge) | 8 | L2 through L4 |
| Competition | 2 | — |
| System | 8 | — |
| **Total** | **38** | **Full curriculum** |

---

## Curriculum Path

### Zeros Track (Robotics)
```
Blink → Light Chaser → Line Follower → Sensor Dashboard
  → Sumo Bot → Maze Solver → Arm Controller → Swarm → Autonomous Nav
```

### DereZ Track (Dev)
```
Hello Chain → Signal Playground → Pixel Art → Music Tracker
  → Chat Room → Platform Run → Block Builder → Tower Line
  → Crew Suspect → Bot Trainer → Dashboard Builder
```

### Bridge (both squads)
```
Shadow Bot → Sound Reactive → Weather World → Mission Control + Rover
  → Digital Twin → Gesture Controller → Battle Arena
```

### Competition Ready
```
FIRST Challenge (per-robot) + Zeros Tournament (event management)
```

---

## Design Rules

1. **Under 2 minutes to first modification** — retention threshold
2. **Every template ships working** — not a skeleton, a complete thing
3. **"TRY THIS" section in every file** — 5-8 experiments, progressive
4. **Comments explain WHY, not WHAT** — respect the reader
5. **Same API for screen and hardware** — `sense()` and `act()` everywhere
6. **Telemetry built in** — every template has `~>` dashboard lines
7. **Save/load via VAULT** — temporal state, not manual file management
8. **Every game concept maps to a robot concept** — documented in each file
