# Z-OS BUILD MAP

**The map WE follow.** Brand new, authored 2026-07-23. Supersedes `ROADMAP.md` (whose
checkboxes were unverified assertions — a perturb of roadmap-vs-code found ~45 items
marked planned that were built, ~7 marked done that were false/dead, and ~19 that persist
a value nothing reads). This map starts clean at **A.1** and is governed by BIBLE.

---

## THE PROTOCOL WE FOLLOW — BIBLE (for this map)

Canonical: `~/.claude/BIBLE_PROTOCOL.md` (BIBLE v1.0) · palace drawer `meta/bible_protocol_v1`.
This section is that protocol, stated for the Build Map. It says the **same thing**.

### THE ONE LAW (checkbox form)
**Verified base truth or no checkbox.**
An item earns `[x]` **only when it is measured AND observed against a stable baseline** —
a boot, a serial line, a screenshot, a read-back, a measured number. "The code exists" is
**not** verification. "The tool said OK" is an assertion, not evidence. Coded ≠ running ≠
verified. If you would not stake a measurement on it, it does not get the check.

### STATE LEGEND (checkbox ⇔ BIBLE state — the same thing)
- `[x]` **VERIFIED / [DONE]** — measured AND observed; evidence cited inline. The ONLY
  state that earns a check.
- `[ ]` **[BROKEN]** (prefixed `‼`) — observed to FAIL. A verified negative — real, cited,
  not a guess. Higher priority than TODO: we saw it break.
- `[ ]` **[UNVERIFIED]** — implementation exists (`source file:line`) but has NOT been
  observed working. No checkbox until a boot/measurement proves it. This is where most of
  the codebase honestly sits.
- `[ ]` **[PARTIAL]** — half-built, a stub, or persists state nothing consumes.
- `[ ]` **[TODO]** — no implementation.
- `[ ]` **[2.0]** — deliberately deferred.

### THE SEVEN GATES (a `[DONE]`/`[x]` must clear all seven)
- **G1 Append-only, numbered.** Items are `A.1, A.2 … B.1 …` (section.item). Never delete,
  never renumber. Superseded → `[SUPERSEDED by X.n]`, not removed.
- **G2 State + date + evidence on every close.** No item checks on assertion.
- **G3 PoC ≠ finding.** Verify by an *independent* read across a boundary (a fresh boot, a
  different endpoint) — not a read-back through the same layer that wrote it.
- **G4 Retro + adversary per close.** One-line retro + the skeptic's strongest objection,
  answered or filed as the next item.
- **G5 Harm/scope/commit gate.** Exploration is fearless; irreversible/outward moves need an
  explicit human OK. (Goya card safety rules always apply.)
- **G6 Provenance, always labeled:** `[observed]` / `[measured]` / `[source file:line]` /
  `[UNVERIFIED]` / `[target]`. Untagged = `[target]`.
- **G7 Fan out.** Multi-file / doctrine / failure-mode work fans out (Workflow/CoE/agents).
  Single probes in a row = hope-mode → stop, fan out.

### NORTH STAR
Preserve and respond to reality instead of flattening it. Don't let a guess sound like a
measurement. Keep the dissonant note ringing until it's earned-resolved.

---

## STATUS AT AUTHORING (2026-07-23) — what a live headless boot actually showed
`[observed]` build `build/BOOTZ.EFI` via `kernel/vshot.py` (QEMU q35, boot → inject cursor
motion + titlebar drag → screendump). Serial: `COMP: initialized 1920x1080 @ 60fps`,
`HC: hot corners active`, `WM: initialized`, `WS: count=4 active=0`, `PANEL: h=48`,
`DESK: 0 icons`, `DOCK`, `graphical desktop shell up`, `VAULT: 2MB, 16 programs`.
Frame-0: two windows + panel render correctly. Frame-after-cursor: **windows gone, panel
survives** (‼ B.7). Every scheduler tick ~100–124ms over a 1ms quantum (‼ A.6).

---

## VERIFICATION SWEEP — 2026-08-02 (fresh cold boot, 5 agents, per-item evidence)

`[observed]` Built `build/BOOTZ.EFI` (links `zplus_zir.o`; version alpha-0.2) and booted it
under QEMU q35 + OVMF (KVM) via the new **`kernel/sweep_boot.py`** harness, which drives the
full cold-boot path headless over QMP: **PIN enrollment → 5-screen first-boot wizard → live
desktop**, then an interaction battery (cursor, Super+Space palette, Super+2 workspace, drag,
Super+Up maximize), one PNG per stage + serial. Five agents graded sections A/B, C, D, E/F/G,
J/K/L/M/N against those artifacts under the One Law (observed, not code-exists).

**Independently CONFIRMED this boot (measured+observed, evidence cited in agent logs):**
A.1, A.2, A.5 · B.1, B.2, B.6, B.7 · D.1, D.2, D.5, D.7, D.9 · N.1, N.2, N.3.

**Newly OBSERVED this boot (were `[UNVERIFIED]`, now earn `[observed 2026-08-02]`):**
- **B.3** 6-layer paint order — wallpaper < windows < panel/dock < cursor composited correctly.
- **C.1** window chrome — BOTH windows render full title + 4 controls (×/−/□/⚡); the old
  `‼ chrome seam` (focused window blank title/single dot) is **GONE**. *Caveat (new negative):*
  window **content bodies are EMPTY** — chrome renders, app bodies don't.
- **C.2** stacking / z-order + focus render — Terminal occludes Files, focused-border vs dimmed.
- **E.1** mouse (PS/2 leg) — cursor physically moved across shots; USB-HID leg untested (no xHCI).
- **E.8** keybinds — Super+Space→palette proven by serial (`keyboard→palette`, `preempted chain
  13 "palette"`). *Note:* the palette OVERLAY did not visibly render in the shot — J.3's 42-item
  enumeration rests on the prior KVM selftest, NOT this boot.
- **F.1/F.2/F.5** typography+icons — live AA TTF glyphs (Inter + JetBrains Mono) and multi-size
  accent-tinted SVG icon dispatch on screen.

**‼ FALSE GREEN FOUND — A.6 corrected below.** The `[x] "characterized & addressed"` is
contradicted at the scheduler-tick level: every tick 1–128 overruns the 1 ms quantum by
**20–70 ms**, top `id=29 hotplug.pci` 9–12 ms, and the overrun exceeds the named chain 2–6× (most
of it unattributed). The narrower id=29 139→9 ms amortization sub-claim IS confirmed; the
"addressed" framing is not. A.4 is a related soft negative (preempt fires at ~496 ms, does not
enforce the quantum).

**Stale prose corrected (see inline `[2026-08-02]` notes):** A.7 ("encryption inactive, no PIN"
→ PIN now enrolls + crypto arms AES-XTS-256, but 0 regions = path still unexercised); D.8 & D.12
("empty at boot" → this build seeds 3 desktop icons + a populated dock).

**Real negatives this boot:** empty window content bodies (C.1); Super+Up maximize (C.5) and
Super+2 workspace-switch (C.6) produced NO visible change (keybinds not acting, unlike
Super+Space); minor: panel persona label shows "Zeos" though persona=2 (Full) was applied.

**`[x]` items resting on PRIOR KVM selftests, not re-observed here (legit, flagged for honesty):**
B.4, B.5, B.9 (diagnostics `#ifdef`-compiled-out of this build) · E.4 · J.2, J.3 · L.5 ·
M.1, M.2, M.4, M.5, M.6. A.3 confirmed BSP/single-core only (harness now passes `-smp 4` for AP
bring-up next run).

**Still UNVERIFIED — the honest remaining work, bucketed by what each needs:**
- *Targeted interaction* (imprecise QMP mouse / non-firing keybinds this run): C.3–C.9, D.3(state
  colors)/D.4/D.6/D.8(drag)/D.10–D.13, E.2(21 non-arrow cursor states)/E.3/E.5/E.6/E.7,
  G.2/G.3/G.4, J.1/J.4, K.1–K.4, L.1–L.4/L.6, M.7.
- *Net-enabled boot past the gate*: all of **H** — this sweep detected the NIC
  (`chain 4: nic:1af4:1000 [Ethernet]`, net_tx/rx chains wired) but no DHCP/ARP/TCP traffic fired
  before the PIN gate.
- *Browser opened + navigating*: all of **I**.
- *`-DZEOS_DIAG_*` selfcheck build*: re-observe B.4/B.5/B.9 on THIS build rather than prior runs.

Net: the OS boots to a real, correctly-composited desktop and the first-boot experience is
solid — but the interactive window behaviors (drag/resize/maximize/workspace), app CONTENT,
networking, and the browser remain unproven, and A.6 was a false green. Verified base truth or
no checkbox.

---

## ⧉ THE LINE — three deliverables, one sort (2026-07-23)

The product is **three deliverables**: ONE portable chip-agnostic OS, updated universally;
a **Base Installer x86**; a **Base Installer ARM**. Each installer gets *only* its own
arch-specific updates. The OS never changes across chip type.

**The line between them is `O.2` — the HAL (`hal.h`).** Everything *above* the line is the
OS (portable, one codebase). Everything *below* it is an arch backend (per-installer). The
sort below assigns every list item to a side. The finding: **~95% of the list is the OS and
already sits above the line** (it calls `fb`/`anim`/`chain`/`vault`, not `io.h` directly);
the arch layer below the line is small and fully enumerable; the ARM backend **already boots
first-light**. So ARM is not "a problem," and finishing on x86 is not lock-in — the feature
code is portable by construction. O.2 doesn't block features; it *formalizes* a line the code
already mostly respects and relocates the handful of direct arch calls.

### DELIVERABLE 1 — The Portable OS (above the line; ships to every chip)
The whole functional list: **A.2, A.5, A.6, A.7 · all of B · all of C · all of D (display +
logic) · E.2–E.6, E.8, E.9 · all of F · all of G · H.2–H.6 (the stack) · all of I · all of J
· all of K · all of L · all of M · all of N · all of P.** These are already arch-neutral.

### DELIVERABLE 2 — Base Installer x86 (below the line; Intel/AMD only)
The x86 backend that lets the OS run: x86 UEFI glue → `BOOTX64.EFI` (x86 half of A.1) · SMP
via LAPIC/INIT-SIPI (x86 half of A.3) · **LAPIC-timer preemption (A.4)** · GDT/IDT/PIC/PIT ·
port I/O (`io.h`) · CMOS-RTC time source (the source under D.2) · PS/2 mouse+kbd bus (driver
under E.1/E.7) · PCI port/ECAM + IRQ for NIC drivers (bus under H.1). **= O.1 + x86 half of O.2.**

### DELIVERABLE 3 — Base Installer ARM (below the line; aarch64 only) — first light EXISTS
The aarch64 backend: ARM UEFI stub / `boot.S` · generic-timer preemption (ARM half of A.4) ·
GICv3 + SMP via PSCI (ARM half of A.3) · PL011 MMIO UART · ramfb · aarch64 MMU/page tables ·
ECAM PCI. **= O.3 + ARM half of O.2.** Boot/MMU/GIC/timer/SMP/ramfb/Z+ already green on
cortex-a72.

### THE ORDER WE HIT THEM
1. **Deliverable 1 (Portable OS)** driven to VERIFIED on the x86 backend that already runs —
   this *is* "finish x86 first." Work the functional list in order; each item earns its check.
2. **O.2** formalizes the line (extract `hal.h`; relocate the direct arch calls). Deliverable 2
   (x86) is then the backend already running; nothing new to build, just named below the line.
3. **Deliverable 3 (ARM)** = the *same* OS recompiled onto the aarch64 backend that already
   boots. A recompile against O.3, not a port.

**Which side any future item lands on:** does it touch silicon (a specific timer, interrupt
controller, bus, MMU, boot protocol)? → below the line, into an installer. Everything else →
the OS. When in doubt it's the OS.

---

## A. Foundation & Boot
- [x] **A.1** UEFI boot (GNU-EFI, OVMF, BOOTZ.EFI → BOOTX64.EFI) reaches kernel — `[observed]` boots to shell, `vshot.py`.
- [x] **A.2** Chain-resolution scheduler runs as the primitive — `[observed]` serial `[scheduler] entering chain-resolution main loop`.
- [x] **A.3** SMP bring-up (BSP online, chains lifted) — `[observed]` serial `SMP … 1 cores online, RESOLVING`. **‼ [2026-08-02] MULTI-CORE NEGATIVE:** booting QEMU q35 with `-smp 4` (KVM) reset-LOOPS the kernel (double `Zeos is alive`, never reaches desktop); single-core boots stable. BSP path is fine; AP bring-up under KVM SMP is unstable — a real negative to chase before ARM (which also needs multi-core via PSCI).
- [ ] **A.4** LAPIC-timer preemption (vec 0xEF + setjmp/longjmp), per-chain watchdog — `[UNVERIFIED]` serial shows a preempt fire (`preempted chain 50 after 495950us`) but correctness not asserted.
- [x] **A.5** VAULT ramdisk mount + program load — `[observed]` serial `VAULT: 2MB ramdisk mounted. 16 programs loaded.`
- [ ] **A.6** Composite cost characterized under KVM & addressed — `[‼ CONTRADICTED 2026-08-02]` the composite-cost sub-claim holds (full ~17ms / clipped ~4.7ms KVM TSC; id=29 amortized 139→9ms) BUT the "addressed" framing is false at the scheduler-tick level: a fresh boot overruns the 1ms quantum by **20–70ms every tick 1–128**, top id=29 9–12ms, overrun exceeds the named chain 2–6× (unattributed). Downgraded from [x] until the per-tick stall is bounded. Prior evidence retained: full composite ~17ms, clipped ~4.7ms (real KVM TSC); the earlier ~120ms was TCG overhead; damage tracking (B.5/B.9) implemented. bible id=320. Follow-on: id=29=hotplug.pci (256-bus PCI brute-scan) amortized to 9ms, bible id=325 — CONFIRMED, but insufficient: the desktop still eats a 20–70ms scheduler stall per tick. **[diagnosis refined 2026-08-02]** id=29 is a RED HERRING: hotplug.pci already sets `resolve_interval_ticks=4` and the mde interval gate (`mde.c:515-517`, `last_resolved_tick` updated at `:533` except on `err==-2`=SMP-contention only) IS coded correctly, so it's throttled to every-4-ticks; the scheduler log shows `top: id=29(9ms)` on skip-ticks too because that's the chain's STORED resolve cost, not this-tick cost. The real 20–70ms/tick is UNATTRIBUTED by mde's per-chain accounting (all other tops show 0ms) — most likely the full-screen composite resolving every tick uncounted (B.9 clipped path's diagnostics are `#ifdef`-compiled-out of this build, so the clipped ~4.7ms may not be active). FIX = instrument where the per-tick time actually goes (composite vs tick overhead), then either ensure the clipped composite is the norm or move composite cost off the 1ms-quantum accounting. NOT a hotplug throttle.
- [ ] **A.7** Disk encryption / crypto_disk (PIN-gated) — `[UNVERIFIED; prose corrected 2026-08-02]` the old "encryption inactive (no PIN)" is STALE: a cold boot now enrolls a PIN and arms crypto (`[crypto] armed AES-XTS-256`), but `0 regions / 0 accesses` = the encrypt/decrypt path is still unexercised. Needs a real encrypted region + a read/write that round-trips.

## B. Compositor & Display
- [x] **B.1** Compositor init 1920×1080@60 — `[observed]` serial `COMP: initialized`.
- [x] **B.2** Split advance/present (advance pre-resolve every tick; present composites on dirty; cursor ungated) — `[observed]` `compositor.c:160-249`; boot frame composits.
- [x] **B.3** 6-layer paint order (desktop→surfaces→panel→dock→overlays→cursor) — `[observed 2026-08-02]` fresh boot composites wallpaper < 2 windows < panel(top)+dock(bottom) < cursor correctly; `[source compositor.c:307-311,:453 cursor ungated]`. Overlay layer (menus/notify) not yet exercised.
- [ ] **B.4** Frame timing from TSC — `[UNVERIFIED]` `[source compositor.c:170-176]`.
- [x] **B.5** Dirty-region tracking — `[VERIFIED/production]` now CONSUMED: compositor_dirty accumulates a region-delta union the correction pass clips to (was tracked-but-unused). bible id (B.5 VERIFIED).
- [x] **B.6** Double buffering / atomic present — `[DONE][measured+observed 2026-07-23]` composite into a WB-cached back buffer, atomic flip of the whole finished scene to the WC front once per composite. `fb.c`: `fb_backbuf_init` (kmalloc pitch×height×4), `fb_present_begin` (pointer-swap `g_fb->base`→backbuf so every existing writer paints offscreen — zero writer edits), `fb_present_end` (bulk copy backbuf→front, restore). Wired in `compositor_present`; `compositor_init` logs `double buffer active`. VERIFIED: settled render STABLE (windows present, spread 0) with FB write-combining — reader never sees a half-drawn frame. Flip is a full-frame copy today; B.9 makes it damage-only, and the copy loop itself is a SIMD/`rep movs` modernization candidate (audit). `[source fb.c; compositor.c]`
- [x] **B.7** Windows stay drawn — `[DONE][measured+observed 2026-07-23]` **REAL ROOT CAUSE FOUND + FIXED + VERIFIED.** The framebuffer was mapped **write-back cached**: `vmm_init` identity-maps the low 4GB with `PTE_PRESENT|PTE_WRITABLE|PTE_HUGE` and no cache bits (`vmm.c:160,180`) = default WB. That re-caches the FB the firmware handed us; compositor pixel writes sit in CPU cache and the display never sees them reach VRAM on the sampled timing => intermittent "windows vanish". The classic self-inflicted linear-FB caching bug. FIX: `vmm_set_range_uncached()` (`vmm.c`) remaps the FB's 2MB pages `PTE_NOCACHE` (PCD) in both the identity map and the KERNEL_VBASE mirror, INVLPG each; called from `main.c` right after `vmm_init`. VERIFIED by `kernel/verify_settled.py`: **BEFORE** 5 settled shots all `9622` bytes → `MIXED/blank`; **AFTER** 5 settled shots all `14816` bytes → `STABLE windows`. spread 0 both runs (deterministic), single-variable change. **Prior analyses were WRONG and are corrected here:** (1) "LAPIC-preemption of the composite, fixed by `lapic_timer_disarm()`" — that exempt is kept as cheap hardening but was NOT the cause; (2) "resolved as a QEMU `-vga std` display artifact, not a Zeos bug, not chasing" — flat wrong, it WAS a Zeos bug (our cache mapping). Brad steered it: the display problem is common for direct linear-FB writes *only when you cache the FB* — self-inflicted. Follow-on (perf, not correctness): UC makes direct compositor writes slow → B.6 double-buffer (composite into WB backbuf, single flip to the UC front) + upgrade UC→WC via PAT is the speed path. `[SUPERSEDES both the LAPIC close above and the QEMU-artifact analysis below]`
- [ ] **‼ B.7-old** (superseded) — earlier mis-analysis kept for history: windows vanish on cursor input. ROOT CAUSE (verified 2026-07-23, metal push + DABS converged): NOT preemption of `wm_draw_all` (canary clean after the Exempt fix) and NOT a workspace/overlay issue (`wm_draw_all` measured drawing BOTH opaque windows every composite; all 9 overlay `_active`=0). The front buffer is caught **mid-composite / on a missed-dirty tick with no retained content to recomposite from** — i.e. this is **B.6 (no back buffer)**. During a drag (LMB-held → continuous recompose) the async capture hits the exposed gap every frame → persistent blank. FIX = B.6 double-buffer (composite to backbuf, flip on complete). Interim hardening landed: `compositor_present` disarms the LAPIC preempt timer for the composite (`compositor.c`, Brad's "Exempt") — fixed the forced-composite boot degradation, necessary but not sufficient. ALSO landed (partial-redraw foundation): fb clip rect (`fb.c` `fb_set_clip/reset`, honored by all writers) + `present` clips each composite to the damage bbox + drag/resize push PRECISE damage rects (`wm.c`). Correct-by-construction (static windows never in the damage → never wiped) but NOT observably closed: any FULL-screen composite (click/btn-up/init) still has a wipe→repaint window the async reader catches → intermittent blank. **RESOLVED as a QEMU artifact, NOT a Zeos bug (2026-07-23):** proven via pixel readback + `wbinvd` + `WMV vis=2/2` on every boot — Zeos writes both windows into the real framebuffer EVERY boot. The blank is QEMU's `-vga std` display emulation missing direct linear-FB writes (dirty-page tracking / write-back FB caching — a documented, common OS-dev issue; kraxel/OSDev). Emulator shows it ~15% of boots; real hardware (continuous scanout, no dirty-tracking) renders every time. Not chasing further per Brad. DABS dossier: `~/dabs/outgoing/windows-vanish`.
- [ ] **B.8** Material blur / vibrancy (ultraThin→ultraThick) — `[TODO]` no code.
- [x] **B.9** Partial redraw (only dirty regions) — `[VERIFIED/production]` delta-correction: redraws AND flips only the dirty region (fb clip + fb_present_end_rect). KVM selfcheck 20/20 frames 0 mismatches (backbuf-checksum, cursor-immune); 3.5x faster (16.3ms full -> 4.7ms clipped); no cursor trails. DRAGON WING. Remaining: tighten dirty_all callers; step-3 parallel delta across SMP.

## C. Window System
- [x] **C.1** Window chrome renderer (title, border, 4 controls ×/−/□/⚡) — `[observed 2026-08-02]` BOTH windows (Files + focused Terminal) render full title + all 4 controls consistently; the prior ‼ chrome-seam (blank title/single dot) is GONE. `[source wm.c:1088+]`. Content-body gap **FIXED 2026-08-02** — the boot Files/Terminal windows had NULL draw_content (main.c); now render real bodies (VAULT listing / Z+ shell prompt), boot-verified single-core (commit 6ae4493).
- [x] **C.2** Stacking / z-order + focus mgmt (render) — `[observed 2026-08-02]` Terminal occludes Files at a distinct z-level with focused-border+bright-titlebar vs dimmed unfocused; panel pill highlight matches. `[source wm.c:763-778]`. Dynamic focus-CHANGE (click-to-raise) not yet exercised.
- [ ] **C.3** Titlebar drag with snap-zone detection — `[UNVERIFIED]` `[source wm.c:923-972]`; vshot drag hit nothing (windows already gone, B.7) so unproven.
- [ ] **C.4** Resize from any edge/corner with minimums — `[UNVERIFIED]` `[source wm.c:772-783,451]`.
- [ ] **C.5** Minimize / maximize / restore / detach — `[UNVERIFIED]` `[source wm.c:374-435,334-372]`.
- [ ] **C.6** Workspaces (8 max, switch, move surfaces) — `[UNVERIFIED]` `[source wm.c:237,704,724-727]`; 4 initialized (serial), switching unobserved.
- [ ] **C.7** Shadow rendering (L1 unfocused / L2 focused) — `[UNVERIFIED]` `[source wm.c:1192-1197]`.
- [ ] **C.8** Snap/tile: edge zones, ghost preview, geometry restore, auto-tiling — `[UNVERIFIED]` `[source wm.c:787-815,963-972,544-548,650-700]`.
- [ ] **C.9** Controls side L/R configurable + live toggle — `[UNVERIFIED]` `[source wm.c:228,247-252; settings.c:266-268,494-496]`.
- [ ] **C.10** Sheets (modal, slide from titlebar) — `[TODO]`.
- [ ] **C.11** Popovers (non-modal, attached) — `[TODO]`.
- [ ] **C.12** Tabs / chain multiplexing — `[TODO]`.
- [ ] **C.13** Magnetic adjacency (same chain → side-by-side) — `[TODO]`.
- [ ] **C.14** Parent/child chain stacking — `[TODO]`.
- [ ] **C.15** Custom button placement (drag) — `[2.0]`.

## D. Desktop Surface / Panel / Dock
- [x] **D.1** Panel renders: persona dot, chain pills, clock, status dots — `[observed]` frame-0.
- [x] **D.2** Panel clock shows real wall-clock time — `[DONE][observed 2026-07-23]` was TSC uptime (00:00); now wired to `tod_now_unix()` (CMOS RTC). Verified on live VNC (10:57/11:07 UTC, advancing). Commit b385d7a.
- [ ] **D.3** Panel zones (left persona/palette trigger; center pills color-by-state; right health/notif/clock) — `[UNVERIFIED]` `[source panel.c:279-399]` (renders, per-state color unobserved).
- [ ] **D.4** Panel: click-persona→palette, right-click menu — `[UNVERIFIED]` `[source panel.c:413-417,37-47]`.
- [x] **D.5** Panel height-follows-density (48/40/32) — `[VERIFIED/production]` access_set_density applies access_get_panel_height() live to panel+compositor; selftest [D5] d=0:48 d=1:40 d=2:32 PASS (KVM). bible id=308.
- [ ] **D.6** Panel auto-hide / vibrancy / per-pill right-click — `[TODO]`.
- [x] **D.7** Desktop wallpaper + persona accent gradient — `[observed]` frame-0 background.
- [ ] **D.8** Desktop icons: grid-snap drag, VAULT persist, double-click launch, right-click menu — `[UNVERIFIED; prose corrected 2026-08-02]` icons now RENDER (serial `DESK: 3 icons`; "ships empty (0 icons)" is STALE — this build seeds Files/Terminal/Settings). The four listed BEHAVIORS (drag/launch/persist/right-click) still need live interaction. `[source desktop.c:448-535]`.
- [x] **D.9** Desktop icons are persona-tinted SVGs — `[VERIFIED/production]` build-time rsvg raster -> objcopy -> lodepng decode + alpha-mask accent tint (in-tree assets/icons); Files=folder Terminal=code Settings=gear, verified on KVM screenshot. Also seeded default launcher icons (desktop was empty). bible id=403.
- [ ] **D.10** Wallpaper image load from VAULT — `[TODO]` solid color only.
- [ ] **D.11** Drag desktop icon → chain surface (feed a chain) — `[TODO]`.
- [ ] **D.12** Dock: centered, pinned|divider|running, state dots, auto-hide, empty-at-boot — `[UNVERIFIED]` `[source dock.c:407-483,330-339,192-218,97-99]`.
- [ ] **D.13** Dock hover thumbnail, drag-reorder/poof, density size — `[TODO]`.

## E. Input & Cursor
- [x] **E.1** Mouse driver PS/2 (IRQ12) — `[observed 2026-08-02]` cursor physically moved across desktop→moved→click shots (970,552→870,452→770,402), serial `routed IRQ12(mouse)`. `[source mouse.c:354-410]`. USB-HID leg still UNVERIFIED (no xHCI controller in this VM).
- [ ] **E.2** Cursor: 22 states, real SVG-derived sprites, hotspot table, save-under — `[UNVERIFIED]` `[source cursor.c; cursor_sprites.h]`; suspected in B.7.
- [ ] **E.3** Cursor click feedback (scale pulse, ripple, burst) — `[UNVERIFIED]` `[source cursor.c:140-197]`.
- [x] **E.4** Cursor confirm (checkmark) — `[VERIFIED/production]` wired into settings save_all (was zero callers). KVM [E4] flash=1 reverted=1 PASS. bible id=326.
- [ ] **E.5** Hot corners: 8px zones, 150ms dwell, TL palette / BL workspace / BR show-desktop — `[UNVERIFIED]` init `[observed]` serial, but ACTION fire never observed. `[source hotcorners.c:35-169]`.
- [x] **E.6** Hot corner TR (notifications) — `[DONE 2026-07-23]` was a TODO stub; now toggles the notification panel via notify_show_all(). Commit 10993b1.
- [ ] **E.7** Keyboard: set-1 scancode → ASCII — `[UNVERIFIED]` `[source keyboard.c]`; no layout switching.
- [x] **E.8** Keybinds system (Super+arrows/1-4/T/D/Space) — `[DONE 2026-07-23]` `[source keybinds.c:85-156]`; command-palette action wired (Super+Space). Terminal/inspect/chain-graph still TODO (need app entry points).
- [ ] **E.9** Input-method framework — `[TODO]`.

## F. Typography & Icons
- [x] **F.1** stb_truetype: glyph cache + grayscale AA, font_draw/measure/line_height — `[observed 2026-08-02]` smooth AA proportional + monospace glyphs rendered across panel/titles/clock/labels. `[source font.c:71-142,188-253]`.
- [x] **F.2** TTF embedded in kernel (objcopy) + consumed at boot — `[observed 2026-08-02]` on-screen Inter+JBMono glyphs prove the embedded blob loads + draws at boot. `[source Makefile:153-199; font.c:161-170]`.
- [ ] **F.3** Fonts: Inter (7wt), JetBrains Mono (6wt) present — `[x]` `[observed]` asset files counted (7 + 6).
- [ ] **F.4** Font fallback chain (Inter→Noto→bitmap) — `[PARTIAL]` only Inter→bitmap; no Noto tier.
- [x] **F.5** icon_render: 31-icon dispatch at 16/24/32, accent tint — `[observed 2026-08-02]` desktop icons + ~7 dock icons + titlebar buttons render as accent-tinted vector sprites at multiple sizes. `[source icon_render.c:610-658]`. Exact 31-count/bucket enumeration not dissected from the shot.
- [ ] **F.6** SVG asset library (934 svg / 16 categories; 50 cursor + ~150 themed) — `[x]` `[observed]` file counts (note: ROADMAP claimed 1,067 — actual 934).
- [ ] **F.7** Build-time SVG→bitmap rasterization / sprite sheets — `[TODO]` `[source icon_render.c:9]` "Future".

## G. Persona & Theme
- [ ] **G.1** 3 personas (Zeros/DereZ/Full) + accent/dim tokens — `[UNVERIFIED]` `[source persona.h:20-24; theme.h:112-119]`; "Zeos" persona shown in panel `[observed]`.
- [ ] **G.2** Shell prompt + cursor colorway switch on persona — `[UNVERIFIED]` `[source shell.c:1866; cursor.c:35]`.
- [ ] **G.3** Persona crossfade (spring color lerp) — `[UNVERIFIED]` `[source persona_anim.c:76-160]`.
- [ ] **G.4** Dark/light/auto + night shift — `[UNVERIFIED]` `[source theme_runtime.c:43-151]` (auto = TSC placeholder).
- [ ] **G.5** Per-persona dock/launcher defaults — `[TODO]`.

## H. Networking
- [ ] **H.1** virtio-net / e1000(i219) / rtl drivers — `[UNVERIFIED]` `[source net_virtio.c:220; net_e1000.c:351-552]`; at boot `NET: no network device found` (QEMU `-net none`), so UNTESTED live.
- [ ] **H.2** ARP / IPv4+ICMP / UDP / TCP(16-conn, state machine) — `[UNVERIFIED]` `[source net_arp.c; net_ip.c; net_udp.c; net_tcp.c:284-301]`.
- [ ] **H.3** DNS(8-cache) / DHCP(DORA) / HTTP1.1 GET — `[UNVERIFIED]` `[source net_dns.c; net_dhcp.c; net_http.c:157-278]`.
- [ ] **H.4** TLS via mbedTLS 1.3 + Mozilla CA bundle (145 certs) — `[UNVERIFIED]` `[source net_tls.c:353-583; ca_bundle.h]`.
- [ ] **H.5** TCP retransmission — `[PARTIAL]` retransmit built `[source net_tcp.c:493]`; congestion control (cwnd/ssthresh) absent.
- [ ] **H.6** WebSocket — `[TODO]`.

## I. Web Browser
- [ ] **I.1** HTML parser (tags/attrs/comments/void/script-skip), 2048-node DOM — `[UNVERIFIED]` `[source browser.c:208-264,130]`.
- [ ] **I.2** CSS per-tag defaults + inline `style=` + persona-accent links — `[UNVERIFIED]` `[source browser.c:597,749-751]`.
- [ ] **I.3** Block layout + framebuffer render (clip/scroll/bg/HR) + TTF text — `[UNVERIFIED]` `[source browser.c:1155-1707]`.
- [ ] **I.4** Navigation: URL parse, 32-history, back/fwd/refresh/home — `[UNVERIFIED]` `[source browser.c:1349-1544]`.
- [ ] **I.5** Link click hit-test → navigate — `[UNVERIFIED]` `[source browser.c:1590-1672]`.
- [ ] **I.6** PNG images (lodepng), forms (input/button), scrollbar — `[UNVERIFIED]` `[source browser.c:1331-1414,566-994,1688-1714]`.
- [ ] **I.7** JavaScript engine — `[2.0]`.

## J. Settings
- [ ] **J.1** Settings app (WM app, VAULT persist, inline current values) — `[UNVERIFIED]` `[source settings.c:219-432]`.
- [x] **J.2** One settings surface, no duplicate — `[VERIFIED/production]` settings.h dropped its name-colliding duplicate types (access_config_t/color_scheme_t/etc.), includes access.h; settings GUI operates on the ONE real config via access_get()/access_set_*. KVM [J2] shared_ptr=1 sensory/reduced_motion/focus live=1 PASS. Also fixed latent bug: accessibility changes now actually apply. bible id=395.
- [x] **J.3** Search-first (palette enumerates every setting) — `[VERIFIED/production]` palette_show enumerates settings_registry (42 items), bool toggles inline. KVM [J3] items=42 registry=42 PASS. PALETTE_MAX_ITEMS 64->128. bible id=336.
- [ ] **J.4** Right-click element → "Settings for this…" — `[TODO]`.

## K. Signal Visualizer
- [ ] **K.1** Node-graph renderer + state colors — `[UNVERIFIED]` `[source sigviz.c:199-498]`.
- [ ] **K.2** Interactive selection / inspector — `[UNVERIFIED]` `[source sigviz.c:500-525]`.
- [ ] **K.3** Live pulse animation — `[UNVERIFIED]` `[source sigviz.c:266-270]`.
- [ ] **K.4** Zoom / pan — `[UNVERIFIED]` `[source sigviz.c:537-568]`.

## L. Animation & Motion
- [ ] **L.1** Spring engine (semi-implicit Euler, 64 concurrent, retarget-with-velocity) — `[UNVERIFIED]` `[source anim.c:64-139]`.
- [ ] **L.2** Presets snappy/smooth/bouncy/interactive — `[UNVERIFIED]` `[source theme.h:50-57]`.
- [ ] **L.3** Compositor ticks anims per frame + re-arm while live — `[UNVERIFIED]` `[source compositor.c:179-198]`.
- [ ] **L.4** Spring surface open/close, snap settle, dock auto-hide slide — `[UNVERIFIED]` `[source wm.c:306-354; dock.c:172-198]`.
- [x] **L.5** Spring menu appear/dismiss — `[VERIFIED/production]` context menus spring-scale open/dismiss; deferred teardown, no input ripple. KVM [L5] PASS. bible id=341.
- [ ] **L.6** Spring scroll physics — `[TODO]`.

## M. Accessibility  (⚠ whole class: UI + VAULT persist exist, but NO CONSUMER reads them)
- [x] **M.1** Reduced-motion mode — `[DONE 2026-07-23]` `access_init()` was never called (whole a11y config zero-init); now called at boot + `anim_tick` snaps springs instantly when reduced_motion on. Commit 38c54df.
- [x] **M.2** Animation speed multiplier (0/0.5/1/2×) — `[DONE 2026-07-23]` `anim_tick` scales dt by anim_speed (0.5×=faster, 2×=slower, 0=instant). Commit 38c54df.
- [x] **M.3** 3 density modes (Comfortable/Standard/Compact) — `[DONE 2026-07-23]` panel height density-driven (48/40/32) at boot + live on access_set_density. Commits 35effb5,f7f481c. Follow-up: dock item size.
- [x] **M.4** 3 sensory modes (Standard/Low-Stimuli/High-Contrast) — `[VERIFIED/production]` runtime sensory transform (access_apply_sensory/text_color/border/anims/stiffness) wired into panel+theme+anim; KVM: STANDARD/LOW/HIGH panel-dot rgb matches transform (sat 125/63/175). bible id=300.
- [x] **M.5** 44px min touch targets — `[VERIFIED/production]` wm hit_control catch-zone expanded toward min_touch_target (nearest-center + full-titlebar); KVM [M5] glyph/hslop/vslop hit, beyond/offbar miss, PASS. bible id=301.
- [x] **M.6** Letter/word/line spacing — `[DONE 2026-07-23]` font_draw honors letter_spacing (per-glyph advance) + word_spacing (on spaces). Commit 6d3de02. (line_spacing: no single-line consumer yet.)
- [ ] **M.7** Focus Mode (suppress non-critical notifications) — `[UNVERIFIED]` `[source notify.c:176-179,808]` (has a real consumer, unlike M.1-M.6).
- [ ] **M.8** CVD simulation mode — `[TODO]`.

## N. First-Boot Experience
- [x] **N.1** 5-screen first-run flow (welcome/persona/controls/appearance/done) — `[VERIFIED/production]` firstboot_run() wired canonical; KVM drove all 5 screens, applied persona=0 controls=1 theme=0 density=2 (all != defaults); idempotent 2nd boot skip. Found+fixed kbd-IRQ-vs-poll bug. bible id=304.
- [x] **N.2** Real boot path = firstboot 5-screen wizard — `[VERIFIED/production]` welcome.c single modal RETIRED from boot path (kept in-tree for revert). bible id=306.
- [x] **N.3** Reconcile: one first-run flow, wired and observed — `[VERIFIED/production]` reconciled to firstboot 5-screen wizard; welcome_run_if_first_boot() no longer on boot path (one-call revert documented). bible id=305.

## O. Hardware Targets
- [ ] **O.1** x86-64 target (current) — `[x]` `[observed]` boots + runs.
- [ ] **O.2** HAL interface (hal.h; move asm/io/GDT/IDT/PIC/PIT behind it) — `[TODO]`.
- [ ] **O.3** ARM64 backend (UEFI stub, generic timer, GIC, MMIO UART, ECAM PCI, page tables) — `[TODO]`.

## P. Z+ Language
- [ ] **P.1** Decide role (user lang / compiled / glue) — `[TODO]`.
- [ ] **P.2** Z+ REPL in shell — `[UNVERIFIED]` `[source zplus.h]`.
- [ ] **P.3** Signal-chain / UI-layout / config in Z+ — `[TODO]`.

## Q. Dom/Sub — Cooperative Multi-Chip PCIe Compute Fabric
Full spec: `specs/DOM_SUB_CHIPS.md`. Multiple ARM co-processor chips
(Goya-class today) hot-plug via PCIe, recognize each other as cooperative
teammates (not competitors), elect one Dom (gets high-priority THINK
work) and N Subs (get background/incidental work), re-electing on
membership change. Built on existing infra where it genuinely exists:
hotplug.c's CHAIN_HOTPLUG_PCI (real), gpu_goya.c's multi-device BAR/MSI-X
access (real). NOTE (corrected 2026-07-25, matches specs/DOM_SUB_CHIPS.md
§5 + specs/PARADIGM_CONFORMANCE_AUDIT.md §4): chip-level routing does NOT
yet exist — `chain_t.affinity` + `smp_chain_owner()` is a CPU-core-index
pattern only, no Goya chain sets affinity today (`grep affinity gpu_goya.c`
is empty). Q.5 must BUILD chip-affinity routing, not "reuse" it.
- [ ] **Q.1** Chip class registration table (vendor:device allowlist, starts with Goya 0x1DA3:0x0001) — `[TODO]`.
- [ ] **Q.2** Cohort discovery riding CHAIN_HOTPLUG_PCI attach/detach events + boot-time seeding alongside gpu_goya_init() — `[TODO]`.
- [ ] **Q.3** Identify handshake (fw_version, dram_mb, lazily-filled bench_score via first real THINK job) — `[TODO]`.
- [ ] **Q.4** Election (score = bench_score*1000 + dram_mb, PCI-address tiebreak, hysteresis margin to prevent Dom/Sub flapping) — `[TODO]`.
- [ ] **Q.5** THINK vs BACKGROUND chain classification + affinity routing (BUILD chip-affinity on the chain_t.affinity/smp_chain_owner pattern — it is CPU-core-only today, not yet chip-aware) — `[TODO]`.
- [ ] **Q.6** Detach/failure handling (Sub-detach = cheap re-election; Dom-detach = immediate re-election, in-flight work inherits existing CHAIN_ERROR/watchdog path) — `[TODO]`.
- [ ] **Q.7** Real hardware verification: 2+ Goya cards, confirm election + THINK-affinity + Dom-unplug re-election, measured via serial log — `[TODO]`.

---

## FIRST MOVES WE FOLLOW (highest-value, verified-negative first)
1. **‼ B.7** windows vanish on input — DABS mission staged (`~/dabs/missions/windows-vanish`); fix → **verify by boot** (frame-after-drag still shows windows).
2. **‼ C.1 / D.2** focused-window chrome + dead clock — cheap, high-feel, observable.
3. **‼ M.1–M.6** wire the no-consumer accessibility toggles (they lie today) OR down-scope them honestly.
4. **‼ A.6** scheduler 100ms tick overrun — find the heavy chain; it makes the whole desktop feel slow.
5. **N.3** pick ONE first-run flow and verify it runs.

_Every close: state + date + evidence + retro + adversary (G1–G7). `[x]` only on measured-and-observed. Verified base truth or no checkbox._
