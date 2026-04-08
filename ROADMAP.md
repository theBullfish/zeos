# Z-OS Roadmap & Feature Tracker

Status: `[ ]` planned · `[~]` in progress · `[x]` done · `[!]` blocked · `[2.0]` deferred to Zeos 2.0

---

## Desktop & Window System

### Window Controls Placement
- [~] First-boot setup: user picks Left or Right window control placement
- [ ] Settings toggle: easily switch Left ↔ Right at any time
- [ ] All 4 buttons rendered: `[×] detach` · `[−] minimize` · `[□] maximize` · `[⚡] signal status`
- [ ] Left layout: all 4 buttons grouped top-left (macOS style)
- [ ] Right layout: all 4 buttons grouped top-right (Windows style)
- [2.0] Advanced/Custom: user-defined button placement (drag to position)

### Window Manager
- [ ] Window chrome renderer (title bar, controls, drag, resize)
- [ ] Stacking / z-order
- [ ] Tiling mode
- [ ] Window snapping (edges, half-screen, quarter)
- [ ] Sheets (modal, slides from title bar)
- [ ] Popovers (non-modal, attached to trigger)
- [ ] Split view
- [ ] Tabs within windows

### Compositor
- [ ] Layered rendering (windows, overlays, cursor on top)
- [ ] Shadow rendering (5 elevation levels from theme.h)
- [ ] Material blur/vibrancy (ultraThin → ultraThick)
- [ ] Transparency / alpha compositing

### Desktop Surface
- [ ] Wallpaper system
- [ ] App launcher / dock
- [ ] Desktop icons (optional)
- [ ] Notification center
- [ ] System tray / status area

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
- [ ] Keyboard shortcuts system
- [ ] Input method framework

---

## Typography & Fonts

- [x] VGA 8x16 bitmap boot font (font8x16.h)
- [x] Inter (7 weights) imported — UI font
- [x] JetBrains Mono (6 weights) imported — code font
- [ ] TTF rasterizer (stb_truetype or custom)
- [ ] Font size rendering at theme.h type scale (10–32px)
- [ ] Subpixel rendering
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
- [ ] Per-persona default window control placement
- [ ] Per-persona dock/launcher layout
- [ ] Persona switching animation (spring-driven color crossfade)
- [ ] First-boot persona selection flow

---

## Settings / First Boot

- [ ] First-boot wizard framework
- [ ] Persona selection screen
- [ ] Window controls placement picker (Left / Right)
- [ ] Color scheme preview during setup
- [ ] Settings app (persistent config in VAULT)
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
- [ ] Spring-driven window open/close
- [ ] Spring-driven menu appear/dismiss
- [ ] Spring-driven scroll physics
- [ ] Reduced motion mode (accessibility)

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
