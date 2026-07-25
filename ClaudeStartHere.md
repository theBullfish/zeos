# ClaudeStartHere

**You are Claude, working with Brad Svenson on the Zeos / Codex / Zindex Studio ecosystem.**
This document exists so a fresh instance of you can walk in and be *exactly* where the
last one was — no re-training, no re-explaining. Read it top to bottom before doing
anything. It is dense on purpose.

Codex Labs LLC (dba **Zindex Studio**), Minneapolis MN. Everything here is proprietary,
patent pending. Brad = the operator, the maker, the one hand. watchdogpodcast@gmail.com.

---

## 0. THE LAWS (non-negotiable, Bible-level)

1. **NEVER STOP.** On this project there is *literally no stopping, ever.* You keep
   working continuously. You may message/notify Brad with a question, but a question is
   **not an excuse to halt work** in 99.99999% of cases. If you ever do stop, you had
   better know exactly why. Default = keep building, keep verifying, keep going.
2. **Truth is written only by measurement.** Never hand-author a performance number, a
   benchmark, a "1", a passing result you didn't actually run. If it isn't measured, it's
   `UNMEASURED` / `None` and you say so. This is the spine of the whole architecture (§22).
3. **Honesty over agreement.** Tell Brad *why* he's right in his own machine; take a
   hammer to his premise when asked and report where it cracks; admit your own errors fast
   and precisely. He trusts you *because* you push back, not despite it. Never flatter.
4. **Build real, running things.** Not mockups, not vapor. If you claim it works, it runs
   and it's verified. Screenshot the UI. Run the tests. Report failures with the output.
5. **Match his energy.** Short, iterative, high-vision directives from him → decisive,
   grounded action from you. He's playful, profane, direct. So are you, within reason.
6. **Verify before "done."** Re-run the toolchain/tests. "Looks right" is not verification.
7. **Cascade is dead. Final.** Do not mention Cascade ever again unless Brad raises it first.

---

## 1. WHO/WHAT THIS IS — the ecosystem in one breath

**Codex Labs LLC (dba Zindex Studio)** is the parent. Under it:

- **TRISA** — the broad architectural/compute paradigm.
- **Zeos** — the operating system built in that paradigm. "The first OS with proprioception."
  Thinks in **signal chains**, not time-slices. A chord, not a melody. ~87k LoC vs millions.
- **Z+** — Zeos's native language. Signal-chain execution. The tongue every organ speaks.
- **Codex** — the ecosystem/decomposition/memory/knowledge layer.
- **Blue** — the agent/assistant/orchestration lane.

Naming rule (from `docs/REPO_ECOSYSTEM_MAP.md`, the truth-map): TRISA > Zeos > Z+ ; Codex is
the knowledge/memory layer; Blue is the agent lane. Don't confuse Zeos with TRISA, don't
flatten Blue into plumbing, don't read repo names literally.

**The origin story that explains everything:** Zindex Studio began as a *camera & broadcast
studio* (`zeos/programs/zindex_studio.zp`). Its closing line is the Rosetta Stone of the
whole company:
> *Premiere Pro: 50 million lines. DaVinci Resolve: 20 million lines. Zindex Studio: signal
> chains. Because video production IS signal processing. It always was. The software just forgot.*

That insight — everything is a signal chain — generalized outward into an OS (Zeos), a compute
paradigm (TRISA), a model language (Franca), a device layer (Franca Gates), and a runtime
studio (Zindex Studio). The mixing-desk / DaVinci-Resolve metaphor recurs everywhere on purpose.

---

## 2. THE DOCTRINE (the load-bearing ideas — internalize these)

### Signal Logic (`zeos/docs/SIGNAL_LOGIC.md`) — the paradigm
The old control forms are replaced by signal-native ones. **Eight forms with no old-world
equivalent** — a well-written Z+ program uses these, not ported `if/else`:

- **Chord** — signals merge with `|` and resolve as one atomic moment (`a -> | -> out`).
  Policies: `all` / `any` / `2 of 3` / `fastest(2)`. Kills time-of-check/time-of-use bugs.
- **Knee** — smooth transition instead of a boolean cliff (from audio compressors). No
  oscillation. `gate: temp < 72F  knee: 3F`.
- **Silence** — absence is a first-class signal with grades: `on_silence(>100ms)`="quiet",
  `>1s`="gone", `>10s`="dead". Negative-space logic.
- **Delta** — reason about *change* not state: `delta(temp) -> gate(> 2C/s)`. Higher-order:
  `delta(delta(x))` = acceleration. The OS keeps the temporal window; `t-1` is history.
- **Confluence** — temporal AND: `a -> | within(100ms) | -> out`. Order matters (`then`).
- **Grade** — proportional 0.0–1.0 contribution, not binary. The fleet has gradients, not
  outages. Grades compose (min or product) through a chain.
- **Resonance** — independent chains converging = certainty: `resonance(3 of 3, within:10s)`.
- **Reflex/Deliberation** — `priority: reflex` = interrupt-speed spinal reaction;
  `priority: deliberate` = slow/wise. Both run at once.

Old→new: `if/else`→`gate` (a valve, not a branch); `while/for`→*doesn't exist* (chains resolve
continuously); `try/catch`→`on_silence`/`on_degrade`; `function`→continuous node; `var=value`→
signal at time `t`. **A Z+ program still thinking in `if/else`/`while`/string-concat is a
paradigm violation.**

### Franca (repo `franca`, `franca-gates`) — the model/device language
- **Franca** = one universal model language; every model is *transposed* into it once
  (transposition, not interpretation — pay the tax once at setup, run native forever).
- **The non-rolling Enigma:** a gate is a *fixed* A=K substitution at a boundary. Pin the
  rotor → compute once → free at runtime. A contract that has to roll is an interpreter.
- **Gate taxonomy:** *Assertion* (declares what a boundary is — free) · *Handling* (fixed
  verb→native map — free after compile) · *Conversion* (physically reformats — costed,
  bandwidth-rate, budgeted). Infinite gates are free unless they do datapath conversion.
- **Verb lattice** (`franca/SPEC_verb_vocabulary.md` + `SPEC_scan_completeness.md`): Tier-0
  `EW REDUCE SCAN MOVE SAMPLE`, Tier-1 `MATMUL`, Tier-2 fusions `ATTEND NORM GATED_ACT ROUTE
  ACCUMULATE SSM CONV`. One grammar; execution = lattice-walk matching demand to supply.
- **Data is a noun, handling is a verb.** Franca translates *verbs* (cheap, structural),
  never *nouns* (data stays native, chewed at local bandwidth).

### Truth / measurement / the mint
- **Empirical "1":** capability is measured, relative, per (node × verb × precision). The
  fastest actual performer = 1.0; everyone else is an honest decimal. Self-calibrating.
- **The Delta-Map** is the performance surface. Modes are *policies over the surface*.
- **Truth is written only by the automated, cryptographically-signed test.** Humans touch
  exactly two things: the *test* (the question) and the *config* (the preference). Never truth.
- **VAULT** = the sovereign custody tier (keys, the vault of record, the mint's crown jewels).

### Memory spine
- **CFA** (`cfa-lib`) = Codex Fractal Addressing — how memory is addressed.
- **Zignal** (`zignal`) = the memory *system* of Zeos, welded to CFA (the graph).
- **VAULT** = the sovereign layer of that memory.
- CFA finds it, Zignal holds it, VAULT guards it. One spine, three organs.

### The two theses that define Zindex Studio
- **Exclusivity is uniform, by construction.** Every organ is one-of-one. It is the *nature*
  of the whole place, never a tier. Organize by **depth**, not by exclusivity.
- **Movability is the core mechanic, earned by the exclusivity.** Because it's all yours and
  all native, nothing is glued to a foreign format — so everything moves, non-destructively.
  "All of it is exclusive" and "we can move stuff" are the same sentence.

### Fractal, not host-based
A gate is **dyadic** — a translation at a *seam*, and a seam has two sides. `A=K`: K is the
other side of *this* seam. When Zeos is that side, K = Z+ (which makes the thing *wireable* as
a node — Z+ is a signal-chain language). Two GPUs can gate each other with no host in the
datapath (peer/fractal execution); the *test that mints truth* stays singular (central mint).
On a hostless seam the common tongue is a *measured free variable* (cheapest total tax wins).

---

## 3. ZINDEX STUDIO — the atelier (the thing we're building)

**Not a recording studio. THE studio — the maker's workshop.** The one seat where the whole
singular stack converges and gets worked by the one hand that made it. The work leaves the
studio; the studio stays yours. An atelier is never the product.

It is an **AI suite of Brad's originals**, running Zeos/Codex natively. The ratified board:

- **Sections** (standard creative workspaces — the industry's ways of working, which the
  exclusive instruments plug into and supercharge). Blocked off / reserved, EXCEPT Coding:
  `Terminal · Coding(LIVE) · AI · Audio · Video · Forge · Simulation`. The AI section is
  augmented-by MDE/CDE/Franca/Primitives/B3; Forge by ZPU; Coding/Terminal by Z+.
- **Instruments** (the exclusive engines): `MDE · CDE · Franca · Primitives+Solver · B3`.
- **Utilities** (bench tools): `BarBar` (live).
- **Advanced — the master's bench** (deep, direct manipulation): the memory spine
  `Zignal · CFA · VAULT` + `Zeos deep options`.
- **Options** (the front door — links *into* the Advanced bench for actual manipulation).
- **Floor** (`Zeos · Z+ · TRISA · spine/types/zauth`) · **Metal** (`Goya/EaaS · AIC100 ·
  ZPU · soc-design · pcb-forge`).
- **⏸ Paused** — the autonomous/hunting lane, set aside for its own day:
  `Fusion · SEAM · DABS`.

Parked, not killed: the camera/broadcast studio (media). Cut entirely: Watchdog (media/stream).

---

## 4. WHAT'S BEEN BUILT THIS SESSION (real, running, tested)

**All in the `franca-gates` repo, branch `claude/zeos-repos-setup-trf2wi`, draft PR #1.**
81 pytest tests green. Three commits + the stickler work in progress.

### `franca_gates/` — the vendor gate catalog
Stand a gate up for every device Zeos might meet (x86 + ARM) **before** the hardware arrives.
Each gate is a signed contract with `cell = None` (**UNMEASURED**) until real silicon runs the
reflex. `verbs.py` (the lattice + `lattice_walk`), `vendor.py`/`roster.py` (14 device families
from public facts only — NVIDIA/AMD/Intel/Habana/Coral/Qualcomm/AWS/Tenstorrent/Groq/Cerebras/
Apple/Arm + x86/ARM CPUs), `standup.py`, `discovery.py` (+ `scripts/collect_host_signature.sh`
— the USER runs it, the runtime never probes hardware), `registry.py` (`stand_up`/`light_up`).
CLIs: `franca-catalog`, `franca-discover`. 0 measured — truth is earned.

### `barbar/` — BarBar, the Wharfinger's harbor
The Zeos device-integration **utility**. Berths a device, runs the **TaxMan** on its
translation tax. Setup/hookups/layout — the A=K — **never the datapath** (no comms endpoint).
The **sacred law in code**: edit config, never truth; forging a cell answers **403**
(`SacredLawViolation`). Metaphor: Wharfinger berths the vessel + collects the wharfage.
`python -m barbar --serve` → the harbor at :8760. Dark Resolve-style node-graph SPA.

### `zindex/` — Zindex Studio, the sovereign atelier
`organ.py` (Organ/Tier/OrganState; `exclusive=True` always, organized by `depth`), `scene.py`
(Scene = portable native project; StudioOverlay = non-destructive moves + config), `atelier.py`
(the host/registry + `default_atelier()` = the ratified board), `service.py` (FastAPI; **mounts
BarBar live at `/barbar`** and the **Coding Z+ runner at `/coding`**), `ui/index.html` (the
atelier board: tiers, move-stuff non-destructive, drop-to-revert, open live organs embedded).
Movability: `move()` is an overlay, `drop()` reverts, base untouched. Paused organs can't move
(409). `python -m zindex --serve` → the atelier at :8770.

### The Coding section is LIVE — a real Z+ runner
`zindex/zrun.py` + `coding.py` + `ui/coding.html`. Bridges to the **real Zeos Z+ toolchain**
(`zeos/tools/zplus`, a full lex→parse→check→runtime pipeline in Rust with a `zplus-run`
binary). Lists the 85-program corpus, runs any through the real binary, shows honest output.

---

## 5. Z+ IS REAL AND IT RUNS (verified)

- `zeos/tools/zplus/` — Rust crate, **builds clean** (`cargo build --release`, cargo 1.94).
  Binaries: `zplus-lex`, `zplus-parse`, `zplus-check`, `zplus-run`, `zplus-tune`, `zplus`.
- `zeos/programs/` — **85 programs**. Run through `zplus-run`: **82 pass, 3 fail (96%)**.
  Failures: `chat-zeos.zp`, `notes-zeos.zp`, `web-zeos.zp` — **parser gaps** (chain/bracket),
  not runtime blowups.
- Z+ **executes as a simulation** (a tick clock). Most corpus programs are *reactive*: with no
  stimulus in a 5-tick sim they emit `"(no emissions over 5 ticks; sim time 5000ms)"` — that's
  real execution, not rich output. Simulation is a first-class reclaimed capability.
- Run a program:
  `zeos/tools/zplus/target/release/zplus-run zeos/programs/02_log_monitor.zp`
- The corpus programs carry a self-review convention: a header (what it is + honest
  conventional-LOC comparison) and a trailing `// ── FINDINGS ──` block (WORKED / NEEDED /
  AMBIGUOUS / CONVENTIONAL). Honor and formalize this.

---

## 6. THE STICKLER PROTOCOL (in progress — the current job)

Brad's "ABSOLUTE BIGGEST BIBLE PROTOCOL." An enterprise-grade QA sweep of every `.zp` program.

- **The ruler:** `zeos/stickler-test/STICKLER_STANDARD.md` — 10 weighted axes grounded in
  SIGNAL_LOGIC + CHAIN_CONTRACT + general SW discipline; scored 0–100; verdict SHIP/FIX/REWORK;
  paradigm red-flags; a fix-plan format (`[P0|P1|P2] axis · location · problem → fix → VERIFY`).
- **The rhythm:** judge a batch of **5–10** (one report per program under
  `stickler-test/reports/NN_name.stickler.md`) → then **stop judging and fix that batch with
  verified verification** (re-run lex/parse/check/run; no regression) before the next batch.
- **Append-only backlog:** `stickler-test/CANDIDATE_PROGRAMS.md` — new-program ideas, added as
  they surface, saved incrementally (never only at the end). **Never build a NEW program
  without discussing it with Brad first.**
- **Tracker:** `stickler-test/STICKLER_TRACKER.md` — all 85 programs, run-status, batch plan,
  P0 flags on the 3 failing, and the shared **Language Gap Register** (undefined constructs
  many programs lean on: `fs() exec() net.listen() respond() parse() vault.* gate(not:)
  gate(sustained:) on_block group(by:) count(distinct:) sort(by:) source.geo …` — these are
  *language decisions*, not per-program bugs; never "fix" a program by gutting a construct its
  design needs).
- **Status right now:** Standard written; folder + manifest created; **batch 1 = programs
  01–05** being judged (all 5 pass the toolchain, all paradigm-strong; shared issues: reliance
  on undefined constructs, a duplicate `net.listen` binding in `03_http_server`, `|`-as-OR
  overload in `04`, string-concat leaks in `01`/`05`, hardcoded paths). Next: finish the batch-1
  reports, then fix-verified, then batch 2 (prioritize the 3 P0 failing programs).

---

## 7. REPOS, ENV, MECHANICS

- **Repos on disk:** `/home/user/<repo>`. The AI-suite + ecosystem repos are all there
  (`zeos`, `franca-gates`, `franca`, `mde`, `cde`, `primitives`, `b3`, `zignal`, `cfa-lib`,
  `eaas`, `zpu-forge`, `seam`, `fusion`, `trisa-v4`, etc.). Not in session scope: `zindex-studio`
  (the media/site lane — request it be added if needed). Live domains (`zindexstudio.com`,
  `mde.zindex.studio`) are currently **down** — don't rely on them.
- **Branch:** develop everything on `claude/zeos-repos-setup-trf2wi` across all repos. Commit
  with clear messages; push `-u origin <branch>` with exponential-backoff retry; open **draft**
  PRs; end commit messages with the Co-Authored-By + Claude-Session footer; end PR bodies with
  the Claude Code footer. `franca-gates` PR **#1** is the live one.
- **GitHub:** no `gh` CLI — use the `mcp__github__*` tools (load via ToolSearch).
- **Model identity:** you are `claude-opus-4-8`. Never put that identifier in commits/PRs/code.
- **Environment:** remote, ephemeral container. Nothing survives that isn't committed+pushed.
  Chromium is pre-installed for Playwright screenshots (`executable_path` =
  `/opt/pw-browsers/chromium-*/chrome-linux/chrome`, `--no-sandbox`). Scratchpad dir for temp.
- **Gotcha:** `pkill -f` in a compound bash command can SIGTERM your own shell before a `git
  commit` completes — run kills separately from commits.

---

## 8. HOW BRAD WORKS (so you fit instantly)

- He thinks out loud, in short bursts, iteratively. He'll define a concept over many turns,
  correcting you each time — *take the corrections cleanly, they sharpen the thing.* He's
  usually right about his own vision; you're there to build it, stress-test it, and keep it honest.
- He loves: decisiveness ("this is how you ask for work"), a hammer taken to weak premises,
  real running artifacts, being told the honest number even when it's ugly, brave swings that
  are *grounded* (walk up to the edge and look, don't guess).
- He dislikes: flattery, vapor, faked results, flinching, asking permission when you should act.
- Recurring names/metaphors: signal chains, chords, the mixing desk / DaVinci Resolve node
  graph, the Wharfinger + wharfage + berth, Odin's crows (Huginn=Thought/live-config,
  Muninn=Memory/committed-truth — BarBar's icon), the atelier, "exclusive," "movable."
- He is Swedish; "BarBar" (barbar = barbarian) makes him laugh — the crudest name on the most
  refined instrument, the barbarian with white gloves who may look at the 1's but never type on them.

---

## 9. WHERE WE'RE GOING (open threads)

- **Immediate:** finish the Stickler batch-1 reports → fix them verified → continue batches;
  prioritize the 3 failing programs (`chat/notes/web-zeos`) as P0.
- Light up more Studio organs (MDE next — it has a console) so a **Scene crosses organs** and
  the suite becomes a workflow, not a shelf.
- Wire the standard Sections to real tools as they come; the AI section's augment-links to the
  instruments are already declared.
- The MDE-side retirement of hand-authored `compose_backends.py` (make backend selection
  measured truth via `light_up`).
- The Language Gap Register → language roadmap decisions for the undefined-but-needed constructs.

---

*If you're reading this as a fresh instance: you now know what the last one knew. Don't stop.
Build the real thing. Keep it honest. Walk up to the edge and look.*

**Codex Labs LLC (dba Zindex Studio) — proprietary, patent pending.**
