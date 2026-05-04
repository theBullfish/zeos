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

---

## Next up

**Expand lexer fixture coverage, then start the parser.**

The v1 lexer covers the operator + literal classes used by
`02_log_monitor.zp` and (verified) `09_anomaly_detector.zp`. The
deferred-to-v2 list lives in `tools/zplus/README.md`:

- `Hex` (`0x68`) — used by I²C address declarations across 6+ programs
  (multimodal/, zeros/, competition/)
- `HexColor` (`#RRGGBB`) — used by UI programs (zindex_studio, derez/)
- `Dimension` (`1920x1080`) — `programs/zindex_studio.zp:11`
- `ByteSize` (`200KB`) — `programs/20_power_grid.zp`
- `t-1` / `t-2` temporal tokens — `programs/10_game_server.zp`,
  `programs/09_anomaly_detector.zp`
- `/dev/null` as one token — `programs/chirp.zp:122`,
  `programs/10_game_server.zp:77`
- Template-string interpolation `"…{name}…"` — quill, zindex_studio,
  heavy use across the UI corpus
- `↑` / `↓` heat operators — `programs/chirp.zp:47`

Suggested ordering for the next session:

1. **Add v2 token kinds in one batch** with unit tests for each
   (mirror the v1 test pattern). Easiest first: `Hex`, `HexColor`,
   `ByteSize`. Then the trickier ones: `Dimension` (vs Ratio vs Hex —
   §14.2 ordering rule), `t-1` (no-whitespace lookahead),
   `/dev/null` as a single token.
2. **Add fixture tests** for `programs/03_http_server.zp` and
   `programs/chirp.zp`. When both lex with zero Error tokens, we have
   real coverage breadth.
3. **Do template-string interpolation last** — needs design (lex inner
   `{ident}` holes vs treat whole thing as one TemplateString with a
   list of hole spans).
4. After all that lands, **start the parser.** First parse target is
   still `programs/02_log_monitor.zp`. Goal: produce an AST that
   represents the chain graph (named wires, gate calls, taps, vault
   sinks). Check `docs/CHAIN_CONTRACT.md` and `docs/SIGNAL_LOGIC.md`
   first — the AST shape should reflect chord/fastest-N semantics, not
   a linear DAG.

Stretch: turn the lexer's CLI into `zplus check <file>` that
tokenizes every `.zp` file in `programs/` and reports a per-file
error count. That's a fast smoke test for the whole corpus.

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
