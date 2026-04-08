# Z-OS Roadmap & Feature Tracker

Status: `[ ]` planned · `[~]` in progress · `[x]` done · `[!]` blocked · `[2.0]` deferred to Zeos 2.0

---

## Desktop & Window System (spec: specs/DESKTOP.md)

### Sacred Corners (designed)
- [x] Top-Left: Command Palette (hot corner + shortcut)
- [x] Top-Right: Notifications / Signal Status
- [x] Bottom-Left: Workspaces
- [x] Bottom-Right: Show Desktop / Minimize All
- [ ] Hot corner activation (8px zones)
- [ ] Keyboard shortcuts for all 4

### Panel / Top Bar (designed)
- [ ] Left zone: persona indicator, command palette trigger
- [ ] Center zone: running chain pills (color-coded by state)
- [ ] Right zone: system health, notification badge, clock
- [ ] Height follows density (32/40/48px)
- [ ] Auto-hide option
- [ ] Translucent vibrancy (compositor dependent)
- [ ] Right-click chain pill → detach, minimize, inspect, settings

### Desktop Surface (designed)
- [ ] Wallpaper system (solid, gradient, image — VAULT path)
- [ ] Desktop icons: drag to place, grid-snap, persona-tinted SVGs
- [ ] Icons persist in VAULT, survive reboot
- [ ] Drag from desktop to chain surface (feed a chain)
- [ ] Optional status strip (bottom edge, TEXT_TERTIARY)
- [ ] Ships empty — no default icons, no junk

### Dock (designed)
- [ ] Bottom edge, centered, auto-hide default
- [ ] Pinned items (left of divider) + running chains (right of divider)
- [ ] Running chain state dots (accent/dim/red)
- [ ] Hover → thumbnail preview
- [ ] Drag to reorder, drag off to unpin (poof spring anim)
- [ ] Size follows density setting
- [ ] Empty at first boot

### Window Controls Placement (designed)
- [x] Spec: user picks Left or Right at first boot
- [ ] Settings toggle: easily switch Left ↔ Right at any time
- [ ] All 4 buttons rendered: `[×] detach` · `[−] minimize` · `[□] maximize` · `[⚡] signal status`
- [ ] Left layout: all 4 buttons grouped top-left
- [ ] Right layout: all 4 buttons grouped top-right
- [2.0] Advanced/Custom: user-defined button placement (drag to position)

### Chain Surface Snap/Tile (designed)
- [ ] Drag-to-edge zones: top=maximize, sides=half, corners=quarter
- [ ] Ghost preview before release (translucent accent rectangle)
- [ ] Spring-animated snap settle + unsnap with velocity
- [ ] Keyboard shortcuts: Super+arrows, Super+1-4 quarters, Super+T tiling
- [ ] Tiling mode: per-workspace, auto-arrange, divider drag
- [ ] Signal chain magnetic adjacency (same chain → prefer side-by-side)
- [ ] Parent/child chain stacking (parent left, child right)
- [ ] 32px snap zone tolerance

### Window Manager
- [ ] Window chrome renderer (title bar, controls, drag, resize)
- [ ] Stacking / z-order
- [ ] Sheets (modal, slides from title bar — sub-chain must resolve)
- [ ] Popovers (non-modal, attached to trigger)
- [ ] Tabs / chain multiplexing

### Compositor
- [ ] Layered rendering (desktop → surfaces → panel → cursor)
- [ ] Shadow rendering (5 elevation levels from theme.h)
- [ ] Material blur/vibrancy (ultraThin → ultraThick)
- [ ] Transparency / alpha compositing

---

## First Boot Experience (spec: specs/DESKTOP.md §5)

- [x] 5 screens, under 60 seconds, all visual
- [x] Passive education: each screen teaches one Z-OS concept
- [ ] Screen 1: Welcome — signals exist (dots connect, pulse)
- [ ] Screen 2: Persona picker — OS adapts to you (live preview)
- [ ] Screen 3: Window controls L/R — detach ≠ close (flash "detach")
- [ ] Screen 4: Appearance — light/dark/auto + density (right-click hint)
- [ ] Screen 5: Done — two paths (palette glow + dock slide)

---

## Cursor & Input

### Cursor System
- [x] 50 cursor SVGs organized by role
- [x] 150 persona-themed variants (Zeros/DereZ/Full)
- [x] cursor.h/c with 22 states, spring-animated click feedback
- [x] Selectable 3-way colorway (cycle or direct select)
- [ ] Bitmap sprite rendering from SVG-derived data
- [ ] Mouse driver (PS/2 or USB HID)
- [ ] Cursor hotspot calibration per sprite

### Click Animations
- [x] Scale pulse (spring-driven press/release)
- [x] Ripple ring (expanding + fading, persona-colored)
- [x] Burst sprite swap (sparkle on click)
- [x] Confirm sprite swap (checkmark on success)
- [ ] Wire to actual mouse events

### Keyboard
- [ ] Full keymap with layout switching
- [ ] Keyboard shortcuts system (Super+arrows, etc.)
- [ ] Input method framework

---

## Typography & Fonts

- [x] VGA 8x16 bitmap boot font (font8x16.h)
- [x] Inter (7 weights) imported — UI font
- [x] JetBrains Mono (6 weights) imported — code font
- [ ] TTF rasterizer (stb_truetype or custom)
- [ ] Font size rendering at theme.h type scale (10–32px)
- [ ] Grayscale antialiasing (no subpixel — spec decision)
- [ ] Font fallback chain (Inter → Noto → bitmap)

---

## Icons & Visual Assets

- [x] 1,067+ SVGs across 16 categories
- [x] Persona color palette (theme.h tokens + palette.png)
- [ ] SVG→bitmap pipeline (build-time rasterization to sprite sheets)
- [ ] Icon rendering in UI at 16/24/32px (ICON_SM/MD/LG)
- [ ] Icon tinting with persona accent at runtime

---

## Persona System

- [x] 3 personas defined (Zeros/DereZ/Full)
- [x] Per-persona accent colors + dim variants
- [x] Shell prompt switching
- [x] Cursor colorway switching
- [ ] Per-persona dock/launcher defaults
- [ ] Persona switching animation (spring-driven color crossfade)
- [x] First-boot persona selection designed

---

## Settings

- [ ] Settings app (persistent config in VAULT)
- [ ] Search-first (every setting searchable from command palette)
- [ ] Organized by goal: Display, Sound, Input, Network, Privacy, Storage
- [ ] Right-click any UI element → "Settings for this..."
- [ ] Inline current values next to every setting name
- [ ] One settings app. No legacy duplicate. Ever.
- [2.0] Advanced window control custom placement UI

---

## Signal Visualizer

- [x] sigviz.h/c — node graph renderer
- [x] State colors (idle/ready/running/done/error)
- [ ] Interactive node selection
- [ ] Live animation (running state pulses)
- [ ] Zoom / pan

---

## Animation & Motion

- [x] Spring physics engine (anim.h/c, 64 concurrent)
- [x] Presets: snappy, smooth, bouncy, interactive
- [x] Retarget with velocity preservation
- [ ] Spring-driven surface open/close
- [ ] Spring-driven snap settle + unsnap with velocity
- [ ] Spring-driven dock auto-hide slide
- [ ] Spring-driven menu appear/dismiss
- [ ] Spring-driven scroll physics
- [ ] Reduced motion mode (instant cuts + opacity fades, not zero motion)
- [ ] Animation speed multiplier: 0x, 0.5x, 1x, 2x

---

## Accessibility

- [ ] 3 sensory modes: Standard, Low Stimuli, High Contrast
- [ ] System-wide letter/word/line spacing adjustment
- [ ] Animation speed multiplier (top-level, not buried)
- [ ] Reduced motion toggle (top-level settings)
- [ ] 3 density modes: Comfortable, Standard, Compact
- [ ] Dark/light/auto color scheme with night shift
- [ ] Focus Mode (one toggle, suppresses non-critical notifications)
- [ ] CVD simulation mode for designers/developers
- [ ] 44px minimum touch targets

---

## Networking

- [x] virtio-net, ARP, IPv4, TCP, UDP, DNS, HTTP (scaffolded)
- [ ] DHCP client
- [ ] TLS / HTTPS
- [ ] WebSocket

---

## Notes

- Window controls `[×]` = **detach** (chain keeps running), NOT kill
- `[⚡]` is unique to Z-OS — live signal health indicator in window chrome
- "Custom placement" is a 2.0 feature — design it, don't build it yet
- All UI motion uses spring physics, never CSS-style easing curves
- Panel shows chains, not apps. Dock shows pinned + running. Desktop shows user icons.
- First boot: 5 screens, passive education, under 60 seconds
- Corners are sacred: palette, notifications, workspaces, show desktop
