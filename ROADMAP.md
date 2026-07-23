> **[SUPERSEDED 2026-07-23 by `BUILD_MAP.md`]** — this tracker's checkboxes were unverified assertions. The map WE follow is `BUILD_MAP.md` (BIBLE-gated: verified base truth or no checkbox). Kept for history, not for status.

# Z-OS Roadmap & Feature Tracker

Status: `[ ]` planned · `[~]` in progress · `[x]` done · `[!]` blocked · `[2.0]` deferred to Zeos 2.0

---

## Architecture

### Hardware Abstraction Layer (HAL)
- [ ] Define `hal.h` interface (interrupts, timer, serial, MMIO, page tables)
- [ ] x86-64 HAL backend (current code, refactored behind hal.h)
- [ ] ARM64 HAL backend (new)
- [ ] Move all `__asm__`, `outb/inb`, `rdtsc`, GDT/IDT, PIC, PIT behind HAL
- [ ] ARM UEFI boot stub (or bare-metal entry)
- [ ] ARM generic timer (replaces PIT+TSC)
- [ ] ARM GIC (replaces PIC 8259)
- [ ] ARM MMIO UART (replaces COM1 port I/O)
- [ ] ARM ECAM PCI (replaces port 0xCF8/0xCFC)
- [ ] ARM page tables (replaces x86-64 4-level)

**x86-locked files (10):** gdt.c/h, idt.c/h, io.h, vmm.c/h, timer.c, serial.c, keyboard.c, pci.c, main.c, Makefile
**Already portable (40+):** fb, heap, signal, sigviz, shell, zplus, wm, compositor, browser, font, all networking, ui, anim, cursor, theme, persona, vault

### Z+ Language Role
- [ ] Decide: Z+ as user language (option 1), compiled (option 2), or shell/glue (option 3)
- [ ] Signal chain definitions in Z+
- [ ] UI layout declarations in Z+
- [ ] App/config logic in Z+
- [ ] Z+ REPL in shell (already exists)

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
- [x] Basic panel rendering (compositor.c — persona dot, chain count, workspace)
- [ ] Left zone: persona indicator, command palette trigger
- [ ] Center zone: running chain pills (color-coded by state)
- [ ] Right zone: system health, notification badge, clock
- [ ] Height follows density (32/40/48px)
- [ ] Auto-hide option
- [ ] Translucent vibrancy (compositor dependent)
- [ ] Right-click chain pill → detach, minimize, inspect, settings

### Desktop Surface (designed)
- [x] Wallpaper color + persona accent gradient (compositor.c)
- [ ] Wallpaper image loading from VAULT
- [ ] Desktop icons: drag to place, grid-snap, persona-tinted SVGs
- [ ] Icons persist in VAULT, survive reboot
- [ ] Drag from desktop to chain surface (feed a chain)
- [ ] Ships empty — no default icons, no junk

### Dock (designed)
- [x] Basic dock rendering (compositor.c — centered, auto-hide)
- [ ] Pinned items (left of divider) + running chains (right of divider)
- [ ] Running chain state dots (accent/dim/red)
- [ ] Hover → thumbnail preview
- [ ] Drag to reorder, drag off to unpin (poof spring anim)
- [ ] Size follows density setting
- [ ] Empty at first boot

### Window Controls Placement (designed)
- [x] Spec: user picks Left or Right at first boot
- [x] wm.c: controls_side configurable (WM_CONTROLS_LEFT / WM_CONTROLS_RIGHT)
- [x] All 4 buttons rendered: `[×] detach` · `[−] minimize` · `[□] maximize` · `[⚡] signal status`
- [ ] Settings toggle: easily switch Left ↔ Right at any time
- [2.0] Advanced/Custom: user-defined button placement (drag to position)

### Chain Surface Snap/Tile (designed + built)
- [x] Drag-to-edge zones: top=maximize, sides=half, corners=quarter
- [x] Ghost preview before release (translucent accent rectangle)
- [x] Snap preserves/restores original geometry
- [x] Auto-tiling: grid layout per workspace (wm_toggle_tiling)
- [ ] Spring-animated snap settle + unsnap with velocity
- [ ] Keyboard shortcuts: Super+arrows, Super+1-4 quarters, Super+T tiling
- [ ] Signal chain magnetic adjacency (same chain → prefer side-by-side)
- [ ] Parent/child chain stacking (parent left, child right)

### Window Manager (built)
- [x] Window chrome renderer (title bar, 4 controls, border)
- [x] Stacking / z-order with focus management
- [x] Drag (title bar) with snap zone detection
- [x] Resize from any edge/corner with minimums
- [x] Maximize / minimize / restore / detach
- [x] Workspaces (8 max, switch, move surfaces between)
- [x] Shadow rendering (L1 unfocused, L2 focused)
- [ ] Sheets (modal, slides from title bar — sub-chain must resolve)
- [ ] Popovers (non-modal, attached to trigger)
- [ ] Tabs / chain multiplexing

### Compositor (built)
- [x] 6-layer pipeline: desktop → surfaces → panel → dock → overlays → cursor
- [x] Frame timing from TSC
- [x] Animation tick integration (anim_tick + cursor_tick per frame)
- [x] Dirty region tracking
- [ ] Double buffering (back buffer allocated, flip to front)
- [ ] Material blur/vibrancy (ultraThin → ultraThick)
- [ ] Partial redraw (only dirty regions)

---

## First Boot Experience (spec: specs/DESKTOP.md §5)

- [x] 5 screens designed, under 60 seconds, all visual
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
- [x] stb_truetype integrated (font.c — glyph cache, grayscale AA)
- [x] font_draw / font_measure / font_line_height API
- [ ] Embed TTF data in kernel binary (objcopy build step)
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
- [x] First-boot persona selection designed
- [ ] Per-persona dock/launcher defaults
- [ ] Persona switching animation (spring-driven color crossfade)

---

## Networking

- [x] virtio-net driver (PCI, RX/TX queues, frame send/recv)
- [x] ARP (cache, request, reply)
- [x] IPv4 + ICMP ping
- [x] UDP (send/recv, port binding, callbacks)
- [x] TCP (3-way handshake, send/recv, FIN, state machine)
- [x] DNS (query, parse, 8-entry cache)
- [x] HTTP/1.1 GET (full workflow: DNS→TCP→request→parse)
- [x] DHCP client (DORA, auto IP/gateway/DNS, exponential backoff)
- [x] TLS layer API (tls_connect/send/recv/close + https_get)
- [x] mbedTLS library integrated (TLS 1.3, bare-metal config)
- [x] Platform shim (malloc→heap, entropy→TSC, time→TSC+epoch, libc stubs)
- [ ] Uncomment mbedTLS calls in net_tls.c (wire to live library)
- [ ] Mozilla root CA bundle (embedded in binary)
- [ ] Multi-connection TCP (currently single connection)
- [ ] TCP retransmission / congestion control
- [ ] Real NIC driver: Intel e1000/i219 (CN60 hardware)
- [ ] WebSocket

---

## Web Browser

- [x] HTML parser (tags, attributes, comments, void elements, script/style skip)
- [x] 512-node static DOM pool
- [x] CSS default styles per tag (h1-h3, p, a, li, ul, code, hr)
- [x] Links colored with persona accent
- [x] Block layout engine (margin, padding, text height estimation)
- [x] Framebuffer renderer (clipping, scroll, backgrounds, text, HR)
- [x] Navigation: URL parsing, history (32 entries), back/forward/refresh/home
- [x] Toolbar + status bar rendering
- [ ] Wire to TTF font rendering (currently uses boot font)
- [ ] Link click handling (hit-test DOM, navigate)
- [ ] Image loading + rendering (PNG decode)
- [ ] CSS inline style parsing
- [ ] Form elements (input, button)
- [ ] Scrollbar rendering
- [2.0] JavaScript engine

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
- [x] Compositor ticks animations per frame
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

## Hardware Targets

### x86-64 (current)
- [x] QEMU (virtio-net, OVMF UEFI)
- [~] ASUS CN60 Chromebox (USB boot image, UEFI)
- [ ] Intel NIC driver (e1000/i219 for CN60)

### ARM64 (planned)
- [ ] Raspberry Pi 4/5 target
- [ ] ARM UEFI boot
- [ ] HAL backend
- [ ] Device tree parsing

---

## Notes

- Window controls `[×]` = **detach** (chain keeps running), NOT kill
- `[⚡]` is unique to Z-OS — live signal health indicator in window chrome
- "Custom placement" is a 2.0 feature — design it, don't build it yet
- All UI motion uses spring physics, never CSS-style easing curves
- Panel shows chains, not apps. Dock shows pinned + running. Desktop shows user icons.
- First boot: 5 screens, passive education, under 60 seconds
- Corners are sacred: palette, notifications, workspaces, show desktop
- x86 code isolated to ~10 files, 40+ files already portable
- Z+ role TBD: user language vs compiled vs shell/glue layer
- No subpixel font rendering (spec decision — grayscale AA only)
- Never roll your own crypto — mbedTLS for all TLS/HTTPS
