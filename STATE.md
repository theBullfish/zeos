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

  Corpus parse coverage: **68/68** files (commit `ca4f57a`). Every
  `.zp` file in `programs/` now parses cleanly. The ratchet test
  `parser_minimum_corpus_coverage` is locked at 68 — any regression
  fails CI. None of the additions affect the chord rule.

- Type system shape (commit `85e49d6`). `src/ty.rs` defines the typed
  enum surface — `Type::{Prim, Tagged, Sig, Range, Rate, Tuple, List,
  Grid, Map, Record, Union, Fn, Nominal, Quorum, Named, Unknown, Any,
  Never}` plus `UnitTag::{Simple, Compound, HardwarePin}`. Same
  pattern as ast.rs — design before checker code locks the shape.
  `tools/zplus/SEMANTIC_CONTRACTS.md` enumerates each of the 28
  synthetic `__op__` callees with input/output type, runtime contract,
  and parser-emit site.

- Type checker first cut (commit `502bd37`). `src/check.rs` runs over
  a parsed Module and reports `TypeError`s. v1 enforces:
  literal typing (every `Atom::Literal` → concrete `Type`) and Merge
  arity (chord rule at the type layer — Quorum / Fastest / All / Any
  / Within / By each have shape rules the runtime needs to honor).
  Corpus integration test against `02_log_monitor.zp` clean.

- Chord-rule violations driven 17 → 0 (commit `13f3b76`). Three
  parser fixes: fork-body merge coalescing, Bind-wrapped merge
  coalescing at stmt level, type-union `| IDENT` shorthand on `->`
  RHS. The full corpus now passes the chord-rule arity check with
  zero exceptions. Ratchet locked at 0.

- Type checker second cut (commit `0cbb1f9`). `TypeEnv` with 19
  builtins from the corpus (fs, gate, parse, delta, rate, on_silence,
  vault.*, alert, count, last, …); `infer_chain` walks the AST and
  returns concrete types where possible (Unknown otherwise);
  `types_compatible` is the flow-boundary compatibility predicate
  (Unknown matches anything, primitive equality, Int → Float
  promotion, Sig<A> ↔ Sig<B> recursive, Nominal by name).

- Type checker third cut: Flow connectivity (commit `5b66da6`).
  `check_flow_connectivity(module, env)` walks every Flow / Tap
  in a module, calls `infer_chain` on both sides, runs
  `types_compatible`. Unknown either side defers. Reports
  TypeErrors with `format_type`-rendered messages
  ("Sig<Float>" not Debug-noise). Bug fix: bare Path in chain
  position now returns the Fn's `returns` (implicit apply).

  Corpus baseline: **2 flow mismatches**, both in `quill.zp:326-327`
  where `opacity(0 -> 1, ...)` and `scale(0.8 -> 1, ...)` overload
  `->` for value-range semantics. Ratchet at 2. Real-by-the-rules
  mismatches; the fix is a syntactic spec decision (use `..`
  instead of `->` for ranges).

  **116 tests green**.

- Measurement spec open questions resolved (commit `f4ad27b`).
  Per-window aggregation default (5min) with per-event for chord-
  rule fallbacks and security violations; ride-along measurement
  via Layer 0 circular buffers; global opt-in toggle in v1, per-
  category in v2; 30-day local / 90-day raw / indefinite aggregate
  retention; annual key rotation; device_id + firmware_signature
  fingerprint format.

- New feature spec: `specs/OPT_IN_TEST_REPORTING.md` (commit
  `7d8c95b`). Zeos ships with a deep self-test suite that runs across
  every TRISAVERSE_STACK layer and, with explicit user opt-in, sends
  anonymized reports back to Codex Labs. P0 per
  `docs/FOUNDATIONAL_PROGRAMS.md` §11 — MDE's heterogeneous-hardware
  learning depends on real-world reports.

- Companion measurement-targets spec: `specs/MEASUREMENT_TARGETS.md`
  enumerates 10 categories of what to measure for future iterations
  (language drift, hardware MasQ, runtime chord rule, memory
  pressure, MDE routing, compat overhead, network, DX, security
  invariants, power/thermal). Mapped to the OPT_IN_TEST_REPORTING.md
  report structure. Sequenced easiest-and-highest-leverage first —
  language drift and chord-rule runtime are the first two to
  instrument.

  **96 tests green**.

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

**Type checker.** Type-shape is in. Next session writes the actual checker:

- **Real builtin signatures.** Current env has `params: [Any]` for
  every builtin — the inference picks up return types but not arg
  types. To do real arg-type checking, pre-bake actual signatures.
  Use `SEMANTIC_CONTRACTS.md` patterns where they exist; ground in
  CHAIN_CONTRACT.md for hardware-class types.

- **Resolve the `->` overload in `quill.zp:326-327`.** Two of the
  remaining flow mismatches are real semantic ambiguity:
  `opacity(0 -> 1, ...)` overloads `->` to mean "value range"
  rather than signal flow. Spec call needed: rewrite to use `..`
  for ranges (`opacity(0..1)`), or formally introduce a value-range
  meaning for `->` inside arg-expr context. Either decision drops
  the ratchet to 0.
- **IR design.** LLVM IR via `inkwell` vs custom backend. Brad has
  flagged this as still open. The chord-rule shape suggests a custom
  backend that can lower `Merge` nodes natively — LLVM has no
  primitive for "fastest-N quorum."
- **Codegen + runtime.** The native signal-chain executor that
  consumes `.zpc`. Empty `runtime/` dir.
- **The other empty `tools/` dirs** — zeos-build, zeos-pkg, zeos-vc.

The 21 synthetic `__op__` callees the parser emits (`__add__`,
`__field__`, `__paren__`, `__tuple__`, `__spread__`, `__ternary__`,
`__or__`, `__not__`, `__quorum__`, `__field_self__`,
`__from_upstream__`, `__rate__`, `__unit__`, `__union__`, `__named__`,
`__apply__`, `__index__`, `__neg__`, `__pos__`, `__mul_lift__`,
`__div_lift__`) are the semantic-layer specification: each has a
runtime contract that the type-checker / lowering passes need to
fulfil. Listing them and writing each contract is a focused next
session.

Other queued items:

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
