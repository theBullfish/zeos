# macOS Design System Reference

> Comprehensive technical reference for what makes macOS feel like macOS.
> Compiled for translation to Z-OS. Focuses on implementable specifics:
> pixel values, spring constants, color tokens, type scales.
>
> **Date**: March 25, 2026
> **Purpose**: Design system translation source material
> **Sources**: Apple HIG, AppKit/SwiftUI API documentation, WWDC sessions
> (2019-2025), reverse-engineering community research

---

## Table of Contents

1. [HIG Category Map](#1-hig-category-map)
2. [Typography System](#2-typography-system)
3. [Color System](#3-color-system)
4. [Spacing and Layout Grid](#4-spacing-and-layout-grid)
5. [Animation and Motion](#5-animation-and-motion)
6. [Spatial Model (Z-ordering, Materials, Vibrancy)](#6-spatial-model)
7. [Window Anatomy](#7-window-anatomy)
8. [Interaction Patterns](#8-interaction-patterns)
9. [Structural Accessibility](#9-structural-accessibility)
10. [macOS Differentiators](#10-macos-differentiators)

---

## 1. HIG Category Map

Apple's Human Interface Guidelines for macOS organize into these top-level
categories. Every category below is a design subsystem, not marketing fluff.

### Foundations
- **Accessibility** — VoiceOver, Switch Control, Full Keyboard Access, Dynamic Type, reduce motion, increase contrast, differentiate without color
- **App icons** — 1024x1024 master, rounded-rectangle superellipse (not a circle, not a squircle — a specific continuous-curvature shape)
- **Branding** — how system chrome vs app identity coexist
- **Color** — semantic tokens, dynamic colors, vibrancy, accent colors
- **Dark Mode** — not just inverted; separate semantic palette
- **Icons (SF Symbols)** — 5000+ symbols, 9 weights x 3 scales, variable color, automatic alignment to text baseline
- **Images** — @1x/@2x, template images, symbol images
- **Inclusion** — gender-neutral language, diverse imagery, cultural sensitivity
- **Layout** — safe areas, margins, alignment guides, adaptive layout
- **Materials** — blur, vibrancy, translucency as semantic layers
- **Motion** — spring animations, meaningful transitions, reduce-motion fallbacks
- **Right to left** — full mirroring, not just text alignment
- **SF Symbols** — the actual icon system (separate from static image icons)
- **Typography** — SF Pro, SF Mono, SF Pro Rounded, New York, dynamic type

### Patterns
- **Accessing private data** — permission prompts, usage descriptions
- **Charting data** — marks, axes, accessibility for charts
- **Collaboration** — shared documents, presence indicators
- **Drag and drop** — spring-loading, drop proposals, visual feedback
- **Entering data** — text fields, steppers, pickers, validation
- **File management** — open/save panels, recent documents, tags
- **Going full screen** — title bar behavior, menu bar auto-hide
- **Launching** — launch images, restoration, handoff
- **Live activities** — (iOS-primary but macOS has equivalents via widgets)
- **Loading** — progress indicators, skeleton screens
- **Managing accounts** — Sign in with Apple, account deletion
- **Managing notifications** — permission, grouping, actions
- **Modality** — sheets, alerts, popovers, inspectors
- **Multitasking** — Mission Control, Spaces, Stage Manager, Split View
- **Offering help** — onboarding, tips, contextual help
- **Onboarding** — first-launch experience
- **Printing** — print dialog, page setup
- **Ratings and reviews** — (App Store context)
- **Searching** — search fields, search scopes, suggestions
- **Settings** — Preferences window, tabs, organization
- **Sharing** — share sheet, extensions
- **Undo and redo** — Cmd-Z/Cmd-Shift-Z, undo grouping, undo stack

### Components
- **Content**
  - Charts, Image views, Text views, Web views
- **Layout and organization**
  - Boxes, Collections, Column views, Disclosure groups, Grids
  - Group boxes, Labels, Lists (outline views), Scroll views
  - Section headers, Split views, Tab views, Tables
- **Menus and actions**
  - Activity views, Buttons, Context menus, Dock menus
  - Edit menus, Menu bars, Pop-up buttons, Pull-down buttons
  - Segmented controls, Shortcut menus, Toolbars
- **Navigation and search**
  - Navigation bars (in Catalyst), Outline views, Path controls
  - Search fields, Sidebars, Tab bars, Token fields
- **Presentation**
  - Alerts, File exporters/importers, Inspectors, Panels
  - Popovers, Sheets, Windows
- **Selection and input**
  - Color wells, Combo boxes, Date pickers, Image wells
  - Sliders, Steppers, Text fields, Toggle buttons
  - Checkboxes, Radio buttons, Switches
- **Status**
  - Activity rings, Gauges, Progress indicators
- **System experiences**
  - Complications, Home Screen widgets, Live Activities
  - Menu bar extras, Notifications, Status items, Widgets
- **Controls (by visual type)**
  - Bordered buttons, Borderless buttons, Bevel buttons
  - Help buttons, Gradient buttons, Disclosure triangles
  - Pop-up buttons, Push buttons, Radio buttons

---

## 2. Typography System

### Font Families

**SF Pro** (system default)
- Variable font with optical sizing
- Two optical size axes: Text (for <20pt) and Display (for >=20pt)
- Automatic optical size switching at 20pt boundary
- Text variant: slightly wider letter spacing, more open shapes
- Display variant: tighter spacing, refined details

**SF Pro Rounded**
- Same metrics as SF Pro but with rounded stroke terminals
- Used for informal/friendly UI contexts (e.g., certain widget styles)

**SF Mono**
- Monospaced companion
- Same vertical metrics as SF Pro for mixing in UI
- 6 weights: Light, Regular, Medium, Semibold, Bold, Heavy

**New York**
- Serif companion (used in Books, News)
- 4 optical sizes: Small, Regular, Medium, Large
- Transitional serif design

### macOS System Font Sizes (AppKit NSFont System Sizes)

These are the actual values from `NSFont.systemFont(ofSize:)` and the
named text styles. All values are in points at @1x (multiply by 2 for
Retina pixels).

| Text Style        | Size (pt) | Weight      | Leading (pt) | Usage                        |
|-------------------|-----------|-------------|---------------|------------------------------|
| Large Title       | 26        | Regular     | 32            | Window/view titles           |
| Title 1           | 22        | Regular     | 26            | Section headers              |
| Title 2           | 17        | Regular     | 22            | Subsection headers           |
| Title 3           | 15        | Regular     | 20            | Tertiary headers             |
| Headline          | 13        | Bold        | 16            | Emphasized body              |
| Subheadline       | 11        | Regular     | 14            | Below headline               |
| Body              | 13        | Regular     | 16            | Default reading text         |
| Callout           | 12        | Regular     | 15            | Annotations                  |
| Footnote          | 10        | Regular     | 13            | Secondary info               |
| Caption 1         | 10        | Regular     | 13            | Image/table captions         |
| Caption 2         | 10        | Medium      | 13            | Bold captions                |

**Critical note**: macOS uses SMALLER sizes than iOS. iOS body = 17pt,
macOS body = 13pt. This reflects mouse-precision vs finger-precision.

### System Font Sizes (NSFont class methods)

| Method                        | Size (pt) |
|-------------------------------|-----------|
| systemFontSize                | 13        |
| smallSystemFontSize           | 11        |
| labelFontSize                 | 10        |
| systemFont (menu)             | 14        |
| toolTipsFontSize              | 11        |
| controlContentFontSize        | 12        |
| headerFontSize                | 13        |
| menuFontSize                  | 14        |
| menuBarFontSize               | 14        |
| messageFontSize               | 13        |
| paletteFontSize               | 11        |
| titleBarFontSize              | 13        |

### SF Pro Weights

9 weights, matching iOS:

| Weight Name    | CSS Equivalent | SF Pro Value |
|----------------|----------------|--------------|
| Ultralight     | 100            | -0.80        |
| Thin           | 200            | -0.60        |
| Light          | 300            | -0.40        |
| Regular        | 400            |  0.00        |
| Medium         | 500            |  0.23        |
| Semibold       | 600            |  0.30        |
| Bold           | 700            |  0.40        |
| Heavy          | 800            |  0.56        |
| Black          | 900            |  0.62        |

### Tracking (Letter Spacing)

SF Pro has built-in tracking tables that vary by size. Apple's
`CTFontGetAdvancesForGlyphs` applies these automatically. Approximate
values extracted from the font tables:

| Size (pt) | Tracking (em) | Notes                   |
|-----------|---------------|-------------------------|
| 6         | +0.060        | Very loose at tiny sizes |
| 9         | +0.032        |                         |
| 10        | +0.024        |                         |
| 11        | +0.020        |                         |
| 12        | +0.016        |                         |
| 13        | +0.012        | Body text               |
| 14        | +0.007        |                         |
| 17        | -0.005        |                         |
| 19        | -0.009        | Optical size switch zone |
| 20        | -0.011        |                         |
| 22        | -0.016        |                         |
| 26        | -0.022        | Large Title             |
| 34        | -0.028        |                         |
| 48        | -0.034        |                         |
| 64        | -0.038        |                         |
| 96        | -0.042        | Hero/display text       |

**Pattern**: Small sizes get positive tracking (looser), large sizes get
negative tracking (tighter). The crossover is around 14-17pt.

### Line Height / Leading

Apple uses "leading" (pronounced "ledding") in the typographic sense.
The system applies default leading based on font metrics:

- **Body text (13pt)**: ~16pt line height (1.23x)
- **Display text (26pt)**: ~32pt line height (1.23x)
- **General formula**: approximately 1.2x-1.3x the point size
- **Paragraph spacing**: typically 0.5x-1.0x the font size between paragraphs

### Font Rendering

- **No subpixel antialiasing** since macOS Mojave (10.14)
- Grayscale antialiasing only
- Font smoothing preference removed in Catalina
- Core Text handles hinting; TrueType hints are respected but Apple
  fonts use minimal hinting (rely on rasterizer quality)

---

## 3. Color System

### Semantic Color Tokens (NSColor)

macOS defines colors by ROLE, not by value. These adapt automatically
to light/dark mode, accent color, increased contrast, and vibrancy context.

#### Label Colors (Text Hierarchy)
| Token                    | Light Mode          | Dark Mode           | Purpose                   |
|--------------------------|---------------------|---------------------|---------------------------|
| labelColor               | #000000D9 (85%)     | #FFFFFFD9 (85%)     | Primary text              |
| secondaryLabelColor      | #0000008C (55%)     | #FFFFFF8C (55%)     | Secondary text            |
| tertiaryLabelColor       | #00000042 (26%)     | #FFFFFF42 (26%)     | Tertiary text             |
| quaternaryLabelColor     | #00000019 (10%)     | #FFFFFF19 (10%)     | Quaternary text           |

**Pattern**: Four-level text hierarchy using alpha on black/white base.
Not separate gray values -- alpha compositing against background.

#### Text Colors
| Token                    | Light Mode          | Dark Mode           |
|--------------------------|---------------------|---------------------|
| textColor                | #000000             | #FFFFFF             |
| placeholderTextColor     | #00000040           | #FFFFFF40           |
| selectedTextColor        | #000000             | #FFFFFF             |
| textBackgroundColor      | #FFFFFF             | #1E1E1E             |
| selectedTextBackgroundColor | Accent-derived   | Accent-derived      |

#### Content Colors
| Token                    | Light Mode          | Dark Mode           | Purpose                   |
|--------------------------|---------------------|---------------------|---------------------------|
| linkColor                | #0068DA             | #419CFF             | Hyperlinks                |
| separatorColor           | #00000019 (10%)     | #FFFFFF19 (10%)     | Lines between items       |
| selectedContentBackgroundColor | Accent color  | Accent color        | Active list selection     |
| unemphasizedSelectedContentBackgroundColor | #DCDCDC | #464646 | Inactive list selection   |

#### Background Colors
| Token                    | Light Mode          | Dark Mode           | Purpose                   |
|--------------------------|---------------------|---------------------|---------------------------|
| windowBackgroundColor    | #ECECEC             | #323232             | Window fill               |
| underPageBackgroundColor | #969696             | #282828             | Behind scrollable content |
| controlBackgroundColor   | #FFFFFF             | #1E1E1E             | Text field/list bg        |
| windowFrameTextColor     | #000000D9           | #FFFFFFD9           | Title bar text            |

#### Control Colors
| Token                    | Light Mode          | Dark Mode           |
|--------------------------|---------------------|---------------------|
| controlColor             | #FFFFFF             | #FFFFFF19           |
| controlTextColor         | #000000D9           | #FFFFFFD9           |
| disabledControlTextColor | #00000040           | #FFFFFF40           |
| selectedControlColor     | Accent-derived      | Accent-derived      |
| selectedControlTextColor | #000000             | #FFFFFF             |
| alternatingContentBackgroundColors[0] | #FFFFFF | #1E1E1E            |
| alternatingContentBackgroundColors[1] | #F4F5F5 | #FFFFFF0A          |

#### Grid and Table Colors
| Token                    | Light Mode          | Dark Mode           |
|--------------------------|---------------------|---------------------|
| gridColor                | #E6E6E6             | #1A1A1A             |
| headerColor              | #FFFFFF80           | #FFFFFF10           |
| headerTextColor          | #000000D9           | #FFFFFFD9           |

### System Accent Colors

macOS supports a user-selected accent color. The system defines these
named accent color options:

| Name        | Light Hex  | Dark Hex   |
|-------------|------------|------------|
| Blue        | #007AFF    | #0A84FF    |
| Purple      | #AF52DE    | #BF5AF2    |
| Pink        | #FF2D55    | #FF375F    |
| Red         | #FF3B30    | #FF453A    |
| Orange      | #FF9500    | #FF9F0A    |
| Yellow      | #FFCC00    | #FFD60A    |
| Green       | #28CD41    | #30D158    |
| Graphite    | #8E8E93    | #98989D    |
| Multicolor  | (per-app)  | (per-app)  |

The accent color is used for:
- Selection highlights
- Button tints
- Focus rings (3pt outside stroke, accent color, 50% opacity)
- Checkboxes, radio buttons, toggles (filled state)
- Progress bars (determinate fill)
- Slider thumbs and filled track

### System Tint Colors (Fixed purpose)

| Name                | Light Hex  | Dark Hex   | Usage                    |
|---------------------|------------|------------|--------------------------|
| systemRed           | #FF3B30    | #FF453A    | Destructive/errors       |
| systemOrange        | #FF9500    | #FF9F0A    | Warnings                 |
| systemYellow        | #FFCC00    | #FFD60A    | Caution                  |
| systemGreen         | #28CD41    | #30D158    | Success/positive         |
| systemMint          | #00C7BE    | #63E6E2    | Fresh accent             |
| systemTeal          | #30B0C7    | #40C8E0    | Communication            |
| systemCyan          | #32ADE6    | #64D2FF    | Links/interactive        |
| systemBlue          | #007AFF    | #0A84FF    | Primary actions          |
| systemIndigo        | #5856D6    | #5E5CE6    | Secondary accent         |
| systemPurple        | #AF52DE    | #BF5AF2    | Creative/rich            |
| systemPink          | #FF2D55    | #FF375F    | Social/playful           |
| systemBrown         | #A2845E    | #AC8E68    | Warm neutral             |
| systemGray          | #8E8E93    | #98989D    | Neutral                  |

### Dark Mode Design Rules

- NOT simple color inversion
- Dark mode backgrounds use elevated luminance for raised surfaces:
  - Base level: ~#1E1E1E (RGB 30,30,30)
  - Elevated (sheet/popover): ~#2C2C2E (RGB 44,44,46)
  - Further elevated: ~#3A3A3C (RGB 58,58,60)
- Shadows are effectively invisible in dark mode; depth is conveyed by
  surface luminance, not shadow
- Colors are slightly desaturated in light mode, slightly more vivid in
  dark mode (to maintain perceived vibrancy against dark backgrounds)
- The semantic token system means apps using system colors get dark mode
  free -- no manual color mapping needed

### Accent Color Adaptation

When a user changes the system accent color, the following change:
- Selection highlights
- Default button fill
- Focus rings
- Checkboxes, radio buttons, switches (active state)
- Stepper/slider interactive elements
- Sidebar selection background
- Calendar, Reminders, and other Apple apps' tint

What does NOT change:
- Red for destructive actions (always systemRed)
- Traffic light colors (always red/yellow/green)
- Status badge colors
- Error/warning/success semantic colors

---

## 4. Spacing and Layout Grid

### The 4pt / 8pt Grid

macOS uses a **4-point base unit** with most spacing being multiples of 4.
At @2x Retina, this is an 8-pixel grid. Apple is not strict about powers
of 2, but recurring values are:

| Token Name        | Value | Usage                                  |
|-------------------|-------|----------------------------------------|
| Micro spacing     | 2pt   | Between icon and label in compact UI   |
| Small spacing     | 4pt   | Minimum padding, between related items |
| Default spacing   | 8pt   | Standard content padding               |
| Medium spacing    | 12pt  | Between groups of related controls     |
| Large spacing     | 16pt  | Section separation                     |
| Extra large       | 20pt  | Major section breaks                   |

### Specific Component Spacing

**Window content margins**:
- Standard window: 20pt from window edge to content
- Sidebar: 0pt horizontal (full-bleed), items have 10pt horizontal padding
- Toolbar: 12pt from edges, 8pt between items

**List/Table rows**:
- Standard row height: 24pt (small), 28pt (medium), 34pt (large)
- Row padding: 4pt vertical, 10pt horizontal
- Between sections: 8pt

**Form layout**:
- Label-to-control spacing: 8pt
- Between form rows: 8pt (compact) to 12pt (standard)
- Control width: minimum 100pt for text fields
- Button height: 22pt (small), 28pt (regular), 32pt (large)
- Segmented control height: 24pt

**Button padding**:
- Push button: 12pt horizontal, 4pt vertical (minimum)
- Default (blue) button: same sizing, filled background
- Toolbar button: 8pt padding, 28x28pt touch target

**Popover/Sheet margins**:
- Content inset: 20pt all sides
- Between title and content: 12pt
- Between content and buttons: 20pt
- Button spacing: 12pt between buttons
- Cancel on left, default action on right (reversed from Windows)

### Safe Areas and Insets

**Title bar**: 28pt standard, 52pt when unified with toolbar
**Tab bar**: 28pt
**Sidebar width**: 220pt default, 180pt minimum, user-resizable
**Inspector width**: 260pt typical
**Content minimum width**: varies by app, typically 400pt

### The Alignment System

macOS uses **baseline alignment** for text, not bounding-box alignment:
- Labels align to the text baseline of adjacent controls
- This means a label next to a text field aligns to the text inside the
  field, not to the field's bounding box
- This is a MAJOR differentiator from CSS-style layout (which aligns boxes)

Auto Layout constraints in Interface Builder/SwiftUI encode this:
- `.firstBaseline` and `.lastBaseline` alignment anchors
- Baseline-to-baseline spacing, not frame-to-frame

---

## 5. Animation and Motion

### The Spring Animation System

macOS (and all Apple platforms) moved from duration-based bezier
animations to **spring-based physics animations** starting with iOS 7 and
progressively through macOS releases. As of macOS Ventura+, springs are
the default for virtually all system animations.

### CASpringAnimation Parameters

`CASpringAnimation` (Core Animation) defines springs with physical parameters:

| Parameter          | Type    | Description                          |
|--------------------|---------|--------------------------------------|
| mass               | CGFloat | Mass of the object (default: 1.0)    |
| stiffness          | CGFloat | Spring stiffness (default: 100.0)    |
| damping            | CGFloat | Damping coefficient (default: 10.0)  |
| initialVelocity    | CGFloat | Starting velocity (default: 0.0)     |
| settlingDuration   | CGFloat | Computed time to rest (read-only)    |

**Default spring**: mass=1, stiffness=100, damping=10
- This is an underdamped spring (oscillates before settling)
- Settling duration: approximately 0.5s
- Damping ratio: damping / (2 * sqrt(stiffness * mass)) = 10 / (2 * sqrt(100)) = 0.5

### SwiftUI Spring Presets (macOS 14+)

SwiftUI introduced named spring presets:

| Preset               | Response | Damping Fraction | Blend Duration | Character         |
|----------------------|----------|------------------|----------------|-------------------|
| .smooth              | 0.5s     | 1.0              | 0.0            | No bounce, fluid  |
| .snappy              | 0.5s     | 1.0              | 0.15           | Crisp arrival     |
| .bouncy              | 0.5s     | 0.7              | 0.0            | Visible bounce    |
| .spring (default)    | 0.55s    | 1.0              | 0.0            | Slightly slower   |
| .interactiveSpring   | 0.15s    | 0.86             | 0.0            | Very responsive   |
| .interpolatingSpring | varies   | varies           | 0.0            | Custom             |

**SwiftUI spring parameter mapping**:
- `response` = approximate duration of the animation (in seconds)
- `dampingFraction` = 0.0 (no damping, infinite oscillation) to 1.0 (critically damped, no overshoot) and above (overdamped)
- `blendDuration` = time to blend from a running animation to this one

### Specific macOS System Animation Parameters

**Window resize**: spring, response ~0.35s, critically damped
**Window minimize (Genie effect)**: ~0.5s duration, custom bezier
**Window minimize (Scale effect)**: ~0.3s, ease-in-out
**Sheet presentation**: spring, response ~0.35s, slight bounce (damping ~0.85)
**Popover appear**: spring, response ~0.3s, critically damped with slight overshoot
**Menu appear**: ~0.15s, ease-out (no spring -- menus are immediate)
**Sidebar show/hide**: spring, response ~0.25s, critically damped
**Toolbar item rearrange**: spring, response ~0.3s, bouncy (damping ~0.7)
**Mission Control spread**: spring, response ~0.4s, critically damped
**Spaces swipe**: spring, tracks gesture velocity, critically damped
**Dock magnification**: ~0.1s, no spring (direct tracking + slight lag)
**Launchpad grid appear**: spring, response ~0.4s, staggered per icon (~20ms delay)
**Notification slide-in**: spring, response ~0.35s, from right edge
**Alert/dialog appear**: spring, response ~0.25s, scale from 0.95 to 1.0 + fade

### macOS Classic Timing Curves (Pre-spring, still used in some contexts)

| Name                  | Control Points (cubic-bezier) | Duration | Usage              |
|-----------------------|-------------------------------|----------|--------------------|
| easeInOut (default)   | (0.42, 0.0, 0.58, 1.0)       | 0.25s    | General transitions|
| easeIn                | (0.42, 0.0, 1.0, 1.0)        | 0.2s     | Exit animations    |
| easeOut               | (0.0, 0.0, 0.58, 1.0)        | 0.2s     | Enter animations   |
| linear                | (0.0, 0.0, 1.0, 1.0)         | varies   | Progress bars      |
| Apple ease (custom)   | (0.25, 0.1, 0.25, 1.0)       | 0.3s     | System transitions |

### The Physics of macOS Animation

What makes macOS animation feel different from Windows/Linux:

1. **Velocity continuity**: If you interrupt a running animation, the new
   animation picks up from the current velocity. No abrupt stops. This is
   what `blendDuration` handles in SwiftUI, and what makes dragging feel
   smooth when you release.

2. **Gesture-driven animation**: Swipe gestures (Spaces, Mission Control,
   back/forward in Safari) directly drive animation progress via gesture
   recognizer. The animation is NOT triggered at gesture end -- it tracks
   the finger 1:1 during the gesture, then springs to the final position
   on release based on release velocity.

3. **Settle, don't stop**: Animations settle to their target with
   exponential decay, not a hard stop. Even "critically damped" springs
   approach asymptotically. The OS uses a settling threshold of ~0.01
   (1% of total distance) to determine when to snap to final value.

4. **Mass varies by element size**: Larger elements (windows) animate
   with more perceived mass (longer response time, less bounce). Smaller
   elements (buttons, toggles) respond with less mass (faster, snappier).
   This is implicit in the response parameter -- not a mass simulation,
   but tuned per-element.

5. **Reduced Motion fallback**: When "Reduce motion" is on, ALL spring
   animations are replaced with crossfade (opacity 0->1 or 1->0) at
   ~0.25s duration. Zoom transitions become fade transitions.
   Mission Control uses instant layout instead of animated spread.

---

## 6. Spatial Model

### Z-Ordering (Depth Stack)

macOS has a strict depth hierarchy, from back to front:

| Layer          | Z-Level           | Description                             |
|----------------|-------------------|-----------------------------------------|
| Desktop        | 0 (CGWindowLevel) | Wallpaper + desktop widgets (Sonoma+)   |
| Normal windows | 0 (kCGNormalWindow)| Standard app windows                   |
| Floating panels| 3 (kCGFloating)   | Utility/inspector windows               |
| Torn-off menus | 3                 | Menus detached from menu bar            |
| Modal dialogs  | 8 (kCGModalPanel) | Application-modal sheets/alerts         |
| Main menu      | 24 (kCGMainMenu)  | The menu bar                            |
| Status items   | 25 (kCGStatusWindow)| Menu bar extras / status items        |
| Dock           | 20 (kCGDock)      | The Dock                                |
| Pop-up menus   | 101 (kCGPopUpMenu)| Context menus, pop-up menus            |
| Overlay        | 102 (kCGOverlay)  | Drag images, selection rectangles       |
| Screen saver   | 1000 (kCGScreenSaver) | Screen saver layer                |
| Notification   | 18                | Notification Center banners             |
| Cursor         | Maximum           | Always on top                           |

**Key rule**: Within a level, windows are ordered by most-recently-focused.
Across levels, the hierarchy is absolute. A normal window can NEVER
appear above a modal panel.

### Materials and Vibrancy

Materials are the translucent/frosted-glass layers that give macOS its
depth. They are NOT just blur -- they are composited layers with specific
optical properties.

#### NSVisualEffectView Materials (AppKit)

| Material                    | Blur Radius | Tint        | Usage                        |
|-----------------------------|-------------|-------------|------------------------------|
| .titlebar                   | ~30px       | Light/dark  | Window title bar             |
| .selection                  | ~20px       | Accent tint | Selected rows in tables      |
| .menu                       | ~50px       | Translucent | Menus and menu bar           |
| .popover                    | ~50px       | Light/dark  | Popover backgrounds          |
| .sidebar                    | ~30px       | Light/dark  | Source list sidebars         |
| .headerView                 | ~20px       | Light/dark  | Table header rows            |
| .sheet                      | ~30px       | Light/dark  | Sheet overlays               |
| .windowBackground           | ~30px       | Light/dark  | Window background fill       |
| .hudWindow                  | ~30px       | Dark tint   | HUD overlays                 |
| .fullScreenUI               | ~50px       | Dark tint   | Full-screen overlays         |
| .toolTip                    | ~15px       | Yellow-tint | Tooltip backgrounds          |
| .contentBackground          | ~20px       | Light/dark  | Content area background      |
| .underWindowBackground      | ~80px       | Heavy blur  | Behind the window            |
| .underPageBackground        | ~50px       | Light tint  | Behind scrollable content    |

#### Blending Modes

| Mode            | Description                                              |
|-----------------|----------------------------------------------------------|
| .behindWindow   | Blurs content BEHIND the window (desktop, other windows) |
| .withinWindow   | Blurs content WITHIN the same window (behind the view)   |

#### Vibrancy

Vibrancy is NOT the same as blur. Vibrancy uses compositing blend modes
to make foreground content (text, icons) appear to be part of the
background material, rather than painted on top of it.

**How it works**:
1. Background is blurred and tinted (the material)
2. Foreground content is drawn using a special blend mode:
   - In light appearance: foreground brightens the background
   - In dark appearance: foreground darkens the background
3. The foreground inherits visual properties of whatever is behind it

**Vibrancy states**:
| State           | Label alpha | Separator alpha | Visual character          |
|-----------------|-------------|-----------------|---------------------------|
| .emphasized     | Higher      | Higher          | High contrast on material |
| default         | Medium      | Medium          | Standard vibrancy         |
| inactive        | Lower       | Lower           | Window not focused        |

**Why vibrancy matters for implementation**: You cannot fake vibrancy
with CSS `backdrop-filter: blur()`. True vibrancy requires:
1. Access to the pixels behind the window (compositor-level)
2. Per-element blend mode compositing
3. Automatic adjustment based on the content being blurred

#### Shadow System

macOS windows cast layered shadows:

**Standard window shadow**:
- Offset: 0pt horizontal, 10pt vertical (downward)
- Blur radius: 30pt
- Color: #00000033 (black at 20% opacity)
- Additional rim shadow: 0pt offset, 1pt blur, #00000019 (10%)

**Popover shadow**: larger radius (~40pt), more offset
**Menu shadow**: 0pt horizontal, 8pt vertical, 20pt blur, 25% opacity
**Sheet shadow**: minimal (sheet is attached to window)
**HUD shadow**: softer, larger spread

**Dark mode shadows**: same parameters but much less visible because
the background is already dark. Depth is communicated via elevated
surface color, not shadow.

---

## 7. Window Anatomy

### Standard Window

```
+-------------------------------------------------------+
|  [O] [O] [O]   Title Text              [Toolbar items] |  <- Title bar (28pt)
+-------------------------------------------------------+
|         |                                   |          |
| Sidebar | Content Area            | Inspector|          |
| (220pt) |                         | (260pt)  |          |
|         |                                   |          |
|         |                                   |          |
+-------------------------------------------------------+
```

### Title Bar (Traffic Lights)

**Traffic light buttons** (window controls):
- Size: 12pt diameter each (12x12pt)
- Spacing: 8pt between centers (so 20pt left edge to center of first, then 8pt gaps)
- Position: 7pt from left edge, vertically centered in title bar
- Inset from window top: 8pt to center of buttons

| Button | Position | Inactive  | Hover         | Active             |
|--------|----------|-----------|---------------|--------------------|
| Close  | Left     | #FF5F57   | #FF5F57 + X   | Close window       |
| Minimize| Center  | #FEBC2E   | #FEBC2E + -   | Minimize to Dock   |
| Zoom   | Right    | #28C840   | #28C840 + +   | Full screen / zoom |

**When window is inactive**: all three become #DCDCDC (gray dot)
**When hovered**: symbols (x, -, +) appear inside the dots
**Spacing from left edge**: first button center at 20pt, second at 40pt, third at 60pt

### Title Bar Variants

| Variant              | Height | Description                          |
|----------------------|--------|--------------------------------------|
| Standard             | 28pt   | Title text, traffic lights           |
| Unified toolbar      | 52pt   | Title bar merged with toolbar        |
| Transparent          | 28pt   | Blends with content (Safari, Maps)   |
| No title bar         | 0pt    | Full content window (media players)  |
| Tabbed               | 28pt   | With tabs in title bar area          |

### Toolbar

**Standard toolbar**:
- Height: 38pt (icons only), 52pt (icons + labels), 28pt (text only)
- Item size: 28x28pt (icon), with 8pt horizontal spacing
- Overflow: chevron menu when window is too narrow
- Customizable: Cmd+click to rearrange, View > Customize Toolbar
- Separator: 1pt wide, 16pt tall, 8pt margins

**Toolbar style** (macOS Monterey+):
| Style      | Appearance                                        |
|------------|---------------------------------------------------|
| .automatic | System decides based on window configuration       |
| .expanded  | Title bar and toolbar are separate rows             |
| .unified   | Title bar and toolbar merge into single row         |
| .unifiedCompact | Unified but reduced height                    |

### Sidebar

**Anatomy**:
- Width: 220pt default, 180pt minimum, user-resizable via divider
- Background: `.sidebar` material (vibrancy)
- Row height: 24pt
- Icon size: 18x18pt (SF Symbol)
- Icon-to-label spacing: 6pt
- Left padding: 10pt
- Section headers: 11pt bold, uppercase, #secondaryLabelColor
- Selected row: accent color background, white text, rounded rect (4pt radius)
- Disclosure triangles: 10x10pt

### Content Area

**Scroll behavior**:
- Scroll bars: overlay style by default (appear on scroll, fade after 1.5s)
- Scroll bar width: 8pt (overlay), 15pt (always visible, legacy)
- Scroll bar track: transparent (overlay), light gray (legacy)
- Scroll indicator: rounded capsule, 6pt wide, dark gray semi-transparent
- Elastic overscroll: spring animation, ~50pt maximum displacement
- Rubber-banding at content edges (physics-based, decelerating)

### Inspector (Detail) Panel

- Width: 260pt typical, right-side of window
- Separated by 1pt divider
- Same vibrancy options as sidebar
- Collapsible via toolbar button or keyboard shortcut

---

## 8. Interaction Patterns

### Cursor States

macOS defines these cursor types (NSCursor):

| Cursor               | Appearance     | Context                              |
|----------------------|----------------|--------------------------------------|
| .arrow               | Default arrow  | General pointing                     |
| .iBeam               | I-beam         | Over editable text                   |
| .crosshair           | Crosshair      | Precision selection (graphics)       |
| .pointingHand        | Hand pointer   | Over clickable links/buttons         |
| .openHand            | Open hand      | Ready to drag (grab)                 |
| .closedHand          | Closed hand    | Actively dragging                    |
| .resizeLeft          | Left arrow     | Left edge resize                     |
| .resizeRight         | Right arrow    | Right edge resize                    |
| .resizeLeftRight     | Double arrow   | Horizontal splitter/resize           |
| .resizeUp            | Up arrow       | Top edge resize                      |
| .resizeDown          | Down arrow     | Bottom edge resize                   |
| .resizeUpDown        | Double arrow   | Vertical splitter/resize             |
| .disappearingItem    | Poof           | Drag-to-remove (toolbar items)       |
| .operationNotAllowed | Circle-slash   | Invalid drop target                  |
| .dragLink            | Arrow+badge    | Create link by dragging              |
| .dragCopy            | Arrow+green+   | Copy by dragging                     |
| .contextualMenu      | Arrow+menu     | Context menu available               |

**Cursor size**: default ~22x22pt at @1x, user-adjustable up to 4x in accessibility

### Click Interactions

**Single click**: Select / activate
**Double click**: Open / edit (e.g., rename in Finder, open file)
**Triple click**: Select line/paragraph (in text)
**Click-and-hold**: Opens menu on toolbar items, activates spring-loading on folders
**Right click / Control-click**: Context menu
**Option-click**: Alternative action (e.g., Option-click close = close all windows)
**Cmd-click**: Non-contiguous selection in lists

### Drag and Drop

**Initiation**: Mouse down + 4pt movement threshold
**Visual feedback**:
- Dragged item becomes semi-transparent (~70% opacity)
- A snapshot image follows the cursor with 10pt offset
- Drop target highlights with accent color border (2pt, rounded)
- Invalid targets show .operationNotAllowed cursor

**Spring-loading**: Hover over a folder/container during drag for 0.5s,
it opens (springs). This is recursive -- you can spring-load through
nested folders.

**Drag types**:
- Move (default within same context)
- Copy (hold Option key -- green + badge)
- Link (hold Cmd+Option -- link badge)
- Delete (drag to Trash, or off toolbar -- poof animation)

**Poof animation**: A small cloud/poof particle effect at the cursor
position when dragging an item to remove it (e.g., removing toolbar items).
Duration ~0.3s, 5 expanding/fading circles.

### Keyboard Shortcuts Architecture

macOS has a LAYERED shortcut system:

| Modifier | Purpose             | Examples                            |
|----------|---------------------|-------------------------------------|
| Cmd      | Primary commands    | Cmd-C, Cmd-V, Cmd-Q, Cmd-W         |
| Cmd-Shift| Secondary commands  | Cmd-Shift-S (Save As)               |
| Cmd-Opt  | Alternate commands  | Cmd-Opt-Esc (Force Quit)            |
| Ctrl     | Control shortcuts   | Ctrl-Tab (next tab)                 |
| Fn       | Function keys       | F11 (Show Desktop)                  |

**System-reserved shortcuts** (apps cannot override):
- Cmd-Tab: App switcher
- Cmd-Q: Quit application
- Cmd-H: Hide application
- Cmd-M: Minimize window
- Cmd-W: Close window/tab
- Cmd-,: Preferences/Settings
- Cmd-Space: Spotlight
- Cmd-Shift-3/4/5: Screenshots

### Undo System

**Undo architecture**:
- NSUndoManager per document/window
- Unlimited undo depth (memory-limited)
- Grouped undo (typing groups into single undo until pause)
- Typing coalescing: continuous typing is one undo group; pause >1s starts new group
- Cmd-Z: Undo, Cmd-Shift-Z: Redo
- Edit menu shows "Undo [action name]" -- action-specific labels

### Selection Patterns

**Text selection**:
- Click to place insertion point
- Shift-click to extend selection
- Double-click to select word
- Triple-click to select paragraph (some apps: line)
- Cmd-A to select all
- Selection color: accent color at ~50% opacity

**List/Grid selection**:
- Click: select single
- Shift-click: range select (from last selected to clicked)
- Cmd-click: toggle individual item in selection
- Cmd-A: select all
- Arrow keys: move selection
- Shift-Arrow: extend selection
- Type-to-select: type characters to jump to matching item

### Focus System

**Focus ring**: 3pt rounded rectangle outside the focused control,
accent color, ~50% opacity. For controls with rounded corners, the
focus ring follows the corner radius + 3pt offset.

**Tab order**: Left to right, top to bottom by default. Custom via
`nextKeyView` chain.

**Full Keyboard Access** (accessibility feature that becomes structural):
- Tab moves between ALL controls (not just text fields)
- Space activates focused button
- Arrow keys navigate within segmented controls, radio groups
- This is OFF by default, ON via System Settings > Keyboard

### Haptic Feedback (MacBook trackpad)

macOS uses the Taptic Engine for:
- **Alignment snaps**: When dragging an object near a guide/edge -- light tap
- **Increment changes**: Rotating through picker values -- light tap per value
- **Action confirmation**: Completing a drag-to-Trash -- firm tap
- **Rubber band**: Reaching scroll boundary -- resistance tap

NSHapticFeedbackManager patterns:
| Pattern    | Intensity | Usage                          |
|------------|-----------|--------------------------------|
| .alignment | Light     | Snapping to position           |
| .levelChange| Medium   | Discrete value change          |
| .generic   | Varies    | General feedback               |

---

## 9. Structural Accessibility

These features are NOT bolt-on. They are architectural decisions that
permeate the entire system.

### VoiceOver (Screen Reader)

- **Rotor navigation**: Two-finger rotate gesture cycles through navigation modes (headings, links, form controls, tables)
- **Cursor tracking**: VoiceOver cursor is SEPARATE from mouse cursor and keyboard focus (three independent focus systems)
- **Semantic roles**: Every NSView subclass declares its accessibility role (AXButton, AXTextField, AXTable, AXImage, etc.)
- **Accessibility hierarchy**: Mirrors but is NOT identical to the view hierarchy. An NSView can expose child elements that are not subviews.
- **Actions**: Each element declares its supported actions (AXPress, AXIncrement, AXDecrement, AXShowMenu, etc.)
- **Notifications**: The accessibility system posts notifications when state changes (AXValueChanged, AXFocusedUIElementChanged, AXSelectedRowsChanged, etc.)

### Dynamic Type on macOS

macOS supports Dynamic Type (user-adjustable text size) system-wide:
- System Settings > Accessibility > Display > Text Size
- Range: ~85% to ~150% of default (7 stops)
- Apps using system text styles automatically respond
- Custom fonts can participate via `UIFontMetrics` (Catalyst) or manual scaling

### Color and Contrast

**Increase Contrast mode**:
- Reduces transparency (materials become opaque)
- Increases border visibility (1pt borders become more prominent)
- Buttons get visible borders (normally borderless in standard mode)
- Increases text/background contrast ratio
- System colors shift to higher-contrast variants automatically

**Differentiate Without Color**:
- When enabled, the system adds shapes/symbols alongside color coding
- Red circles get an X, green circles get a checkmark
- Charts add patterns in addition to colors

**Reduce Transparency**:
- All materials become opaque (solid background color)
- Sidebar becomes solid gray instead of translucent
- Menu bar becomes solid instead of blurred
- Title bar becomes solid

### Motion Accessibility

**Reduce Motion**:
- Slide transitions become crossfades
- Zoom effects become opacity fades
- Parallax effects are disabled
- Auto-playing animations in content are paused
- The Dock zoom effect is disabled
- Mission Control uses instant layout

**Auto-play settings**:
- System-wide preference for auto-playing video/animation
- Websites can read `prefers-reduced-motion` media query

### Pointer Accessibility

- **Cursor size**: Adjustable from 1x to 4x (accessibility settings)
- **Shake to locate**: Rapidly wiggle mouse to temporarily enlarge cursor
- **Head pointer**: Use head movement to control cursor (via camera)
- **Dwell control**: Pause cursor to click
- **Alternative input**: Switch Control, Voice Control, Eye Tracking (macOS Monterey+)

### Keyboard Accessibility

- **Sticky Keys**: Modifier keys stay active after single press (for one-handed typing)
- **Slow Keys**: Adjustable key acceptance delay (filter accidental presses)
- **Full Keyboard Access**: Tab through all controls, not just text fields
- **Custom keyboard shortcuts**: System-wide, per-app, re-bindable for any menu item

### What Makes This STRUCTURAL

The key insight: macOS accessibility is structural because:

1. **Every NSView is accessible by default**. You have to actively break accessibility, not build it. Text labels, buttons, sliders all declare their roles automatically.

2. **The accessibility API is the SAME API used by automation** (AppleScript, Automator, Shortcuts). `AXUIElement` is used for both accessibility and scripting. This means accessibility is never an afterthought -- it is the automation substrate.

3. **Semantic tokens handle mode switching**. An app using `labelColor` automatically gets the right color in normal mode, high contrast mode, and dark mode. The app developer never writes conditional color code.

4. **Layout participates**. When text size increases, Auto Layout constraints reflow the UI. The layout system IS the accessibility system -- they share constraints.

5. **Focus is a first-class concept**. The responder chain (NSResponder) provides a consistent, structural model for where keyboard focus is and where it goes. Every view participates in the responder chain by default.

---

## 10. macOS Differentiators

What macOS does that Windows and Linux desktop environments do NOT:

### 1. The Menu Bar Is Global (Not Per-Window)

This is the single most distinctive macOS interaction. The active app's
menu appears at the top of the screen, NOT in each window.

**Why it matters**:
- Fitts's Law: the menu bar is at the screen edge (infinite depth target)
- Consistent position reduces visual search
- Saves vertical space in every window
- Every app gets the exact same menu structure location

**Windows/Linux**: Each window has its own menu bar (or hamburger menu).
Some Linux DEs (Ubuntu Unity, KDE global menu) tried to copy this but
it never became standard.

### 2. Unified Drag-and-Drop Across the Entire System

On macOS, you can drag:
- Files from Finder to any app
- Text from any text field to any other text field
- Images from Safari to Photoshop
- URLs from address bar to a text document
- Content across spaces (drag to screen edge, pause, space switches)

The pasteboard (NSPasteboard) supports multiple representations of the
same data simultaneously (file URL + file data + icon image + text name),
and the receiving app picks the best one.

Windows has OLE drag-and-drop but it is rarely implemented consistently.
Linux has XDND but consistency varies wildly.

### 3. Services Menu

Any app can register as a service provider for any data type. Right-click
selected text in any app, Services menu offers: "Look Up," "Search with
Google," "Add to Stickies," "Convert to PDF," and any third-party
services. This is a system-level plugin model for inter-app capability.

Nothing equivalent exists in Windows or mainstream Linux.

### 4. Continuity and Handoff

- **Universal Clipboard**: Copy on iPhone, paste on Mac (same iCloud account)
- **Handoff**: Start on one device, continue on another
- **AirDrop**: Proximity file sharing
- **Sidecar**: iPad as second display
- **iPhone mirroring** (macOS Sequoia+): iPhone UI on Mac screen

These are OS-level features, not app-level. The operating system brokers
cross-device continuity transparently.

### 5. Quartz Compositor (WindowServer)

macOS composites ALL windows GPU-side with per-pixel alpha. Every window
is a texture. This is what enables:
- True per-pixel transparency
- Window shadows that are real alpha-blended renders
- Materials/vibrancy (compositor reads pixels behind the window)
- Smooth window dragging at display refresh rate
- Mission Control and Spaces with GPU-composited transitions

Windows: DWM (Desktop Window Manager) does similar since Vista/7, but
materials are more limited and vibrancy was removed, then partially re-added.
Linux: Wayland compositors vary. None match the material/vibrancy system.

### 6. PDF-Native Rendering (Quartz 2D)

The entire macOS display system is based on PDF rendering (via Quartz 2D /
Core Graphics). Every window, every view, every piece of text is rendered
through a resolution-independent vector path. This is why:
- macOS scaled resolution-independently before Retina existed
- Printing exactly matches screen appearance
- Screenshots are resolution-independent
- Any content can be "printed" to PDF with zero loss

Windows: GDI was bitmap-based until WPF (2006). WPF is vector but few
apps use it. Win32/GDI apps are not resolution-independent.
Linux: Cairo + Pango provide similar capabilities but the stack is not unified.

### 7. NSResponder Chain (Unified Event Routing)

Events in macOS travel through the responder chain:
First Responder -> Next Responder -> ... -> Window -> Window Controller -> App

Any responder can handle any event. If it does not handle it, it passes
up the chain. This means:
- Keyboard shortcuts work even when no specific UI element is focused
- Menu actions route to the correct handler automatically
- Undo/redo is automatic if the document object handles it
- The system NEVER needs to know which object handles an action at compile time

Windows: Message pump routes to window procedures. Less automatic.
Linux: Signal/slot (Qt) or signal/callback (GTK) -- explicit wiring required.

### 8. Automatic Appearance Updates

When the user changes:
- Light/Dark mode
- Accent color
- Text size
- Contrast level
- Transparency preference

EVERY running app updates INSTANTLY with NO restart, NO reload. This is
because:
- Colors are resolved at draw time (semantic tokens)
- Layout is constraint-based (reflows on parameter change)
- Materials query the system appearance at composite time
- The NSAppearance object propagates changes through the view tree

Windows: Theme changes require restart in many apps.
Linux: Theme engines vary. Qt apps update, GTK apps update, but
cross-toolkit consistency is nonexistent.

### 9. Rubber-Banding / Elastic Scrolling

macOS scroll physics:
- Content tracks finger 1:1 during scroll gesture
- At content boundary: rubber-band effect (spring, stiffness ~300, damping ~20)
- On release past boundary: spring back with slight bounce
- Momentum scrolling: continues after finger lift, decelerating (friction ~0.998/frame)
- Momentum can be interrupted by touching the trackpad

This creates a physical sensation that the content HAS edges and RESISTS
going past them. Windows traditionally hard-stops at edges (Win10/11
added a bounce effect, but it is less refined). Linux varies by toolkit.

### 10. Consistent Text Editing Everywhere

macOS text editing shortcuts work in EVERY text field, in EVERY app,
because they are handled at the NSResponder level:

| Shortcut      | Action            | Emacs origin |
|---------------|-------------------|-------------- |
| Ctrl-A        | Beginning of line | Yes           |
| Ctrl-E        | End of line       | Yes           |
| Ctrl-F        | Forward one char  | Yes           |
| Ctrl-B        | Backward one char | Yes           |
| Ctrl-N        | Next line         | Yes           |
| Ctrl-P        | Previous line     | Yes           |
| Ctrl-D        | Delete forward    | Yes           |
| Ctrl-H        | Delete backward   | Yes           |
| Ctrl-K        | Kill to end of line| Yes          |
| Ctrl-T        | Transpose chars   | Yes           |
| Ctrl-O        | Insert line after | Yes           |
| Opt-Delete    | Delete word back  |               |
| Opt-Fwd-Del   | Delete word fwd   |               |
| Cmd-Left      | Beginning of line |               |
| Cmd-Right     | End of line       |               |
| Opt-Left      | Previous word     |               |
| Opt-Right     | Next word         |               |

These work in TextEdit, Safari address bar, Terminal, Slack, VS Code,
Xcode, Notes -- everywhere. Because they are in the default key bindings
dictionary at the NSTextView level.

**No other OS has this level of text editing consistency.**

### 11. Quick Look

Press Space on any selected file in Finder: instant preview. No app
launch. Supports: images, PDFs, text, video, audio, 3D models (USDZ),
Office documents, archives, Markdown, code (with plugins).

The plugin model (QLPreviewProvider) lets any app add preview support
for any file type. This is a system-level content preview pipeline.

Windows: Preview pane exists but is limited and not keyboard-triggered.
Linux: GNOME Sushi is a partial equivalent. Never system-wide.

### 12. Spotlight Integration

Spotlight is not just file search. It is:
- Application launcher
- Calculator (type math, get results)
- Unit converter
- Dictionary lookup
- Weather, stocks, sports
- Web search preview
- Contact lookup
- File content search (indexes PDFs, Office docs, email, messages)
- Custom search via Spotlight importers (apps provide searchable metadata)

The search is indexed, near-instant, and system-wide. It uses the
CSSearchableItem API so any app's data is searchable from Spotlight.

### 13. The Trackpad Experience

Force Touch / Haptic trackpad:
- Does not physically click (solid state, vibration motor simulates click)
- Force levels: light press (click), deep press (force click)
- Force click: Look Up (dictionary), Quick Look preview, address preview
- Three-finger drag: move windows without holding click
- Pinch to zoom: everywhere (maps, images, PDFs, web)
- Two-finger scroll: everywhere, with momentum
- Four-finger swipe: Spaces, Mission Control
- All gestures customizable

The trackpad-to-display latency pipeline is optimized end-to-end.
Apple claims ~4ms gesture-to-pixel response on Apple Silicon.

### 14. Automatic Save and Versions

Since macOS Lion (2011):
- Documents auto-save continuously (no manual save needed)
- Version history browser (Time Machine-style for individual files)
- Revert to any previous version
- Document locking after period of inactivity
- Duplicate instead of Save As

This is architectural: NSDocument subclasses get this automatically.
The file coordination system (NSFileCoordinator) handles conflicts.

---

## Implementation Priority for Z-OS Translation

Based on what creates the strongest "this feels polished" impression:

### Tier 1 — Must Have (These Create the Feel)
1. Spring-based animation system with velocity continuity
2. Semantic color token system with automatic dark mode
3. Consistent typography scale with optical sizing
4. Proper focus ring and keyboard navigation
5. Elastic scroll / rubber-banding physics
6. Layered window shadows with proper compositing
7. 4pt spacing grid, baseline text alignment

### Tier 2 — Strong Differentiators
8. Materials/vibrancy system (blur + tint + compositing)
9. Global command interface (equivalent to Spotlight + menu bar)
10. System-wide drag-and-drop with multiple pasteboard types
11. Consistent text editing keybindings everywhere
12. Haptic-level feedback for alignment/snapping
13. Undo as a system-level, per-context stack

### Tier 3 — Long-Term System Maturity
14. Quick Look equivalent (instant preview pipeline)
15. Accessibility-as-automation-substrate pattern
16. Cross-device continuity
17. Automatic save + version history
18. Resolution-independent rendering from the ground up

---

## Key Takeaway for Z-OS

The thing that makes macOS feel like macOS is NOT any single feature.
It is the **coherence** — every subsystem speaks the same design language:

- Springs everywhere (not beziers in some places, springs in others)
- Semantic tokens everywhere (not hardcoded colors in some places, tokens in others)
- Baseline alignment everywhere (not box alignment in some places)
- The same 4pt grid everywhere
- The same focus ring style everywhere
- The same shadow recipe everywhere
- The same text editing shortcuts everywhere

**Coherence is the product.** The design system IS the feel.

Z-OS's signal chain architecture actually has a structural advantage
here: if every UI element is a node in a signal graph, then design
tokens (colors, spacing, animation parameters) can propagate as signals.
Change one token value and it flows through the graph to every consumer.
This is what NSAppearance does in macOS, but Z-OS could make it explicit
and inspectable.
