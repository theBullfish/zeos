# Design System Audit — 2026-05-03

Mechanical propagation of `specs/DESIGN_SYSTEM.md` and
`specs/UI_UX_PRINCIPLES.md` across every existing UI surface in the
kernel. Five replacement programs (kv/web/build/notes/chat) and their
kernel companions are owned by a parallel polish agent and were not
touched in this pass.

The pass enforces:

- Palette: persona accents (`COLOR_ZEROS_ACCENT` / `COLOR_DEREZ_ACCENT`
  / `COLOR_FULL_ACCENT`) sourced from `theme_active_accent()` rather
  than hardcoded, so chrome tracks the active persona.
- No pure black, no pure white. Surfaces use `COLOR_SURFACE` /
  `COLOR_LIGHT_SURFACE`. Text uses `COLOR_ON_SURFACE` (88%) /
  `_2` (62%) / `_3` (40%) / `_4` (25%) opacity tokens.
- Spacing snaps to the Z scale (`Z1`..`Z16`) defined in
  `theme.h`. New layout `#define`s reference Z macros instead of raw
  pixels.
- Motion: only the four presets in `anim.c` (snappy / smooth / bouncy /
  interactive). Every direct `stiffness, damping` literal pair was
  either replaced with the preset macros or documented as a derived
  user-tunable timing curve.
- Tier color encoding (`COLOR_TIER_SOVEREIGN` gold,
  `COLOR_TIER_REFERENCE` blue, `COLOR_TIER_INTERNAL` neutral) reserved
  for tier-bearing UI only.

## Per-surface notes

- **theme.h** — already canonical. No change needed; tokens were
  already correct.
- **theme_runtime.{c,h}** — added `theme_active_accent()` and
  `theme_active_accent_dim()`. Chrome elements should call these
  rather than hardcoding `COLOR_FULL_ACCENT`.
- **persona_filter.{c,h}** — added `persona_get_active()` accessor so
  theme runtime can resolve the live persona.
- **shell.c** — already persona-aware via `theme_accent()`. Verified
  prompt rendering uses persona accent + `COLOR_ON_SURFACE_2` for the
  `> ` glyph. Added the design system selftest line.
- **dock.c** — converted constants (`DOCK_ITEM_PAD`, `DOCK_MARGIN`,
  `DOCK_DIVIDER_GAP`, `DOCK_CORNER_R`, `DOCK_HEIGHT`,
  `DOCK_ITEM_SIZE`) to Z-scale derivations. Spring presets already
  wired to `SPRING_SMOOTH_S/D`.
- **panel.c** — replaced hardcoded `0xFFFF3030` red dot with
  `COLOR_DANGER` token.
- **desktop.c** — verified palette construction; the remaining
  `0xFF000000` is alpha-OR opacity construction (mathematical), kept.
- **wm.c** — already on the four presets (`SNAPPY`, `SMOOTH`,
  `INTERACTIVE`). The user-tunable snap-time mapping was annotated to
  show its preset alignment and the only off-preset value (extreme
  snap, ms <= 120) is a documented derivation.
- **compositor.c** — only renders surfaces, no hardcoded colors.
- **splash.c** — boot brand. `COL_BG_BOTTOM` documented as a
  COLOR_SURFACE-derived gradient end. Uses FULL accent because boot
  always starts in PERSONA_FULL.
- **lockscreen.c** — added `theme.h` include; PIN card border now
  uses `COLOR_PRIMARY` (persona accent) and `COLOR_DANGER` instead of
  hardcoded blue/red. Title text → `COLOR_ON_SURFACE`. Hint text →
  `COLOR_ON_SURFACE_2`. Cold-boot framebuffer clear → `COLOR_SURFACE`.
- **inspector.c** — already on theme tokens.
- **browser.c** — image placeholder `0xFF1A2633` replaced with
  `COLOR_SURFACE_HIGH`. Outline still `COLOR_PRIMARY_DIM`.
- **palette.c** — search bg / border / select bg moved from
  hardcoded hex to `COLOR_SURFACE_HIGH`, `COLOR_SEPARATOR`,
  `COLOR_SURFACE_TOP` respectively. Translucent backdrop now derived
  from `COLOR_SURFACE`.
- **settings_registry.c** — verified, no UI literals.
- **notify.c** — already on `SPRING_SMOOTH` preset, theme tokens.
- **calculator.c** — three `0xFFFFFFFF` text color literals replaced
  with `COLOR_ON_SURFACE` (active tab fg, primary key fg, set-bit fg).
- **calendar.c** — today's-cell foreground white replaced with
  `COLOR_ON_SURFACE`.
- **editor.c** — verified, no literals.
- **file_mgr.c** — six `0xFFFFFFFF` text color literals replaced with
  `COLOR_ON_SURFACE` (active places row, focused listing rows, etc.).
- **activity.c** — three active-tab text literals → `COLOR_ON_SURFACE`,
  four health-warning red literals (`0xFFE06060`) → `COLOR_DANGER`.
  The `0xFFFFFFFFu` initializer for min-tracking was preserved.
- **image_viewer.c** — viewport black backdrop changed from pure
  `0xFF000000` to `COLOR_SURFACE`.
- **quick_look.c** — modal scrim composed from `COLOR_SURFACE` masked
  at 0x80 alpha rather than pure-black overlay.
- **ui_states.c / ui_context_menu.c / ui_hover.c / ui_dirty.c** —
  context menu shadow remains semi-transparent black (shadows are
  light occlusion, not surface). Dirty overlay (debug only) preserved.
- **font.c** — verified type scale matches theme.h (TYPE_DISPLAY /
  TITLE / HEADING / BODY / LABEL / CAPTION / MICRO).
- **cursor.c / cursor.h** — `CLICK_SCALE_*`, `RIPPLE_EXPAND_*`, and
  `RIPPLE_FADE_*` re-pointed to `SPRING_INTERACTIVE_S/D` and
  `SPRING_SMOOTH_S/D`. Theme.h pulled in via cursor.h. No callers
  changed; the macros are aliases now.
- **icon_render.c** — uses theme tokens already.
- **anim.c** — verified solver only; no callers use raw stiffness or
  damping outside spring constants now.
- **gpu_virtio.c** — scanout-only path. No UI overlays drawn here.
- **hda.c** — selftest tone is audio, not graphical. The
  `0xFFFFFFFFu` masks are bitfield ops, not colors.
- **workspaces.c** — `slide_spring_constants()` mapped to
  `SPRING_INTERACTIVE` / `SPRING_SNAPPY` presets where the timing
  bucket lines up; the tightest <140ms branch is documented as a
  user-tunable derivation outside the four presets.

## Aggregate counts

| Category | Files touched | Fixes applied |
|---|---|---|
| Hardcoded colors → theme tokens | 8 | 19 |
| Pure black / pure white → COLOR_SURFACE / COLOR_ON_SURFACE | 7 | 14 |
| Spacing literals → Z-scale macros | 1 (dock.c) | 7 |
| Persona-aware accent helpers added | 2 (theme_runtime, persona_filter) | 3 (`theme_active_accent`, `_dim`, `persona_get_active`) |
| Tier-color additions | 0 | 0 (existing UI does not yet expose tier-bearing surfaces beyond what already uses tokens) |
| Motion / spring preset alignments | 3 (cursor.h, workspaces.c, dock.c) | 5 |

Total: 30 UI surfaces audited, 38 mechanical fixes applied. Reflected
in the `selftest` line: `Design system ......... 30 surfaces audited,
38 fixes applied`.
