# Z-OS UI/UX Design Principles

> Evidence-based design decisions for Z-OS. Every claim cites its source and
> distinguishes between peer-reviewed research, industry studies, and design
> arguments without empirical backing. Where data is thin, we say so.
>
> **Date**: March 22, 2026
> **Status**: Research complete, not yet implemented

---

## 1. Layout and Targeting

### Fitts's Law Is Non-Negotiable
Screen edges are infinitely deep targets — the cursor cannot overshoot. Corners
are the fastest targets on screen (infinite in two dimensions). This is the most
replicated finding in HCI.

**Z-OS decisions**:
- Primary controls live on screen edges
- The four corners are reserved for the four most frequent system actions
- No floating toolbars that waste Fitts's Law advantage

**Evidence**: Strong. Fitts (1954), MacKenzie (1992), hundreds of replications.
Apple's Lisa team tested per-window vs fixed menu bars; fixed top-edge won.

Sources: Fitts (1954); NNGroup "Fitts's Law"; AskTog "A Quiz Designed to Give
You Fitts"; Coding Horror "Fitts's Law and Infinite Width"

### Panel Position
Both top and bottom edges are Fitts-optimal. Neither is inherently superior.
Side panels are slightly worse on widescreen displays (narrower vertical edge
relative to horizontal).

**Z-OS decision**: User-configurable. Default to top (leaves bottom edge free
for app content, matches the "stage" metaphor — controls above, work below).

**Evidence**: Moderate. Fitts's Law applies to both; no study directly compares
top vs bottom panel for overall productivity.

### Launcher Model
No rigorous study compares dock vs taskbar vs keyboard launcher. But adoption
patterns show power users converge on keyboard launchers (Spotlight, Albert,
Rofi, command palettes). The command palette pattern (VS Code, Figma, Notion,
Slack, IntelliJ) is the strongest convergent evolution signal in modern UI.

**Z-OS decisions**:
- System-wide command palette, invocable from anywhere (one shortcut)
- Every system action reachable through it, searchable by name
- Edge-placed panel for spatial awareness of running signal chains
- Mouse/touch path for every action — keyboard is faster, never exclusive

**Evidence**: Thin for direct comparison. Strong adoption signal.

---

## 2. Window Management

### Tiling Wins for Multi-Window Work
A 2025 study (Chouhan, arXiv:2511.17516) found tiling window managers improved
task completion time by **37.83%** in multi-window workflows. Stack & Tile
research (Springer, 2013) found significantly faster task switching.

Microsoft reported 50% of Windows 8.1 snap usage was user-initiated — strong
organic demand for tiling in a floating-first OS.

**Z-OS decisions**:
- Hybrid model: floating by default, snap/tile with keyboard or drag-to-edge
- Keyboard-driven layout switching (quarters, thirds, halves, full)
- Signal chain awareness — windows from the same chain auto-group
- Tiling is the power mode, not a separate WM to install

**Evidence**: Moderate. One controlled study, one evaluation study, Microsoft
usage data. Community evidence is strong but self-selected.

### Focus Policy
No empirical data exists comparing focus-follows-mouse vs click-to-focus.
Focus-follows-mouse reduces clicks but causes accidental focus changes with
overlapping windows. It works best with tiling.

**Z-OS decision**: Click-to-focus default. Focus-follows-mouse available,
auto-suggested when tiling mode is active.

### Display Profiles
No OS handles monitor hot-plug well. KDE and GNOME have partial "display
profile" support but it's fragile.

**Z-OS decision**: Match monitors by EDID serial number. Stash window layout
on disconnect, restore on reconnect. Display is a persistent signal graph
node, not an ephemeral output. (See QOS_WINS.md, Bucket 2.)

### Virtual Workspaces
No rigorous study quantifies productivity gains from virtual desktops.
Theoretical basis is cognitive load theory — "lightweight cognitive separation."

**Z-OS decision**: Support workspaces. Allow pre-arranged workspace templates
("Development," "Communication," "Media"). Don't over-invest in novel workspace
paradigms without user testing data.

---

## 3. Typography

### Font Size
- **16px minimum** for body text (US DHHS, NNGroup, multiple studies)
- **18px optimal** for broad audiences including dyslexic readers
- Below 14px significantly increases eye strain during sustained reading

**Evidence**: Strong. Rello & Baeza-Yates, CHI 2016: larger fonts and increased
line spacing significantly improve readability, especially for dyslexic users.

### Line Height, Letter Spacing, Line Length
- **Line height**: 1.4–1.5x font size. 120% vs 100% improves accuracy ~20%
- **Letter spacing**: Minimum 0.12em. Wider spacing improves dyslexic reading
  accuracy **2x** and speed 20%+ (Zorzi et al., 2012, PNAS)
- **Line length**: 50–75 characters per line. 66 chars is the classic sweet spot
  (Bringhurst, "The Elements of Typographic Style")

**Z-OS decisions**:
- System-wide typography settings: font size, line height, letter spacing
- Default: 16px body, 1.5x line height, 0.12em letter spacing, 66-char measure
- Adjustable per-user as accessibility feature (more impactful than font swaps)

**Evidence**: Strong across all three metrics.

### Monospace vs Proportional
- Code/data: monospace (alignment, disambiguation of l/1/I, O/0)
- Prose/UI: proportional sans-serif (faster reading)
- Dyslexic readers: monospace with increased spacing outperforms proportional

**Z-OS decision**: Proportional for UI chrome, monospace for code/terminal/data.
System-wide spacing adjustment available.

### Subpixel Rendering
Declining in relevance. Apple removed it for Retina displays (2012+). Above
~150 PPI, native resolution is sufficient. OLED PenTile layouts produce worse
results with subpixel rendering.

**Z-OS decision**: Grayscale antialiasing only. No subpixel rendering. Not
worth the engineering investment for modern displays.

---

## 4. Color and Mode

### Dark Mode vs Light Mode — The Actual Data
This is well-studied. The answer is nuanced:

**Light mode wins for visual performance**:
- Positive polarity advantage: better acuity, proofreading, focus
  (Piepenbrock et al., 2013; Buchner & Baumgartner, 2007)
- Mechanism: light backgrounds constrict pupil → reduced aberrations
- ~50% of population has some astigmatism; dark mode causes halation
  (text appears to glow/bleed)

**Dark mode has situational benefits**:
- Dim ambient lighting: reduces glare
- Evening use: less circadian disruption (lower luminance)
- Some low-vision users prefer it

**Z-OS decisions**:
- Offer both. Default to light mode.
- Auto-switch based on time of day (or ambient light sensor if available)
- **Never pure black (#000)** — use dark gray (#1a1a1a to #2d2d2d) to
  reduce halation
- **Never pure white (#fff)** — use slightly warm off-white (#fafafa to
  #f5f0eb) to reduce glare
- Dark mode is "dark warm," not "dark clinical"

**Evidence**: Strong. Multiple controlled studies, vision science backing.

Sources: NNGroup "Dark Mode vs. Light Mode"; PMC (2025) visual fatigue study;
"Beyond Dark Mode: Making White on Black Text Accessible"

### Contrast: Optimal, Not Maximum
WCAG AA: 4.5:1 normal text, 3:1 large text. AAA: 7:1 / 4.5:1. But maximum
contrast is not optimal — pure white on pure black causes halation.

**Z-OS decision**: Target 8:1–12:1 for body text. Dark gray on warm off-white
for light mode (#333 on #FAFAFA). Warm light text on dark gray for dark mode
(#E0D8D0 on #1E1E1E). Adopt APCA (Advanced Perceptual Contrast Algorithm)
when WCAG 3.0 stabilizes.

### Color Temperature
- Cool/blue light (5000K+) suppresses melatonin — 10% suppression at 5700K
  vs 0.1% at 2100K (Nature, 2025)
- Warm light (2700–3000K) reduces stress markers
- Higher temperatures (4000–5000K) improve daytime alertness

**Z-OS decision**: Night shift is default-on. Neutral-warm white point (~5500K)
during day, shifting to 2700–3000K in evening. User-adjustable.

### Color Vision Deficiency
1 in 12 men, 1 in 200 women. 99% is red-green.

**Z-OS decisions**:
- Never use color alone to convey meaning — always pair with icon, label,
  pattern, or position
- Safe combinations: blue + orange, blue + red. Avoid red + green.
- Lightness variation in all palettes (distinguishable in grayscale)
- CVD simulation mode in accessibility settings for designers/developers

---

## 5. Information Density

### The Density Dial
NNGroup: progressive disclosure reduces errors for novices while cutting visual
scanning for experts. But no controlled study directly compares dense-default
vs sparse-default UIs across novice-to-expert users.

Professional tools (Bloomberg, Grafana, DAWs) are faster for trained experts
but have brutal learning curves. Consumer tools that hide everything are
approachable but frustrate power users.

**Z-OS decisions**:
- Default to clean/sparse
- **System-wide density preference** — not per-app progressive disclosure.
  Respects user identity rather than forcing rediscovery in every context.
- Three modes: Comfortable (sparse), Standard, Compact (dense)
- The density setting affects all signal chain UIs consistently
- "Density is respect — don't hide info behind hovers" (Z-OS design principle)

**Evidence**: Moderate. NNGroup consulting data, practitioner wisdom. No single
controlled experiment comparing density modes.

---

## 6. Animation and Motion

### When It Helps
- Spatial continuity: animations connecting cause→effect reduce cognitive load
- Feedback: responses under 400ms maintain user flow (Doherty Threshold, IBM 1982)
- Orientation: transitions build mental models of spatial relationships

### Optimal Timing
- 100ms: perceived as instant, too fast to parse
- **150–250ms: sweet spot for desktop UI transitions**
- >500ms: feels sluggish
- >1000ms: breaks flow of thought

### When It Hurts
- Vestibular disorders: triggers vertigo, nausea, migraines
- ADHD/autism: decorative animation impairs recall, increases cognitive load
- Power users: animation becomes noise after mental model is learned

**Z-OS decisions**:
- All transitions < 250ms
- System-wide animation speed multiplier: 0x (instant), 0.5x, 1x, 2x
- Default: 1x
- Reduced-motion toggle in top-level settings (not buried in accessibility)
- "Reduced motion" = instant cuts + opacity fades, not zero motion
- **Motion has physics** — inertia, damping, settle time. Not CSS easing curves.
- **Never require animation for comprehension** — every animated transition
  must be understandable as an instant state change

**Evidence**: Moderate. NNGroup timing research, Material Design guidelines,
TPGi cognitive disability study. Consistent across sources but not from
controlled productivity experiments.

Sources: NNGroup "Animation Duration"; TPGi "Impact of Motion on Cognitive
Disability"; WCAG 2.1 SC 2.3.3; MDN "prefers-reduced-motion"

---

## 7. Input

### Keyboard vs Mouse — The Truth
Tognazzini's claim that mousing is always faster was debunked by Dan Luu (2017):
the underlying Apple studies are unreproducible and lack methodology.

**Actual finding** (Omanson et al., 2010, SAGE): It depends on the task.
- Keyboard: faster for text entry and command execution by expert users
- Mouse: faster for spatial targeting and selection from visual arrays

**Z-OS decisions**:
- Every action reachable by both keyboard and mouse
- Command palette for keyboard-speed command execution with discoverability
- Keyboard shortcuts for all frequent actions, discoverable via command palette
- Never make keyboard-only or mouse-only the exclusive path

### Touch Targets
- 44x44px minimum (WCAG, Apple HIG, Google Material)
- Error rates exceed 20% below 5mm (Parhi et al., 2006)

**Z-OS decision**: 44px minimum for all interactive elements. Touch-capable
mode auto-detected, increases spacing/targets when touchscreen is primary input.

### Scroll Behavior
Natural scrolling has cognitive advantages (response-effect compatibility) but
many desktop scroll-wheel users prefer traditional. Scrolljacking universally
degrades UX.

**Z-OS decisions**:
- Natural/traditional as a user toggle
- Never override native scroll physics in the compositor
- Kinetic scrolling with momentum for trackpad/touch

---

## 8. Notifications and Interruptions

### The Data Is Damning
- **23 minutes 15 seconds** to refocus after interruption (Gloria Mark, UCI, 2008)
- **47 seconds** average attention span on screen in 2020 (Mark, 2023)
- **63.5 notifications per day** average across apps
- **40% of productive time** lost to context switching (APA estimate)
- **2x error rate** from even brief interruptions in sequence tasks
  (Altmann et al., Journal of Experimental Psychology, 2014)

**Z-OS decisions**:
- **Batched notification delivery by default** — every 15–30 minutes, not instant
- Prominent Focus Mode — one toggle, suppresses non-critical notifications
- User-trainable priority levels per source
- Notification count badge without detail (reduces curiosity-driven checking)
- Daily notification digest view
- **No notification ever interrupts full-screen work** without explicit permission
- Chirp protocol applies: the OS asks "you good?" — if the user is in flow,
  the OS does not interrupt

**Evidence**: Strong. 20+ years of interruption research from Gloria Mark's lab.
Multiple controlled studies on error rates and recovery time.

Sources: Mark (2008) "Cost of Interrupted Work"; Mark (2023) "Attention Span";
Altmann et al. (2014) Journal of Experimental Psychology

---

## 9. System Settings

### Why Every Settings App Feels Wrong
- No standard mental model for where settings live
- Technical jargon as labels
- Flat (overwhelming) vs deep (lost in hierarchy) — both common
- Settings accumulate without reorganization over decades
- Windows still has both Settings and Control Panel after 10+ years

### What Works
- **Search-first**: Highest-impact single improvement (GitLab study data)
- **Organize by user goal**, not technical subsystem
- **Contextual access**: right-click any UI element → relevant settings
- **Inline current values**: see state without opening sub-pages

**Z-OS decisions**:
- Settings search from day one, system-wide
- Organization by goal: "Display," "Sound," "Input," "Network," "Privacy,"
  "Storage" — not "Kernel Parameters," "ACPI," "DRM Subsystem"
- Right-click any system UI element → "Settings for this..."
- Current values shown inline next to every setting name
- One settings app. No legacy duplicate. Ever.

**Evidence**: Moderate. GitLab study provides quantitative backing for search.
Practitioner consensus on organization-by-goal. No controlled study comparing
settings taxonomies.

---

## 10. File Management

### Folders + Tags + Search
Bergman et al. (2013): folders are better for keeping/organizing; tags are
better for cross-cutting retrieval. Combining both outperforms either alone.

**Z-OS decisions**:
- VAULT provides the foundation — temporal state, provenance, structured storage
- Folders for primary organization (matches physical-world mental model)
- Tags for cross-cutting access (MasQ perception layers enable this naturally)
- Search as first-class navigation, not a fallback
- Smart folders / saved searches as permanent UI citizens
- No separate "Recents," "Downloads," "Favorites" — these are just saved searches

**Evidence**: Moderate. Bergman et al. controlled study. Spatial vs browser
file managers: one old Microsoft study, no definitive answer.

---

## 11. Neurodivergent-First Design

### Scale
15–20% of the global population is neurodivergent. This is not an accessibility
edge case — it is a core user population.

### What the Research Says

**Dyslexia fonts don't work**:
OpenDyslexic and similar fonts show weak to no improvement in controlled studies.
Wery & Diliberto (2017, PMC5629233): no improvement in reading rate or accuracy;
some students were slower.

**Letter spacing DOES work**:
Zorzi et al. (2012, PNAS): wider letter spacing improved dyslexic reading
accuracy **2x** and speed 20%+. This is the single most impactful typographic
intervention for dyslexic readers.

**Sensory sensitivity is common**:
37–69% of autistic individuals experience sound sensitivity. Children with ADHD
have higher rates of sound sensitivity. Bright colors, flashing, high-contrast
patterns cause discomfort.

**Decorative animation hurts**:
TPGi study: decorative animations impair recall and increase extraneous cognitive
load for users with cognitive disabilities.

**Z-OS decisions**:
- **Three sensory modes**: Standard, Low Stimuli, High Contrast
- Low Stimuli: muted palette, no decorative animation, reduced motion, quiet
  notification sounds, warm color temperature
- High Contrast: maximum legibility, strong borders, no gradients
- System-wide letter/word/line spacing adjustment (more impactful than font swaps)
- No "dyslexia font" — offer spacing controls instead (evidence-based)
- Animation speed multiplier (covered in Section 6)
- Consistent, predictable layout across all system UIs
- **Neurodivergent-friendly is not a mode — it's a spectrum of adjustments**
  Each setting (motion, density, color temperature, spacing, sound) is
  independently tunable. Not a single "accessibility mode" checkbox.

**Evidence**: Strong for spacing interventions and sensory sensitivity. Strong
against specialized fonts. Moderate for animation impact on cognition.

Sources: Wery & Diliberto (2017, PMC5629233); Zorzi et al. (2012, PNAS);
UX Magazine "Designing Inclusive and Sensory-Friendly UX"; Oxford Academic
"Designing assistive technologies for neurodivergent users" (2025)

---

## 12. Novel Paradigms Worth Stealing

### What Z-OS Should Take

**From Mercury OS (Jason Yuan, 2019)**:
- Intent-driven interaction — user expresses goal, system routes to capability
- No evidence, but the concept maps to signal chains: the user starts a signal,
  the graph routes it

**From Raskin's Archy / THE**:
- No modes, ever. Every action always available.
- Content persistence — no "saving." Everything is always saved.
- Navigation by incremental search. (Z-OS: command palette is this.)
- VAULT's temporal state already provides content persistence.

**From Plan 9 rio**:
- Extreme simplicity for expert users
- Everything is a file, including the window system
- Clean composability

**From Oberon System**:
- Any visible text can be a command (click to execute)
- Zero separation between document and interface
- This is what Z+ could look like in the UI — signal chain definitions
  that are both readable AND executable

**From Dynamicland (Bret Victor)**:
- Not the mechanism (rooms, paper, cameras) but the values:
  transparency, inspectability, social computation
- Z-OS's signal graph should be inspectable by default — you can always
  see what's connected to what

**Evidence for all**: Thin to nonexistent. Design arguments and concepts.
No user studies on any of these at scale. Worth stealing ideas from, not
worth citing as evidence.

---

## Design Principles Summary

These emerge from the research:

1. **Edges are sacred** — Fitts's Law, always
2. **Light by day, dark by night** — follow the science, not the trend
3. **Never pure black, never pure white** — warm, not clinical
4. **Spacing beats fonts** — for dyslexia, for everyone
5. **250ms or instant** — no slow animations, ever
6. **Batch interruptions** — 23 minutes is too expensive to waste
7. **Search everything** — settings, files, commands, signal chains
8. **Density is a dial, not a decision** — user controls it system-wide
9. **Neurodivergent-friendly is the default** — not a mode, a spectrum
10. **Glow is earned** — light equals signal, not decoration
11. **The tool disappears into the work** — UI is bezel, not the subject
12. **Motion has physics** — if it moves, it has mass
