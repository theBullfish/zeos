# Display Architecture

**There is no windowing system. The signal graph IS the compositor.**

*Codex Labs LLC — 2026*

---

## The Problem With Displays

Every operating system treats the display as a special case. X11. Wayland. Quartz. DirectComposition. Compositor daemons. Window managers. Surface allocators. Buffer flipping protocols. Frame schedulers that fight the kernel scheduler. GPU drivers that bypass the OS entirely.

All of this exists because traditional operating systems have no concept of signal flow. So they invented an entire parallel universe — the "graphics stack" — with its own memory management, its own scheduling, its own IPC, and its own security model. A second operating system, running on top of the first, just to put pixels on glass.

Zeos doesn't have this problem. Zeos already thinks in signal chains. A display is a node. Pixels are signals. Compositing is merging. There's no special case because **display was never special — it was just another output the old world couldn't express.**

---

## Core Principle

A display is a signal endpoint. Like a motor. Like a speaker. Like a GPIO pin.

```zplus
// This is a complete graphical program:
camera -> eyes -> label_overlay -> display
```

The camera emits frames. The AI classifies them. The overlay composites text onto the frame. The display renders it. No window. No compositor. No surface allocator. No swap chain. Signal in, photons out.

---

## How It Works

### Display As Node

Every discovered display device becomes a signal graph node at boot:

```
I have:
  Display: HDMI-0 (3840x2160, 60Hz, HDR10)
  Display: OLED-I2C (128x64, I2C @ 0x3C)
  Display: DSI-0 (800x480, touch, 5-point)
```

Each display publishes a signal contract:

```zplus
contract hdmi_display {
    interface: hdmi(2.1)
    accepts: frame @ rgb | yuv | hdr10
    resolution: 3840x2160
    refresh: measured_at_boot      // Not 60Hz. Whatever THIS panel actually does.
    latency: measured               // Pixel response, not spec sheet
    touch: none
    audio: arc                      // HDMI ARC passthrough as signal
}

contract touch_panel {
    interface: dsi(0)
    accepts: frame @ rgb
    resolution: 800x480
    refresh: 60Hz
    latency: measured
    touch: 5-point
    emits: touch_event @ point[]    // Touch is a signal SOURCE
}
```

The display is bidirectional. It accepts visual signals AND emits interaction signals. One contract. One node. Input and output unified.

### Compositing Is Merging

Traditional compositors maintain a tree of surfaces, sort them by Z-order, clip, blend, and present. A complex state machine that runs every frame.

Zeos compositing is signal merging. The same `|` operator that merges sensor data merges visual layers:

```zplus
// Three visual signal chains merge at the display
camera_feed      -> |
status_overlay   -> | -> display
alert_panel      -> |
```

The merge node resolves all inputs simultaneously — just like every other merge in the signal graph. Layer order is declared in the merge, not managed by a compositor daemon.

```zplus
// Explicit layer ordering with blend modes
node compose {
    in: background, ui_layer, alert_layer
    out: frame

    layers:
        background   @ z(0)
        ui_layer     @ z(1), blend(alpha)
        alert_layer  @ z(2), blend(alpha), anchor(top_right)
}

compose -> display
```

No compositor process. No surface allocator. No shared-memory buffer handoff protocol. The signal graph resolves the composition as part of its normal operation. Compositing is just another signal chain.

### Interaction Is Bidirectional Signal Flow

Touch, mouse, keyboard, stylus — they're all input nodes that emit signals. Traditional OSes route input through a separate event system (evdev, HID, input subsystem) that runs parallel to the display system. Then window managers do hit testing to figure out which surface should receive the event.

In Zeos, input and output are the same signal graph:

```zplus
// Touch panel emits touch signals, display accepts frame signals
// Same device. Same signal contract. Same node.
touch_panel.touch -> ui_logic -> compose -> touch_panel.display

// Keyboard is a signal source
keyboard -> text_input -> editor_state -> compose -> display

// Mouse is a signal source with delta position
mouse -> cursor_position -> {
    hit_test -> focused_widget -> widget_update -> compose,
    cursor_overlay -> compose
}
```

Hit testing isn't a window manager's job. It's a gate in the signal chain. The focused widget receives the signal because the signal chain routes there. Focus, hover, click — they're all signal routing decisions, not event dispatch.

---

## Visual Surfaces

Traditional GUIs have windows. Zeos has **surfaces** — rectangular regions of a display that accept visual signals.

```zplus
// Declare a surface — a region of the display that accepts visual signals
surface main_panel {
    display: hdmi(0)
    region: 0, 0, 1920, 1080       // Or: region: full
    accepts: frame @ rgb
}

surface sidebar {
    display: hdmi(0)
    region: 1920, 0, 640, 1080
    accepts: frame @ rgb
}

// Wire signals to surfaces
data_viz -> main_panel
nav_tree -> sidebar
```

Surfaces don't overlap by default. They tile. If you want overlapping surfaces (popups, modals, alerts), you use the merge/compose pattern:

```zplus
// Modal dialog overlays the main content
node modal_compose {
    in: main_content, dialog
    out: frame

    layers:
        main_content @ z(0)
        dialog       @ z(1), blend(alpha), anchor(center), size(400, 300)
}

modal_compose -> display
```

No window decoration. No title bars (unless you wire them). No minimize/maximize/close buttons imposed by the OS. The visual surface is exactly what your signal chain produces. Nothing more.

### Layout Is Signal-Driven

Layout isn't a CSS engine or a constraint solver running in a separate thread. Layout is a signal chain that transforms spatial relationships:

```zplus
// Responsive layout — the display's resolution is a signal
display.resolution -> layout_engine -> {
    main_region   -> data_viz,
    sidebar_region -> nav_tree,
    header_region  -> status_bar
}

// When the display resolution changes (monitor swap, orientation change),
// the layout signal chain re-resolves automatically.
// No resize event handler. No layout invalidation. No reflow.
// The signal changed. The chain resolves.
```

---

## Rendering

### Signal-Native Rendering

Zeos doesn't have a "rendering API" in the OpenGL/Vulkan sense. Rendering is producing frames — visual signals — from data signals.

```zplus
// A data visualization node
node chart {
    in: sensor_data : list<f32>
    out: frame

    render:
        type: line_chart
        x_axis: time
        y_axis: sensor_data
        style: { stroke: "#4FC3F7", fill: gradient(down, "#4FC3F7", transparent) }
}

sensor -> chart -> display
```

The `render` block is declarative. You describe what the visual output looks like. The OS routes the actual rendering to whatever hardware can produce it fastest — GPU, CPU, FPGA. MDE makes that decision using Zixel measurements, not a hardcoded graphics driver.

### GPU As Signal Graph Node

GPUs are nodes. Not special. Not privileged. Not running a separate driver universe.

```zplus
// GPU signal contract
contract gpu_renderer {
    interface: pcie(gen4, x16)
    accepts: render_command, vertex_buffer, texture, shader
    emits: frame
    memory: 8GB VRAM
    throughput: measured_at_boot
    temporal_resolution: per_frame
}

// Route rendering to GPU explicitly
chart.render @ gpu(0) -> display

// Or let MDE decide
chart.render -> display    // MDE routes to fastest available renderer
```

The GPU doesn't own the display pipeline. It's one possible renderer. An FPGA could render. The CPU could render. A Coral TPU could accelerate specific visual transformations. The signal graph routes to capability, not to a predetermined driver stack.

### FPGA-Accelerated Visuals

This is where Zeos gets interesting for real-time visualization:

```zplus
// FPGA does wire-speed delta processing on the visual stream
// Only changed pixels flow through the pipeline
camera -> delta_compress @ fpga(0) -> process @ goya(0) -> delta_expand @ fpga(0) -> display

// The FPGA is doing what modern display protocols try to do
// (DSC, partial updates) — but at the signal graph level,
// with measured timing, automatically.
```

---

## The Visual Editor IS The Reference App

Z+'s bidirectional visual editor — where kids drag nodes and draw connections — is itself a signal graph application:

```zplus
// The visual editor, expressed as Z+
discovered_nodes    -> |
                       | -> editor_canvas -> compose -> display
user_connections    -> |

touch_panel.touch -> gesture_recognizer -> {
    drag    -> move_node     -> editor_canvas,
    connect -> draw_wire     -> editor_canvas,
    tap     -> select_node   -> property_panel -> compose
}

editor_canvas ~> code_generator -> text_editor -> compose
text_editor   ~> code_parser    -> editor_canvas

// Bidirectional. Always in sync.
// Edit the graph, the code updates.
// Edit the code, the graph updates.
// Both are taps (~>) — observation without disruption.
```

The visual editor proves the display architecture. If you can build a full IDE as a signal graph, you can build anything.

---

## Multi-Display

Multiple displays are multiple nodes. No "extended desktop" mode. No "mirrored" mode. Signal routing.

```zplus
// Different data to different displays
data_viz       -> hdmi(0)
control_panel  -> dsi(0)         // Touch panel on the robot
debug_stream   -> oled(0x3C)     // Tiny OLED shows status

// Same signal to multiple displays (mirror)
camera_feed -> {
    hdmi(0),
    dsi(0)
}

// One display's interaction drives another display's content
dsi(0).touch -> control_logic -> data_viz -> hdmi(0)
// Touch the small screen, see the result on the big screen.
```

No display manager. No xrandr. No "detect monitors" utility. Zixel discovered the displays. The signal graph wires them. You decide what goes where.

---

## Dashboard Mode

For headless servers, AI workstations, and monitoring rigs — the common case for Zeos deployments — the display story is a web dashboard served as a signal tap:

```zplus
// Any signal chain can be tapped to a web dashboard
mde.routing      ~> web.dashboard("/mde")
zixel.thermals   ~> web.dashboard("/thermals")
vault.throughput ~> web.dashboard("/storage")

// The web dashboard is just a tap on the signal graph.
// It doesn't poll. It doesn't query. It observes.
// Real-time, zero-overhead, because taps are structural.
```

The dashboard doesn't run a web framework. It exposes signal taps over WebSocket. The browser renders. The server just streams what it already knows.

```zplus
// Custom monitoring dashboard — a Z+ program
node ops_dashboard {
    in: mde.routing, zixel.thermals, vault.io, masq.timeline
    out: frame

    render:
        layout: grid(2x2)
        cells: [
            { type: graph,    data: zixel.thermals,  title: "Silicon Temperature" },
            { type: sankey,   data: mde.routing,     title: "Workload Distribution" },
            { type: timeline, data: masq.timeline,   title: "Temporal Navigation" },
            { type: meters,   data: vault.io,        title: "Storage Throughput" }
        ]
        style: dark
        refresh: continuous    // Not polling. Signal-driven.
}

ops_dashboard -> hdmi(0)
// Or
ops_dashboard ~> web.dashboard("/ops")
```

---

## What This Replaces

| Old World | Zeos |
|-----------|------|
| X11 / Wayland / Quartz | Signal graph compositing |
| Window manager | Surface declarations + merge nodes |
| Compositor daemon | Merge operator (`\|`) |
| Display server (separate process) | Display is a node in the signal graph |
| Input event system (evdev, HID) | Input nodes emit signals directly |
| Hit testing / focus management | Gates in the signal chain |
| Graphics driver (OpenGL/Vulkan/Metal) | GPU is a signal graph node, MDE routes rendering |
| Frame buffer management | VAULT handles frame storage, CFA addresses it |
| VSync / frame pacing | Zixel measures actual panel timing, signal chain syncs to measured rate |
| xrandr / display configuration | Zixel discovery + signal routing |
| CSS layout engine | Layout signal chain |
| React / Flutter / Qt / GTK | Z+ render blocks + signal composition |

---

## For The Thea Pitch

The question isn't "what's the Zeos GUI framework." The question is **why does every OS need a separate GUI framework at all?**

Answer: because they don't think in signals.

X11 was invented in 1984 because Unix had no concept of visual output as a data flow. Wayland was invented in 2008 because X11's architecture was unrecoverable. Both are patches on the fundamental problem: **traditional operating systems can't express "this data should become pixels" as a first-class operation.**

Zeos can. A display is a node. A pixel is a signal. Compositing is merging. Interaction is bidirectional flow. There is no "graphics stack" because graphics aren't special — they're signals, like everything else.

This means:
- **Zero-copy display pipeline.** Signals flow through CFA-addressed memory. No buffer copies between compositor, application, and GPU.
- **Measured frame timing.** Zixel reads the actual panel refresh rate and pixel response time. Not the spec sheet. The signal chain paces to measured reality.
- **Hardware-agnostic rendering.** MDE routes render commands to GPU, CPU, FPGA, or TPU based on what's fastest for this specific frame on this specific hardware right now.
- **Display scales from OLED to data center.** Same architecture for a 128x64 I2C OLED on a Pi as for an 8K HDR panel on a workstation. The signal contract is different. The architecture is identical.
- **Temporal visual state.** MasQ applies to display state. You can recall what the screen showed at any point in time. Not a screenshot — the full signal graph state that produced that frame.
- **Kids drag nodes. Engineers write Z+. Same program.** The visual editor is itself proof that the display architecture works.

No layers. No daemons. No "graphics stack." No second operating system for putting things on screen.

**The signal graph was always a visual medium. We just let it reach the glass.**

---

*Display is not special. Display is the signal graph, rendered.*

**Codex Labs LLC — 2026**
