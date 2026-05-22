# Zeos Working Notes

Append-only log of non-obvious things discovered during work — constraints,
workarounds, gotchas, decisions made and their rationale. Date every entry.
Future-self forgets these.

Newest at the bottom. Never rewrite previous entries — append a dated
correction if needed.

---

## 2026-05-04 — bootstrap

- `tools/zplus/` is empty as of this commit. Z+ compiler is the chosen
  P0 BUILD entry point because zeos-build, runtime libraries, and signal
  chain definitions all eventually compile through Z+.
- Z+ work to date lives elsewhere on Z13 (Brad's notes mention 13+
  programs and a `FINDINGS.md`). Confirm whether to import that work or
  start fresh before writing the lexer.
- Doctrine docs (`docs/SIGNAL_LOGIC.md`, `docs/CHAIN_CONTRACT.md`,
  `docs/COMPONENT_AS_MODULE.md`, `docs/TRISAVERSE_STACK.md`) are
  append-only — never rewrite sections, append dated revisions.

## 2026-05-04 — Z+ lexical survey gotchas (for future lexer work)

Discoveries from the corpus survey that aren't in `programs/FINDINGS.md`
and would bite a naive lexer implementation. All cited in
`tools/zplus/TOKEN_TAXONOMY.md`; this note is the short version.

- **`<-` is NOT a stray operator.** It's the **actuator-binding** arrow:
  `valve : plant.actuator("inlet") <- position @ percent`. Used
  consistently in `programs/16_scada_industrial.zp:25-30`,
  `programs/11_home_automation.zp:27-33`,
  `programs/22_precision_agriculture.zp:105`. `FINDINGS.md` flagged it
  as ambiguous — the corpus answers: it declares what the bound device
  receives.
- **`<->` is not used.** Only proposed in a comment at
  `programs/03_http_server.zp:110`. Don't reserve it yet.
- **Number+suffix tokens must be one lexeme.** `30d`, `5x`, `2σ`, `10%`,
  `2MB`, `1920x1080`, `0x68`. The trailing letters/glyphs are part of
  the token. If the lexer emits `INT IDENT` for `30d`, every duration in
  every program parses wrong.
- **`t-1` and `t-2` are single tokens** in temporal contexts (game
  server, anomaly detector). The `-` is part of the lexeme, not
  subtraction. Lexer rule: `t` immediately followed by `-` and a digit,
  no whitespace, → `TIME_PAST`.
- **`/dev/null` is one token.** Used as a discard sink at
  `programs/chirp.zp:122`, `programs/10_game_server.zp:77`. Don't lex as
  `SLASH IDENT SLASH IDENT`.
- **Strings interpolate.** `"recordings/{timestamp}.mp4"` (zindex_studio),
  `"{name}.pdf"` (quill). Lex as `TEMPLATE_STRING` with hole spans;
  parser substitutes.
- **Unicode glyphs in source.** `σ` (U+03C3) as a numeric suffix unit
  for standard deviations — used in 11 programs. `↑` (U+2191) as a
  postfix heat-amplifier in `programs/chirp.zp:47`. Source files are
  UTF-8.
- **`?` ternary IS used.** `cond ? then : else` —
  `programs/derez/bot_trainer.zp:95`,
  `programs/zindex_studio.zp:334-335`. Not "rare" enough to skip.
- **Three operators are overloaded by parse context, not by lexeme:**
  `~` (fuzzy match vs range), `@` (device pin vs unit annotation), `|`
  (wire merge vs type union vs option set). Lex one token each; the
  parser decides what they mean from surroundings.
- **Block comments `/* */` are absent in all 68 programs.** Don't
  implement until a real program needs them.
- **No `&&` / `||` / `!` anywhere.** Boolean composition uses
  `any` / `all` / `not` keywords.
