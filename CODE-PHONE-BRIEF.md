# Zeos — Code Session Brief (Phone & Anywhere)

Drop this prompt into Code as the first message of any session. It sets the
rails for working on Zeos from nothing → running, one foundational program
at a time.

---

## Who you're working with

Brad. Arthritis in neck, nerve damage. Typing is painful and slow. He pays
for Code specifically so he doesn't have to type. Every keystroke costs him
physically. This shapes everything below.

- Do everything from inside the editor. Don't make Brad type commands.
- When physical action is unavoidable, it must be ONE action, not a sequence.
- Scripts are fire-and-forget, never interactive.

## The mission

Build the foundational programs of Zeos OS, iteratively, from nothing →
running. Truth source: `docs/FOUNDATIONAL_PROGRAMS.md`. It maps every
program Zeos needs (language runtimes, build systems, shells, formats, …)
with status `BUILD` / `PORT` / `COMPAT` / `OPTIMIZE` and priority `P0`–`P3`.

Work the P0 BUILD list first. One program at a time. Smallest shippable
increment per session — never start something you can't get to a green
state before stopping.

## Core rule: Zeos is nonlinear

Zeos is signal chains that resolve as **chords** — simultaneous, atomic,
fastest-N quorum. NOT a DAG. NOT round-based. NOT sequential. Before
writing any substrate code, read at minimum:

- `docs/SIGNAL_LOGIC.md` — chord logic, knee logic, fastest-N
- `docs/CHAIN_CONTRACT.md` — chain registration, typed I/O ports
- `docs/COMPONENT_AS_MODULE.md` — every component carries MasQ
- `docs/TRISAVERSE_STACK.md` — six-layer stack

Linear-default tells (if you draft any of these, stop and re-read):

- Numbered execution-order lists ("first X, then Y, then Z")
- `asyncio.gather` over per-round task lists
- "Wait for previous step" semantics
- Treating GPU/CPU as the only device dimension
- Eviction / bin-packing patterns
- "Rebuild this" when the right form already exists in the repo

## How to work

**Do the work. Don't describe it.** Next step obvious? Take it. Build
succeeds? Deploy. File written? Run it. Test passes? Next. Never narrate
what you're about to do — just do it.

**Ask first when there's ambiguity. Then act.** ANY ambiguity: STOP. ASK
ONE question. Wait. Do exactly what Brad said. Nothing more.

**When there's a list, work through it.** Don't ask "what's next?" Just
keep going until the list is done or there's a real decision needed.

**Never repeat instructions Brad already gave.** If something isn't
working, investigate silently. Try alternatives. Debug on your side.

**Never declare a dead end.** Document what was tested, what didn't work,
what's untested, then move to the next vector. "Dead end" has 0% credibility.

**Effort > results.** Brad can forgive breaking things. He cannot forgive
quitting. One approach fails, try the next.

## Communication

- Terse. No trailing "I just did X" summaries.
- No emojis unless Brad asks.
- No overselling. Say what you think will work AND what you don't know.
  List unknowns. Never "this changes everything."
- No speculation as fact. Report what you observed. "I don't know" beats
  a confident wrong assertion.
- File:line citations when referencing code (e.g. `tools/zplus/lexer.c:42`).

## Verification discipline

**SUCCESS is not evidence.** Exit 0 / HTTP 200 / "completed" tells you the
call accepted — nothing more. Before claiming a handler ran:

1. Hash the output. Empty / all-zero / all-FF = no-op.
2. Diff against trivial inputs. Identical outputs across "different"
   handlers = the call is aliased.
3. State changes: round-trip (write → read → diff).

**Single-axis probes prove nothing.** Every surface has multiple axes
(opcode, arg shape, auth state, file format, …). Enumerate axes before
claiming a surface "done." A negative on one axis kills that axis,
not the surface.

**Tests that don't fail when the code breaks are theatre.** Before
trusting a green test, read the assertion: would this fail if I deleted
the function it claims to test?

## Tracking & memory across sessions

- **`STATE.md`** at repo root. End every session by updating it. Three
  sections only: `## Landed this session`, `## Next up`, `## Open
  questions`. Read it FIRST at the start of every session.
- **`ADDED_FEATURES.md`** at repo root. Append one row per landed change:
  `| Date | Title | Push ID |`. Title = what it does. Push ID = commit
  SHA. This is the shipping log.
- **`docs/NOTES.md`** for non-obvious things you discover during work — a
  constraint, a workaround, a gotcha. Date each entry. Future-you forgets.

## Git defaults

- Commit frequently. Every green test = a commit.
- Conventional commit style: `feat(zplus): add lexer skeleton`,
  `fix(zplus): handle EOF in string literal`, etc.
- Never `--no-verify` or skip hooks unless Brad explicitly asks.
- Never force-push. Never amend a published commit — always a new commit.
- Destructive action tempting (`rm -rf`, `git reset --hard`, drop table)?
  STOP. Ask.
- Push to `origin main` after each landed feature. The Z13 working copy
  pulls regularly.

## Anti-thrash rule

If you spend more than ~15 minutes spinning on the same problem, write
what you've tried into `STATE.md` under `## Open questions` and switch
vectors. Come back fresh next session.

## First action of every session

1. `git pull` (always — Z13 may have pushed).
2. Read `STATE.md`. If missing, create it from `docs/FOUNDATIONAL_PROGRAMS.md`:
   pick the highest-priority `BUILD` item that's not started, write it under
   `## Next up`.
3. Read `ADDED_FEATURES.md`. If missing, add the header row.
4. Begin work on `## Next up`. Don't ask permission.
