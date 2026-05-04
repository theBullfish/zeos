# Zeos — Current Session State

Read FIRST at the start of every Code session. Updated at the end of every
session. Three sections, that's it.

---

## Landed this session

_(none yet — bootstrap commit only)_

---

## Next up

**Z+ compiler — bootstrap pass.** P0 BUILD per `docs/FOUNDATIONAL_PROGRAMS.md`.
The directory `tools/zplus/` is empty. Everything else (zeos-build, native
runtime, signal-chain definitions in Z+) eventually compiles through this.

The grammar is **already implicitly specified** by `programs/*.zp` (27 real
Z+ programs ranging from 30 to 130 lines) and `programs/FINDINGS.md`
(operator vocabulary, universal pattern, what works across all 27). The
compiler's job is to compile what's already there. Don't reinvent syntax.

Reference docs (read before writing code):

- `docs/ZPLUS_LANGUAGE.md` — language overview
- `docs/ZPLUS_SPEC_V2.md` — current spec
- `docs/SIGNAL_LOGIC.md` — chord logic; Z+ programs ARE signal chains
- `docs/CHAIN_CONTRACT.md` — typed I/O ports
- `programs/FINDINGS.md` — confirmed operators (`->`, `~>`, `|`, `{}`,
  `gate()`, `knee`), the universal pattern, evidence across 27 programs

Reference inputs (real Z+ programs, smallest first):

- `programs/02_log_monitor.zp` — 45 lines, classic source→filter→sink
- `programs/03_http_server.zp` — 50 lines
- `programs/chirp.zp` — short
- (full list in `programs/`; pick the smallest as first parse target)

Smallest shippable increment ideas (pick one, finish it, commit, push):

1. Project skeleton in `tools/zplus/` — `Cargo.toml` (Rust frontend) or
   `Makefile` (C frontend) + `README.md` + a hello-world test that
   `cargo test` / `make test` exercises. Decide the host language first
   (ASK Brad if unsure).
2. Lexer for the smallest legal Z+ subset (literals, identifiers, the
   six confirmed operators, parens, braces, semicolons). Test target:
   tokenize `programs/02_log_monitor.zp` cleanly. Round-trip the token
   stream back to source as a property test.
3. Token taxonomy file enumerating every token kind seen across
   `programs/*.zp` with one example each. Future-self reference.

Don't try to do all three in one session. Pick one. Land it green. Commit.
Update this file. Stop.

---

## Open questions

- Host language for the Z+ compiler frontend? Rust (matches the modern
  toolchain) vs C (matches the kernel) vs self-hosted-eventually-bootstrap-in-X?
  Brad has not decided. ASK before committing to one.
- Does Z+ go through LLVM IR or have its own backend from day one?
  `docs/FOUNDATIONAL_PROGRAMS.md` notes "Z+ likely compiles through LLVM"
  but flags the decision as still open.
