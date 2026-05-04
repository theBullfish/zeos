# Zeos — Current Session State

Read FIRST at the start of every Code session. Updated at the end of every
session. Three sections, that's it.

---

## Landed this session

- `tools/zplus/TOKEN_TAXONOMY.md` — empirical lexical inventory built from
  all 68 `programs/*.zp` files. Every token kind grounded in a real
  file:line citation. Includes lexer hazards (multi-char arrow ordering,
  number-with-suffix tokens like `30d`/`5x`/`2σ`/`1920x1080`, `t-1` as
  one token, `/dev/null` as one token, `~`/`@`/`|` overloading rules) and
  a "first test target" recommendation (`02_log_monitor.zp`).
- Caught two errors in the agent's first-pass survey: `<-` is the
  **actuator-binding** arrow (e.g. `valve : actuator(...) <- position @
  percent` — used in 16, 11, 22), not just an artifact in gate
  expressions. `<->` is **proposed only** in a comment at
  `programs/03_http_server.zp:110`, not used anywhere.
- Discovered token kinds the survey would miss without spot-checks:
  `TEMPLATE_STRING` interpolation (`"{name}.pdf"` — quill, zindex_studio),
  `DIMENSION` literal (`1920x1080`), ternary `?:` confirmed real
  (`derez/bot_trainer.zp:95`, `zindex_studio.zp:334-335`).

- Brad decided host language: **Rust** for the bootstrap frontend.
  Reasoning: AST/IR work fits enums + pattern matching; `inkwell` for LLVM
  if we go that route; `cargo test` is fire-and-forget. Self-hosting in Z+
  later means throwing the Rust frontend away — acceptable.

---

## Next up

**Z+ lexer in Rust.** With host language settled and the token taxonomy
landed, the next session can go straight to code:

1. `tools/zplus/Cargo.toml` workspace skeleton + `crates/zplus-lex/` (or
   single-crate at first — keep it small).
2. Implement the lexer per `tools/zplus/TOKEN_TAXONOMY.md`. Honor the
   hazards in §14 — especially the multi-char arrow precedence and the
   number-with-suffix tokens.
3. **Green-light test:** tokenize `programs/02_log_monitor.zp`
   end-to-end without errors. Round-trip the token stream back to source
   text as a property test (modulo whitespace + comment positions). When
   that passes, commit.
4. Stretch (only if §3 lands fast): try `programs/03_http_server.zp` and
   `programs/chirp.zp` as additional fixtures. Add any new token kinds
   they reveal back into `TOKEN_TAXONOMY.md`.

Reference docs to keep open:

- `tools/zplus/TOKEN_TAXONOMY.md` — the lexer spec, with file:line
  citations to ground every token kind
- `programs/02_log_monitor.zp` — first test target
- `programs/FINDINGS.md` — universal pattern + operator semantics
- `docs/ZPLUS_SPEC_V2.md` — current spec (parser will need this, not the
  lexer)

---

## Open questions

- LLVM backend vs custom backend: `docs/FOUNDATIONAL_PROGRAMS.md` notes
  "Z+ likely compiles through LLVM" but flags the decision as still open.
  This decision is **not** blocking the lexer or parser — defer until
  IR-emit time.
- `NEWLINE` significance: is it a chain terminator or just whitespace?
  The corpus uses one chain per line as the dominant form. Lex
  `NEWLINE` as a token for now and decide at parse time.
- Single-quoted strings: one occurrence in
  `programs/derez/bot_trainer.zp:80` inside a template interpolation.
  Real syntax or noise? Defer until parser session.
- Reserved-word minimality: `TOKEN_TAXONOMY.md` §11.1 lists 20 control-shape
  keywords and §11.2 lists ~50 contextual keywords (lex as IDENT, parser
  decides). Validate this split when writing the parser; some §11.2 names
  may need to move to §11.1.
