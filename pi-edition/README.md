# TRISA OS — Pi Edition

**Signal-chain programming for the next generation.**

A bare-metal TRISA OS image for Raspberry Pi that replaces procedural programming with signal graph wiring. Built for robotics classrooms, maker spaces, and anyone who thinks connecting a sensor to a motor shouldn't require 200 lines of boilerplate.

---

## Supported Hardware

| Board | Status |
|-------|--------|
| Raspberry Pi 5 | Primary target |
| Raspberry Pi 4B | Supported |
| Raspberry Pi Zero 2 W | Supported (lightweight signal graphs) |
| Raspberry Pi Pico / Pico 2 | Z+ bare-metal target (no OS layer) |

### First-Class Peripherals

Every peripheral is a node in the signal graph. No drivers. No libraries. No imports.

- All GPIO pins (digital in/out, PWM)
- I2C bus (sensors, displays, IMUs)
- SPI bus (high-speed sensors, LED strips)
- UART (serial devices, GPS, LoRa modules)
- CSI camera (Pi Camera, ArduCam)
- Audio (I2S, USB audio)
- Coral USB TPU (MDE inference node)
- LiteFury M.2 FPGA (TRISA delta preprocessing)
- LoRa modules (mesh networking, The Enigma)
- BLE (T-Encoder Pro, T-Watch, peripherals)

---

## Quick Start

```bash
# Flash TRISA OS to SD card
trisa flash pi5 --sd /dev/sdX

# Boot the Pi. Open the visual editor in a browser.
# Or SSH in and write Z+

# my-first-robot.zp
node button : gpio(17) -> pressed : bool
node led    : gpio(27) <- on : bool

button -> led

# Run it
trisa run my-first-robot.zp
```

Button pressed. LED on. Button released. LED off. One connection. No code beyond the relationship.

---

## Why This Matters For Kids

Programming today teaches kids to think like 1970s mainframes. Sequential. Polling. One thing at a time. The real world doesn't work that way.

A kid who learns Z+ on a Pi learns:

- **Systems thinking** — everything is connected, signals flow, relationships matter
- **Temporal reasoning** — things change, rate of change matters, history informs decisions
- **Graceful degradation** — when something breaks, the system adapts instead of crashing
- **Consequence awareness** — wire something wrong and the system tells you what will happen
- **Simultaneous resolution** — the world doesn't take turns

These aren't programming skills. They're **thinking skills.** They apply to biology, music, engineering, economics, ecology — every domain that involves interacting systems.

---

## Classroom Mode

Teachers get a dashboard. Students get sandboxed signal graphs. The AI assist dial goes from off to full.

```
AI OFF        → Student wires everything manually
AI HINTS      → Compiler suggests connections
AI GUIDED     → System shows possible next steps
AI FULL       → Student describes intent, system wires it
```

Same dial as PCB FORGE. Teachers control it. Students grow into autonomy.

**Z+ is free for students. Full version. No tiers. No feature gates. No "education edition" that's missing the good parts.**

---

## Visual Editor

Drag nodes. Draw connections. The Z+ code generates live.

Works in any browser. No install. Connects to the Pi over the local network.

The visual graph and the text code are always in sync. Edit either. The other updates. A kid who starts dragging blocks naturally transitions to typing Z+ because they can see the correspondence in real time.

---

**Codex Labs LLC — 2026**
