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

Reference docs (read before writing code):

- `docs/ZPLUS_LANGUAGE.md` — language overview
- `docs/ZPLUS_SPEC_V2.md` — current spec
- `docs/SIGNAL_LOGIC.md` — chord logic; Z+ programs ARE signal chains
- `docs/CHAIN_CONTRACT.md` — typed I/O ports

Smallest shippable increment ideas (pick one, finish it, commit, push):

1. Project skeleton in `tools/zplus/` — `Cargo.toml` (Rust frontend) or
   `Makefile` (C frontend) + `README.md` + a hello-world test that
   `cargo test` / `make test` exercises. Decide the host language first.
2. Lexer for the smallest legal Z+ subset (literals, identifiers,
   parens, semicolons). Property-based test: round-trip a token stream
   back to source.
3. Token taxonomy file enumerating every token kind the grammar can
   produce, with one example each. Future-self reference.

Don't try to do all three in one session. Pick one. Land it green. Commit.
Update this file. Stop.

---

## Open questions

- Host language for the Z+ compiler frontend? Rust (matches the modern
  toolchain) vs C (matches the kernel) vs self-hosted-eventually-but-bootstrap-in-X?
  Brad has not decided. ASK before committing to one.
- Does Z+ go through LLVM IR or have its own backend from day one?
  `docs/FOUNDATIONAL_PROGRAMS.md` notes "Z+ likely compiles through LLVM"
  but flags the decision as still open.
- Is there existing Z+ compiler work in another repo that should be
  pulled in instead of starting empty? Brad's notes mention "13+
  programs" and a `FINDINGS.md`. Check before reinventing.
