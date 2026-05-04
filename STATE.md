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

- AST type landed (commit `b630c5d`). `tools/zplus/src/ast.rs` defines
  the typed enum surface for Z+. The chord rule from
  `docs/SIGNAL_LOGIC.md` §1 is **structural**: a `|` merge is one
  `Merge` node carrying its policy (All / Any / Quorum{n,m} /
  Fastest(N) / Within / By), never a DAG of independent edges. Comments
  at the top of `ast.rs` name the linear-default tells we explicitly
  designed to NOT support. Hand-built AST for the top section of
  `programs/02_log_monitor.zp` lives in `tests/ast_log_monitor.rs`
  with the chord-rule canary `merge_is_one_node_not_three_flow_edges`
  that fails first on any future parser lowering mistake. 47 tests
  green.

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

**Parser, smallest first cut.** AST is in. Lexer covers 65/68 files.
The bridge from token stream → AST is the next thing.

Suggested smallest shippable increment:

1. **Parse a single wire-declaration line** — `name : <chain>` where
   `<chain>` is the simplest form: `Call` followed by zero or more
   `Flow` to atom paths. Target: parse line 8 of
   `programs/02_log_monitor.zp` (`syslog : fs("/var/log/syslog") -> lines`)
   into the same `WireDecl` shape that `tests/ast_log_monitor.rs`
   hand-built. Round-trip via `assert_eq!(parsed, hand_built)`.
2. **Then merge resolution** — handle the vertical `-> |` /
   `-> | -> name` block as one `Merge` node with `MergePolicy::All`.
   This is where the chord rule first hits parser code; the canary
   test in `ast_log_monitor.rs` will catch lowering mistakes.
3. **Then the rest of `02_log_monitor.zp`** — gate(...), parse(...),
   delta(...), rate(per: ...), within(...), on_silence(...),
   vault.store(ttl: ...). Each adds Call shape variants but no new
   chain-level operators.

Defer until after `02_log_monitor.zp` parses end-to-end:

- The three v3 lexer questions (↑ heat operator, `\"` string escapes,
  `─...>` long-form arrow). They block 3 of 68 files; the 65 clean
  files are enough to drive parser design.
- `<-` actuator binding (used in 11/16/22 — different programs).
- `<->` bidirectional (proposed only).
- IR / lowering / codegen.

Open architectural question for the parser: should it produce a
`Result<Module, Vec<ParseError>>` with error recovery (continue past
syntax errors) or a `Result<Module, ParseError>` (fail fast)? The
brief implies fast feedback matters; recommend fail-fast for v1 and
add recovery only when the corpus surfaces the need.

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
