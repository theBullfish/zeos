# Zeos AUTO_RUNBOOK — autonomous list-completion loop

**Purpose:** drive `BUILD_MAP.md` items to PRODUCTION-scope VERIFIED, one per cycle,
with an adversarial review of the **last two items** every cycle to contain drift.
A fresh (cron-fired) context re-reads THIS FILE first, then executes ONE cycle.

Owner: Brad. Engine: whatever context the cron fires. Branch: `bible/zplus-zir-convergence`.

---

## THE ONE LAW (non-negotiable)
A claim counts only when it is **measured AND observed** against a stable baseline.
- No checkbox flips, no VERIFIED, without a command run + real output read + evidence saved.
- Production scope = built with ZERO `-DZEOS_DIAG*/BYPASS` flags and observed on the real
  cold-boot path (`sweep_boot.py`: PIN→wizard→live desktop). Harness scope ≠ production.
- Verify with a SETTLED oracle (repeat the observation), not a single racy capture.

## SCOPE — what this loop may touch
ELIGIBLE (software-reachable):
- The ~57 `harness`-scope ledger items → re-verify on the PRODUCTION path, upgrade to
  `--scope production`. Prefer interactive/observable items first (drive them in `sweep_boot.py`).
- `O.3` ARM64 boot port (feature work), advanced in small compile-verified bricks.

EXCLUDED — never touch autonomously:
- `Q.7` (needs 2+ physical Goya cards) and ANY Goya/hardware register work. Card-brick rules apply.
- `C.15` / `I.7` (explicit `[2.0]` deferrals). `B.7-old` (resolved history, QEMU artifact).
- Never `git push`. Never write Goya reset regs. Never touch anything outside this repo.

## PER-CYCLE PROTOCOL (do exactly one item per cycle)
1. `cd /home/brad/zeos-repos/zeos`. Read this file's STATE section + `git log --oneline -3`.
2. Pick the NEXT eligible item (top of STATE.queue; if empty, rebuild queue from
   `python3 ~/bible-db/bible.py lint zeos` harness-scope list, interactive items first).
3. Do the work to verify it on the PRODUCTION path:
   - `cd kernel && make clean && make all` (plain = traces/diag OFF). Confirm `build/BOOTZ.EFI`.
   - If the item is observable, ensure `sweep_boot.py` exercises it (add a capture if needed).
   - `pkill -9 -f 'qemu-sys[t]em'; sleep 2` (bracket avoids self-kill), then run `sweep_boot.py`.
   - Read the actual screenshot(s) with the Read tool + confirm the pixel change. SETTLED oracle:
     if flaky, re-run; a single racy capture is NOT evidence.
4. If verified: `python3 ~/bible-db/bible.py append zeos <ITEM> --state VERIFIED
   --provenance observed --scope production --claim "..." --evidence "evidence/... + what was seen"
   --supersedes <ITEM>`. Flip the `BUILD_MAP.md` box only now. Copy artifacts to `kernel/evidence/`.
   If NOT verifiable this cycle: append `--state BROKEN` (or PARTIAL) with the real failure output,
   DO NOT flip the box, and leave a STATE note. Never fake a green check.
5. **ADVERSARIAL REVIEW OF THE LAST TWO ITEMS** (the drift guard) — see next section.
6. If the review passes: `git add -A && git commit` (local only) with a factual message +
   the Co-Authored-By / Claude-Session trailer. Update STATE (rotate last-two, advance cursor).
7. If the review FAILS: fix the problem THIS cycle ("don't move on till it's fixed"). If it
   can't be fixed cleanly, revert the item's box + ledger it BROKEN and STOP with a STATE note.

## ADVERSARIAL REVIEW OF THE LAST TWO ITEMS
After verifying item N, take **item N-1 (the approved paradigm/context)** and **item N (new work)**
and spawn an INDEPENDENT reviewer (Agent tool, subagent_type "general-purpose", run_in_background:false)
with this brief:
- Inputs: the bible ledger entries for N-1 and N, their diffs (`git show`), and their evidence files.
- N-1 is the ESTABLISHED-GOOD paradigm. Judge N against it:
  1. Does N's evidence actually support its claim, or is it PoC-not-finding? Try to REFUTE it.
  2. Did N drift from the paradigm N-1 set (same verification rigor, same scope discipline,
     same code idiom/patterns)? Name any drift.
  3. Is N's `--scope production` real, or smuggled harness evidence?
- Verdict: CONFIRMED (evidence holds, no drift) or REJECTED (with the specific defect).
Default to REJECTED if uncertain. A REJECTED verdict blocks the commit (protocol step 7).

## GUARDRAILS
- Self-police the One Law. No completion claims without exit-0 + observed output.
- Local commits only; NEVER push. No hardware/Goya. No files outside this repo.
- On genuine block/ambiguity/irreversibility: STOP, write a STATE note for Brad, do not thrash.
- One item per cycle. Idempotent: if a cycle half-finished, the next reconciles from git + ledger.

## STATE  (each cycle updates this section, then commits it)
- cursor: K.4 done (bible id=476). SKIPPED D.3 (color-by-state needs chain-state forcing, not input-drivable on prod). Next candidates: K.2 (click-to-select node — mouse-drivable in sigviz), E.2 (cursor render), C.13/C.12/C.14 (multi-surface). D.3/D.6 (need state-force/settings API), M.8 (CVD toggle) = NOT input-observable, defer. Pure-logic (L.1/L.2/L.6, A.7, P.2/P.3, B.4/B.8, E.9) = build-invariant, plan prod-selftest or scope-note.
- ‼ OPEN ISSUE for Brad: INPUT.DROP.COMPOSITE (bible id=475) — ~2/8 (up to 2/3 under load) boots a Super+combo never fires; i8042 1-byte buffer overrun during long A.6 composite ticks. Affects ALL combos → single-shot captures can false-negative; loop now needs multi-boot oracle + retries per item (expensive). Real fix = A.6 composite region-dirty (compositor.c:275). Needs a decision: fix A.6 next, or accept multi-boot oracle.
- queue: [K.2, E.2, C.13, C.12, C.14, L.5, M.7, ... then D.3/D.6/M.8(need non-input path), then pure-logic]
- last-two verified (most recent last): [G.1 (prod), G.2 (prod)]
- cycle log:
  - (seed) 2026-08: PROD.COLDBOOT.CORE, PALETTE.FLOW, K.1 upgraded harness→production (bible 467-471). commit b971034.
  - cycle 1 (2026-08): C.2 harness→production (bible 472→473). Adversarial review (vs K.1) CONFIRMED; corrected a "dimmed controls" overstatement.
  - cycle 2 (2026-08): K.4 harness→production (bible 474→476). Zoom 100→150 + pan settled on real cold-boot. Adversarial review (vs C.2) REJECTED first pass — pan asserted w/o saved settled evidence; FIXED this cycle (captured 2 settled pan samples, Read-confirmed). Discovered+logged INPUT.DROP.COMPOSITE (id=475). Hardened sweep combo() with inter-event settles (helped, didn't cure the drop).
  - cycle 3 (2026-08): K.2 ATTEMPTED, NOT verified, NO box flipped (honest miss). Two harness approaches to click a sigviz node both failed: usb-tablet abs is ignored by the Zeos input stack (cursor never reached node); relative-mouse positioning is eaten by INPUT.DROP. Reverted dead usb-tablet code. Concrete conclusion: click-target + state-force items (sigviz-select, D.3 color-by-state, D.6 auto-hide, M.8 CVD) are all gated by INPUT.DROP (id=478) whose fix = A.6 composite region-dirty (compositor.c:275). commit 771c1d2.
  - cycle 3b CORRECTION (measured, bible id=480): the INPUT.DROP mechanism I asserted (i8042 overflow during long composite) was WRONG. ISR+dispatch trace shows bytes arrive+dispatch on completed boots, and slowing the guest (trace build) nearly ELIMINATED the drop — opposite of what composite-overflow predicts. It's a synthetic fast-input timing race (harness delivers a combo <1ms; guest occasionally doesn't latch), NOT a shipping defect, and A.6 is NOT its fix. My cycle-2 "detour to A.6" recommendation was based on the wrong hypothesis — withdrawn. Combo-items ARE grindable via multi-boot oracle. FORK DISSOLVED (no A.6 decision needed for input).
- BUCKETS now: (a) combo/keyboard items → grind with multi-boot oracle (no blocker). (b) click-TARGET items (sigviz-select etc.) → need a mouse-POSITIONING solution (relative-from-known-anchor; usb-tablet is ignored by guest). Tractable, next. (c) state-force items (D.3/D.6/M.8) → need a settings/state path. (d) pure-logic (L.*/A.7/P.*/B.4/B.8/E.9) → build-invariant; method decision (prod-selftest vs scope-note) — the ONE thing still worth Brad's input.
  - cycle 4 (2026-08): E.7 harness→production (bible 422→481). Set-1 scancode→ASCII: typed 'kbtest7' plain keys into the live shell on the real cold-boot desktop → serial 'unknown command: kbtest7', exact in-order decode, settled 2-boot. Adversarial review (vs K.4) CONFIRMED (independently decoded the scancodes, verified marker not hardcoded). FINDING: the Terminal WINDOW is a static font_draw mockup (main.c:60-71) — serial is the shell's real echo channel. commit pending.
  - cycle 5 (2026-08): D.10 harness→production (bible 417→482). Wallpaper load from VAULT — serial 'wallpaper loaded from VAULT 480x270 (31485 bytes)' (VAULT path, not fallback) + gradient rendered, settled 2-boot, no input (no drop exposure). Adversarial review (vs E.7) CONFIRMED from source (serial fires only after vault_read+decode both succeed; gradient excludes flat fallback).
  - cycle 6 (2026-08): D.12 harness→production (bible 418→483), STATIC RENDER scoped. Centered dock, 5 pinned + divider + 2 running + state dot + active highlight, observed + settled 2-boot (64677/64677). FENCED auto-hide slide (auto_hide=0) + empty-at-boot as harness-only. Adversarial review (vs D.10) CONFIRMED (counted 5+gap+2 in pixels, fencing honest).
- PATTERN working well: partial-upgrade of static render + EXPLICIT fence of the animated/triggered sub-feature (C.2 click-to-raise, K.4 pan-then-evidenced, D.12 slide). Reviewer accepts honest fencing; rejects unevidenced bundling. Keep fencing precisely.
  - cycle 7 (2026-08): D.3 harness→production (bible 419→484), zone LAYOUT scoped. LEFT persona dot+label / CENTER pills / RIGHT health dots+clock, observed + settled 2-boot. FENCED color-by-state (needs chain-state forcing) as harness-only (independently proven id=419). Adversarial review (vs D.12) CONFIRMED (all 3 zones pixel-visible; fence honest).
  - cycle 8 (2026-08): E.5 harness→production (bible 420→485), FULL claim (no fence). All 4 hot corners fire post-dwell on production: un-gated serial [HC] TL→palette/BL→workspace/BR→show-desktop/TR→notify, each once, settled 2-boot; TL→palette visually confirmed (cursor at 0,0). Corner-slam clamps exact. Adversarial review (vs D.3) CONFIRMED (markers inside post-dwell dispatch, un-gated).
- KEY UNLOCK from E.5: rel() clamps EXACTLY at screen edges, so absolute positioning IS achievable via rel-from-a-clamped-corner-anchor (slam to a known corner, then rel by a known delta). This is the bucket-b mouse-positioning solution for K.2/D.11 click-targets — try next: slam to (0,0), then rel(+node_x,+node_y). (Earlier K.2 failures were the drop hitting the 2nd rel; retry under multi-boot oracle.)
  - cycle 9 (2026-08): K.2 harness→production (bible 441→487) — the item BLOCKED since cycle 3. Select (thick border+data) + inspector (IDENTITY+B3 BELIEF+NODES+ROUTES+TIMING real data) + deselect (border→thin), settled 2-boot. Adversarial review (vs E.5) REJECTED first pass (deselect unevidenced — only select+inspector saved) → FIXED in-cycle (Read+saved k2-deselect.png, node border thin). CORNER-ANCHOR positioning FAILED (hot-corner 150ms dwell fires under slow composite); SOLVED via known-position rel from init center (960,540)→(760,390)→node, no corner.
- ‼ BUCKET-B UNBLOCKED: mouse-POSITIONING solved = known-position rel (cursor is deterministic after the fixed early rel from init center). This unlocks ALL click-target items: D.11 (drag icon→surface), and any node/coordinate click. Corner-anchor DIDN'T PAN OUT (measured: TL hot-corner dwell fires under slow composite while the node-rel sits queued — not proven categorically impossible, just lost to a better lever); known-position rel is the working lever. Prefer it.
  - cycle 10 (2026-08): D.11 harness→production (bible 450→488). Dragged Files icon → Files window → un-gated serial 'DESK: fed Files -> surface 1', settled 2-boot. First click-target item via the known-position-rel unlock. Adversarial review (vs K.2) CONFIRMED from source (marker gated on wm_feed_surface, can't false-fire; serial is the only observable). Fixed a cosmetic dup [source] tag.
  - cycle 11 (2026-08): B.8 harness→production (bible 446→489) AND established the PURE-LOGIC METHOD. Extended the production `selftest` shell command to run the real fb_material_alpha() + print to serial; typed 'selftest' at the live zeos> shell → 'B.8 material ladder: 140/179/209/230/247 -> PASS', settled 2-boot. Adversarial review (vs D.11) CONFIRMED the item AND the method (real build-invariant fn, un-gated cmd, meaningful check, production scope).
- ‼ BUCKET-D UNBLOCKED (method resolved by execution, not waiting): pure-logic items are now verifiable on production via the `selftest` shell command — call the real build-invariant fn + observe serial. EXTEND cmd_selftest (shell.c ~4547) with more checks and drive via typing 'selftest'. Candidates: E.9 (ime_feed compose), L.1/L.2/L.6 (spring math), A.7 (aes-xts round-trip), P.2/P.3 (zp_compile), B.4 (frame timing), G.1-G.4 (color tables/lerp/prompts), O.2 (hal arch_name/io forwards). ~1 selftest check per cycle, adversarially reviewed.
  - cycle 12 (2026-08): E.9 harness→production (bible 456→490) via selftest method. cmd_selftest runs real ime_feed(): '+e→é, `+a→à, ~+n→ñ, "+u→ü, buffering, passthrough, no-match fallback → serial 'E.9 ime compose (...): PASS', settled 2-boot. Adversarial review (vs B.8) CONFIRMED (real fns, exact codepoints, no false-PASS; circumflex not sampled but honestly not claimed).
- ‼ BRAD ITEM (cycle 11 finding, he reacted strongly): the Terminal WINDOW is a static font_draw mockup (main.c:60-71) — the real Z+ shell works (E.7) but only talks to serial; the window doesn't render the live shell output buffer. FIX = render the shell's output/prompt buffer in the Terminal window instead of the static strings. Offered to Brad as a follow-up feature; do it when he greenlights (or if grinding stalls, it's high-value real work).
  - cycle 13 (2026-08): O.2 harness→production (bible 464→492) via selftest. Real hal_in8(0x64)==raw inb + arch=x86-64 → 'O.2 hal (...): PASS', settled 2-boot; PCI-config-io-behind-hal exercised at boot (sigviz device nodes). Adversarial review (vs E.9) CONFIRMED (source: hal_in8 is 'return inb(p)'; PCI fenced). Fixed evidence path nit (k4-sigviz-100pct.png not sigviz.png). cmd_selftest now runs B.8+E.9+O.2 (all PASS, no regression).
  - BY-HAND FEATURE (Brad "just get it"): TERMINAL is now REAL (fake font_draw mockup → live Z+ shell view). bible id=493,494; commit a78712c. Shell-only ring (term_console_shell gate) + main.c live render + scheduler dirties on shell input (the recomposite bug that hid output). Settled 2-boot: 'zeos> kbtest7 / unknown command: kbtest7 / zeos>' IN the window. Adversarial review CONFIRMED. (shell.c edits swept into other lane's commit d445dbd via git add -A — harmless.)
  - cycle 14 (2026-08): L.1 harness→production (bible 427→495) via selftest. Real anim_spring_default(0→100) ticks to ~100 in 45 ticks + inactive → 'L.1 spring converge (...): PASS', settled 2-boot. Adversarial review (vs O.2) CONFIRMED (real integrator, can't false-PASS, global-tick side effect bounded+honest, no leak). cmd_selftest now runs B.8+E.9+O.2+L.1.
  - cycle 15 (2026-08): L.2 harness→production (bible 428→496) via selftest. Real 4 presets (theme.h) 0→100: bouncy peak 128, interactive 104 vs smooth 226 ticks (2x faster), all converge → 'L.2 presets ... -> PASS', settled 2-boot, matches harness figures exactly. Adversarial review (vs L.1) CONFIRMED (real constants+integrator, no false-pass path, no slot leak). cmd_selftest now runs B.8+E.9+O.2+L.1+L.2.
  - cycle 16 (2026-08): A.7 harness→production (bible 462→497) via selftest. LIVE boot-armed AES-XTS round-trip (NO re-key: skipped crypto_disk_init to protect VAULT) — create region → encrypt(cipher differs) → decrypt(recovers) → destroy → 'A.7 aes-xts (armed=1 cipher_differs=1 recovered=1): PASS', settled 2-boot. Adversarial review (vs L.2) CONFIRMED real mbedtls AES-XTS + verified SAFE from source (no init, no drive write, memory-buffers only, region cleaned up). cmd_selftest now runs B.8+E.9+O.2+L.1+L.2+A.7.
  - cycle 17 (2026-08): P.3 harness→production (bible 459→499) via selftest. Real zp_parse (nodes=2) + zp_compile (chain_id=1) → 'P.3 zplus ...: PASS', settled 2-boot. Adversarial review (vs A.7) CONFIRMED (real zplus, genuine engine outputs) + flagged honest caveats now in ledger: parsed==0 is tautological (lenient parser) so proof rests on node_count+chain_id; execute-correctness out of scope; 1 chain/boot no-destroy (deterministic, ok). cmd_selftest now runs B.8+E.9+O.2+L.1+L.2+A.7+P.3.
  - cycle 18 (2026-08): P.2 harness→production (bible 435→501) via selftest. Real zp_run (parse→compile→EXECUTE) double+adder rc>=0 → 'P.2 zplus REPL (zp_run double=1 adder=1): PASS', settled 2-boot. Adversarial review (vs P.3) CONFIRMED (real execute pipeline, rc>=0 real gate, lenient-parser caveat honest/scoped, no audio/net side effects). cmd_selftest now runs B.8+E.9+O.2+L.1+L.2+A.7+P.3+P.2 (8 checks).
  - cycle 19 (2026-08): L.6 harness→production (bible 448→502) via selftest. Real scroll_phys_t: flick moves+decel, settles in-bounds, overscroll 1200→~1000 rubber-band → 'L.6 scroll (moved=1 decel=1 settled=1 rubberband=1): PASS', settled 2-boot (boot2 needed a retry — selftest cmd dropped once via input race, B.8/A.7 also absent, NOT an L.6 fail). Added #include anim.h (compatible w/ L.1/L.2 externs). Adversarial review (vs P.2) CONFIRMED (real damped-spring rubberband not clamp). cmd_selftest now 9 checks.
  - cycle 20 (2026-08): G.1 harness→production (bible 431→503) via selftest. Added read-only persona_accent_of/persona_dim_of getters (persona_anim.c); cmd_selftest asserts 3 accents distinct + 3 dims distinct + dim!=accent → 'G.1 persona tokens (...): PASS', settled 2-boot (retries: input-drop dropped the selftest cmd on some boots, B.8 absent, NOT a G.1 fail). Adversarial review (vs L.6) CONFIRMED (real tables, distinct constants, read-only). cmd_selftest now 10 checks.
  - cycle 21 (2026-08): G.2 harness→production (bible 432→504) via selftest. persona_prompt 3 distinct (zeros>/derez>/zeos>) + cursor_select_colorway 3 distinct accents, restored to FULL default → 'G.2 persona (...): PASS', settled 2-boot. Adversarial review (vs G.1) CONFIRMED (real fns, distinct accents 09BC8A/7A306C/2E86AB, restore-to-2 is true boot default, no lasting change). cmd_selftest now 11 checks.
- STATUS: RUNNING. selftest method proven for 11 pure-logic items; Terminal real. NEXT bucket-d via selftest: G.3 (persona crossfade color_lerp mid strictly between src/dst — need a public lerp/transition accessor), G.4 (dark/light theme tokens distinct+ordered, scheme round-trip), B.4 (frame_dt TSC — compositor-embedded, harder). Then click/multi-surface (C.12/13/14), M.7/M.8 (settings toggle).
