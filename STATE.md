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
  designed to NOT support.

- v3 lexer cleanup (commit `7e008b2`). 8/8 corpus clean — every .zp
  file tokenizes with zero Error tokens. Added: HeatUp (↑), HeatDown
  (↓), C-style string escapes (\" \\ \n \t \r \0), and `─...>`
  long-form Flow arrow.

- Parser landed (commits `2f8ac9b` + `02c7ebf`). Recursive descent over
  a tokenized stream with whitespace/comments filtered, newlines
  retained as statement separators at depth 0. **Vertical merge
  coalescing** in parse_module folds `INPUT -> |` fragments and
  `| policy | -> DOWNSTREAM` policy-only lines into a single
  `Merge` node — the chord rule at the parser level. Also: BinExpr
  / UnaryCmp / BinOp added to AST for comparison ops in arg
  position (`message ~ "..."`, `gate(> 5x)`).

  **`programs/02_log_monitor.zp` parses end-to-end** with 6
  integration tests in `tests/parse_log_monitor.rs` asserting
  structural properties — the all_lines merge has 3 inputs (chord
  canary), the within(30s) merge has 2 inputs + Within policy +
  downstream, the dashboard section produces ≥3 Tap statements.

  Corpus parse coverage: **2/68** files (02_log_monitor.zp,
  12_search_engine.zp). The other 66 exercise constructs the parser
  doesn't yet handle — see Next up. None affect the chord rule.

  `zplus-parse` CLI: `cd tools/zplus && cargo run --bin zplus-parse -- <file.zp>`.

  **73 tests green** (54 unit + 19 integration).

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

**Expand parser corpus coverage.** 02_log_monitor.zp is the green-light
target and works. The other 66 .zp files trip the parser on
straightforward additive features. None of these affect the AST shape
or the chord rule — just adding cases. Suggested order, easiest first:

1. **Fork blocks `{ a, b, c }`.** Most common gap (~30 files use
   forks). Add to `parse_chain_term`: on `LBrace`, parse
   comma-separated chains, expect `RBrace`. AST already has
   `Chain::Fork`. Test against `01_file_watcher.zp:535`.

2. **Unit annotations `@ ident`.** Programs/16_scada_industrial.zp,
   11_home_automation.zp, 22_precision_agriculture.zp use `position
   @ percent`, `rate @ rpm`, `target_temp @ F`. Either: extend
   `Atom` with `Atom::Unit { value, unit }`, or treat `@ Ident` as
   a postfix annotation captured on the preceding chain term. Latter
   is simpler.

3. **`N of M` and `2 of 5` in arg position.** programs/09_anomaly_detector.zp:50
   `resonance(2 of 5, within: 5m)`. The "N of M" form already exists
   in merge-policy parsing; lift it into arg-expression parsing as
   well, with an AST `Chain::Quorum { n, m }`.

4. **Fork branches with `mode(x): chain` syntax** (chirp.zp:137).
   Treat as a "labeled fork branch" — extend `Chain::Fork` to
   `Vec<ForkBranch>` where `ForkBranch { label: Option<Chain>, body: Chain }`.

5. **Array literals `[ ... ]`** (shield.zp). Add `Atom::List(Vec<Chain>)`.

6. **Hex / HexColor / Dimension / ByteSize / Bang / TimePast / DevNull
   atoms in chain term position.** All exist in the AST; just need
   the parser to recognize and convert. Some may already work — sweep
   and verify.

7. **Negation `!ident`** in arg position (zeros/arm_controller.zp:55).
   Add `Chain::UnaryNot` or similar.

8. **`<-` actuator binding** (16/11/22). Treat as a top-level
   declaration metadata operator — `name : actuator(...) <- type @ unit`
   could be a `WireDecl` extension `bound_inputs: Option<Chain>`.

A simple metric for "done" — the corpus.rs smoke test gets a
parse-coverage assertion: parses all 68 files cleanly. Right now
only 2 do.

Beyond the parser, deferred:

- `<->` bidirectional (proposed only)
- Type checking / typed ports (CHAIN_CONTRACT.md)
- IR lowering, codegen, runtime

The `<-` actuator-binding operator is the only AST shape change
remaining. Everything else is additive.

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
