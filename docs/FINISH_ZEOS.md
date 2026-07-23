# FINISH ZEOS — Road to a Shipped Desktop

**The deal:** get Zeos from "boots to a real but unfinished GUI with visible
seams" to "a finished consumer OS — everything an OS should have, usable, and
it looks good enough that someone sits down." No item is `[DONE]` until it is
**measured AND observed** — i.e. verified in a screenshot / captured input, per
BIBLE v1.0 (One Law + Seven Gates). Append-only. States:
`[TODO] [WIP] [BLOCKED] [PARTIAL] [DONE]`.

**Baseline observed 2026-07-22** (x86 UEFI build `BOOTZ.EFI`, QEMU+OVMF, headless
QMP capture): OS builds clean (130+ modules), boots splash→lockscreen. Cold-boot
**PIN enrollment works** (keyboard input confirmed). **Welcome mode-picker
renders** (Zeros / DereZ / Full). Desktop stack exists in-tree: `compositor.c
wm.c desktop.c dock.c panel.c cursor.c mouse.c settings.c settings_registry.c
theme_runtime.c` + apps `browser editor calculator calendar notes_zeos chat_zeos
file_mgr image_viewer` + `lockscreen welcome hotcorners workspaces notify
quick_look ui_context_menu`. **Two real seams seen:** (a) kprint diagnostics
bleed onto the framebuffer *over* the UI; (b) welcome→desktop handoff not yet
reached in the harness. **First polish fix landed:** welcome mode-card blurbs
now word-wrap inside their boxes (was: text bled into the next card).

Most of Phases 1–4 likely already *work* (mature 101k-LOC stack). This list is
therefore **verify-then-finish**: prove each against a screenshot; where it's
broken or unfinished, fix it; mark `[DONE]` only when seen working.

---

## PHASE 0 — Reach the live desktop & clean the boot seam
- **L0.1** `[TODO]` Reach the live **Full** desktop from cold boot in the headless
  harness; screenshot the actual desktop (wallpaper, dock, panel). DoD: a frame
  showing the composited desktop, no picker.
- **L0.2** `[TODO]` Kill kprint-over-framebuffer bleed: after `splash_dismiss()`,
  diagnostics go to **serial only**, never painted over the compositor. DoD:
  clean desktop frame, zero log text on screen.
- **L0.3** `[TODO]` Deterministic visual-regression rig: scripted cold-boot →
  enroll → Full → desktop → drive input, emitting a labeled screenshot set each
  run. This rig verifies every item below.

## PHASE 1 — Input & the feel (cursor, click, drag)
- **L1.1** `[TODO]` Mouse cursor renders and tracks pointer motion on the desktop.
- **L1.2** `[TODO]` Click-to-focus + raise; drag titlebar to move; spring settle.
- **L1.3** `[TODO]` Window resize (edges/corners) with min-size clamp.
- **L1.4** `[TODO]` Minimize / maximize / restore.

## PHASE 2 — Window chrome done right  (Brad's headline)
- **L2.1** `[TODO]` Consistent titlebar: title + app icon + window controls, with
  hover/press states.
- **L2.2** `[TODO]` **Close-button ("X") corner position is a setting** —
  top-right ↔ top-left — applied **live** to every window's chrome, routed
  through `settings_registry` so it persists. DoD: flip it in Settings, watch
  every window's X jump sides in the same frame.
- **L2.3** `[TODO]` Theme: accent color + light/dark applied live to chrome,
  panel, dock.

## PHASE 3 — The shell (OS-ness)
- **L3.1** `[TODO]` Desktop wallpaper + dock + top panel (live clock, status,
  active-app title).
- **L3.2** `[TODO]` App launcher / menu; open each app as a window.
- **L3.3** `[TODO]` Multiple windows + z-order + taskbar entries + Alt-Tab.
- **L3.4** `[TODO]` Notifications visible; right-click context menus; quick look.
- **L3.5** `[TODO]` Hot corners + workspaces working and discoverable.

## PHASE 4 — The apps (what people USE) — each opens, renders, is usable
- **L4.1** `[TODO]` **Settings** — every control does something live: close-X
  side, theme, wallpaper, animations on/off, PIN change.
- **L4.2** `[TODO]` **Files** (`file_mgr` + FAT32/VAULT) — browse, open.
- **L4.3** `[TODO]` **Editor / Notes** — type and save (persists to VAULT).
- **L4.4** `[TODO]` **Terminal** app that runs **Z+** programs — type a chain,
  see it fire + per-node timing. (Ties the signal engine to a visible app.)
- **L4.5** `[TODO]` Calculator, Calendar/Clock, Image viewer, Browser — each
  opens and works.

## PHASE 5 — "What's New in Zeos!" + onboarding
- **L5.1** `[TODO]` **"What's New in Zeos!"** panel (first-run + reopenable),
  featuring headline items. The **close-button-side** control is featured HERE
  with a "Try it" that jumps straight to Settings — per the exact spec.
- **L5.2** `[PARTIAL]` Welcome mode-picker polish — blurb-in-box **done**;
  remaining: smooth handoff into the desktop with a short intro.

## PHASE 6 — Polish that makes them sit down (looks good)
- **L6.1** `[TODO]` Real proportional font (Inter / JetBrains Mono are already
  embedded) used across the UI, not the 8px boot font.
- **L6.2** `[TODO]` Rounded corners, drop shadows, hover/press feedback,
  consistent spacing/padding.
- **L6.3** `[TODO]` Window open/close/minimize animations (spring); splash →
  desktop crossfade.
- **L6.4** `[TODO]` Settings + session persist across reboot (VAULT); warm-boot
  unlock instead of re-enroll.

## PHASE 7 — Ship & verify
- **L7.1** `[TODO]` End-to-end reel: cold boot → desktop → open every app →
  flip close-X side (see it live) → reboot → setting persisted. One screenshot
  sequence, no seams.
- **L7.2** `[TODO]` Port the finished desktop onto the **ARM framebuffer**
  (the Zeos Box target — Snapdragon/Dragonwing).

---

*Log opened 2026-07-22. Each `[DONE]` carries its screenshot as evidence.*
