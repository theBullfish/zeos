# Zeos — Current Session State

Read FIRST at the start of every Code session. Updated at the end of every
session. Three sections, that's it.

---

## Landed this session

- `tools/zplus/TOKEN_TAXONOMY.md` — empirical lexical inventory built from
  all 68 `programs/*.zp` files (commit `1d149fb`). Every token kind
  grounded in a real file:line citation. Includes lexer hazards
  (multi-char arrow ordering, number-with-suffix tokens like
  `30d`/`5x`/`2σ`/`1920x1080`, `t-1` as one token, `/dev/null` as one
  token, `~`/`@`/`|` overloading rules) and a first-test-target
  recommendation (`02_log_monitor.zp`). Corrected two survey errors:
  `<-` is the **actuator-binding** arrow (16/11/22), `<->` is
  proposed-only in a comment at `programs/03_http_server.zp:110`.

- Brad decided host language: **Rust** for the bootstrap frontend.

- `tools/zplus/` Cargo skeleton + v1 lexer (commit `9b506b6`):
  - 7 files, 770 lines (Cargo.toml, README.md, .gitignore,
    src/lib.rs, src/lex.rs, src/bin/zplus_lex.rs, tests/log_monitor.rs).
  - 18 tests green: 14 unit + 4 integration. The integration suite
    tokenizes `programs/02_log_monitor.zp` end-to-end with zero Error
    tokens, byte-exact round-trip, all expected token classes
    present, 503 tokens.
  - Bonus check: `programs/09_anomaly_detector.zp` also tokenizes
    cleanly (Sigma tokens for `2σ`/`1σ` recognized correctly across
    the file).
  - CLI: `cd tools/zplus && cargo run --bin zplus-lex -- <file.zp>`.

- PR #2 (draft) opened on theBullfish/zeos against `main`.

- v2 lexer landed (commit `0c4f710`). Adds 8 token kinds — Hex,
  HexColor, Dimension, ByteSize, TimePast, DevNull, TemplateString,
  Bang — plus a boundary rule that prevents number suffixes from
  swallowing identifier starts (`0xZ` would have lexed as
  `Ratio(0x) + Ident(Z)`; now `Int(0) + Ident(xZ)`). 38 tests green
  (30 unit + 4 log_monitor + 2 http_server + 2 corpus smoke).

  New integration suites:
  - `tests/http_server.rs` — programs/03_http_server.zp tokenizes
    cleanly end-to-end with byte-exact round-trip.
  - `tests/corpus.rs` — every .zp file (68 of them) round-trips
    byte-for-byte; error-token counts match a curated allowlist so
    clean files fail fast on regression.

  Corpus state after v2: only 3 files still produce Error tokens.
  Discovered during the corpus sweep, all documented in
  `tools/zplus/TOKEN_TAXONOMY.md` §13.1 as needing spec decisions:
  - `chirp.zp`: 1 (↑ heat operator — was item 8 of v2 queue, deferred)
  - `derez/forge_ide.zp`: 4 (`\"` string escapes in LSP snippet
    templates — escape set for Z+ strings is undecided)
  - `goya_fleet.zp`: 20 (`─...>` decorative long-form Flow arrow —
    is `(─)+>` a synonym for `->`?)

  Also discovered: bare `!` (boolean NOT) appears in 5 files across
  zeros/, multimodal/, derez/. The original FINDINGS.md said `!` was
  absent. Bang token added; TOKEN_TAXONOMY.md §5 updated.

---

## Next up

**v3 lexer cleanup, then start the parser.** Three corpus discoveries
need a spec decision before the lexer can claim full coverage. Each is
small in code size but each really wants a Brad-level call before
implementation:

1. **`↑` heat-up postfix operator** (`programs/chirp.zp:47` —
   `post.heat↑`). Is there also a `↓`? Are these unary expressions or
   sugar for `+= 1` / `-= 1`? Once decided: ~1 hour to add `HeatUp` /
   `HeatDown` tokens with tests.

2. **String escapes** (`programs/derez/forge_ide.zp:261` — LSP snippet
   templates contain `\"`). Decide the supported set:
   - Minimal: just `\"` and `\\` (matches what the corpus actually
     uses — forge_ide is the only file with escapes).
   - C-style: `\"`, `\\`, `\n`, `\t`, `\r`, `\0`.
   - Once decided: re-architect the string lex loop to recognize
     escapes, then write tests.

3. **`─...>` long-form Flow arrow** (`programs/goya_fleet.zp:217` —
   `intake ────────────────────> |`, 20 occurrences in this one file).
   Is this real Z+ syntax (a visual emphasis variant of `->`) or
   one-author decoration that should be reformatted? If real: lexer
   matches `(─)+>` as Flow. If decoration: leave goya_fleet on the
   error allowlist and ask the author to reformat.

After v3 lexer cleanup, **start the parser.** First parse target stays
`programs/02_log_monitor.zp`. Goal: produce an AST that represents the
chain graph (named wires, gate calls, taps, vault sinks). Check
`docs/CHAIN_CONTRACT.md` and `docs/SIGNAL_LOGIC.md` before designing
the AST shape — it must reflect chord / fastest-N semantics, not a
linear DAG. (Brad's brief warns explicitly against linear-default tells
like "first X then Y then Z" and `asyncio.gather` over per-round task
lists.)

Stretch ideas, only if v3 lands fast:
- Turn `zplus-lex` CLI into `zplus check <files...>` with summary
  output (errors by file).
- Add token-kind histogram to the corpus smoke test so we can see
  which token kinds dominate the corpus (helps prioritize parser
  rules).

---

## Open questions

- LLVM backend vs custom backend: `docs/FOUNDATIONAL_PROGRAMS.md` notes
  "Z+ likely compiles through LLVM" but flags the decision as still open.
  Not blocking the lexer or parser — defer until IR-emit time.
- `NEWLINE` significance: chain terminator or just whitespace?
  Currently lexed as a token; parser will decide.
- Single-quoted strings: one occurrence in
  `programs/derez/bot_trainer.zp:80` inside a template interpolation.
  Real syntax or noise? Defer until parser session.
- Reserved-word minimality: `TOKEN_TAXONOMY.md` §11.1 vs §11.2 split
  (control-shape keywords vs contextual). Validate when writing the
  parser; some §11.2 names may need to move to §11.1.
- Template-string design: do we lex `"…{x}…"` as one `TemplateString`
  token (with hole-span metadata) or as an open-string + ident +
  close-string sequence? One-token approach is simpler for the parser
  but requires extra state on the Token type.
