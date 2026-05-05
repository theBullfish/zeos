# Added Feature List

Tracking of features that have landed on `main`. Append one row per landed
change. Title says what it does. Push ID is the commit SHA.

| Date | Title | Push ID |
|------|-------|---------|
| 2026-05-04 | Code session brief + STATE.md + ADDED_FEATURES + docs/NOTES.md bootstrap so phone-Code can clone the repo and pick up exactly where Z13-Code left off | 539a3a5 |
| 2026-05-04 | tools/zplus/TOKEN_TAXONOMY.md — empirical lexical inventory of Z+ built from all 68 programs/*.zp files; every token kind cited to a real file:line, with lexer hazards and a first-test-target recommendation | 1d149fb |
| 2026-05-04 | tools/zplus v1 lexer + Cargo skeleton — Rust crate with src/lex.rs, src/bin/zplus_lex.rs CLI, 18 tests green; programs/02_log_monitor.zp tokenizes end-to-end with byte-exact round-trip | 9b506b6 |
| 2026-05-04 | tools/zplus v2 lexer — Hex / HexColor / Dimension / ByteSize / TimePast / DevNull / TemplateString / Bang token kinds + corpus-wide round-trip smoke test; 38 tests green; only 3 of 68 .zp files still produce Error tokens (chirp ↑, forge_ide \", goya_fleet ─...>) | 0c4f710 |
| 2026-05-04 | tools/zplus AST type — typed enums where the chord rule is structural (Merge is one node carrying its policy, never a DAG of edges); hand-built fixture for 02_log_monitor.zp top section + chord-rule canary test; 47 tests green | b630c5d |
| 2026-05-04 | tools/zplus v3 lexer — ↑/↓ heat operators, C-style string escapes (\" \\ \n \t \r \0), ─...> long-form Flow arrow; 68/68 corpus files now tokenize cleanly; 54 tests green | 7e008b2 |
| 2026-05-04 | tools/zplus parser skeleton — recursive descent with chord-rule coalescing; vertical merge `INPUT -> \|` lines fold to one Merge node; 7 new tests; 61 green total | 2f8ac9b |
| 2026-05-04 | tools/zplus parser end-to-end on programs/02_log_monitor.zp — BinExpr/UnaryCmp/BinOp added to AST; merge policies (All/Any/Quorum/Fastest/Within/By); 6 integration tests asserting structural properties of full-file parse; zplus-parse CLI; 73 tests green | 02c7ebf |
| 2026-05-05 | tools/zplus parser corpus coverage 2→22/68 — large additive feature batch (forks/labeled forks, arrays, unit annotations @ ident and tight 60Hz, compound units m/s, rate literal 100/m, Hex/HexColor/Dimension/ByteSize/Bang/TimePast/DevNull in chain term, unary +/-, HeatUp postfix, then keyword, field access on Call, dotted-path named args, = as named-arg sep, <- actuator binding with comma alts, multi-line chain continuation, call-named wire decls, IDENT { fork } form, stmt-level postfix Bind, pipe-eat in named args, not / bang prefix, N of M quorum, binary +/-/*/% in args) | 44cb94e |
| 2026-05-05 | tools/zplus parser corpus coverage 22→28/68 — paren expr, @Call hardware-pin, then: as named-arg label, implicit-flow fork branches, chain-level arithmetic + corpus parser_minimum_corpus_coverage ratchet test | 0a7c72a |
| 2026-05-05 | tools/zplus parser corpus coverage 28→68/68 — all 68 .zp files parse cleanly; broad additive feature batch (postfix indexing, ranges, spread, pipe-OR, ternary at chain & arg, chain-level cmp, ws-args, ws-fork, node/chain decls, tuples, bare ops as values, implicit-self field, generic <...>, Call.field, Call(args), Call{fork}, ...) | ca4f57a |
| 2026-05-05 | tools/zplus type-shape + semantic-contracts spec — src/ty.rs (Type enum + UnitTag), SEMANTIC_CONTRACTS.md enumerating each of the 28 synthetic __op__ callees with type signature and runtime contract; 9 new ty unit tests; 83 tests green | 85e49d6 |
