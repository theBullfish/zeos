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
- cursor: (loop start — build queue on first fire)
- queue: [rebuild from lint on first fire; interactive items first]
- last-two verified (most recent last): [PALETTE.FLOW (prod), K.1 (prod)]
- cycle log:
  - (seed) 2026-08: PROD.COLDBOOT.CORE, PALETTE.FLOW, K.1 upgraded harness→production (bible 467-471). commit b971034.
- STATUS: ARMED (awaiting first cron fire)
