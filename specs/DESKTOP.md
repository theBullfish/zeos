# Z-OS Desktop Specification

> **Date**: April 7, 2026
> **Status**: Designed, not yet implemented
> **Depends on**: DESIGN_SYSTEM.md, UI_UX_PRINCIPLES.md, theme.h, cursor.h

---

## 1. Sacred Corners

Fitts's Law: corners are infinite targets in two dimensions — the fastest
possible click targets on screen. These four corners are reserved system-wide.

| Corner | Action | Trigger |
|--------|--------|---------|
| **Top-Left** | Command Palette | Hot corner + keyboard shortcut |
| **Top-Right** | Notifications / Signal Status | Hot corner |
| **Bottom-Left** | Workspaces | Hot corner |
| **Bottom-Right** | Show Desktop / Minimize All | Hot corner |

- Top = system-level (launch, alerts) — controls above, work below
- Bottom = spatial navigation (workspaces, desktop) — manage the stage
- Left = action/go (launch, switch)
- Right = status/retreat (alerts, clear)
- All four also have keyboard shortcuts
- Hot corner activation area: 8px × 8px in each corner

---

## 2. Panel (Top Bar)

Top edge by default. User-configurable position. Shows running chains,
system status, persona identity. Three zones:

```
┌─[persona]─┬──────────────────────────────────────────┬─[status]─┐
│  ● Zeros   │  ● chain_1  ● chain_2  ○ chain_3  ◉ err │  ⚡ 🔔 🕐 │
└────────────┴──────────────────────────────────────────┴──────────┘
  LEFT               CENTER                               RIGHT
```

### Left Zone — Identity & Action
- Persona indicator (colored dot + name in compact, icon only in dense)
- Click opens command palette (ties to top-left corner)

### Center Zone — Running Chains
- Each running chain gets a pill/tab
- Color-coded by state:
  - Persona accent = running
  - Dim accent = paused
  - `COLOR_DANGER` = error
- Click → bring that chain surface to front
- This IS the taskbar — for signal chains, not apps
- Chains from the same workspace group together
- Drag pills to reorder
- Right-click pill → detach, minimize, inspect, settings

### Right Zone — System Status
- `[⚡]` system health summary (green/amber/red)
- Notification count badge (links to top-right corner)
- Clock
- Network/battery indicators if applicable

### Panel Behavior
- Height follows density: 32px compact, 40px standard, 48px comfortable
- Auto-hide option: slides up, hot edge to reveal
- Translucent with vibrancy when compositor supports material blur
- Always above chain surfaces in z-order

---

## 3. Desktop Surface

The desktop is a **functional workspace**, not a dashboard, not bare wallpaper.
Ships clean — empty. User places what they want.

### Wallpaper
- Solid color (default: `COLOR_SURFACE` #0D1117), gradient, or image
- User-selectable in Settings and first-boot
- Persona-tinted option: subtle accent gradient at bottom edge (5% opacity)
- Ship a few good default wallpapers, user can add their own
- Wallpaper is a VAULT path — changeable programmatically or manually

### Desktop Icons
- Drag any chain surface / app / VAULT folder to desktop → creates icon
- Icons use SVG icon set, tinted with persona accent
- Grid-snap by default (Z8 = 32px grid), free placement with modifier key
- Icon size follows density: 48px comfortable, 40px standard, 32px compact
- Double-click opens. Right-click → Open, Remove from desktop, Properties
- Icons persist in VAULT — survive reboot
- Icons are real VAULT references, not dead shortcuts
- Drag FROM desktop to a chain surface to feed it (e.g., file → editor)

### Status Strip (optional, bottom edge)
- Single line: chain count, node summary, VAULT health (left), date/time (right)
- `TEXT_TERTIARY` opacity — functional, not decorative
- User can hide it (panel already has clock/status)

### What the Desktop is NOT
- No auto-populating icons at install (ships empty)
- No ads, suggestions, "trending," or "Welcome to Z-OS!" junk
- No widgets grid (2.0 consideration)
- Clutter is bad. The user controls what's here.

---

## 4. Dock / Launcher

Two paths to launch anything. Both always available, neither exclusive.

### Command Palette (keyboard path)
- Top-left corner hot corner OR keyboard shortcut
- Type to search: chains, apps, settings, VAULT files, commands
- Every system action reachable through it
- The power user path

### Dock (mouse/visual path)

```
                ┌──┬──┬──┬──┐ │ ┌──┬──┐
                │📁│⌨ │🎬│🔧│ │ │○ │○ │
                └──┴──┴──┴──┘ │ └──┴──┘
                 pinned        │  running
                              divider
```

- Bottom edge, centered
- Auto-hides by default (slides down, hot edge to reveal)
- Left of divider: pinned items (user chose these)
- Right of divider: running chains (automatic, live)
- Running chains show state dot: accent = live, dim = paused, red = error
- Hover running chain → thumbnail preview of chain surface
- Click to open/focus
- Right-click → detach, minimize, inspect, settings
- Drag to reorder pinned items
- Drag off dock to unpin (poof animation, spring-driven)
- Dock size follows density setting
- Option: always visible (eats ~48px of screen)
- Empty at first boot — zero icons until user pins something

### Why Bottom Edge
- Top is taken by the panel (chain pills, system status)
- Bottom-left corner = workspaces, bottom-right = show desktop — dock between them
- Thumb-friendly on laptops

### Why Auto-Hide Default
- Clean desktop = no clutter
- Power users use command palette anyway
- Screen real estate matters

---

## 5. First Boot Experience

Five screens, under 60 seconds. Every choice is visual, one click, reversible.
Each screen passively teaches one Z-OS concept.

### Screen 1 — Welcome (teaches: signals exist)
- Z-OS logo, persona accent pulse (subtle)
- "Z-OS runs on signals — everything is connected."
- Tiny animation: three dots connect with lines, pulse once
- Not explained further. Seeds the mental model.
- Single button: Continue

### Screen 2 — Persona (teaches: the OS adapts to you)
- Three cards side by side, live preview behind each:
  - **Zeros** (teal) — "Build, make, explore"
  - **DereZ** (magenta) — "Code, create, ship"
  - **Full** (steel blue) — "Everything"
- Click one → whole screen shifts to that accent instantly
- "Your persona shapes what you see first. The full system is always underneath."
- Sets: accent color, shell prompt, cursor colorway, default suggestions

### Screen 3 — Window Controls (teaches: detach ≠ close)
- Visual mockup of a chain surface, full width
- Two buttons: **Left** and **Right**
- Click one → 4 window control buttons animate to that side live
- "In Z-OS, closing a window doesn't stop its work. Your chains keep running."
- `[×]` button label briefly flashes "detach" as you pick

### Screen 4 — Appearance (teaches: everything is adjustable)
- Top half: **Light / Dark / Auto** (three cards, live preview)
- Bottom half: **Comfortable / Standard / Compact** (three cards, live preview)
- "These aren't locked in. Right-click anything in Z-OS to find its settings."

### Screen 5 — Done (teaches: two paths)
- "Two ways to do anything:"
- Left side: keyboard icon + "Command Palette — search for any action" (top-left corner glows)
- Right side: mouse icon + "Dock — pin what you use" (bottom edge glows)
- Both glow once, fade
- Desktop appears with chosen wallpaper and theme
- Empty dock slides up once, then auto-hides (teaches it exists)
- Done. You're in.

### What's NOT in First Boot
- No account creation (local-first OS)
- No network setup (that's Settings)
- No telemetry opt-in (there is no telemetry)
- No sensory mode picker (default Standard, discoverable in Settings)
- No tutorial. The OS teaches through use.

---

## 6. Chain Surface Snap/Tile Behavior

Hybrid model: floating by default, snap/tile with keyboard or drag-to-edge.

### Drag-to-Edge Snap Zones

```
┌──────────┬────────────────────────┬──────────┐
│  TOP-L   │      TOP EDGE          │  TOP-R   │
│ (quarter)│     (maximize)         │ (quarter)│
│  LEFT    ├────────────────────────┤  RIGHT   │
│  EDGE    │                        │  EDGE    │
│  (left   │       CENTER           │  (right  │
│   half)  │    (no snap zone)      │   half)  │
│          ├────────────────────────┤          │
│  BOT-L   │     BOTTOM EDGE        │  BOT-R   │
│ (quarter)│     (no snap)          │ (quarter)│
└──────────┴────────────────────────┴──────────┘
```

- Drag to top edge → maximize
- Drag to left/right edge → half screen
- Drag to corner → quarter screen
- Ghost preview before release (translucent accent rectangle)
- Release to snap. Spring animation settles into place.
- Drag away from edge → unsnap, surface picks up with velocity (spring physics)
- Snap zone activation area: 32px at edges (generous tolerance)

### Keyboard Tiling

| Shortcut | Action |
|----------|--------|
| `Super + ←` | Left half |
| `Super + →` | Right half |
| `Super + ↑` | Maximize |
| `Super + ↓` | Restore / minimize |
| `Super + Shift + ←/→` | Move to adjacent workspace |
| `Super + 1-4` | Quarter snap (TL, TR, BL, BR) |
| `Super + T` | Toggle tiling mode |

### Tiling Mode (opt-in power feature)
- `Super + T` toggles full auto-tiling for current workspace
- New surface opens → everything re-tiles (spring animated)
- Close a surface → others expand to fill gap
- Drag dividers between tiled surfaces to resize, neighbors adjust
- Per-workspace: workspace 1 can tile while workspace 2 floats

### Signal Chain Awareness (the Z-OS difference)
- Surfaces from the **same chain** magnetically prefer adjacency
- Open a signal inspector for chain_1 → snaps next to chain_1's output automatically
- Parent/child chains stack: parent left, child right
- Magnetic preference, not forced — drag elsewhere and it stays
- Default placement is smart, user override is instant

### Resize Behavior
- Surfaces have minimum sizes (respects content)
- Snapped surfaces share edges — resize one, neighbor adjusts
- Free-floating resize from any edge or corner
- All resize animations: SPRING_INTERACTIVE (400/28) — snappy, no bounce

### What it's NOT
- Not mandatory tiling — floating is default, tiling is opt-in
- Not a rigid grid — zones are guides, not cells
- Not pixel-perfect OCD — generous snap tolerance

---

## Window Controls (reference)

Four buttons per chain surface. User picks Left or Right at first boot.
Changeable in Settings. Custom placement deferred to Z-OS 2.0.

```
[×]  = detach (stop rendering, chain keeps running)
[−]  = minimize to tray (chain resolves, output cached)
[□]  = maximize / zoom to fill
[⚡]  = signal status (green pulse / gray / red pulse)
```

- `[×]` is DETACH, not close/kill. The chain lives.
- `[⚡]` is unique to Z-OS. Live health indicator in window chrome.
- Spring-animated hover/press states on all 4 buttons.
- Persona accent coloring on hover.

---

## Design Principles (from this spec)

1. **Corners are sacred** — 4 corners, 4 system actions, always
2. **Ships clean** — empty desktop, empty dock, user fills it
3. **Two paths** — keyboard (command palette) and mouse (dock) for everything
4. **Passive education** — first boot teaches through doing, not reading
5. **Chains, not apps** — the panel shows running signals, not application windows
6. **Magnetic, not rigid** — snap zones guide, chain awareness suggests, nothing forces
7. **Detach, not destroy** — closing a surface never kills the work behind it
8. **Spring everything** — all motion has physics, all motion < 250ms
