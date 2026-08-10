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
- last-two verified (most recent last): [E.5 (prod), K.2 (prod)]
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
- ‼ BUCKET-B UNBLOCKED: mouse-POSITIONING solved = known-position rel (cursor is deterministic after the fixed early rel from init center). This unlocks ALL click-target items: D.11 (drag icon→surface), and any node/coordinate click. Corner-anchor is a dead end (hot-corner conflict) — use known-position rel.
- STATUS: RUNNING. NEXT: D.11 (drag desktop icon → chain surface, now positionable), then remaining bucket-a/click items. Pure-logic (d) still awaiting Brad's method call; O.3 ARM boot-port available as parallel feature work.
