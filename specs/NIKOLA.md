# Nikola (Niko) — Production Spec

**Edit WITH Nikola.**

*A signal-graph editor that makes DaVinci Resolve's architecture obsolete on everything but color.*

*Codex Labs LLC — 2026*

---

## 1. What Resolve Actually Is

DaVinci Resolve is six applications duct-taped together inside one window:

| Page | What it does | Architecture |
|------|-------------|-------------|
| Media | Import, organize | File browser + database |
| Cut | Fast editing | Simplified timeline |
| Edit | Full editing | Track-based timeline |
| Fusion | VFX/compositing | Node graph |
| Color | Color grading | Node graph |
| Fairlight | Audio post | Mixing console |
| Deliver | Export | Render queue |

Six different paradigms. Six different mental models. Six different ways of thinking about the same footage. Students spend months learning to switch between them.

Resolve's color page is untouchable. Decades of engineering. The DaVinci name literally means color. Don't compete there. Compete everywhere else.

---

## 2. Where Resolve Bleeds

### Performance
Resolve runs on top of an OS that's eating 30-40% of your GPU cycles for desktop compositing, window management, and system overhead. The application itself is millions of lines of C++ with layers of abstraction between user intent and hardware execution. On a $2,000 machine, you get maybe $1,200 worth of editing performance.

Niko runs on Zeos. The OS is the signal graph. There is no application boundary. There is no abstraction tax. $2,000 of hardware = $2,000 of editing performance.

### AI Integration
Resolve bolts AI features onto existing workflows. Magic Mask is a button you press. Speed Warp is a setting you enable. DaVinci Neural Engine is a black box that sometimes does what you want.

Niko wires AI as nodes in the signal chain. Not a button. A wire. Scene detection isn't a feature — it's a node you connect. Audio cleanup isn't a menu option — it's a transform in the chain. You see it. You control it. You can replace it.

### Collaboration
Resolve's collaboration requires DaVinci Resolve Project Server, PostgreSQL database, specific network configuration, and the Studio license ($295). It's fragile. It's enterprise. Kids can't use it.

Niko's collaboration is structural. Two editors on the same project = two signal sources merging into the same graph. The same construct as the chat room. The same construct as the message queue. Collaboration isn't a feature. It's wiring.

### Timeline Model
Resolve's timeline is track-based. Borrowed from tape machines in the 1950s. Video on tracks. Audio on tracks. Effects applied per-track or per-clip. Want to do something nonlinear? Go to Fusion (different page, different paradigm, different mental model).

Niko has no tracks. The timeline is temporal access. Clips are signal sources that exist at time ranges. Effects are transforms in the signal chain. Everything is one paradigm. One page. One mental model.

### Audio
Fairlight is competent. But it's a mixing console simulation running inside a video editor running on top of an OS. Three layers of abstraction between the sound engineer and the audio hardware.

Niko IS a mixing console. The Z+ paradigm came from audio engineering. Signal chains, buses, sends, taps, knee compression — these aren't metaphors in Z+. They're the language itself. Audio in Niko is the most natural thing the system does.

### Export
Resolve has a Deliver page. You set your format, add it to the render queue, press start, and wait. Sometimes for hours.

Niko resolves continuously. The export format is just another output node at the end of the chain. Connect it and the signal flows. Real-time export when your hardware can sustain it. Background export when it can't — but the signal is always flowing, not queued.

### Learning Curve
Resolve's interface has over 2,000 buttons, sliders, dropdowns, and options across its six pages. The median time to basic proficiency is 3-6 months.

Niko is the same language a 12-year-old uses to wire a robot. Progressive disclosure. Level 0 is drag clips onto the timeline and they play. Level 5 is wiring custom inference nodes for real-time VFX. Same language. Same paradigm. The curtain lifts.

---

## 3. Niko's Architecture

There are no pages. There is one signal graph with different views.

### The Signal Chain

Every project is a signal graph:

```
// A simple edit — two clips with a transition

clip_a : media("interview.mov") -> frames
clip_b : media("broll.mov") -> frames

// timeline is temporal routing
timeline : {
    clip_a @ t(0s ~ 30s),
    transition(cross_dissolve, duration: 1s) @ t(29s ~ 31s),
    clip_b @ t(30s ~ 60s)
}

timeline -> display
timeline -> encode(h265) -> vault.store("export/final.mp4")
```

That's a two-clip edit with a transition and an export. In 10 lines.

### Views (not pages)

The signal graph is always the truth. Views are just different ways of looking at it.

| View | What it shows | Z+ equivalent |
|------|-------------|---------------|
| **Timeline** | Temporal arrangement of clips | `clip @ t(start ~ end)` |
| **Graph** | The full signal chain, visually | The playground — nodes and wires |
| **Mix** | Audio channels and buses | Signal merge and taps |
| **Canvas** | Visual preview and composition | Output node display |
| **Inspector** | Properties of selected node | Node metadata |

Switch between views instantly. They're all looking at the same graph. Edit in any view and the others update. No "go to Fusion for this effect." The graph is the graph.

---

## 4. Media

### Import

```
// import is discovery — same as hardware discovery
media.scan("/footage/shoot_day_1/") -> clips

// AI auto-tag on import
clips -> ai("scene-classify.zdx") -> tags
clips -> ai("face-detect.zdx") -> faces
clips -> ai("speech-to-text.zdx") -> transcripts

// everything stored with metadata
clips -> vault.store("project/media", {
    tags, faces, transcripts,
    technical: { codec, resolution, fps, duration, audio_tracks }
})
```

Import isn't a step. It's a signal chain. Footage arrives, AI processes it, metadata attaches, vault stores it. Continuously. Drop new files in the folder and they flow through the chain automatically.

### Organization

```
// smart bins are gates
clips -> gate(tags ~ "interview") -> bin("Interviews")
clips -> gate(tags ~ "broll")     -> bin("B-Roll")
clips -> gate(faces ~ "subject_a") -> bin("Subject A")

// search is a gate on the vault
query -> vault.search("project/media", text ~ query) -> results
```

Bins aren't folders. They're gates. A clip isn't "in" a bin — it passes through a gate that defines the bin. Change the gate expression, the bin changes. A clip can pass through multiple gates and appear in multiple bins simultaneously.

---

## 5. Editing

### The Timeline Is Temporal Access

```
// place clips in time
timeline : {
    clip_a @ t(0s ~ 15s),
    clip_b @ t(15s ~ 45s),
    clip_c @ t(45s ~ 90s)
}

// trim = adjust the temporal range
clip_b @ t(18s ~ 42s)    // trimmed 3s from head, 3s from tail

// slip = shift the source window without moving the clip
clip_b @ t(15s ~ 45s, source_offset: 5s)

// speed = temporal compression/expansion
clip_b @ t(15s ~ 45s, speed: 0.5)    // half speed, clip now takes 60s of source

// reverse
clip_b @ t(15s ~ 45s, speed: -1)
```

No tracks. Clips exist at time ranges. Overlapping clips merge (compositing). Non-overlapping clips sequence. The timeline is just temporal routing.

### Cuts and Transitions

```
// cut = clips abut temporally
clip_a @ t(0s ~ 15s)
clip_b @ t(15s ~ 45s)
// that's a cut. the signal from a ends, the signal from b begins.

// transition = temporal blend between two signals
clip_a -> |
           | transition(cross_dissolve, duration: 1s, position: t(14.5s ~ 15.5s)) |
clip_b -> |

// custom transition — just a Z+ function
transition(wipe_left) : {
    in: clip_a, clip_b
    out: blended
    resolve: mix(clip_a, clip_b, progress: t.normalized, mask: horizontal_gradient)
}
```

Transitions aren't presets you pick from a menu. They're merge nodes with a blending function. Build your own. Wire it. The concept of "transition packs" dissolves — anyone can write a transition in Z+.

### Multi-cam

```
// multi-cam is a merge with a selection gate
cam_a : media("cam_a.mov") -> frames
cam_b : media("cam_b.mov") -> frames
cam_c : media("cam_c.mov") -> frames

// sync by audio fingerprint
{cam_a, cam_b, cam_c} -> sync(method: audio_fingerprint) -> synced

// live switch — select which camera is active at each moment
synced -> switch(
    cam_a @ t(0s ~ 12s),
    cam_b @ t(12s ~ 25s),
    cam_a @ t(25s ~ 40s),
    cam_c @ t(40s ~ 60s)
) -> timeline

// or: AI auto-switch based on who's talking
synced -> ai("speaker-detect.zdx") -> active_speaker
active_speaker -> switch(cam: closest_to(speaker)) -> timeline
```

AI-driven multicam cut. Who's speaking? Switch to that camera. Not a plugin. A node. Two lines.

---

## 6. Effects / VFX

Effects aren't a separate page. They're transforms in the signal chain.

```
// blur
clip -> blur(radius: 10px, type: gaussian) -> output

// with a mask
clip -> {
    mask(shape: ellipse, feather: 20px) -> blur(radius: 15px),
    pass_through
} -> composite -> output

// motion tracking — AI node
clip -> ai("point-track.zdx", target: face) -> track_data
track_data -> mask.follow -> output

// stabilization
clip -> ai("stabilize.zdx", smoothness: 0.8) -> stabilized

// green screen
clip -> key(type: chroma, color: green, tolerance: 0.15, knee: 0.05) -> keyed
background -> |
               | composite(mode: over) | -> output
keyed      -> |
```

The chroma key has a **knee**. Of course it does. The edge between keyed and not-keyed isn't a hard cutoff. It's a smooth transition. Every other keyer bolts this on as "edge refinement." In Z+, the knee IS the edge refinement. It's the same construct that controls thermostats and rate limiters and PID loops.

### Generative

```
// AI-generated background extension
clip -> ai("outpaint.zdx", extend: { left: 200px, right: 200px }) -> extended

// style transfer
clip -> ai("style-transfer.zdx", reference: "oil_painting.jpg") -> styled

// object removal
clip -> ai("inpaint.zdx", mask: selected_object) -> clean

// upscale
clip_480p -> ai("upscale.zdx", target: 4k) -> clip_4k
```

Every AI feature is a node. Not a menu item. Not a plugin you buy. A node you wire. See it in the graph. Control the input. Observe the output with a tap. Replace the model with a better one when it exists.

---

## 7. Audio

This is where Niko is born to win. The mixing console metaphor isn't a metaphor. It's the language.

```
// audio tracks from timeline
dialogue  : timeline.audio(track: 1) -> audio
music     : media("soundtrack.wav") -> audio
sfx       : media("foley/") -> audio

// processing chain — same as a real mixing console
dialogue -> {
    eq(low_cut: 80Hz, presence: 3dB @ 3kHz),
    compressor(threshold: -18dB, ratio: 4:1, knee: 6dB, attack: 10ms, release: 100ms),
    de_esser(frequency: 6kHz, threshold: -20dB)
} -> dialogue_processed

music -> {
    eq(low_shelf: -3dB @ 200Hz),
    compressor(threshold: -12dB, ratio: 2:1, knee: 10dB),
    sidechain(trigger: dialogue, depth: 6dB, attack: 20ms, release: 200ms)
} -> music_processed

sfx -> {
    reverb(type: room, size: 0.4, decay: 1.2s, mix: 30%)
} -> sfx_processed

// mix bus
dialogue_processed -> | mix(level: 0dB)   |
music_processed    -> | mix(level: -6dB)  | -> master
sfx_processed      -> | mix(level: -3dB)  |

// master chain
master -> {
    eq(high_shelf: 1.5dB @ 10kHz),
    limiter(ceiling: -1dB, release: 50ms)
} -> master_out

master_out -> audio.output(device: default)    // monitoring
master_out -> encode(aac, bitrate: 320kbps)    // export

// metering — taps, not separate tools
dialogue_processed ~> meter(type: vu)
master_out         ~> meter(type: lufs)
master_out         ~> spectrum_analyzer
```

Read that. That's a complete audio post setup. Dialogue processing with EQ, compression, and de-essing. Music with sidechain ducking triggered by dialogue. SFX with room reverb. A mix bus. A master chain with EQ and limiting. Metering via taps.

The **compressor knee** is literally a Z+ knee. The **sidechain** is a signal from one chain gating another. The **mix bus** is a merge. The **tap** is metering without affecting the signal.

This isn't Z+ pretending to be audio. Audio IS Z+. The language was designed by someone who thinks in mixing consoles.

### AI Audio

```
// noise reduction — AI node, not a plugin
dialogue -> ai("denoise.zdx") -> clean_dialogue

// auto-ducking — AI detects when someone talks, ducks music automatically
dialogue -> ai("speech-detect.zdx") -> speech_signal
speech_signal -> music.sidechain(depth: 8dB, knee: 3dB)

// auto-mix — AI balances multiple speakers
{speaker_a, speaker_b, speaker_c} -> ai("auto-mix.zdx") -> balanced

// music generation
prompt("upbeat acoustic, 120bpm, 30 seconds") -> ai("music-gen.zdx") -> generated_track
```

---

## 8. Motion Graphics / Titles

```
// text is a signal source
title : text("CHAPTER ONE", font: "Geist Mono", size: 72, color: white)

// animate with temporal access
title -> animate({
    opacity: { t(0s): 0, t(0.5s): 1, t(4.5s): 1, t(5s): 0 },
    y_offset: { t(0s): 20, t(0.5s): 0 }
}) -> positioned(x: center, y: bottom_third)

// lower third template
lower_third(name, title) : {
    bar : rect(width: 400, height: 60, color: accent, opacity: 0.9),
    name_text : text(name, size: 24, color: white, weight: bold),
    title_text : text(title, size: 16, color: white, opacity: 0.7)
} -> animate(slide_in_left, duration: 0.5s)

// use it
lower_third("Brad Svenson", "Codex Labs") @ t(10s ~ 15s) -> timeline
```

Lower thirds, titles, motion graphics — they're Z+ programs. Not preset templates you tweak in a GUI. Programs you write, share, publish to the registry.

`zpm install lower-thirds-clean`

Someone else's title design, wired into your project as a node. Edit the parameters. The signal flows.

---

## 9. Export / Deliver

There is no Deliver page.

```
// export is an output node. connect it and the signal flows.
timeline -> encode(h265, bitrate: 50Mbps, resolution: 4k) -> vault.store("exports/final.mp4")

// multiple formats simultaneously — fork
timeline -> {
    encode(h265, bitrate: 50Mbps) -> vault.store("exports/master.mp4"),
    encode(h264, bitrate: 8Mbps, resolution: 1080p) -> vault.store("exports/web.mp4"),
    encode(prores_422, resolution: 4k) -> vault.store("exports/archive.mov"),
    encode(aac, bitrate: 320kbps) -> vault.store("exports/audio.m4a")
}

// upload directly
timeline -> encode(h264, bitrate: 8Mbps) -> upload(youtube, title: "My Video")
timeline -> encode(h264, bitrate: 12Mbps) -> upload(vimeo, title: "My Video")
```

Four formats, two uploads. As a fork. The signal flows to all of them simultaneously. No render queue. No "Add to Queue" button. No waiting.

If your hardware can sustain real-time encoding, the export happens as you play. If it can't, it buffers and completes asynchronously. But there's no separate "render" step. The graph resolves.

---

## 10. Collaboration

```
// two editors on the same project
editor_a : zauth("brad") -> edits
editor_b : zauth("fynn") -> edits

// edits merge into the same timeline
editor_a.edits -> |
                   | merge(conflict: last_write_wins) | -> timeline
editor_b.edits -> |

// or: lock-based — editors claim regions
editor_a -> claim(timeline @ t(0s ~ 60s))     // Brad owns the first minute
editor_b -> claim(timeline @ t(60s ~ 120s))   // Fynn owns the second minute

// live presence — see who's editing where
editor_a.cursor ~> editor_b.display(avatar: brad, color: cyan)
editor_b.cursor ~> editor_a.display(avatar: fynn, color: green)

// review — tap the timeline, add comments at timecodes
reviewer -> comment(at: t(45s), text: "tighten this cut") -> timeline.comments
timeline.comments ~> editors.notification
```

Two people editing. Live. No project server. No PostgreSQL. No $295 Studio license. The signal graph is inherently collaborative because merge is a first-class operator. Two signal sources into one merge point is how the language already works.

---

## 11. Hardware Awareness

```
// Niko knows your hardware because the OS does
// routing is automatic — but you can see it and override it

decode(h265)          @ gpu          // GPU handles decode
ai("denoise.zdx")    @ npu          // NPU handles AI inference
encode(h265)          @ gpu          // GPU handles encode
composite             @ gpu          // compositing on GPU
audio.process         @ cpu          // audio stays on CPU (latency)

// monitor hardware load in real time — taps
gpu.utilization   ~> meter
npu.utilization   ~> meter
cpu.utilization   ~> meter
memory.usage      ~> meter

// thermal awareness — the editor adapts to your cooling
gpu.thermal -> gate(> 80C, knee: 5C) -> {
    reduce(preview_resolution, proportional),
    reduce(playback_cache, proportional)
}
// your laptop gets hot? niko reduces preview quality smoothly
// before the GPU throttles. no dropped frames. no stuttering.
```

Resolve runs on an OS that doesn't know your GPU temperature. When it throttles, playback stutters. You notice.

Niko runs on an OS that feels the thermal state continuously. It degrades gracefully — reducing preview resolution with a knee — before the hardware complains. You never notice because it never stutters.

---

## 12. Progressive Disclosure (for Students)

### Level 0 — Reskin
Drag clips onto the timeline. They play. Change the order. Add music. Export.

### Level 1 — Tweak
Adjust transition duration. Change text. Modify audio levels with sliders. The sliders are gates with knees underneath, but the student sees sliders.

### Level 2 — Extend
Add effects from the registry. Wire an AI denoiser. Build a lower third from a template. See the signal graph for the first time.

### Level 3 — Script
Write Z+ to create custom effects, transitions, titles. Understand the signal chain. Wire nodes manually.

### Level 4 — Create
Build from scratch. Custom color pipeline (not DaVinci grade, but functional). Audio post chain. Export automation.

### Level 5 — Engine
Modify Niko's internals. Write custom codecs. Build new AI nodes. Publish to the registry.

A kid starts at Level 0 and makes a YouTube video on day one. A professional works at Level 3-4 and never hits a ceiling. Same tool. Same language. Same paradigm from first drag to final master.

---

## 13. What Niko Doesn't Do (Honestly)

| Feature | Niko's Position |
|---------|----------------|
| **DaVinci-grade color science** | No. Don't compete. Let Resolve be Resolve. Niko has functional color correction but doesn't pretend to be a colorist's tool. |
| **ACES / OCIO pipeline** | Import and apply, but not the deep color management Resolve offers. |
| **Film emulation LUTs** | Apply them, but don't build a LUT ecosystem. |
| **Broadcast monitoring** | Basic SDI output. Not broadcast-grade scopes. |
| **Tape ingest** | No. Tape is dead. |
| **EDL/XML import** | Yes — interop with other NLEs matters. |
| **AAF/OMF exchange** | Yes — interop with Pro Tools matters for audio post. |

Don't try to be everything. Be better at everything except color. Be honest about color. Professionals who need DaVinci-grade color will use DaVinci for color. Let them. They'll use Niko for everything else.

---

## 14. File Format

`.nko` — Nikola project file.

It's not a database. It's not a binary blob. It's a `.zp` file with media references. You can read it. You can version control it. You can diff it. You can merge it.

```bash
$ cat my-edit.nko
// Nikola project
// Auto-saved continuously to vault

source_a : media("footage/interview.mov") -> frames
source_b : media("footage/broll.mov") -> frames

timeline : {
    source_a @ t(0s ~ 30s),
    source_b @ t(30s ~ 60s)
}

timeline -> audio_chain -> master_out
timeline -> encode(h265) -> vault.store("exports/draft.mp4")
```

The project file IS the program. The program IS the edit. Open it in any text editor. Understand it. Version it with git. That's not possible with any other NLE.

---

## 15. Registry Integration

```
// install a transition pack
zpm install transitions-film

// install an audio processing chain
zpm install broadcast-audio-chain

// install a motion graphics template
zpm install lower-thirds-minimal

// install an AI model for noise reduction
zpm install denoise-pro.zdx

// use them immediately — they're just nodes
clip_a -> transitions-film.light_leak -> clip_b
dialogue -> broadcast-audio-chain -> master
lower-thirds-minimal("Name", "Title") @ t(10s) -> timeline
clip -> denoise-pro -> clean
```

Every effect, transition, title template, audio chain, and AI model is a registry package. Install it and it's a node. Share your creations. Sell your creations. The ecosystem builds itself.

---

## 16. Why This Kills (Except Color)

| Capability | DaVinci Resolve | Niko |
|-----------|----------------|------|
| Architecture | 6 separate paradigms | 1 signal graph |
| AI integration | Bolted-on buttons | Wired nodes |
| Collaboration | $295 + PostgreSQL server | Structural merge |
| Audio | Competent mixing console simulation | IS a mixing console |
| Performance | 30-40% lost to OS overhead | OS IS the editor |
| Learning curve | 3-6 months to basic | Day one to first video |
| VFX | Separate page (Fusion) | Same graph, same paradigm |
| Export | Render queue + wait | Output node, signal flows |
| Project file | Binary database | Readable Z+ program |
| Extensibility | OFX plugins ($$$) | Registry packages (free/paid) |
| Thermal management | Stutters when GPU throttles | Degrades smoothly with knee |
| Timeline model | 1950s tape tracks | Temporal signal routing |
| Custom effects | C++ plugin SDK | Z+ — same language as everything |
| Hardware routing | Manual GPU selection | Automatic, telemetry-aware |
| Price | Free / $295 Studio | Free (runs on Zeos) |

---

## 17. Bottom Line

Resolve is six applications that simulate signal flow on an OS that doesn't understand signal flow.

Niko is one signal graph on an OS that IS signal flow.

The color page is sacred. Don't touch it. Let Resolve keep the crown.

Take everything else.

---

*Edit WITH Nikola.*

**Codex Labs LLC — 2026**
