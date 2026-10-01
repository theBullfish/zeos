# Z+ — Queued Syntax Changes

*Queued 2026-10-01. Not started. Pick up here next session.*

Two small, independent changes that came out of reading the 25 industry
programs against the spec. Neither needs the spec rewritten.

---

## 1. Inline chord (close the merge on one row)

**Problem.** Fork already closes inline (`a -> {b, c}`), so one-to-many fits
on a row. The chord does not. Many-to-one is still drawn as a margin diagram:

```zplus
heart_rate  -> |
                | -> shock_index : heart_rate / bp.systolic
bp.systolic -> |
```

This is the one level of the language that can't close on the row it started
on, and it is the reason most of the industry programs sprawl vertically.

**Proposal.** A bracket pair that opens a chord and closes it, with the policy
riding on the closer, so many-to-one reads like one-to-many:

```zplus
[heart_rate, bp.systolic] -> shock_index : heart_rate / bp.systolic
[left_eye, right_eye] |all| -> steer -> {left_wheel, right_wheel}
[a, b, c] |2 of 3| -> output
[a, b, c] |within(100ms)| -> output
```

- Default policy when none is written: `all` (strict chord), matching `|all|`.
- The existing multi-line pipe form stays legal. The bracket form lowers to
  the same single `merge` node; ZIR is unchanged.
- The chord rule (one merge node carrying `merge.policy`, never N edges)
  must hold for the bracket form exactly as it does for the pipe form —
  extend the Rust AST guard and `zir::tests` to cover it.

**Where it lands.** Rust front-end lexer/parser (`tools/zplus/src`), the C
front-end if it is still kept in sync, `TOKEN_TAXONOMY.md`, and
`ZPLUS_SPEC_V2.md` §2 Structure table. Then re-run the 85-file corpus through
`zplus-check` and convert a few programs to the new form as proof.

---

## 2. Section markers as syntax (the levels)

**Problem.** Every industry program is already written in the same four
bands, in the same order, marked with comment bars:

```
// ── BEDSIDE SENSORS ──    sources:      x : device(...) -> signal @ unit
// ── DERIVED VITALS ──     derivations:  a -> detect(...) -> b
// ── ALARM GATES ──        gates:        b -> gate(...) -> alarm
// ── ALARM FATIGUE ──      chords/routes: [..] -> count(...) -> { ... }
```

The language doesn't know the bands exist; the author draws them by hand.

**Proposal.** Promote the band to a first-class marker so a program can be
read at the band level and tools can fold, lint, and visualize by it. Exact
spelling undecided; candidates:

```zplus
== sources ==
== derive ==
== gate ==
== route ==
```

Open questions before touching the parser:
- Are the four bands fixed vocabulary or free labels?
- Does a band constrain what may appear inside it (a `gate()` in `sources`
  is a lint warning?) or is it purely organizational?
- Does the band map to anything in ZIR, or is it front-end only?

Decide those with Brad first. This one is a design call, not a mechanical
change.

---

## Also fixed this session

`programs/FINDINGS.md` and `programs/CATALOGUE.md` no longer quote
line-count ratios against shipping products; counts are now measured from the
files and the "conventional equivalent" claims were removed as not
like-for-like (wiring spec vs finished product). Keep it that way.
