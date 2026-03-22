# Zeros & DereZ — Kids Development Platform

> Z-OS native development environment for ages 8–16.
> Game clones as entry point. Robotics as destination.
> Same signal chain architecture, kid-sized.
>
> **Date**: March 22, 2026
> **Status**: Research complete, not yet implemented

---

## The Names

**Zeros** — the community. The kids. The identity.
"I'm a Zero. I start from nothing. I build."

**DereZ** — the development environment. The tool.
Deconstruct. De-resolve. Take apart the thing to understand it.

Same letters, rearranged. Xamot and Tomax — nonlinear twins.
One breaks, one builds. One reads the signal, one writes the response.

**The Zeros** — the robotics team name. Two sides:
- **DereZ side**: the analysts, debuggers, the ones who figure out WHY it failed
- **Zeros side**: the builders, creators, the ones who make it work

Maps to TRISA's two-stage pipeline: Stage 1 detects (DereZ), Stage 2 acts (Zeros).

---

## The Pedagogy

Greatest American Hero: got the suit, lost the manual. Had to figure out what
everything did by crashing into things.

That's DereZ. Here's a working clone — the suit. No manual. Break it. Figure
out why it flies. **FAFO as a learning model.**

Kids don't learn by reading documentation. They learn by:
1. Seeing a working thing
2. Breaking it
3. Figuring out why it broke
4. Making it do something new

Every template is a working game clone. Every clone is breakable. Every break
is a lesson. Every lesson transfers to robotics.

---

## Game Clone Templates

### Legal Framework
- **Game mechanics cannot be copyrighted** (established case law)
- **Art, names, characters CAN be copyrighted/trademarked**
- Strategy: identical gameplay, original assets, different name
- Name templates by genre, never by reference game
- All assets original or CC0 licensed
- Built-in attribution system teaching kids about IP from day one

### Priority Clones (in shipping order)

| Template Name | Clone Of | Genre | Age | Teaches |
|---------------|----------|-------|-----|---------|
| **Block Builder** | Minecraft | Voxel sandbox | 8+ | 3D space, inventory, crafting loops |
| **Platform Run** | Mario / Celeste | Platformer | 8+ | Physics, collision, level design |
| **Crew Suspect** | Among Us | Social deduction | 10+ | Networking, state machines, UI |
| **Tower Line** | Bloons TD | Tower defense | 10+ | Pathfinding, spawning, economy |
| **Top Quest** | Zelda / Pokemon | Top-down RPG | 10+ | Dialogue, inventory, state, story |
| **Obby Rush** | Roblox Obby | Obstacle course | 8+ | 3D platforming, social sharing |
| **Drop Zone** | Fortnite | Battle royale | 13+ | Networking, physics, large-scale state |
| **Beat Dash** | Geometry Dash | Rhythm platformer | 10+ | Timing, audio sync, level editor |
| **Pixel Brawl** | Brawl Stars | Arena brawler | 10+ | Controls, abilities, matchmaking |
| **Pet World** | Adopt Me | Virtual pet/social | 8+ | Economy, social, persistence |

### What Ships With Each Template
- Complete, playable game (not a skeleton — a CLONE)
- 3–5 character sprites with walk/idle/jump animations
- 1 cohesive tile set (30–50 tiles)
- 2–3 background layers
- 10–15 sound effects
- 2–3 music loops
- Basic HUD kit
- 3–5 particle effects
- Heavily commented, readable source code
- **View Source on everything** — this was the web's original superpower

### Art Style
**Primary**: Pixel art (16x16 and 32x32)
- Kids can create and modify these themselves
- Constraints breed creativity
- Looks "intentionally retro" not "bad"
- Huge CC0 library available (Kenney.nl, OpenGameArt, itch.io)
- Forgiving of low skill

**Secondary**: Clean vector / flat design (Among Us style)
- Scalable, looks good at any resolution
- Harder for kids to create but excellent for templates
- Good for UI-heavy games (tycoons, card games)

**For 14+**: Low-poly 3D
- Appeals to kids who think 2D is "old"
- Higher creation barrier, only for advanced templates

---

## Progressive Complexity

### The Six Levels

**Level 0: RESKIN (Age 8+, Day 1)**
- Complete working clone
- Kid changes ONLY art assets — swap sprites, colors, sounds
- Zero code interaction
- Learning: file formats, asset pipeline, cause and effect
- Time to first modification: **under 2 minutes** (critical retention threshold)

**Level 1: TWEAK (Age 8+, Week 1)**
- Same complete clone
- Kid modifies exposed variables via simple UI or config file
- Speed, gravity, health, damage, colors, spawn rates
- Could use a physical knob interface (ties to LILYGO hardware)
- Learning: parameters affect behavior, numbers have meaning

**Level 2: EXTEND (Age 10+, Month 1)**
- Clone with clear extension points
- Kid adds new levels, enemies, items using guided tools
- Block-based scripting for behavior ("when enemy sees player, chase")
- Learning: events, conditions, sequences

**Level 3: SCRIPT (Age 11+, Month 2–3)**
- Text scripting exposed alongside blocks (bidirectional — can always switch back)
- Kid writes simple scripts: custom AI, new power-ups, modified physics
- Functions and variables introduced
- Learning: text syntax, debugging, logic

**Level 4: CREATE (Age 13+, Month 3–6)**
- Blank template with framework
- Entity system, physics, rendering provided
- Kid builds game logic from scratch
- Full text programming (Z+ or Python-like)
- Learning: architecture, systems thinking, abstraction

**Level 5: ENGINE (Age 14+, Month 6+)**
- Access to engine internals
- Custom shaders, physics mods, tool creation
- Version control, collaboration
- Learning: software engineering, optimization, teamwork

### Critical Transitions (Where Kids Quit)

**Blocks → Text**: 40–60% attrition (Weintrop & Wilensky, 2015, 2017).
Mitigation: bidirectional toggle (same program as blocks OR text, like MakeCode).

**Tutorial → Original**: The "blank canvas problem."
Mitigation: clones ARE the answer. You're never starting from blank.

**Solo → Collaborative**: No kids' tools handle version control well.
Mitigation: built-in collaboration that's simpler than git.

---

## The Robotics Bridge

### The Killer Feature Nobody Has
**Same code that makes an enemy chase you in a game makes a robot follow a line.**

If DereZ uses one API for game entities and hardware actuators:

```
# In a game template (DereZ):
if distance_to(player) < 100:
    move_toward(player, speed=5)

# Same pattern for a robot (DereZ):
if distance_sensor.read() < 100:
    motors.move_toward(target, speed=5)
```

The transfer is literal. Not metaphorical. Not conceptual. Same function calls.

### Concept Transfer Map

| Game Concept | Robot Equivalent | Which Template |
|-------------|-----------------|----------------|
| Game loop (update per frame) | Control loop (sense, decide, act) | All |
| Collision detection | Proximity / bumper sensors | Platformer, maze |
| Pathfinding (A*) | Robot navigation | Tower defense, RTS |
| State machines (idle/walk/attack) | Robot behavior states | RPG enemy AI |
| Physics (gravity, velocity) | Motor control, PID loops | Physics puzzler |
| Sprite animation (frame timing) | Servo sequencing, LED patterns | All animated |
| Health / damage | Sensor thresholds, fault detection | Combat games |
| Inventory management | Battery, payload planning | Survival/crafting |
| Multiplayer networking | Robot swarm communication | Any multiplayer |
| Procedural generation | Adaptive behavior, exploration | Roguelike, sandbox |
| Save/load state | Robot memory, persistent mapping | RPG, sandbox |

### Competition Format: The Zeros

A robotics team called The Zeros:
- **DereZ squad**: analyze the challenge, debug failures, find the signal
- **Zeros squad**: build the solution, write the code, make it work
- Walking into competition with $38 Goya cards and robots programmed by kids
  who learned by breaking game clones
- Running Z-OS. Because they built on it.

---

## Market Gaps We Fill

### The Post-Scratch Gap (Ages 11–14)
Biggest gap in kids' dev tools. Scratch is "for babies" but Unity/Unreal are
overwhelming. Godot is closest but has no kid onboarding. Roblox Studio fills
partially but is platform-locked.

**DereZ fills this exactly.**

### Offline-First
Scratch requires internet. Roblox requires internet. Most kids' tools are
cloud-dependent. Schools with bad WiFi, rural areas, developing countries — blocked.

**Z-OS is an OS. Everything is local. Structural advantage.**

### View Source Culture
No kids' platform ships readable, commented source code for working games.
The web taught a generation to code through View Source. We bring it back.

### Game-to-Hardware Bridge
No platform cleanly bridges screen games with physical hardware in one tool.
MakeCode comes closest but game and hardware are separate environments.

**DereZ is one environment. Same API. Same language. Screen or robot.**

### Underserved Kids

**Girls (45% of gamers, 20% of creators)**: Most templates are combat-focused.
Ship narrative, world-building, character design, and cooperative templates.
Top Quest (RPG), Pet World, and Block Builder directly address this.

**Neurodivergent kids**: Adjustable sensory settings in DereZ itself — sound
levels, animation speed, color modes. Clear structure with flexible execution.
Checkpoints everywhere. Support hyperfocus (don't interrupt long sessions).

**Non-English speakers**: Templates with minimal text, visual instructions.
Localization-ready architecture. Block-based scripting is language-independent.

### Integrated Audio Creation
Every kids' tool treats audio as an afterthought. Kids love making music
(GarageBand, BandLab adoption among teens). DereZ includes a built-in
tracker/step-sequencer for game music creation. Make the soundtrack, not just
the game.

---

## Technical Requirements

### Asset Formats
- **Sprites**: PNG with transparency, 2x for HiDPI (32x32 game → 64x64 source)
- **Tile maps**: Tiled (.tmx) — open standard, well-supported
- **Audio**: OGG Vorbis for music, WAV for SFX (low latency)
- **Fonts**: TTF/OTF, Open Font License only
- **Color palettes**: Ship curated palettes (Pico-8 16-color, Endesga 32)

### Framework Design
- Single API abstracting game entities and hardware devices
- Signal chain native — game objects ARE signal chain nodes
- Same `sense → decide → act` loop for everything
- Hot-reload on asset change (kid saves a sprite, game updates live)
- Built-in profiler visible at all levels (even Level 0 can see FPS)

---

## Design Principle

> DereZ and Zeros are not separate products. They are two observations of
> the same thing — like TRISA's two stages, like Xamot and Tomax. The tool
> and the person. The breaking and the building. One cannot exist without
> the other.
>
> A kid who has only built has no intuition. A kid who has only broken has
> no agency. DereZ gives them both.
