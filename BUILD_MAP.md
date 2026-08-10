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
- [x] **A.3** SMP bring-up (BSP online, chains lifted) — `[observed]` serial `SMP … 1 cores online, RESOLVING`. **[2026-08-02] MULTI-CORE reset-loop ROOT-CAUSED + FIXED:** `-smp 4` reset-LOOPED (5+ continuous resets, never past SMP init). Root cause: **APs never loaded an IDT** — the trampoline loads a GDT but nothing did `lidt` on APs, so an AP's first exception/IPI (after `sti`) triple-faulted → reset. FIX: `idt_load_ap()` (idt.c) called in `ap_main` (smp.c) before enabling interrupts. Result: reset-loop GONE. VERIFIED `[observed 2026-08-02]` a `-smp 4` boot runs stably to **4 cores online**, PAST the PIN gate, `first-run applied` (completed the 5-screen first-boot flow to the desktop stage), reset count stable (2, not climbing) over a full ~90s run, **0 panics/faults**. Residual: ~1 early transient reset before first-boot (minor, under investigation); the `graphical desktop shell up` marker doesn't grep-match under SMP because concurrent AP `kputs` interleaves the serial (kprint locks per-call, not per-line — cosmetic), so a clean multi-core desktop screenshot needs per-line serial locking or a settled shot. The LOOP that blocked multi-core is fixed. Bears directly on ARM (multi-core via PSCI). Commit: idt.c/idt.h/smp.c.
- [x] **A.4** LAPIC-timer preemption (vec 0xEF + setjmp/longjmp), per-chain watchdog — `[VERIFIED/harness]` preempt fires + recovers safely (EOI, chain->ERROR, force-unlock, sti-on-longjmp so IRQs survive — id=425); system continues to live desktop. Quantum-timing accuracy (fires ~496ms not 1ms) is separate (A.6). bible id=458.
- [x] **A.5** VAULT ramdisk mount + program load — `[observed]` serial `VAULT: 2MB ramdisk mounted. 16 programs loaded.`
- [x] **A.6** Composite cost characterized under KVM & addressed — `[‼ CONTRADICTED 2026-08-02]` the composite-cost sub-claim holds (full ~17ms / clipped ~4.7ms KVM TSC; id=29 amortized 139→9ms) BUT the "addressed" framing is false at the scheduler-tick level: a fresh boot overruns the 1ms quantum by **20–70ms every tick 1–128**, top id=29 9–12ms, overrun exceeds the named chain 2–6× (unattributed). Downgraded from [x] until the per-tick stall is bounded. Prior evidence retained: full composite ~17ms, clipped ~4.7ms (real KVM TSC); the earlier ~120ms was TCG overhead; damage tracking (B.5/B.9) implemented. bible id=320. Follow-on: id=29=hotplug.pci (256-bus PCI brute-scan) amortized to 9ms, bible id=325 — CONFIRMED, but insufficient: the desktop still eats a 20–70ms scheduler stall per tick. **[diagnosis refined 2026-08-02]** id=29 is a RED HERRING: hotplug.pci already sets `resolve_interval_ticks=4` and the mde interval gate (`mde.c:515-517`, `last_resolved_tick` updated at `:533` except on `err==-2`=SMP-contention only) IS coded correctly, so it's throttled to every-4-ticks; the scheduler log shows `top: id=29(9ms)` on skip-ticks too because that's the chain's STORED resolve cost, not this-tick cost. The real 20–70ms/tick is UNATTRIBUTED by mde's per-chain accounting (all other tops show 0ms). **[ATTRIBUTED 2026-08-02]** Instrumented the tick (scheduler.c): `compositor_advance()`+`compositor_present()` and `persistence_checkpoint_if_due()` are called DIRECTLY in the loop, NOT as measured chains, so their cost was invisible. The overrun log now prints `composite=Nms persist=Nms`, and the phantom time is FULLY explained: normal ticks = **composite ~34-46ms** (+ id=29 ~19ms); every ~64th tick = **persist 66-119ms** (the VAULT checkpoint block-chain flush). Accounting is now HONEST. Remaining PERF work (separate from accounting): (1) ensure composite uses the clipped/partial-redraw path (B.9 ~4.7ms) instead of full ~40ms — **pinpointed to compositor.c:275-276**: `if (anim_active_count() > 0) compositor_dirty_all()` forces a FULL-screen recomposite every tick whenever ANY spring is live, even one touching a tiny region (pulsing dot). Fix = have animations dirty only their bounding region so `use_clip` engages, instead of blanket `dirty_all`. (2) periodic ~100ms VAULT checkpoint — **FIXED `[observed 2026-08-02]`**. It was the *perpetual* stall: `persistence_on_resolve_complete` (persistence.c:546) marked a snapshot due every `CHECKPOINT_EVERY` resolves (count-based), so idle read-only pumps triggered a full block-chain flush every ~64 ticks forever. Fix (persistence.c, commit e39deb1): CRC the chain records in `save_snapshot_now` and skip the `vault_write`+`vault_sync` when byte-identical to the last save — SAFE (any real mutation changes the CRC; first save always runs). VERIFIED by controlled same-harness comparison: before, ticks 64/128/192 overran with `persist=66-119ms`; after, those spikes are GONE (persist=0, no tick-64+ overruns). **Key insight:** the persist spike was PERPETUAL; the composite ~40ms is TRANSIENT (only while a spring is animating — at steady idle `anim_active_count()==0` so composite=0 and ticks fit budget). So this persist fix is the larger "feels slow" win; the composite region-dirty optimization (fix #1, compositor.c:275) remains but only affects animation frames, not idle. NOT a hotplug throttle. **[RESOLVED 2026-08-03: perpetual stall BOUNDED — CHECKPOINT_EVERY 500->2000, ~38ms flush now ~every-960-ticks not every-64; steady-state fits budget; skip-CRC found ineffective (state churns); async flush = future. bible id=463]**
- [x] **A.7** Disk encryption / crypto_disk (PIN-gated) — `[VERIFIED/production]` live boot-armed AES-XTS-256 round-trips via the production `selftest` shell: create region → encrypt (cipher differs) → decrypt (recovers exactly) → destroy. Serial `A.7 aes-xts (armed=1 cipher_differs=1 recovered=1): PASS`, settled 2-boot, adversarial review confirmed. (Uses the live key — no re-key, unlike harness.) bible id=497. `[source crypto_disk.c; shell.c cmd_selftest]`. `[source crypto_disk.c crypto_disk_transform]`.

## B. Compositor & Display
- [x] **B.1** Compositor init 1920×1080@60 — `[observed]` serial `COMP: initialized`.
- [x] **B.2** Split advance/present (advance pre-resolve every tick; present composites on dirty; cursor ungated) — `[observed]` `compositor.c:160-249`; boot frame composits.
- [x] **B.3** 6-layer paint order (desktop→surfaces→panel→dock→overlays→cursor) — `[observed 2026-08-02]` fresh boot composites wallpaper < 2 windows < panel(top)+dock(bottom) < cursor correctly; `[source compositor.c:307-311,:453 cursor ungated]`. Overlay layer (menus/notify) not yet exercised.
- [x] **B.4** Frame timing from TSC — `[VERIFIED/harness]` frame_dt TSC-derived: summed 93202us == raw-TSC 93202us (diff=0), vs 49998us 1/60 fallback -> on TSC path. Fixed selftest exact-equality false-alarm. bible id=436. `[source compositor.c:226-258]`.
- [x] **B.5** Dirty-region tracking — `[VERIFIED/production]` now CONSUMED: compositor_dirty accumulates a region-delta union the correction pass clips to (was tracked-but-unused). bible id (B.5 VERIFIED).
- [x] **B.6** Double buffering / atomic present — `[DONE][measured+observed 2026-07-23]` composite into a WB-cached back buffer, atomic flip of the whole finished scene to the WC front once per composite. `fb.c`: `fb_backbuf_init` (kmalloc pitch×height×4), `fb_present_begin` (pointer-swap `g_fb->base`→backbuf so every existing writer paints offscreen — zero writer edits), `fb_present_end` (bulk copy backbuf→front, restore). Wired in `compositor_present`; `compositor_init` logs `double buffer active`. VERIFIED: settled render STABLE (windows present, spread 0) with FB write-combining — reader never sees a half-drawn frame. Flip is a full-frame copy today; B.9 makes it damage-only, and the copy loop itself is a SIMD/`rep movs` modernization candidate (audit). `[source fb.c; compositor.c]`
- [x] **B.7** Windows stay drawn — `[DONE][measured+observed 2026-07-23]` **REAL ROOT CAUSE FOUND + FIXED + VERIFIED.** The framebuffer was mapped **write-back cached**: `vmm_init` identity-maps the low 4GB with `PTE_PRESENT|PTE_WRITABLE|PTE_HUGE` and no cache bits (`vmm.c:160,180`) = default WB. That re-caches the FB the firmware handed us; compositor pixel writes sit in CPU cache and the display never sees them reach VRAM on the sampled timing => intermittent "windows vanish". The classic self-inflicted linear-FB caching bug. FIX: `vmm_set_range_uncached()` (`vmm.c`) remaps the FB's 2MB pages `PTE_NOCACHE` (PCD) in both the identity map and the KERNEL_VBASE mirror, INVLPG each; called from `main.c` right after `vmm_init`. VERIFIED by `kernel/verify_settled.py`: **BEFORE** 5 settled shots all `9622` bytes → `MIXED/blank`; **AFTER** 5 settled shots all `14816` bytes → `STABLE windows`. spread 0 both runs (deterministic), single-variable change. **Prior analyses were WRONG and are corrected here:** (1) "LAPIC-preemption of the composite, fixed by `lapic_timer_disarm()`" — that exempt is kept as cheap hardening but was NOT the cause; (2) "resolved as a QEMU `-vga std` display artifact, not a Zeos bug, not chasing" — flat wrong, it WAS a Zeos bug (our cache mapping). Brad steered it: the display problem is common for direct linear-FB writes *only when you cache the FB* — self-inflicted. Follow-on (perf, not correctness): UC makes direct compositor writes slow → B.6 double-buffer (composite into WB backbuf, single flip to the UC front) + upgrade UC→WC via PAT is the speed path. `[SUPERSEDES both the LAPIC close above and the QEMU-artifact analysis below]`
- [ ] **‼ B.7-old** (superseded) — earlier mis-analysis kept for history: windows vanish on cursor input. ROOT CAUSE (verified 2026-07-23, metal push + DABS converged): NOT preemption of `wm_draw_all` (canary clean after the Exempt fix) and NOT a workspace/overlay issue (`wm_draw_all` measured drawing BOTH opaque windows every composite; all 9 overlay `_active`=0). The front buffer is caught **mid-composite / on a missed-dirty tick with no retained content to recomposite from** — i.e. this is **B.6 (no back buffer)**. During a drag (LMB-held → continuous recompose) the async capture hits the exposed gap every frame → persistent blank. FIX = B.6 double-buffer (composite to backbuf, flip on complete). Interim hardening landed: `compositor_present` disarms the LAPIC preempt timer for the composite (`compositor.c`, Brad's "Exempt") — fixed the forced-composite boot degradation, necessary but not sufficient. ALSO landed (partial-redraw foundation): fb clip rect (`fb.c` `fb_set_clip/reset`, honored by all writers) + `present` clips each composite to the damage bbox + drag/resize push PRECISE damage rects (`wm.c`). Correct-by-construction (static windows never in the damage → never wiped) but NOT observably closed: any FULL-screen composite (click/btn-up/init) still has a wipe→repaint window the async reader catches → intermittent blank. **RESOLVED as a QEMU artifact, NOT a Zeos bug (2026-07-23):** proven via pixel readback + `wbinvd` + `WMV vis=2/2` on every boot — Zeos writes both windows into the real framebuffer EVERY boot. The blank is QEMU's `-vga std` display emulation missing direct linear-FB writes (dirty-page tracking / write-back FB caching — a documented, common OS-dev issue; kraxel/OSDev). Emulator shows it ~15% of boots; real hardware (continuous scanout, no dirty-tracking) renders every time. Not chasing further per Brad. DABS dossier: `~/dabs/outgoing/windows-vanish`.
- [x] **B.8** Material vibrancy (ultraThin→ultraThick) — `[VERIFIED/production]` 5-level ladder (alpha 140/179/209/230/247 monotonic) via the real fb_material_alpha() run from the production `selftest` shell command: serial `B.8 material ladder: 140/179/209/230/247 -> PASS`, settled 2-boot, adversarial review confirmed. (First pure-logic item verified via the production-shell selftest method.) bible id=489. NOTE: translucency tier, true gaussian blur not implemented. `[source fb.c fb_material_alpha; shell.c cmd_selftest]`. `[source fb.c fb_material_fill]`.
- [x] **B.9** Partial redraw (only dirty regions) — `[VERIFIED/production]` delta-correction: redraws AND flips only the dirty region (fb clip + fb_present_end_rect). KVM selfcheck 20/20 frames 0 mismatches (backbuf-checksum, cursor-immune); 3.5x faster (16.3ms full -> 4.7ms clipped); no cursor trails. DRAGON WING. Remaining: tighten dirty_all callers; step-3 parallel delta across SMP.

## C. Window System
- [x] **C.1** Window chrome renderer (title, border, 4 controls ×/−/□/⚡) — `[observed 2026-08-02]` BOTH windows (Files + focused Terminal) render full title + all 4 controls consistently; the prior ‼ chrome-seam (blank title/single dot) is GONE. `[source wm.c:1088+]`. Content-body gap **FIXED 2026-08-02** — the boot Files/Terminal windows had NULL draw_content (main.c); now render real bodies (VAULT listing / Z+ shell prompt), boot-verified single-core (commit 6ae4493).
- [x] **C.2** Stacking / z-order + focus mgmt (render) — `[VERIFIED/production]` Terminal occludes Files at a distinct z-level; focus discriminator = bright cyan border on the focused window + matching highlighted panel pill (traffic-light controls are lit on BOTH, not a focus tell). Observed on the real cold-boot path (no diag/bypass), settled 2-boot oracle; independent adversarial review confirmed. bible id=473. `[source wm.c:763-778]`. Dynamic focus-CHANGE (click-to-raise) not yet exercised.
- [x] **C.3** Titlebar drag with snap-zone detection — `[VERIFIED/production]` USB-mouse drag to edge snapped Files to left half. bible id=409.
- [x] **C.4** Resize from any edge/corner with minimums — `[VERIFIED/production]` grab band widened 4->8px; selftest [C4] all edges+corner detected, center/12px-in rejected. bible id=410.
- [x] **C.5** maximize / restore — `[observed 2026-08-02]` Super+Up MAXIMIZES the focused Terminal to fill the screen (content intact); Super+Down restores it. Verified after correcting TWO harness confounds (my first "non-acting" verdict was WRONG): (1) the modal palette was left open and ate all subsequent keys; (2) Super+2 is snap-TR, not workspace. A `kbd-diag` trace confirmed `sc=48 ext=1 mod=8` (Super+Up, extended, SUPER) reaches the matcher and fires ACTION_MAXIMIZE. Extended-scancode path is FINE. Minimize/detach still unexercised. `[source wm.c:374-435; keybinds.c:95-96]`.
- [x] **C.6** Workspaces (switch) — `[observed 2026-08-02]` Super+F2 switches to an EMPTY workspace (both windows gone; panel ws-dot turns yellow), Super+F1 returns them; serial `WS: switch 0 -> 1` / `1 -> 0`; shots 32661→14621(empty)→32357(back). `[source wm.c:734-748; keybinds.c:125-126]`. "move surfaces to workspace" not yet exercised.
- [x] **C.7** Shadow rendering (L1 unfocused / L2 focused) — `[VERIFIED/production]` focused window carries L2 shadow (observed). bible id=412.
- [x] **C.8** Snap to quadrant — `[observed 2026-08-02]` Super+2 (ACTION_SNAP_TR) snaps the focused Terminal cleanly into the top-right quadrant. `[source wm.c:787-815; keybinds.c:100]`. Edge-zone DRAG snap, ghost preview, and auto-tiling still unexercised.
- [x] **C.9** Controls side L/R configurable + live toggle — `[VERIFIED/production]` controls_side=LEFT -> buttons render left (screenshot); apply path wired. bible id=413.
- [x] **C.10** Sheets (modal, slide from titlebar) — `[VERIFIED/harness]` sheet_open springs from parent titlebar, modal-to-parent-only (scrim), sheet_close animates out. bible id=453. `[source ui_context_menu.c sheet_*]`.
- [x] **C.11** Popovers (non-modal, attached) — `[VERIFIED/harness]` popover_open/draw/close: anchor-attached, on-screen clamp (flip near edges), NON-modal (context_menu_active stays 0). bible id=452. `[source ui_context_menu.c popover_*]`.
- [x] **C.12** Tabs / chain multiplexing — `[VERIFIED/harness]` wm_tab_add/switch: surface multiplexes up to 8 chains as tabs; switch repoints rendered chain; OOB rejected. bible id=454. `[source wm.c wm_tab_*]`.
- [x] **C.13** Magnetic adjacency (same chain → side-by-side) — `[VERIFIED/harness]` wm_snap_adjacent docks same-chain_id surfaces side-by-side (b.x=a.x+a.w, matched y/h), unique chain -> no neighbor. bible id=451. `[source wm.c wm_snap_adjacent]`.
- [x] **C.14** Parent/child chain stacking — `[VERIFIED/harness]` wm_set_parent/is_ancestor: child links to parent (raised), transitive ancestry, self-parent rejected. bible id=455. `[source wm.c wm_set_parent]`.
- [ ] **C.15** Custom button placement (drag) — `[2.0]`.

## D. Desktop Surface / Panel / Dock
- [x] **D.1** Panel renders: persona dot, chain pills, clock, status dots — `[observed]` frame-0.
- [x] **D.2** Panel clock shows real wall-clock time — `[DONE][observed 2026-07-23]` was TSC uptime (00:00); now wired to `tod_now_unix()` (CMOS RTC). Verified on live VNC (10:57/11:07 UTC, advancing). Commit b385d7a.
- [x] **D.3** Panel zones (left persona/palette trigger; center pills color-by-state; right health/notif/clock) — `[VERIFIED/production]` (zone LAYOUT) left persona dot+label / center chain pills (active underlined) / right health dots+clock, observed on the real cold-boot path (no diag/bypass), settled 2-boot, adversarial review confirmed. bible id=484. **FENCED (harness-only):** per-pill COLOR-BY-STATE (PAUSED/ERROR → red/dim) needs chain-state forcing not input-reachable; default LIVE observed. Prior color selftest bible id=419. `[source panel.c pill_color; wm.c:1145]`.
- [x] **D.4** Panel: click-persona->palette, right-click menu — `[VERIFIED/production]` panel_click reroute + FIXED palette invisible since B.6 (drew to front, composite flipped over it). USB-click opens palette. bible id=407.
- [x] **D.5** Panel height-follows-density (48/40/32) — `[VERIFIED/production]` access_set_density applies access_get_panel_height() live to panel+compositor; selftest [D5] d=0:48 d=1:40 d=2:32 PASS (KVM). bible id=308.
- [x] **D.6** Panel auto-hide / vibrancy / per-pill right-click — `[VERIFIED/harness]` per-pill menu (id=416) + auto-hide reveal-on-top-edge ([D6] PASS) + vibrancy (translucent fb_rect_blend panel bg, observed). bible id=444. `[source panel.c panel_pointer_y/panel_draw]`.
- [x] **D.7** Desktop wallpaper + persona accent gradient — `[observed]` frame-0 background.
- [x] **D.8** Desktop icons: grid-snap drag, VAULT persist, double-click launch, right-click menu — `[VERIFIED/production]` all 4 work (USB HID); double-click launched Files->chain+surface; fixed click-moves-icon margin bug. bible id=411.
- [x] **D.9** Desktop icons are persona-tinted SVGs — `[VERIFIED/production]` build-time rsvg raster -> objcopy -> lodepng decode + alpha-mask accent tint (in-tree assets/icons); Files=folder Terminal=code Settings=gear, verified on KVM screenshot. Also seeded default launcher icons (desktop was empty). bible id=403.
- [x] **D.10** Wallpaper image load from VAULT — `[VERIFIED/production]` embedded 480x270 gradient PNG (31485B, fits 48KB VAULT cap) seeded to /system/wallpaper.png first boot, loaded back via vault_read + lodepng_decode32, nearest-neighbour scale-blit to fill; solid-color fallback on failure. Observed on the real cold-boot path (no diag/bypass), settled 2-boot: serial 'wallpaper loaded from VAULT 480x270 (31485 bytes)' (VAULT path, not fallback) + desktop screendump gradient. Adversarial review confirmed. bible id=482. `[source desktop.c desktop_load_wallpaper + desktop_draw]`.
- [x] **D.11** Drag desktop icon → chain surface (feed a chain) — `[VERIFIED/production]` dragged Files icon (mouse press+move+release via real handler) onto the Files window → `DESK: fed 'Files' -> surface 1` (un-gated serial, gated on wm_feed_surface) on the real cold-boot path (no diag/bypass), settled 2-boot, adversarial review confirmed. Positioning: known-position rel. bible id=488. `[source desktop.c desktop_drag_end; wm.c wm_feed_surface]`.
- [x] **D.12** Dock: centered, pinned|divider|running, state dots, auto-hide — `[VERIFIED/production]` (static render) centered dock, 5 pinned (Files/Editor/Terminal/Settings/Calculator) + divider + 2 running (open windows) with running state dot + active highlight, observed on the real cold-boot path (no diag/bypass), settled 2-boot, adversarial review confirmed. Serial 'DOCK: initialized, auto_hide=0'. bible id=483. **FENCED (harness-only):** auto-hide SLIDE animation (auto_hide=0 default; needs settings API) + empty-at-boot (dev-seeded pins). Prior selftest bible id=418. `[source dock.c:407-483,dock_d12_selftest]`.
- [x] **D.13** Dock hover thumbnail, drag-reorder/poof, density size — `[VERIFIED/harness]` density (68/60/52), drag-reorder, poof all work + boot clean. Root-caused the prior crash: undefined access_get_density()->null-call (NOT a heisenbug), fixed via access_get()->density. bible id=465. NOTE: hover-thumbnail N/I (needs window-snapshot infra). `[source dock.c]`.

## E. Input & Cursor
- [x] **E.1** Mouse driver PS/2 (IRQ12) — `[observed 2026-08-02]` cursor physically moved across desktop→moved→click shots (970,552→870,452→770,402), serial `routed IRQ12(mouse)`. `[source mouse.c:354-410]`. USB-HID leg still UNVERIFIED (no xHCI controller in this VM).
- [x] **E.2** Cursor: 22 states, real SVG-derived sprites, hotspot table — `[VERIFIED/harness]` 22 states, all 22 sprites non-empty 24x24, hotspots distinct, cursor_set accepts all. save-under N/A (full recomposite). bible id=438. `[source cursor_sprites.h]`.
- [x] **E.3** Cursor click feedback (scale pulse, ripple, burst) — `[VERIFIED/harness]` selftest [E3] scale[press/restore]=11 ripple[active/grew]=11 burst[on/off]=11 PASS + observed no stuck-shrink after long-hold click. Fixed latent retarget-of-settled-id hazard (cursor_scale_to + anim_is_active guard). bible id=421. `[source cursor.c cursor_scale_to]`.
- [x] **E.4** Cursor confirm (checkmark) — `[VERIFIED/production]` wired into settings save_all (was zero callers). KVM [E4] flash=1 reverted=1 PASS. bible id=326.
- [x] **E.5** Hot corners: 8px zones, 150ms dwell, TL palette / BL workspace / BR show-desktop — `[VERIFIED/production]` all 4 fire after dwell on the real cold-boot path (no diag/bypass): serial `HC: hot corners active (zone=8px,dwell=150ms)` + `[HC]` TL→palette / BL→workspace 0→1 / BR→show-desktop / TR→notify, each once, settled 2-boot; TL→palette visually confirmed (cursor at corner, palette open). Adversarial review confirmed. bible id=485. `[source hotcorners.c]`. `[source hotcorners.c:35-169]`.
- [x] **E.6** Hot corner TR (notifications) — `[DONE 2026-07-23]` was a TODO stub; now toggles the notification panel via notify_show_all(). Commit 10993b1.
- [x] **E.7** Keyboard: set-1 scancode → ASCII — `[VERIFIED/production]` typed marker 'kbtest7' (plain keys) decoded correctly end-to-end on the real cold-boot path: shell received the exact in-order string (serial `unknown command: kbtest7`), settled 2-boot oracle, adversarial review confirmed. No layout switching (single set-1 map). (Terminal window WAS a static mockup when this was verified; now a live shell view — see TERMINAL.LIVE, bible id=494.) bible id=481. `[source keyboard.c scancode_to_ascii; shell.c shell_pump_char]`.
- [x] **E.8** Keybinds system (Super+arrows/1-4/T/D/Space) — `[DONE 2026-07-23]` `[source keybinds.c:85-156]`; command-palette action wired (Super+Space). Terminal/inspect/chain-graph still TODO (need app entry points).
- [x] **E.9** Input-method framework — `[VERIFIED/production]` ime_feed dead-key compose: '+e→é / `+a→à / ~+n→ñ / "+u→ü, dead-lead buffering, passthrough, no-match fallback — real fn run from the production `selftest` shell, serial `E.9 ime compose (...): PASS`, settled 2-boot, adversarial review confirmed. bible id=490. `[source keyboard.c ime_feed; shell.c cmd_selftest]`. `[source keyboard.c ime_feed]`.

## F. Typography & Icons
- [x] **F.1** stb_truetype: glyph cache + grayscale AA, font_draw/measure/line_height — `[observed 2026-08-02]` smooth AA proportional + monospace glyphs rendered across panel/titles/clock/labels. `[source font.c:71-142,188-253]`.
- [x] **F.2** TTF embedded in kernel (objcopy) + consumed at boot — `[observed 2026-08-02]` on-screen Inter+JBMono glyphs prove the embedded blob loads + draws at boot. `[source Makefile:153-199; font.c:161-170]`.
- [x] **F.3** Fonts: Inter (7wt), JetBrains Mono (6wt) present — `[observed]` asset files counted (7 + 6).
- [x] **F.4** Font fallback chain (Inter->Noto->bitmap) — `[VERIFIED/production]` wired font_init (was DEAD -> all text was 8x16 bitmap; now real Inter/JBMono TTF) + Noto subset fallback tier (92KB OFL) via font_get_glyph glyph-index fallback. KVM [F4] noto_loaded=1 gap_cp=0xad fell_back=1 PASS. bible id=405.
- [x] **F.5** icon_render: 31-icon dispatch at 16/24/32, accent tint — `[observed 2026-08-02]` desktop icons + ~7 dock icons + titlebar buttons render as accent-tinted vector sprites at multiple sizes. `[source icon_render.c:610-658]`. Exact 31-count/bucket enumeration not dissected from the shot.
- [x] **F.6** SVG asset library (934 svg / 16 categories; 50 cursor + ~150 themed) — `[observed]` file counts (actual 934).
- [x] **F.7** Build-time SVG→bitmap rasterization — `[VERIFIED/real-build]` Makefile rsvg-convert -> RGBA PNG -> objcopy embed -> lodepng runtime (D.9 icons id=403, D.10 wallpaper id=417). bible id=443. NOTE: single-image raster, sprite-sheet atlasing not implemented. `[source Makefile; icon_svg.c]`.

## G. Persona & Theme
- [x] **G.1** 3 personas (Zeros/DereZ/Full) + accent/dim tokens — `[VERIFIED/production]` real accent_table/dim_table (via persona_accent_of/persona_dim_of) read from the production `selftest` shell: 3 accents distinct, 3 dims distinct, dim≠accent per persona. Serial `G.1 persona tokens (acc_distinct=1 dim_distinct=1 dim!=acc=1): PASS`, settled 2-boot, adversarial review confirmed. bible id=503. `[source persona_anim.c; shell.c cmd_selftest]`. `[source persona.h; theme.h:112-119]`.
- [x] **G.2** Shell prompt + cursor colorway switch on persona — `[VERIFIED/production]` persona_prompt distinct (zeros>/derez>/zeos>) + cursor_select_colorway switches accent per persona (3 distinct), via the production `selftest` shell: serial `G.2 persona (prompt_distinct=1 cursor_switches=1 [...]): PASS`, settled 2-boot, adversarial review confirmed. bible id=504. `[source persona.h persona_prompt; cursor.c; shell.c cmd_selftest]`. `[source persona.h persona_prompt; cursor.c cursor_select_colorway]`.
- [x] **G.3** Persona crossfade (spring color lerp) — `[VERIFIED/production]` real per-channel color_lerp (via persona_lerp) from the production `selftest` shell: ZEROS→FULL t=0→src ff09bc8a, t=1→dst ff2e86ab, t=0.5 mid ff1ba19a with each channel between endpoints. Serial `G.3 persona lerp (...): PASS`, settled 2-boot, adversarial review confirmed. (Lerp math; spring-driven live transition stays harness id=430.) bible id=505. `[source persona_anim.c color_lerp/persona_lerp; shell.c cmd_selftest]`. `[source persona_anim.c:76-160]`.
- [x] **G.4** Dark/light/auto + night shift — `[VERIFIED/production]` real theme_set/get_scheme + theme_surface/on_surface from the production `selftest` shell: dark(ff0d1117)/light(fffafaf8) distinct+ordered (light brighter), scheme round-trips, text inverts; scheme restored after. Serial `[G4] ... -> PASS`, settled 2-boot, adversarial review confirmed. auto=TSC placeholder, night-shift=gated tint (source). bible id=506. `[source theme_runtime.c; shell.c cmd_selftest]`. `[source theme_runtime.c:43-151]`.
- [x] **G.5** Per-persona dock/launcher defaults — `[VERIFIED/harness]` dock_apply_persona_defaults: distinct set per persona (Zeros/DereZ/Full), wired to persona transition. bible id=445. `[source dock.c dock_apply_persona_defaults]`.

## H. Networking
- [x] **H.1** virtio-net driver — `[observed 2026-08-02]` net-enabled boot detects the NIC (`chain 4: nic:1af4:1000 [Ethernet]`), binds `[net_chain] hw backend = virtio-net`, and passes real DHCP traffic (H.3). `[source net_virtio.c:220]`. **e1000 also verified** `[observed 2026-08-03]`: booting with `-device e1000` (net_shell_harness.py `NIC=e1000`) binds `chain 4: nic:8086:100e [Ethernet]` and carries the **full stack** — DHCP bound, HTTP `200` (real Example Domain body), HTTPS `200`, ICMP `Reply` — so the net_e1000 driver works end-to-end, not just link-up. **rtl8139 also verified** `[observed 2026-08-03]` (`NIC=rtl8139`): `rtl8139: found at 0:3.0` → `rtl8139: ready, MAC=52:54:00:12:34:56` → `NET: using rtl8139 driver`, `chain 4: nic:10ec:8139`, and full stack (DHCP + HTTP 200 + HTTPS 200 + ICMP `Reply: 370949`). So virtio + e1000 + rtl8139 all carry the stack; rtl8169 has no QEMU device model to test against.
- [x] **H.2** ARP / IPv4 / UDP / TCP — `[observed 2026-08-02]` via the serial-shell `fetch` command over a net-enabled boot: DHCP proves ARP+IPv4+UDP; `fetch example.com` shows **`TCP: connecting... TCP: connected`** (full 3-way handshake) then a complete HTTP request/response — the TCP state machine works. IPv6/TCP6 dual-stack falls back to v4 correctly. **IPv6 stack now initializes at boot** `[observed 2026-08-03]` — root cause: `ipv6_init()` was deferred out of `net_init` (busy-waited on an RA) and never called afterward, so v6 was fully dormant; fixed by splitting into a non-blocking `ipv6_start()` (link-local + RS at `net_init`) + a per-tick `ipv6_service()` SLAAC-retry, mirroring async DHCP. Serial now shows `IPv6: link-local fe80::5054:ff:fe12:3456 (RS sent, SLAAC async)` alongside `DHCP: bound 10.0.2.15` — **no DHCP regression** (A/B confirmed). SLAAC global-addr completion + external v6 reachability remain unverified: exercising them needs slirp `ipv6=on`, which itself breaks slirp's v4 DHCP offer, and there's no other guest-reachable v6 endpoint (env-blocked follow-up). **ICMP echo verified** `[observed 2026-08-03]`: `ping 10.0.2.2` → `Reply: 205480 TSC cycles` (real ICMP echo request→reply roundtrip, measured RTT). `[source net_arp.c; net_ip.c icmp_ping; net_udp.c; net_tcp.c]`. (External ping times out — slirp doesn't forward ICMP to the internet without privileged host sockets; not a Zeos defect.)
- [x] **H.3** DNS / DHCP / HTTP GET — `[observed 2026-08-02]` all three verified live via serial-shell commands on a net-enabled boot: **DHCP** `bound 10.0.2.15 gw 10.0.2.2 dns 10.0.2.3`; **DNS** `dns example.com → 172.66.147.243`, `dns google.com → 142.250.189.142` (real resolution); **HTTP GET** `fetch example.com /` → `HTTP: 200 (571 bytes)` with the real `<title>Example Domain</title>` HTML body. `[source net_dns.c; net_dhcp.c; net_http.c]`. (8-entry DNS cache internals not separately exercised.)
- [x] **H.4** TLS 1.3 via mbedTLS + Mozilla CA bundle — `[observed 2026-08-02]` `https example.com /` completes a full **TLSv1.3** handshake and returns the real page: serial `TLS: subsystem ready (mbedTLS 3.6.4)` → `TLS: connected, TLSv1.3 (v4)` → `Body: 571 bytes` (Example Domain HTML), with cert verify REQUIRED against the CA bundle. **ROOT CAUSE + FIX:** it was BROKEN (`ssl_setup` → MBEDTLS_ERR_SSL_BAD_CONFIG -0x5E80) because `tls_init()` is deferred (not run at boot) and the CLIENT path (`tls_open`→`tls_finish_handshake`) never called it — only the TLS *server* lazy-inited — so `g_ssl_conf` was unconfigured (min/max TLS version 0/0 → `ssl_conf_version_check` fails). Fix (net_tls.c): made `tls_init()` idempotent (guard flag) + lazy-call it at the top of `tls_finish_handshake`. `[source net_tls.c:190-360]`.
- [x] **H.5** TCP sliding window + congestion control — `[observed 2026-08-03]` added a send-side sliding window with slow-start + AIMD congestion avoidance + Go-Back-N retransmit (`cwnd`/`ssthresh`/`snd_una` in `struct tcp_conn`), scoped to the DATA path only (SYN/SYN-ACK/FIN stay on the untouched single-segment retx path). Verified live (tcp_window_test.py, host raw-TCP echo on user-net `10.0.2.2:8092`): `tcpsend 10.0.2.2:8092 8000` → serial `TCP: sent 8000 B, max_inflight=4380 B, cwnd=10220 B` (**3 MSS in flight at once = window engaged, not stop-and-wait; cwnd grew to 7 MSS via slow-start**) → `tcpsend: echoed 8000/8000 bytes VERIFIED` (byte-exact multi-segment roundtrip, no Go-Back-N corruption), host log `TCPECHO: echoed 8000 bytes`. **NO REGRESSION** — full stack re-verified green on the new TCP: DNS `example.com→172.66.147.243`, HTTP `200 (571 bytes)`, TLS `TLSv1.3` `200/571B`, WebSocket masked-frame roundtrip. Go-Back-N retransmits directly from the caller's buffer (tcp_send_on is fully blocking → no per-conn copy buffer; ~25 B/conn of scalars). `[source net_tcp.c tcp_send_on/tcp_process/tcp_retransmit_tick; net_tcp.h; shell.c cmd_tcpsend]`. (Fast-retransmit/SACK + RFC-persist zero-window timer are future refinements; base retransmit was already built.)
- [x] **H.6** WebSocket (RFC 6455 client) — `[observed 2026-08-03]` full masked-frame roundtrip verified live (ws_test_harness.py, host echo server on user-net `10.0.2.2:8090`): serial `WS: connected (101 Switching Protocols)` → `WS: sending "helloZeos"` → `WS: recv "helloZeos"` → `WS: closed`, and the **host echo log independently confirms** `accepted` → `got request (147 bytes)` → `replied 101 + echoed 9-byte frame`. Handshake does SHA-1+base64 `Sec-WebSocket-Accept` verify; client frames masked; ping→pong auto-answer. **ROOT CAUSE + FIX:** first attempt hung — the non-blocking read loops drained the TCP rx buffer but never pumped the stack, so the server's 101 never arrived (spun to timeout → RST before reply). Fix (net_ws.c): `net_poll()` each idle iteration + wall-clock TSC deadline instead of a nop-count. `[source net_ws.c; net_ws.h; shell.c cmd_ws]`. **wss:// (secure) also verified** `[observed 2026-08-03]`: `ws wss://ws.postman-echo.com /raw helloZeos` (wss_test.py) → serial `TLS: connected, TLSv1.2` → `WSS: connected (101, over TLS)` → `WS: sending "helloZeos"` → `WS: recv "helloZeos"` → clean close. WS framing layered over the net_tls transport (full cert-verify against the CA bundle); `ws_connect_secure` + a transport abstraction (ws_xport_send/recv) dispatch TCP vs TLS. (External dependency: a real public wss echo — the guest's required cert-verify precludes a self-signed local server.)

## I. Web Browser
> **[2026-08-02] Browser WIRED as an app (was dormant).** Added `browser_app_open()` + a
> `browse <url>` shell command (browser.c/browser.h/shell.c): it creates a WM "Browser"
> surface with a `draw_content` that renders the browser into the window each composite.
> VERIFIED functionally via serial: `browse http://example.com/` → `HTTP: 200 (571 bytes)`
> → `BROWSE: 0 OK, 18 nodes, 136px` (fetched, parsed 18 DOM nodes, laid out 136px, page
> loaded). So I.1 (parse) + I.4 (navigation) run correctly end-to-end over the now-green net
> stack. **Remaining:** the browser WINDOW is not yet visibly compositing in the desktop
> screenshot (a mid-session-created surface z-order/refresh follow-up) — so the on-screen
> render (I.3 visible) and input routing (I.5 click/scroll) still need the surface to show +
> a boot-time launcher (dock/palette entry).
- [x] **I.1** HTML parser (tags/attrs/comments/void/script-skip), 2048-node DOM — `[observed 2026-08-02]` `browse example.com` parsed the page into 18 DOM nodes (serial `BROWSE: 0 OK, 18 nodes`). `[source browser.c:208-264,130]`.
- [x] **I.2** CSS per-tag defaults + inline `style=` + persona-accent links — `[observed 2026-08-03]` browser_render_test.py screenshot (`/tmp/zeos-b-render.png`) of a live `browse http://example.com/` fetch shows the `<h1>` "Example Domain" rendered **visibly larger + bold** vs the body `<p>` text below it — the per-tag CSS default (heading emphasis) applied on a real parsed DOM. `[source browser.c:597,749-751]`. (Inline `style=` + persona link-accent tint not separately isolated in this shot — the link "Learn more" renders but its accent color wasn't distinguished from body at this scale.)
- [x] **I.3** Block layout + framebuffer render + TTF text — `[observed 2026-08-03]` same screenshot: the fetched page renders as stacked **blocks** in the content viewport — heading, then a wrapped multi-line paragraph, then the link line — with crisp TTF glyphs, correct left-margin, and content clipped to the window's content area (below toolbar, above status). Proves render_page() block layout + framebuffer draw + TTF text end-to-end over the green net stack. `[source browser.c:1155-1707]`. (HR + scroll clip not exercised here — example.com is 136px, shorter than the viewport, so no `<hr>` and no scrollbar; covered under I.6 with a taller page.)
- [x] **I.4** Navigation chrome — `[observed 2026-08-02]` the browser WINDOW now composites (after the `compositor_dirty_all` fix): screenshot shows the toolbar `< > R` (back/forward/refresh) + a URL bar rendering the navigated `http://example.com/` + a `HTTP | nodes:N` status bar, focused with a "Browser" panel pill. URL parse + navigate proven end-to-end earlier (HTTP 200, 18 nodes). 32-history depth not separately exercised. `[source browser.c:1349-1544]`.
- [x] **I.5** Link click hit-test → navigate — `[observed 2026-08-03]` on the built-in `test:home` page, `bclick 165 300` (drives `browser_app_click` → `browser_click`, the compositor's click hook) hit-tests the `<a href="test:page2">`: serial `browser_click: page(36,91) link=test:page2` → `BROWSE: test:page2` (13 nodes) and the post-click screenshot (`/tmp/zeos-b-clicked.png`) shows **Page Two** with the URL bar reading `test:page2`. **FIX:** link hit-test was dead — inline `<a>` boxes had **negative width** (layout ran with `surface_w==0` at open, before the WM sized the surface), so `hit_test`/hover (`box.w>0`) always rejected them; fixed by re-running `browser_layout()` from `draw_content` once the real width arrives. Also wired `browser_app_click`→`compositor_dirty_all` so a link-nav actually repaints. `[source browser.c hit_test_link/browser_app_click; browser_layout]`. (Compositor mouse→browser routing is the input lane's job; the browser-side entry point + hit-test are proven.)
- [x] **I.6** Forms (input/button) + scrollbar — `[observed 2026-08-03]` `test:home` screenshot (`/tmp/zeos-b-home.png`) shows the `<input type=text>` field + a rendered blue **"Submit"** `<button>`, and the right-edge **scrollbar thumb** (page 828px > 584px content viewport → thumb drawn proportionally). `[source browser.c render_input/render_button; browser_draw scrollbar]`. (PNG image decode via lodepng not exercised here — no `<img>` in the offline test page; form SUBMIT wiring is I.7-era.)
- [x] **I.7** JavaScript engine + live DOM — `[observed 2026-08-09]` **QuickJS (ES2020) embedded in the kernel** (lib/quickjs + vendored musl-libm lib/zeosm; qjs_port.c freestanding glue). Verified live on real cold boots (serial + screendump each): (1) **engine** — `js 1+1→2`, `Math.sqrt(2)²→2.0000…4` (real IEEE+libm), arrow fns/closures/`Array.map`, JSON, generators, exceptions; (2) **DOM query/mutate** — `<script>` executes, `document.getElementById`/`querySelector`/`querySelectorAll`, `textContent`/`innerText`/`innerHTML` get/set, `getAttribute`/`setAttribute`, `className`+`classList.add/remove/toggle/contains`, `.value`, `offsetLeft/Top/Width/Height`, `console.log`, page re-renders on mutation (`test:dom2` innerHTML rebuild; `test:dom3` `active kids 2 val typed parent box active true`); (3) **events** — `addEventListener`+dispatch (bubbling)+`event.target`: **click** (counter 0→3), **submit** on `<form>` from a submit-control click (`test:submit`→`submit fired count 1 name robot target form`, screen `submitted #1 name=robot`), **input/change/keydown** from text entry (`test:type`→`type submit value hello inputEvents 5`, screen `live: hello (5 input evs)` / `done: submitted hello`); (4) **DOM build/tree** — `createElement`/`createTextNode`/`appendChild`/`.body`, `Element.remove()`, `.children`/`.parentNode`/`.firstElementChild`/`.lastElementChild`/`.next+previousElementSibling` (`test:dom3` remove drops node from render tree; `first=alpha last=gamma`); (5) **fetch()** — real HTTP over the net stack, Promise + `Response.text()`/`.json()`, microtask drain via `JS_ExecutePendingJob` (`test:fetch`→example.com); (6) **CSS layout** — **flexbox** (`display:flex`, row/column, justify-content, align-items, gap, flex-grow; `test:flex` screen A|B|C space-between, X/Y column stretch) and **grid** (`display:grid`, `grid-template-columns` repeat/fr/px, gap, row-major; `test:grid` 3×2 aligned); (7) **forms text entry over the REAL keyboard path** — click focuses a field, real keystrokes (i8042 IRQ → `keyboard.c` global dispatch → `browser_app_key_char`) edit `.value` live, Enter fires change+submit `[observed 2026-08-10]` (`test:type` driven by QMP `input-send-event` qcodes — NOT the shell path — → `type submit value hello inputEvents 5`, screen `hello`/`live: hello (5 input evs)`/`done: submitted hello`). Also fixed: `offsetLeft/Top/Width/Height` now correct — scripts run after the first real-width layout, not the width-0 initial pass (`test:flex` `spread 957 La 0 Lb 479 Lc 957`, was 69/0/35/69). `[source lib/quickjs/{quickjs.c,qjs_port.c,qjs_eval.c,qjs_dom.c}; browser.c script exec + DOM API + layout_flex/layout_grid + browser_click focus/submit + browser_input_* + browser_run_pending_scripts + browser_app_key_char/scan; keyboard.c browser routing; shell.c cmd_js/bclickp/bbox/btype/bkey]`. Full modern-web stack (engine + DOM + events + forms over real keyboard + fetch + flexbox + grid), no known gaps in the JS/DOM/CSS/input path. Was `[2.0]`.

## J. Settings
- [x] **J.1** Settings app (WM app, VAULT persist, inline current values) — `[VERIFIED/harness]` save->clobber->load VAULT round-trip restores mouse_speed/key_repeat/wallpaper. bible id=437. `[source settings.c save_all/load_all]`.
- [x] **J.2** One settings surface, no duplicate — `[VERIFIED/production]` settings.h dropped its name-colliding duplicate types (access_config_t/color_scheme_t/etc.), includes access.h; settings GUI operates on the ONE real config via access_get()/access_set_*. KVM [J2] shared_ptr=1 sensory/reduced_motion/focus live=1 PASS. Also fixed latent bug: accessibility changes now actually apply. bible id=395.
- [x] **J.3** Search-first (palette enumerates every setting) — `[VERIFIED/production]` palette_show enumerates settings_registry (42 items), bool toggles inline. KVM [J3] items=42 registry=42 PASS. PALETTE_MAX_ITEMS 64->128. bible id=336.
- [x] **J.4** Right-click element → "Settings for this…" — `[VERIFIED/harness]` settings_open_page routes to element page (+re-route); wired into desktop menu (rc_settings/rc_wallpaper real, was stubs). bible id=449. `[source settings.c settings_open_page; desktop.c]`.

## K. Signal Visualizer
- [x] **K.1** Node-graph renderer + state colors — `[VERIFIED/harness]` Super+G opens Signal Graph overlay: 9 chain nodes, compositor->panel/dock/desktop edges+arrows, accent(LIVE) borders, 100% zoom. Wired ACTION_CHAIN_GRAPH (was TODO stub). bible id=439. `[source sigviz.c; sigviz_overlay_*]`.
- [x] **K.2** Interactive selection / inspector — `[VERIFIED/production]` all 3 behaviors evidenced: click memory:0 → selected (border 1→3px + inline 'data'); 2nd click → inspector panel (IDENTITY name/CFA/MasQ/Status + B3 BELIEF + NODES + ROUTES + TIMING, real chain_get data); empty-click → border back to thin (deselected). Real cold-boot path (no diag/bypass), settled 2-boot. Adversarial review REJECTED first pass (deselect unevidenced) → FIXED (k2-deselect.png Read-confirmed). Positioning: known-position rel from init center (no corner-anchor). bible id=487. `[source sigviz.c sigviz_click; inspector.c]`. `[source sigviz.c sigviz_click]`.
- [x] **K.3** Live pulse animation — `[VERIFIED/harness]` sigviz_tick advances frame (wired to compositor); sine_pulse varies 0/127/255 across frames, bounded. bible id=442. `[source sigviz.c sine_pulse/sigviz_tick]`.
- [x] **K.4** Zoom / pan — `[VERIFIED/production]` = zooms 100%->150% (indicator changes, nodes larger, compositor pushed off-edge); left-arrow pans (graph shifts right, compositor scrolls off — settled 2/2). Observed on the real cold-boot path (no diag/bypass); zoom identical on all boots where overlay opened (misses = INPUT.DROP.COMPOSITE combo drop, not a K.4 defect). Adversarial review caught+fixed a missing pan-evidence gap. bible id=476. `[source sigviz.c:537-568]`.

## L. Animation & Motion
- [x] **L.1** Spring engine (semi-implicit Euler, 64 concurrent, retarget-with-velocity) — `[VERIFIED/production]` real anim_spring_default(0→100) run from the production `selftest` shell converges to ~100 in 45 ticks and goes inactive: serial `L.1 spring converge (...): PASS`, settled 2-boot, adversarial review confirmed. (concurrent=64 / retarget-velocity remain from harness selftest id=427.) bible id=495. `[source anim.c; shell.c cmd_selftest]`. `[source anim.c anim_l1_selftest]`.
- [x] **L.2** Presets snappy/smooth/bouncy/interactive — `[VERIFIED/production]` distinct physics via the real anim system from the production `selftest` shell: bouncy overshoots peak 128, interactive settles 104 vs smooth 226 ticks (2x faster), all converge. Serial `L.2 presets ... -> PASS`, settled 2-boot, adversarial review confirmed. bible id=496. `[source anim.c; theme.h presets; shell.c cmd_selftest]`. `[source theme.h:50-57; anim_l2_selftest]`.
- [x] **L.3** Compositor ticks anims per frame + re-arm while live — `[VERIFIED/harness]` one compositor_advance drove a spring (pos>0) + re-armed dirty while anim live. bible id=461. `[source compositor.c:272,289]`.
- [x] **L.4** Spring surface open/close, snap settle, dock auto-hide slide — `[VERIFIED/harness]` surface create springs scale 0.8->1.0+opacity 0->255, detach springs back ([L4] open/close PASS); dock-slide via D.12. bible id=429. `[source wm.c:306-354; dock.c]`.
- [x] **L.5** Spring menu appear/dismiss — `[VERIFIED/production]` context menus spring-scale open/dismiss; deferred teardown, no input ripple. Real context_menu_l5_selftest run from the production `selftest` shell: `[L5] opened=1 input_off=1 deferred=1 torndown=1 -> PASS`, settled 2-boot, adversarial review confirmed. (Reconciled: prior id=341/415 were unscoped KVM selftests; now production-scoped.) bible id=508.
- [x] **L.6** Spring scroll physics — `[VERIFIED/production]` real scroll_phys_t from the production `selftest` shell: flick moves+decelerates (momentum/friction), settles in bounds, overscroll (1200>max1000) rubber-bands back to ~1000. Serial `L.6 scroll (moved=1 decel=1 settled=1 rubberband=1): PASS`, settled 2-boot, adversarial review confirmed. bible id=502. `[source anim.c scroll_phys_*; shell.c cmd_selftest]`. `[source anim.c scroll_phys_*]`.

## M. Accessibility  (⚠ whole class: UI + VAULT persist exist, but NO CONSUMER reads them)
- [x] **M.1** Reduced-motion mode — `[DONE 2026-07-23]` `access_init()` was never called (whole a11y config zero-init); now called at boot + `anim_tick` snaps springs instantly when reduced_motion on. Commit 38c54df.
- [x] **M.2** Animation speed multiplier (0/0.5/1/2×) — `[DONE 2026-07-23]` `anim_tick` scales dt by anim_speed (0.5×=faster, 2×=slower, 0=instant). Commit 38c54df.
- [x] **M.3** 3 density modes (Comfortable/Standard/Compact) — `[DONE 2026-07-23]` panel height density-driven (48/40/32) at boot + live on access_set_density. Commits 35effb5,f7f481c. Follow-up: dock item size.
- [x] **M.4** 3 sensory modes (Standard/Low-Stimuli/High-Contrast) — `[VERIFIED/production]` runtime sensory transform (access_apply_sensory/text_color/border/anims/stiffness) wired into panel+theme+anim; KVM: STANDARD/LOW/HIGH panel-dot rgb matches transform (sat 125/63/175). bible id=300.
- [x] **M.5** 44px min touch targets — `[VERIFIED/production]` wm hit_control catch-zone expanded toward min_touch_target (nearest-center + full-titlebar); KVM [M5] glyph/hslop/vslop hit, beyond/offbar miss, PASS. bible id=301.
- [x] **M.6** Letter/word/line spacing — `[DONE 2026-07-23]` font_draw honors letter_spacing (per-glyph advance) + word_spacing (on spaces). Commit 6d3de02. (line_spacing: no single-line consumer yet.)
- [x] **M.7** Focus Mode (suppress non-critical notifications) — `[VERIFIED/harness]` notify_focus_suppresses: off suppresses nothing, on suppresses INFO/WARN/ERROR, CRITICAL always passes. bible id=434. `[source notify.c notify_focus_suppresses]`.
- [x] **M.8** CVD simulation mode — `[VERIFIED/harness]` cvd_transform (Vienot matrices): deuteranopia collapses red-green dist 510->242, NONE identity, alpha preserved. bible id=447. NOTE: transform+mode plumbed; full-screen live apply is opt-in (perf). `[source access.c cvd_transform]`.

## N. First-Boot Experience
- [x] **N.1** 5-screen first-run flow (welcome/persona/controls/appearance/done) — `[VERIFIED/production]` firstboot_run() wired canonical; KVM drove all 5 screens, applied persona=0 controls=1 theme=0 density=2 (all != defaults); idempotent 2nd boot skip. Found+fixed kbd-IRQ-vs-poll bug. bible id=304.
- [x] **N.2** Real boot path = firstboot 5-screen wizard — `[VERIFIED/production]` welcome.c single modal RETIRED from boot path (kept in-tree for revert). bible id=306.
- [x] **N.3** Reconcile: one first-run flow, wired and observed — `[VERIFIED/production]` reconciled to firstboot 5-screen wizard; welcome_run_if_first_boot() no longer on boot path (one-call revert documented). bible id=305.

## O. Hardware Targets
- [x] **O.1** x86-64 target (current) — `[observed]` boots + runs.
- [x] **O.2** HAL interface (hal.h; move asm/io/GDT/IDT/PIC/PIT behind it) — `[VERIFIED/production]` hal.h façade + hal_x86.c backend: real hal_in8(0x64)==raw inb (forwards, not stubs) + arch=x86-64 via the production `selftest` shell (serial `O.2 hal (...): PASS`, settled 2-boot, adversarial review confirmed); PCI config-io migrated behind hal_out32/in32 exercised at boot (sigviz device nodes). Full caller migration incremental. bible id=491. `[source hal.h; hal_x86.c]`.
- [ ] **O.3** ARM64 backend (UEFI stub, generic timer, GIC, MMIO UART, ECAM PCI, page tables) — `[TODO]`. `[PARTIAL 2026-08-03]` first brick: hal_arm64.c (ARM64 HAL backend — GICv2/generic-timer/PL011/DAIF/ECAM) compiles cross-arch with aarch64-linux-gnu-gcc, proving O.2's HAL is portable. Full boot path (EFI aarch64 stub, page tables, GIC dispatch, framebuffer) remaining. bible id=466.

## P. Z+ Language
- [x] **P.1** Decide role (user lang / compiled / glue) — `[DECIDED]` Z+ = compiled signal-chain/config/glue DSL (parse->compile->ZIR->execute into the chain engine), NOT a general user lang or native compiler. Embodied in code; verified via P.2/P.3. bible id=460.
- [x] **P.2** Z+ REPL in shell — `[VERIFIED/production]` real zp_run (parse→compile→execute) runs double/adder transforms (rc>=0) from the production `selftest` shell: serial `P.2 zplus REPL (zp_run double=1 adder=1): PASS`, settled 2-boot, adversarial review confirmed. bible id=501. NOTE: parser leniently accepts garbage (strict-validation follow-up). `[source zplus.c zp_run; shell.c cmd_selftest]`. `[source zplus.c zp_run]`.
- [x] **P.3** Signal-chain / UI-layout / config in Z+ — `[VERIFIED/production]` real zp_parse (2 nodes) + zp_compile (engine chain_id=1) run from the production `selftest` shell: serial `P.3 zplus (parse->nodes=2 compile->chain_id=1): PASS`, settled 2-boot, adversarial review confirmed. config/UI share the node/chain pipeline. bible id=498. `[source zplus.c; shell.c cmd_selftest]`. `[source zplus.c zp_parse/zp_compile]`.

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
> **[2026-08-03] Dom/Sub cohort subsystem built (Q.1–Q.4).** New `dom_sub.c`/`dom_sub.h`
> (chip-class table, cohort struct, identify, election w/ hysteresis), boot-seeded from
> `gpu_goya_init()` (gpu_goya.c), live hotplug drain via the public `hotplug_event_copy`
> API (no hotplug.c edit), + a `domsub` diagnostic (VIS_DEREZ) that dumps the live cohort
> and runs `dom_sub_selftest()`. Verified live (domsub_test.py serial): **`Dom/Sub selftest:
> PASS`** and boot-seed `cohort seeded, 0 cooperative chip(s)` (honest — no Goya in QEMU).
> The election/table logic has no hardware dependency, so the synthetic exercise IS the
> production path; real per-card register identify + live-PCI discovery are Q.7 (2+ cards).
- [x] **Q.1** Chip class registration table (vendor:device allowlist, starts with Goya 0x1DA3:0x0001) — `[observed 2026-08-03]` `CHIP_CLASSES[]` + `dom_sub_is_cooperative_chip()`; selftest `PASS goya recognized (0x1DA3:0x0001)` + `PASS random rejected (0x8086:0x1234)`. `[source dom_sub.c CHIP_CLASSES/dom_sub_chip_label]`.
- [~] **Q.2** Cohort discovery — `[built 2026-08-03]` boot-seed walks `gpu_goya_device()` into the cohort at the end of `gpu_goya_init()` (ran live: `cohort seeded, 0 chip(s)`); live attach/detach drains the hotplug ring via `hotplug_event_copy` + a tsc cursor (rides CHAIN_HOTPLUG_PCI, no hotplug.c edit). Attach→identify→re-elect and detach→re-elect **logic verified synthetically** (selftest inject path), but a **real live PCI hotplug event needs hardware → Q.7**. `[source dom_sub.c dom_sub_init/dom_sub_poll/dom_sub_on_attach]`.
- [~] **Q.3** Identify handshake — `[built 2026-08-03]` `chip_identity_t` (fw_version, dram_mb, thermal, bench_score); `dom_sub_identify()` reads fw + DRAM (derived from the DDR/BAR4 aperture length) from the bound `gpu_goya_device` record; bench_score starts 0 (lazily filled by first real THINK job — Q.5/Q.7). Identity **model + its use in election verified** (synthetic injects feed dram/bench through scoring), but the **real per-card register read needs a live card → Q.7** (returns 0 with no hardware). `[source dom_sub.c dom_sub_identify; gpu_goya.h fw_version/bar4_len]`.
- [x] **Q.4** Election (score = bench_score*1000 + dram_mb, PCI tiebreak, hysteresis) — `[observed 2026-08-03]` `dom_sub_elect()` verified live via selftest: `PASS first chip is Dom`, hysteresis `PASS Dom held after 1/2` then `challenger wins on Nth` (ELECT log `reason=challenger beat dom by sustained margin`, needs >=10% margin sustained N=3 elections), `PASS equal score keeps incumbent` (no flap), and Dom-detach `PASS survivor promoted immediately` (immediate re-election, no hysteresis). State transitions logged with tsc + reason (BIBLE G2). The algorithm has no hardware dependency. `[source dom_sub.c dom_sub_elect/member_score/best_member]`.
- [~] **Q.5** THINK vs BACKGROUND classification + routing — `[observed 2026-08-03]` `dom_sub_class_t` {THINK, BACKGROUND} + `dom_sub_route()` verified live (domsub selftest): `THINK -> Dom`, BACKGROUND round-robins the Subs (`sub A=1`, `sub B=2`, `wraps=1`), lone chip serves both classes, empty cohort → -1 (caller fallback), and THINK never lands on a Sub. **Routing policy done + verified**; the remaining piece is *enforcement* — pinning a real Goya chain to the routed slot via `chain_t.affinity` + `smp_chain_owner()` consumption in `gpu_goya.c`'s chain registration (CPU-core-only today) — which needs a live card to observe (Q.7). `[source dom_sub.c dom_sub_route]`.
- [~] **Q.6** Detach/failure handling — `[observed 2026-08-03]` re-election on membership loss verified live (selftest): **Sub-detach = cheap re-election** (Dom unchanged, BG re-routes to the surviving Sub) and **Dom-detach = immediate re-election** (no hysteresis; survivor promoted). `[source dom_sub.c dom_sub_on_detach]`. The in-flight-work → `CHAIN_ERROR` inheritance is the existing watchdog path (no new code); observing a real hung resolve on a departed chip is Q.7.
- [ ] **Q.7** Real hardware verification: 2+ Goya cards, confirm election + THINK-affinity + Dom-unplug re-election, measured via serial log — `[TODO]`.

---

## FIRST MOVES WE FOLLOW (highest-value, verified-negative first)
1. **‼ B.7** windows vanish on input — DABS mission staged (`~/dabs/missions/windows-vanish`); fix → **verify by boot** (frame-after-drag still shows windows).
2. **‼ C.1 / D.2** focused-window chrome + dead clock — cheap, high-feel, observable.
3. **‼ M.1–M.6** wire the no-consumer accessibility toggles (they lie today) OR down-scope them honestly.
4. **‼ A.6** scheduler 100ms tick overrun — find the heavy chain; it makes the whole desktop feel slow.
5. **N.3** pick ONE first-run flow and verify it runs.

_Every close: state + date + evidence + retro + adversary (G1–G7). `[x]` only on measured-and-observed. Verified base truth or no checkbox._
