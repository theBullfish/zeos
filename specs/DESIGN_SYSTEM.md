# Z-OS Design System — Signal-Native Interface Language

> Extracted from macOS Human Interface Guidelines, translated to Z-OS primitives.
> Every concept Apple achieves through a massive toolkit layered on a kernel that
> doesn't understand UI, Z-OS achieves structurally — because the execution model
> IS the interface.

---

## 0. THE CORE DIFFERENCE

macOS: UI toolkit (AppKit/SwiftUI) → framework layer → kernel (XNU). The kernel
has no opinion about what a button is. Three separate systems pretending to be one.

Z-OS: Signal chains ARE the UI. A button is a signal chain. A drag is a signal
chain. A VAULT read is a signal chain. There is no gap between "the system" and
"the interface." The motion physics, the data flow, and the visual rendering are
the same graph resolving.

This means every macOS concept that requires explicit implementation in their
toolkit is either free or trivially derivable from Z-OS's execution model.

---

## 1. MOTION & PHYSICS

### What macOS Does
- **Spring-based animation everywhere.** Not keyframe, not ease-in-out — spring
  physics. Everything has mass, stiffness, and damping. A window opening, a
  notification sliding, a button pressing, a scroll bouncing — same physics,
  different parameters.
- **Apple's spring model:** `CASpringAnimation` defaults:
  - mass: 1.0
  - stiffness: 100.0
  - damping: 10.0
  - initialVelocity: 0.0
  - Resulting natural frequency: ~1.6 Hz, damping ratio: ~0.5 (underdamped)
- **SwiftUI `.spring()` default:** response: 0.5s, dampingFraction: 0.825 (slightly underdamped — overshoots ~2% then settles)
- **Implicit animation:** You don't animate properties. You change state, the
  system animates the transition. The animation is a property of the system, not
  the action.
- **Rubber banding:** Overscroll, over-drag, over-zoom — everything has elastic
  limits. Pull past the edge, it resists logarithmically, then snaps back.
- **Momentum:** Scrolling carries velocity. Flick and release — the content keeps
  moving, decelerating naturally.
- **Matched motion:** When something moves between contexts (list → detail, dock →
  window), it physically travels. No disappear/reappear teleportation.

### Z-OS Translation: Signal Chain Physics

Every motion is a signal chain with three nodes:

```
input -> spring_resolver -> output
```

The spring resolver node holds state: position, velocity. Each frame, it computes:
```
acceleration = (target - position) * stiffness - velocity * damping
velocity += acceleration * dt
position += velocity * dt
```

**Z-OS spring constants (global, configurable per persona):**
```
SPRING_STIFFNESS  = 120.0    // slightly tighter than Apple — Z-OS feels precise
SPRING_DAMPING    = 14.0     // slightly more damped — less bounce, more purpose
SPRING_MASS       = 1.0
```

**Why this is better than macOS:** In macOS, spring physics are simulated by the
UI framework. In Z-OS, they ARE signal chain resolution. The spring node is the
same kind of node as a TRISA scoring node or a VAULT read node. You can:
- **Tap** any animation to see its current state (position, velocity, target)
- **Fork** an animation to create a parallel motion
- **Gate** an animation to pause it conditionally
- **Delta** to detect when animation state changes significantly
- Inspect the temporal history: what was this spring doing at t-1, t-2?

**Rubber banding** is a gate with a logarithmic knee:
```
overscroll -> gate(> boundary) -> log_knee(stiffness: 0.3) -> spring_back
```

**Momentum** is a signal chain with a decay node:
```
velocity_input -> decay(friction: 0.985) -> position_accumulator -> output
```

**Matched motion** is chain handoff: the output node of one chain becomes the
input node of another. Position, velocity, and acceleration transfer. No
discontinuity because it's the same signal, rerouted.

**Implicit animation** is native to signal chains: changing a target value on a
spring node automatically causes resolution. There is no separate "animate" call.
You change the target. The chain resolves. The pixels move.

---

## 2. SPATIAL MODEL (Depth & Materials)

### What macOS Does
- **Z-axis layering:** Desktop < Windows < Sheets < Alerts < Notifications
  - Each layer has a distinct elevation
  - Shadows encode elevation (higher = larger, softer shadow)
- **Materials (blur/vibrancy):**
  - `.ultraThin` — barely visible blur (sidebar backgrounds)
  - `.thin` — light blur
  - `.regular` — standard (default window chrome)
  - `.thick` — heavy blur (overlay backgrounds)
  - `.ultraThick` — maximum blur
  - Background content bleeds through, creating depth perception
  - Materials adapt to light/dark mode
- **Translucency:** The menu bar, sidebars, and toolbars show what's behind them.
  This creates a sense of physical space — things exist at different depths.
- **Shadow system:**
  - Key shadow (hard, small, directional — simulates overhead light)
  - Ambient shadow (soft, larger, omnidirectional — simulates environment)
  - Combined creates convincing elevation

### Z-OS Translation: Signal Depth

Z-OS doesn't simulate depth. Signal chains have actual depth — they're directed
acyclic graphs with measurable distance from input to output. The visual depth
of an element IS its signal chain depth.

**Elevation model:**
```
Layer 0: Desktop / Root chain         — no shadow, full opacity
Layer 1: Application windows          — shadow(offset: 2, blur: 8, opacity: 0.15)
Layer 2: Panels / inspectors          — shadow(offset: 4, blur: 16, opacity: 0.20)
Layer 3: Sheets / attached modals     — shadow(offset: 6, blur: 24, opacity: 0.25)
Layer 4: Alerts / system dialogs      — shadow(offset: 8, blur: 32, opacity: 0.30)
Layer 5: Notifications / overlays     — shadow(offset: 10, blur: 40, opacity: 0.35)
```

**Materials as signal processing:**
macOS fakes depth with blur. Z-OS can do better: the "material" of a surface IS
the signal processing applied to the visual data passing through it.

```
background_pixels -> blur(radius: r) -> tint(color, opacity) -> composite -> output
```

The blur radius is a function of depth:
```
blur_radius = depth * 4px
```

**Material tiers (mapped to VAULT tiers):**
```
Sovereign surface  — opaque, no bleed-through (private data, no leakage)
Internal surface   — slight blur (r=4), subtle tint (your data, your context)
Reference surface  — heavy blur (r=16), strong tint (shared, ambient)
```

**Why this is better:** macOS materials are cosmetic — they look like depth but
don't represent anything real. Z-OS materials encode actual information security
tiers. A Sovereign surface is opaque because the data behind it is genuinely
private. You can SEE the security model.

---

## 3. COLOR SYSTEM

### What macOS Does
- **Semantic colors (not hardcoded):** `labelColor`, `secondaryLabelColor`,
  `tertiaryLabelColor`, `quaternaryLabelColor`, `placeholderTextColor`,
  `linkColor`, `accentColor`, `controlAccentColor`, `selectedContentBackgroundColor`
- **System accent color:** User picks one color, it permeates the entire OS.
  Buttons, selections, focus rings, progress bars — all use it.
- **10 named system colors:** blue, brown, cyan, green, indigo, mint, orange,
  pink, purple, red, teal, yellow + gray (6 shades)
- **Dynamic colors:** Colors change based on:
  - Light/dark mode (`.systemBackground` is white in light, near-black in dark)
  - Increased contrast mode
  - Active/inactive window state
  - Vibrancy context (in a sidebar vs. in content)
- **Separator colors:** `.separatorColor`, `.gridColor` — distinct from content
- **Control colors:** `.controlColor`, `.controlBackgroundColor`,
  `.selectedControlColor`, `.alternatingContentBackgroundColors`

### Z-OS Translation: Signal-Semantic Color

Z-OS doesn't have light/dark mode. It has **personas** (Zeros, DereZ, Full) and
**tiers** (Sovereign, Internal, Reference). Color encodes meaning, not aesthetics.

**Base palette derivation:**
One seed hue per persona, all other colors derived mathematically:
```
Zeros:  seed_hue = 160° (teal-cyan — robotics, physical, mechanical)
DereZ:  seed_hue = 270° (violet-indigo — code, digital, abstract)
Full:   seed_hue = 210° (blue-steel — complete, neutral, powerful)
```

From the seed hue, derive:
```
primary       = hsl(seed, 70%, 55%)     // main interactive color
primary_dim   = hsl(seed, 50%, 35%)     // inactive/muted
secondary     = hsl(seed + 30, 60%, 50%) // complementary accent
surface       = hsl(seed, 8%, 12%)      // dark background
surface_high  = hsl(seed, 8%, 18%)      // elevated background
on_surface    = hsl(seed, 5%, 88%)      // primary text
on_surface_2  = hsl(seed, 5%, 62%)      // secondary text
on_surface_3  = hsl(seed, 5%, 40%)      // tertiary text
on_surface_4  = hsl(seed, 5%, 25%)      // quaternary text
separator     = hsl(seed, 5%, 20%)      // dividers
danger        = hsl(0, 70%, 55%)        // errors, destructive
warning       = hsl(40, 80%, 55%)       // caution
success       = hsl(140, 60%, 45%)      // confirmation
```

**Tier color encoding (always present, across all personas):**
```
Sovereign  = warm accent (gold/amber tint on borders, subtle)
Internal   = neutral (no tint, default)
Reference  = cool accent (silver/blue tint, slightly transparent)
```

**TRISA decision colors (universal, never change):**
```
IS    = signal_green  hsl(145, 75%, 50%)  // data that matters
ISNT  = signal_gray   hsl(0, 0%, 35%)     // data that doesn't
```

**MasQ perception colors:**
When MasQ hides data, the element doesn't disappear — it becomes a uniform
surface at the tier's background color. You can SEE that something exists
without seeing what it is. The shape of hidden data is visible; the content
is not.

**Why this is better:** macOS accent color is cosmetic preference. Z-OS color
encodes real system state: what tier you're in, what persona you're using, what
TRISA decided about the data. Color is information, not decoration.

---

## 4. TYPOGRAPHY

### What macOS Does
- **SF Pro:** One family, all purposes.
  - Weights: Ultralight, Thin, Light, Regular, Medium, Semibold, Bold, Heavy, Black
  - Widths: Compressed, Condensed, Regular, Expanded
  - Optical sizes: Text (under 20pt), Display (20pt+) — the font SHAPE changes
  - Tracking adjusts with size (tighter large, looser small)
- **SF Mono:** Fixed-width variant for code
- **SF Pro Rounded:** Rounded variant for playful/casual contexts
- **Type scale (macOS):**
  - Large Title: 26pt, Regular
  - Title 1: 22pt, Regular
  - Title 2: 17pt, Regular
  - Title 3: 15pt, Regular
  - Headline: 13pt, Bold
  - Body: 13pt, Regular
  - Callout: 12pt, Regular
  - Subheadline: 11pt, Regular
  - Footnote: 10pt, Regular
  - Caption 1: 10pt, Regular
  - Caption 2: 10pt, Medium (numerals in lists)
- **Dynamic Type:** Text respects user size preferences. Layout adapts.
- **Tabular figures:** Numbers align in columns (monospaced digits)

### Z-OS Translation: Signal Typography

Z-OS needs two typefaces:
1. **Screen font** — for all GUI surfaces (equivalent to SF Pro)
2. **Console font** — for kernel framebuffer, shell, Z+ code (equivalent to SF Mono)

**Recommended families (open source, high quality):**
- Screen: **Inter** — designed for screens, 9 weights, variable, excellent hinting,
  tabular figures, optical size axis. The closest open-source equivalent to SF Pro.
- Console: **JetBrains Mono** — designed for code, ligatures, excellent at small
  sizes. Or **Iosevka** for extreme configurability.

**Z-OS type scale (8pt base, 1.25 ratio — minor third):**
```
display    = 32px / 40px line-height / Semibold   // hero content, page titles
title      = 24px / 32px / Semibold                // section headers
heading    = 20px / 28px / Medium                  // subsection headers
body       = 16px / 24px / Regular                 // primary content
label      = 14px / 20px / Medium                  // controls, buttons
caption    = 12px / 16px / Regular                 // secondary info
micro      = 10px / 14px / Medium                  // timestamps, counts
```

**Persona-specific typography:**
- **Zeros:** Slightly larger body (18px) — younger users, readability first
- **DereZ:** Standard scale, monospace for code blocks
- **Full:** Compact option available (14px body) — power users, information density

**Console type scale (kernel framebuffer):**
The kernel's `font8x16.h` VGA bitmap font is the boot font. Once the display
subsystem initializes, switch to rendered console font at:
```
console_normal  = 14px / 18px / Regular
console_bold    = 14px / 18px / Bold
console_dim     = 14px / 18px / Light, opacity 0.6
```

---

## 5. SPATIAL LAYOUT

### What macOS Does
- **8pt grid:** Everything aligns to 8px increments. Margins, padding, heights.
- **Standard metrics:**
  - Window minimum width: 400px
  - Sidebar width: 200-300px (resizable)
  - Toolbar height: 52px (unified) or 38px (compact)
  - Tab bar height: 28px
  - Content padding: 20px (standard), 12px (compact)
  - Control spacing: 8px between related controls, 20px between groups
  - Button height: 22px (small), 28px (regular), 32px (large)
  - Icon sizes: 16px (inline), 24px (toolbar), 32px (list), 64px (grid), 128px+ (preview)
- **Safe areas:** Content never touches screen edges. Minimum 20px margin.
- **Alignment:** Left edges align. Baseline text aligns. Grid snapping.

### Z-OS Translation: Fractal Grid

Z-OS uses a **fractal spatial system** — the same ratio repeats at every scale.

**Base unit:** `z = 4px` (half of macOS — Z-OS targets higher density)

**Spacing scale (powers of 2 × z):**
```
z1  =  4px    // minimum gap, hairline spacing
z2  =  8px    // tight spacing (between related items)
z3  = 12px    // standard internal padding
z4  = 16px    // standard gap between elements
z6  = 24px    // group separation
z8  = 32px    // section separation
z12 = 48px    // major landmark spacing
z16 = 64px    // panel/region boundaries
```

**Standard metrics:**
```
toolbar_height      = z12 (48px)
sidebar_width       = z16 * 4 (256px) to z16 * 6 (384px)
content_padding     = z4 (16px) standard, z3 (12px) compact
control_height      = z6 (24px) small, z8 (32px) regular
icon_size           = z4 (16px) inline, z6 (24px) toolbar, z8 (32px) list
separator_thickness = 1px, color: separator token
border_radius       = z1 (4px) small, z2 (8px) standard, z3 (12px) large
```

**Signal chain spacing in sigviz:**
```
node_size      = z8 (32px) diameter
node_gap       = z6 (24px) horizontal
chain_gap      = z4 (16px) vertical
edge_thickness = 2px
```

**Fractal property:** The ratio between spacing levels is consistent (~1.5x).
A signal chain visualization at zoom level 1 looks structurally identical to
zoom level 2 — just with different data visible. Self-similar at every scale.

---

## 6. INTERACTION & FEEDBACK

### What macOS Does
- **Immediate press feedback:** Button darkens on mousedown, before action completes
- **Hover states:** Subtle highlight on every interactive element
- **Focus rings:** 3px blue ring around focused element (keyboard navigation)
- **Cursor changes:** Arrow → hand (links) → I-beam (text) → crosshair → resize
  arrows → grab hand → spinning wait. ~15 distinct cursors.
- **Haptic feedback:** Force Touch trackpad provides physical clicks and taps
- **Sound feedback:** Trash sound, screenshot sound, volume pop, mail sent whoosh
- **Tooltips:** Hover 0.5s → tooltip appears near cursor
- **Undo everywhere:** ⌘Z undoes ANY action. Multi-level undo stack.
- **Drag and drop:** Everything is draggable. Text, files, images, URLs, colors.
  Drop targets highlight when valid.
- **Context menus:** Right-click anywhere → relevant actions. Consistent structure.
- **Keyboard shortcuts:** ⌘C/V/X/Z/A/S/W/Q are sacred. Never remapped.
- **Progress indication:** Determinate (progress bar), indeterminate (spinner),
  or activity indicator. Never leave the user staring at nothing.

### Z-OS Translation: Signal Feedback

Every interaction in Z-OS is a signal. The feedback IS the signal resolving.

**Press/hover/focus — signal states:**
```
Interactive element states:
  idle     → surface_color
  hover    → surface_color + primary(opacity: 0.08)
  pressed  → surface_color + primary(opacity: 0.16)
  focused  → 2px ring, color: primary, offset: 2px
  disabled → opacity: 0.38
  loading  → pulse animation (spring oscillation, stiffness: 40, damping: 0)
```

These are signal chain states: the element's input signal determines its visual
state. No separate state machine — the chain resolves to the correct appearance.

**Undo — VAULT temporal access:**
macOS has an undo stack (linear, app-managed). Z-OS has VAULT: every state
change is a new version. Undo is `vault.read(path, version - 1)`. Redo is
`vault.read(path, version + 1)`. The undo history IS the version chain.

This means:
- Undo is unlimited (VAULT never deletes)
- Undo works across app boundaries (it's at the data layer)
- You can undo to any point, not just linear back
- You can BRANCH: undo 5 steps, make a change, both timelines exist

**Drag and drop — signal rerouting:**
In macOS, drag-drop is a data transfer protocol (pasteboard). In Z-OS, dragging
an element is literally rerouting its signal chain. You pick up a node, move it
to a new location in the graph, and drop it — the edge reconnects. The visual
drag IS the signal operation.

Drop target highlighting is gate evaluation: the target gate checks if the
incoming signal type is compatible. Green highlight = gate would pass. Red = reject.

**Progress — chain resolution visibility:**
There is no spinner in Z-OS. You can SEE the signal chain resolving. Each node
lights up as it fires. The chain visualization IS the progress indicator. For
long operations, the sigviz shows exactly which node is currently executing and
how many remain.

**Keyboard shortcuts:**
```
Core (never change, all personas):
  Ctrl+Z  = vault.undo (temporal step back)
  Ctrl+Y  = vault.redo (temporal step forward)
  Ctrl+C  = signal.copy (duplicate node/data)
  Ctrl+V  = signal.paste (insert copied into chain)
  Ctrl+X  = signal.cut (copy + remove from chain)
  Ctrl+S  = vault.checkpoint (create named version)
  Ctrl+F  = search (chain-aware: searches data AND signal metadata)
  Ctrl+/  = command palette (all available actions)

Signal-specific:
  Tab     = next node in chain
  S-Tab   = prev node in chain
  Space   = fire/trigger selected node
  Enter   = expand/inspect selected node
  Esc     = collapse/deselect
  ~>      = tap (read-only observe selected signal)
```

---

## 7. WINDOW ANATOMY

### What macOS Does
- **Traffic lights:** Close (red), Minimize (yellow), Zoom (green) — always top-left
- **Title bar:** Window name, centered. Draggable.
- **Toolbar:** Actions, below title bar. Customizable.
- **Sidebar:** Navigation/hierarchy, left side. Collapsible.
- **Content area:** The actual content. Scrollable.
- **Inspector/detail pane:** Right side. Properties/metadata.
- **Status bar:** Bottom. Counts, info.
- **Sheets:** Modal attached to window (slides from title bar)
- **Popovers:** Non-modal, attached to trigger element
- **Split view:** Two windows side-by-side
- **Tabs:** Multiple documents in one window

### Z-OS Translation: Chain Surfaces

Z-OS doesn't have "windows" — it has **chain surfaces**: visual regions that
render the output of a signal chain.

**Chain surface anatomy:**
```
┌─[chain_id]──────────────────────────[persona]─[tier]─[⚡]─┐
│ ◀ Signal path breadcrumb                                    │
├─────────────────────────────────────────────────────────────┤
│ ┌──────────┐  ┌───────────────────────────┐  ┌───────────┐ │
│ │          │  │                           │  │           │ │
│ │  Chain   │  │       Output              │  │  Signal   │ │
│ │  Graph   │  │       View                │  │  Inspector│ │
│ │  (nav)   │  │                           │  │  (taps)   │ │
│ │          │  │                           │  │           │ │
│ └──────────┘  └───────────────────────────┘  └───────────┘ │
├─────────────────────────────────────────────────────────────┤
│ nodes: 12 │ resolved: 8 │ pending: 4 │ version: 17        │
└─────────────────────────────────────────────────────────────┘
```

- **Header:** Chain ID (name of the signal chain), persona badge, tier badge,
  live indicator (⚡ = chain is actively resolving)
- **Signal path breadcrumb:** Like a file path but for signal chains.
  `root > audio > mixer > channel_3 > eq`
- **Chain graph (left):** Sigviz rendering of the chain structure. Navigate by
  clicking nodes. Equivalent to macOS sidebar.
- **Output view (center):** The rendered output of the selected node or the
  chain endpoint. This is where the "content" lives.
- **Signal inspector (right):** Tap view of the selected signal. Shows current
  value, type, timing (TSC), connected edges, version history. Equivalent to
  macOS inspector pane.
- **Status bar:** Node count, resolution state, VAULT version number.

**Traffic lights equivalent:**
```
[×]  = detach chain (stop rendering this surface, chain keeps running)
[−]  = minimize to tray (chain still resolves, output cached)
[□]  = maximize / zoom to fill
[⚡]  = chain is live (green pulse) / paused (gray) / error (red pulse)
```

**Sheets → Signal modals:**
A sheet in Z-OS is a sub-chain that must resolve before the parent chain can
continue. It slides down from the header. The parent chain's pending nodes
dim while the sheet chain resolves.

**Tabs → Chain multiplexing:**
Multiple chains rendered in the same surface. Tab bar shows chain IDs. Switching
tabs switches which chain's output is rendered. All chains continue resolving
in background (unlike macOS where background tabs may suspend).

---

## 8. DOCUMENT MODEL

### What macOS Does
- **Auto-save:** Documents save automatically, continuously
- **Versions:** Browse previous versions (Time Machine-style), restore any version
- **Resume:** Apps reopen exactly where you left them
- **Quick Look:** Preview any file without opening it (Space bar)
- **Tags:** Color-coded organizational tags, cross-folder
- **Handoff:** Start on one device, continue on another

### Z-OS Translation: VAULT-Native

Z-OS doesn't need a document model. VAULT IS the document model.

- **Auto-save:** Every write to VAULT creates a version. There is no "save" action.
  The concept doesn't exist. All data is always current.
- **Versions:** `vault.read(path, version)` — browse any previous state. Not a
  feature, a fundamental property of the storage system.
- **Resume:** Application state is a signal chain stored in VAULT. On restart,
  the chain loads from its last resolved state. Resume is free.
- **Quick Look:** Every VAULT entry has a signal chain that renders a preview.
  Triggering Quick Look just fires that preview chain. The preview IS a signal.
- **Tags:** VAULT entries have tiers (Sovereign/Internal/Reference) and arbitrary
  metadata in the CFA manifest. Tags are metadata, queryable through gates.
- **Handoff:** CFA addresses are device-independent. The same data_id produces the
  same address on any device with the same seed. Handoff is: share the seed,
  both devices can now resolve the same addresses.

---

## 9. SYSTEM INTEGRATION

### What macOS Does
- **Spotlight:** Universal search across files, apps, web, calculations, definitions
- **Services:** Any app offers actions to any other app (right-click → Services)
- **Share sheet:** Universal sharing to any destination
- **Notifications:** Grouped, actionable, center
- **Control Center:** Quick toggles (WiFi, Bluetooth, display, sound)
- **Menu bar extras:** Persistent status indicators

### Z-OS Translation: Chain-Native

- **Search:** Search is a gate query across all VAULT entries and active signal
  chains. `gate(contains: "query")` applied to the global chain graph. Results
  are live — a search result is a signal that updates if the source changes.

- **Services:** Every signal chain exposes its capabilities as named taps. Any
  chain can tap into any other chain's taps. "Services" is just the tap registry.
  No separate protocol needed.

- **Share:** Sharing is forking a signal to a new destination chain. Fork the
  output, route it to the target. The data doesn't copy — it flows.

- **Notifications:** A notification is a signal that hasn't been consumed yet.
  It sits in a notification chain, pulsing at the spring animation frequency,
  until a gate (user acknowledgment) lets it through.

- **Status indicators:** The header bar of the root chain surface shows status
  signals from all registered services. Each service publishes a status signal
  to `zeos:spine:status`. The header renders them as colored dots:
  ```
  ● green  = service healthy
  ● yellow = service degraded
  ● red    = service error
  ○ gray   = service offline
  ```

---

## 10. ACCESSIBILITY

### What macOS Does (structural, not bolt-on)
- **VoiceOver:** Screen reader for every element. Every control has a label.
- **Reduce Motion:** Replaces animations with cross-fades
- **Increase Contrast:** Stronger borders, more opaque backgrounds
- **Reduce Transparency:** Solid backgrounds instead of blur/vibrancy
- **Full Keyboard Access:** Tab through every control
- **Dynamic Type:** Text sizes adjust to user preference
- **Color filters:** Grayscale, red/green filter, etc.
- **Switch Control:** Single-switch scanning navigation

### Z-OS Translation: Signal Accessibility

Because the UI IS signal chains, accessibility is signal processing — not a
separate system.

- **Screen reader:** Every signal node has a `label` field (string). A screen
  reader walks the chain graph and reads labels. The graph structure provides
  natural navigation order (chain flow = reading order).

- **Reduce Motion:** Replace spring nodes with instant-resolve nodes. The chain
  still resolves, but position snaps to target immediately instead of springing.
  One flag change per spring node.

- **Increase Contrast:** Modify the color derivation function to increase
  saturation and reduce opacity steps. `on_surface` jumps from 88% to 95%.
  Separators go from 20% to 35%.

- **Reduce Transparency:** Set all material blur radii to 0. Surfaces become
  opaque. The VAULT tier colors still encode meaning but backgrounds are solid.

- **Keyboard navigation:** Signal chains have natural traversal order. Tab =
  next node. Shift+Tab = previous. Enter = fire. This is structural — the chain
  graph IS the tab order.

- **Persona as accessibility:** Zeros mode IS a simplified interface. Not
  because it's less capable — because it's curated for the user's current needs.
  The progressive curtain is the most honest accessibility model possible: full
  capability always exists, disclosure is the user's choice.

---

## 11. CONSISTENCY RULES (The Z-OS HIG)

### What macOS Does
Apple's Human Interface Guidelines are 1000+ pages of rules that every app must
follow. Standard controls, standard behaviors, standard layouts.

### Z-OS Rules

1. **Every interactive element is a signal chain.** No exceptions. If it responds
   to input and produces output, it's a chain.

2. **Every state change goes through VAULT.** No in-memory-only state. Everything
   is versioned, everything is recoverable.

3. **Every animation uses the global spring constants.** Override stiffness/damping
   per element, but never use linear or keyframe animation.

4. **Color always encodes meaning.** Tier, persona, TRISA decision, service status.
   Never use color for pure decoration.

5. **Typography follows the scale.** No arbitrary font sizes. Every text element
   uses one of the 7 scale steps.

6. **Spacing follows the grid.** No arbitrary pixel values. Every gap, margin,
   and padding uses a value from the spacing scale.

7. **Hover → Pressed → Focused states exist on every interactive element.** No
   interactive element without visual feedback.

8. **Signal inspector is always available.** Any element can be inspected. Hold
   the inspect key (F12 or designated key), click any element, see its chain.

9. **Keyboard shortcuts are sacred.** Ctrl+Z/Y/C/V/X/S/F are never remapped.

10. **MasQ applies everywhere.** If data has a tier, the visual treatment reflects
    the tier. No surface ever reveals data above its tier.

11. **The persona curtain never reduces capability.** Zeros/DereZ hide complexity,
    never remove it. Every command available in Full mode exists in every persona.

12. **Temporal access is always possible.** Any piece of data, any UI state, any
    signal value — you can always ask "what was this at time T?"

---

## 12. FIRST BOOT / FIRST RUN

### What macOS Does
- Language selection → WiFi → Apple ID → Terms → Desktop
- Smooth, guided, one question at a time
- Progress dots at bottom
- Welcome video plays
- Desktop appears with curated default state

### Z-OS First Boot

The first boot IS a signal chain that resolves through setup nodes:

```
boot -> detect_hardware -> [choose_persona] -> [set_name] -> [network] -> desktop
```

Each node is a full-screen surface with one question/action. Spring transition
between nodes. The chain is visible as dots at the bottom (each dot = a node).

**Persona selection is the first real choice:**
```
┌─────────────────────────────────────────────┐
│                                             │
│        Who are you today?                   │
│                                             │
│   ┌──────────┐  ┌──────────┐               │
│   │          │  │          │               │
│   │  ZEROS   │  │  DEREZ   │               │
│   │          │  │          │               │
│   │ Build it │  │ Code it  │               │
│   └──────────┘  └──────────┘               │
│                                             │
│        (You can always switch later)        │
│                    ● ● ◉ ● ●               │
└─────────────────────────────────────────────┘
```

After persona selection, the shell appears in that persona's color scheme with
that persona's curated command set. The full system is running. The curtain is
up. Everything works.

No Apple ID equivalent. No terms and conditions. No telemetry opt-in.
You're running YOUR computer.

---

## 13. SOUND DESIGN

### What macOS Does
- Startup chime
- Screenshot shutter click
- Trash crumple sound
- Volume pop
- Mail sent whoosh
- Notification pings
- USB connect/disconnect sounds

### Z-OS Sound Design

Sounds should reflect the signal chain nature of the system. Not skeuomorphic
(no "crumple" for delete), not sci-fi bleeps. Subtle tonal signals.

**Sound vocabulary:**
```
chain_resolve   — soft harmonic (a signal chain completed)
chain_error     — dissonant short tone (a chain hit an error node)
vault_write     — very subtle tick (data versioned)
gate_pass       — quiet rising tone (data passed a gate)
gate_block      — quiet falling tone (data blocked by gate)
notification    — two-note ascending (attention needed)
connect         — resonant ping (device/service connected)
disconnect      — dampened ping (device/service disconnected)
boot            — chord that builds from single note (system assembling)
```

All sounds are generated from signal chains (synthesized, not sampled). The
sound engine IS the signal chain engine processing audio-rate signals.

Brad's audio engineering background is the design authority here. The MDE
mixing console metaphor applies: sounds have channel strips, sends, buses.
The notification sound goes through the same signal processing as everything else.

---

## IMPLEMENTATION PRIORITY

1. **Spring animation system** — implement in Z-OS framebuffer (fb.c)
2. **Color token system** — define palette derivation, implement in shell
3. **Type scale** — integrate Inter/JetBrains Mono into the display system
4. **Spacing grid** — codify z-unit system in all layout code
5. **Interaction states** — hover/pressed/focused in shell and UI elements
6. **Chain surface anatomy** — implement the standard window/surface layout
7. **Signal inspector** — the killer feature, make it work early
8. **Sound synthesis** — audio signal chain engine
9. **First boot flow** — the persona selection chain
10. **Accessibility signals** — reduce motion, increase contrast, keyboard nav
