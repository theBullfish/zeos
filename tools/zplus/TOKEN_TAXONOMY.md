# Z+ Token Taxonomy

**Lexical-level inventory of every token kind observed in the 68-program corpus
under `programs/*.zp`.** Built before the lexer so the lexer has a target.

This is an **empirical** taxonomy: every category is grounded in at least one
real-program file:line citation. The corpus is the source of truth; this doc
just enumerates what's there. Things called out in `programs/FINDINGS.md` as
"proposed but not yet used" are listed in §13 and **not** considered part of
the lexer's required surface.

Frequency tiers:
- **ubiquitous** — appears in nearly every program
- **common** — appears across many programs / multiple domains
- **rare** — one or two occurrences, may be domain-specific

---

## 1. Flow operators (the spine)

| Token | Lexeme | Example | Tier | Notes |
|---|---|---|---|---|
| `FLOW` | `->` | `programs/02_log_monitor.zp:8` | ubiquitous | the wire |
| `TAP` | `~>` | `programs/02_log_monitor.zp:67` | ubiquitous | read-only telemetry |
| `SEVER` | `-x>` | `programs/chirp.zp:31` | rare | unfollow / disconnect |
| `BIND_LEFT` | `<-` | `programs/16_scada_industrial.zp:25` | common | **actuator input binding**, e.g. `valve_inlet : plant.actuator(...) <- position @ percent`. Used in `11_home_automation.zp:27-33`, `22_precision_agriculture.zp:105`, `16_scada_industrial.zp:25-30`. |

> `<->` (bidirectional) is **proposed only** in a comment at
> `programs/03_http_server.zp:110`. Not used in any program. See §13.

## 2. Merge & grouping

| Token | Lexeme | Example | Tier | Notes |
|---|---|---|---|---|
| `MERGE` | `\|` | `programs/02_log_monitor.zp:13` | ubiquitous | Also the union operator inside types (e.g. `error \| warn \| info`) and inside option lists (`on \| off`). Disambiguation deferred to parse context. |
| `LBRACE` | `{` | `programs/01_file_watcher.zp:12` | ubiquitous | fork / record / block |
| `RBRACE` | `}` | `programs/01_file_watcher.zp:15` | ubiquitous | |

## 3. Punctuation

| Token | Lexeme | Example | Tier |
|---|---|---|---|
| `LPAREN` | `(` | everywhere | ubiquitous |
| `RPAREN` | `)` | everywhere | ubiquitous |
| `LBRACK` | `[` | `programs/shield.zp:59` | rare |
| `RBRACK` | `]` | `programs/shield.zp:59` | rare |
| `COLON` | `:` | `programs/02_log_monitor.zp:8` | ubiquitous |
| `COMMA` | `,` | `programs/02_log_monitor.zp:20` | ubiquitous |
| `DOT` | `.` | `programs/chirp.zp:8` | ubiquitous |
| `AT` | `@` | `programs/16_scada_industrial.zp:25` | common |
| `QUESTION` | `?` | `programs/derez/bot_trainer.zp:95` | rare |
| `SEMI` | `;` | _not used in code; only in `//` comments_ | — |

`?` participates in **ternary expressions**: `cond ? then_expr : else_expr` —
`programs/derez/bot_trainer.zp:95`, `programs/zindex_studio.zp:334-335`.

`@` has two roles:
- **device pinning** in MDE chains: `... @ mde("toxic-detect.zdx")` — `programs/chirp.zp:107`
- **unit annotation**: `position @ percent`, `rate @ gal_acre`, `f64 @ cents` — `programs/16_scada_industrial.zp:25`, `programs/22_precision_agriculture.zp:105`, `programs/15_trading_system.zp` schema lines

## 4. Comparison operators

| Token | Lexeme | Example | Tier |
|---|---|---|---|
| `LT` | `<` | `programs/02_log_monitor.zp:43` | ubiquitous |
| `GT` | `>` | `programs/02_log_monitor.zp:43` | ubiquitous |
| `LE` | `<=` | `programs/15_trading_system.zp` | common |
| `GE` | `>=` | `programs/15_trading_system.zp:75` | common |
| `EQ` | `==` | `programs/15_trading_system.zp:119` | common |
| `NE` | `!=` | corpus | rare |

## 5. Arithmetic operators

| Token | Lexeme | Example | Tier |
|---|---|---|---|
| `PLUS` | `+` | `programs/23_supply_chain.zp:26` (string concat in alert) | common |
| `MINUS` | `-` | `programs/15_trading_system.zp:43` (numeric) | common |
| `STAR` | `*` | `programs/derez/hello_chain.zp:15` | common |
| `SLASH` | `/` | path separator inside string literals; rare bare | rare |
| `PERCENT` | `%` | corpus | rare |

> Boolean `&&` / `||` / `!` are **not present** in any program. Boolean logic
> is expressed via keywords (`any`, `all`, `not`) and via gate composition.

## 6. Range / fuzzy

| Token | Lexeme | Example | Tier | Notes |
|---|---|---|---|---|
| `TILDE` | `~` | `programs/02_log_monitor.zp:35` (fuzzy), `programs/18_patient_monitor.zp:12` (range `0 ~ 4`) | common | Two parse-context meanings: substring/fuzzy match in gate body, range between two numerics. |

## 7. Heat operators (Unicode)

| Token | Lexeme | Example | Tier | Notes |
|---|---|---|---|---|
| `HEAT_UP` | `↑` (U+2191) | `programs/chirp.zp:47` (`post.heat↑`) | rare | engagement amplifier |
| `HEAT_DOWN` | `↓` (U+2193) | _none in current corpus_ | — | implied symmetric pair |

## 8. Literals

### 8.1 Numeric

| Token | Pattern | Example | Tier |
|---|---|---|---|
| `INT` | `[0-9]+` | `programs/chirp.zp:151` (`top(10)`) | ubiquitous |
| `FLOAT` | `[0-9]+\.[0-9]+` | `programs/chirp.zp:62` (`0.3`) | ubiquitous |
| `HEX` | `0x[0-9a-fA-F]+` | `programs/multimodal/rover.zp:13` (`0x68`) | rare (mostly I²C addresses) |

### 8.2 Tagged numeric (number + unit suffix as a single token)

These are **lexed as one token**, not as `INT IDENT`. Without that, the lexer
will produce wrong tokens for `30d`, `5x`, `2σ`, `200KB`, etc.

| Token | Pattern | Example | Tier |
|---|---|---|---|
| `DURATION` | `[0-9]+(ms\|s\|m\|h\|d)` | `programs/02_log_monitor.zp:62` (`30d`), `programs/07_chat_system.zp:66` (`300ms`) | ubiquitous |
| `RATIO` | `[0-9]+(\.[0-9]+)?x` | `programs/02_log_monitor.zp:43` (`5x`, `2x`) | common |
| `PERCENT_LIT` | `[0-9]+(\.[0-9]+)?%` | `programs/09_anomaly_detector.zp:63` (`10%`) | common |
| `SIGMA` | `[+-]?[0-9]+(\.[0-9]+)?σ` | `programs/09_anomaly_detector.zp:38` (`2σ`, `1σ`, `-2σ`) | common |
| `BYTESIZE` | `[0-9]+(KB\|MB\|GB)` | `programs/20_power_grid.zp` (`2MB`, `200KB`) | rare |
| `DIMENSION` | `[0-9]+x[0-9]+` | `programs/zindex_studio.zp:11` (`1920x1080`) | rare |

> **Lexer hazard:** `RATIO` (`5x`), `DIMENSION` (`1920x1080`), and `HEX`
> (`0x68`) all share the letter `x`. Decision rule: leading `0x` followed by
> hex digits → `HEX`; `<int>x<int>` → `DIMENSION`; `<int>x` (end-of-token) →
> `RATIO`. Order matters in the lexer.

### 8.3 Strings

| Token | Pattern | Example | Tier | Notes |
|---|---|---|---|---|
| `STRING` | `"[^"]*"` | `programs/02_log_monitor.zp:35` | ubiquitous | double-quoted only |
| `TEMPLATE_STRING` | `"...{ident}..."` | `programs/quill.zp:140` (`"{name}.pdf"`), `programs/zindex_studio.zp:240` (`"recordings/{timestamp}.mp4"`) | common | interpolation hole `{name}` inside string |

> Single-quoted strings appear once, inside a template interpolation in
> `programs/derez/bot_trainer.zp:80`. Treat as ambiguous until resolved (§13).
> Strings may contain glob patterns (`"*.log"`, `"~/.surf/cache/**"`) — those
> are string contents, not separate tokens.

### 8.4 Color literals

| Token | Pattern | Example | Tier |
|---|---|---|---|
| `HEX_COLOR` | `#[0-9a-fA-F]{6,8}` | `programs/zindex_studio.zp:334` (`#29ADFF`) | common in UI programs |

> `#` is **only** the start of a hex color. It is not a comment, not a
> shebang, not a preprocessor directive in any program.

### 8.5 Sentinel sinks / paths

| Token | Lexeme | Example | Tier | Notes |
|---|---|---|---|---|
| `DEV_NULL` | `/dev/null` | `programs/chirp.zp:122`, `programs/10_game_server.zp:77` | rare | the discard sink. Lex as a single keyword/path-literal — it's not a string and not three tokens. |

## 9. Temporal access

| Token | Lexeme | Example | Tier | Notes |
|---|---|---|---|---|
| `TIME_NOW` | `t` | corpus | common | bare identifier representing "now" in temporal expressions |
| `TIME_PAST` | `t-1`, `t-2`, … | `programs/10_game_server.zp` | rare | **lex as one token**, not `IDENT MINUS INT`. The minus inside is part of the lexeme, not subtraction. |

## 10. Comments

| Token | Pattern | Example | Tier | Notes |
|---|---|---|---|---|
| `LINE_COMMENT` | `// ... \n` | every program | ubiquitous | |
| `SECTION_DIVIDER` | `// ── ... ──` | `programs/02_log_monitor.zp:6` | common | decorative; lexer treats as ordinary line comment |
| `BLOCK_COMMENT` | `/* */` | _not used in any program_ | — | spec decision: omit from v1 lexer |

## 11. Identifiers & keywords

Identifiers match `[a-zA-Z_][a-zA-Z0-9_]*`. Most identifiers are bare names;
the language is keyword-light because most "verbs" (gate, knee, delta, …) act
syntactically like callable identifiers — they're parsed contextually rather
than reserved. The lexer can emit them all as `IDENT` and let the parser
disambiguate, except for the few control-shape keywords below.

### 11.1 Control-shape keywords (recommend reserving)

These appear in positions where a bare `IDENT` would be ambiguous:

`gate`, `knee`, `delta`, `baseline`, `deviation`, `decay`, `rate`,
`resonance`, `on_silence`, `sustained`, `vault`, `parse`, `merge`, `weighted`,
`mode`, `not`, `then`, `within`, `where`, `otherwise`.

### 11.2 Contextual keywords (lex as IDENT, recognize in parser)

Argument-position names: `by`, `per`, `from`, `to`, `for`, `of`, `any`,
`all`, `window`, `until`, `interval`, `half_life`, `sustained`, `sort`,
`group`, `count`, `top`, `last`, `take`.

Action / sink names: `accept`, `drop`, `respond`, `abort`, `disconnect`,
`hold`, `pass`, `alert`, `emit`, `print`, `exec`, `notify`, `flag`.

Source builders: `fs`, `net`, `device`, `mesh`, `tick`, `time`, `sense`,
`zixel`.

Mode tags: `chronological`, `heated`, `quiet`, `reflex`, `deliberate`.

Type-ish names (in schemas only, e.g. `15_trading_system.zp`): `string`,
`f64`, `bool`, `required`, `optional`.

Identity / scope: `me`, `self`.

State tags: `forever`, `now`, `open`, `closed`, `valid`, `invalid`,
`distinct`, `outside`, `matches`, `mentions`, `contains`.

> Treating §11.2 as **contextual** (lex → IDENT, parser decides) keeps the
> reserved-word list small and matches how the corpus uses these names —
> they're often passed as bare argument values (`level: error`,
> `mode(chronological)`).

### 11.3 Type-union value names

In annotation positions like `level : error | warn | info | debug`
(`programs/02_log_monitor.zp:21`) or `<- on | off`
(`programs/11_home_automation.zp:31`), the right-hand identifiers are bare
values, not strings. Lex as `IDENT`; parser handles the `|`-union shape.

## 12. Whitespace & layout

| Token | Notes |
|---|---|
| spaces / tabs | not significant; used for alignment |
| newline | **probably significant** — most chains terminate at newline. The corpus uses one chain per line as the dominant form. Confirm with parser; for now lex `NEWLINE` as a token and decide at parse time. |
| indentation | not significant. Nesting is via `{ … }` and parens, not indent. |

## 13. Proposed-only / not in corpus

These are mentioned in `docs/` or in code comments as future syntax but **do
not appear in any program**. The v1 lexer can ignore them; flag for spec
discussion before reserving.

| Item | Source of proposal |
|---|---|
| `<->` bidirectional | comment at `programs/03_http_server.zp:110` |
| `'...'` single-quoted strings | one occurrence inside template interpolation `programs/derez/bot_trainer.zp:80` — unclear whether this is real syntax or noise |
| `&&` `\|\|` `!` boolean ops | absent everywhere |
| `null` / `none` | not observed as a literal |
| `true` / `false` literals | observed as bare `true` in `programs/chirp.zp:131` (`no_tap: true`); `false` not yet seen — lex as IDENT for now |
| Block comments `/* */` | absent everywhere |

## 14. Lexer hazards (read this before writing the scanner)

1. **Multi-char arrows.** Match in this order: `-x>`, `->`, `<->`, `<-`,
   `~>`, `~`, `-`, `<`, `>`. Otherwise `-x>` lexes as `MINUS IDENT GT`.
2. **Number-with-suffix tokens.** Match `DURATION`, `SIGMA`, `RATIO`,
   `PERCENT_LIT`, `BYTESIZE`, `DIMENSION`, `HEX_COLOR` **before** falling
   back to `INT` / `FLOAT`. The trailing letters/glyphs are part of the
   token, not the next identifier.
3. **`t-1` / `t-2`.** When the lexer sees identifier `t` followed
   immediately (no whitespace) by `-` and a digit, emit one `TIME_PAST`
   token. Otherwise `t - 1` is `IDENT MINUS INT`.
4. **`/dev/null`.** Recognize as a single token; do not emit `SLASH IDENT
   SLASH IDENT`.
5. **Unicode.** UTF-8 source. `σ` (U+03C3) appears as a numeric suffix.
   `↑` / `↓` (U+2191 / U+2193) appear as standalone postfix operators.
   Other identifier characters are ASCII.
6. **String interpolation.** A `"…{ident}…"` may contain `{ident}` holes.
   For v1, lex the whole thing as `TEMPLATE_STRING` with a list of hole
   spans; the parser substitutes. Do not try to lex inside the string.
7. **Glob and path content inside strings.** `"*.log"`, `"/api/*"`,
   `"~/.surf/cache/**"` are string contents only — no special tokens.
8. **`|` is overloaded.** Wire merge (`a -> | -> b`), type union
   (`error | warn`), gate-arg union (`gate(command: any("set", "del"))`
   uses `any()` rather than `|`, but `<- on | off` does use `|` directly).
   Lex one `MERGE` token; parser disambiguates.
9. **`~` is overloaded.** Fuzzy match (`message ~ "pattern"`) and range
   (`0.6 ~ 0.9`). Lex one `TILDE`; parser disambiguates.
10. **`@` is overloaded.** Device pin (`@ mde(...)`) and unit annotation
    (`@ percent`, `@ cents`). Lex one `AT`; parser disambiguates.

## 15. Open questions for the parser/spec, not the lexer

- Is `NEWLINE` a statement terminator, or just whitespace?
- How does precedence work between `->`, `~>`, `|`, `{}` ?
- Are gate bodies `gate(<expr>)` parsed as arbitrary expressions or as a
  small DSL of `<key>:<value>` pairs?
- Is `t-1` only valid in temporal-aware constructs, or anywhere?
- Single-quoted string status (see §13).

## 16. What the lexer's first test target should be

`programs/02_log_monitor.zp` (45 lines) is the smallest classic
source→filter→sink chain. It exercises:

- `FLOW`, `TAP`, `MERGE`, `LBRACE`/`RBRACE`
- `STRING`, `DURATION`, `RATIO`, `INT`
- `LINE_COMMENT`, `SECTION_DIVIDER`
- `gate`, `delta`, `rate`, `vault.*`, `on_silence`, `sustained`,
  `parse`, `within`
- `~` (fuzzy), `>`, `(`, `)`, `:`, `,`, `.`

If the lexer cleanly tokenizes that file end-to-end and round-trips back
to source, that's the green-light moment for moving on to the parser.

---

*Built 2026-05-04 against 68 .zp files. Update this doc when new programs
introduce new lexical surface.*
