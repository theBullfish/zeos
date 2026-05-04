# Added Feature List

Tracking of features that have landed on `main`. Append one row per landed
change. Title says what it does. Push ID is the commit SHA.

| Date | Title | Push ID |
|------|-------|---------|
| 2026-05-04 | Code session brief + STATE.md + ADDED_FEATURES + docs/NOTES.md bootstrap so phone-Code can clone the repo and pick up exactly where Z13-Code left off | 539a3a5 |
| 2026-05-04 | tools/zplus/TOKEN_TAXONOMY.md — empirical lexical inventory of Z+ built from all 68 programs/*.zp files; every token kind cited to a real file:line, with lexer hazards and a first-test-target recommendation | 1d149fb |
| 2026-05-04 | tools/zplus v1 lexer + Cargo skeleton — Rust crate with src/lex.rs, src/bin/zplus_lex.rs CLI, 18 tests green; programs/02_log_monitor.zp tokenizes end-to-end with byte-exact round-trip | 9b506b6 |
| 2026-05-04 | tools/zplus v2 lexer — Hex / HexColor / Dimension / ByteSize / TimePast / DevNull / TemplateString / Bang token kinds + corpus-wide round-trip smoke test; 38 tests green; only 3 of 68 .zp files still produce Error tokens (chirp ↑, forge_ide \", goya_fleet ─...>) | 0c4f710 |
