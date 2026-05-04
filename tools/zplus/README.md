# zplus

Z+ language frontend — bootstrap compiler.

## Status

**v2 lexer landed.** Token surface covers v1 + Hex (`0x68`), HexColor
(`#29ADFF`), Dimension (`1920x1080`), ByteSize (`200KB`), TimePast
(`t-1`), DevNull (`/dev/null`), TemplateString (`"…{name}…"`), and Bang
(`!`). Corpus-wide round-trip clean across all 68 .zp files; only 3
files still produce Error tokens (chirp's `↑`, forge_ide's string
escapes, goya_fleet's `─...>` long-form arrow — see TOKEN_TAXONOMY.md
§13.1).

**AST type landed (no parser yet).** `src/ast.rs` defines the typed
enum surface. The chord rule from `docs/SIGNAL_LOGIC.md` §1 is
structural: a `|` merge is **one** `Merge` node carrying its policy
(All / Any / Quorum / Fastest / Within / By), never a DAG of
independent edges. Hand-built fixture for the top section of
`programs/02_log_monitor.zp` lives in `tests/ast_log_monitor.rs` with
a chord-rule canary test that fails first on any future lowering
mistake. 47 tests green.

## Layout

```
tools/zplus/
├── Cargo.toml
├── README.md
├── TOKEN_TAXONOMY.md       # the lexer spec (empirical, from 68 .zp programs)
├── src/
│   ├── lib.rs              # crate entry — re-exports lex + ast modules
│   ├── lex.rs              # the lexer
│   ├── ast.rs              # typed AST — chord rule documented at top
│   └── bin/zplus_lex.rs    # CLI: dumps token stream for a file
└── tests/
    ├── log_monitor.rs      # green-light: programs/02_log_monitor.zp tokens
    ├── http_server.rs      # second fixture: programs/03_http_server.zp tokens
    ├── corpus.rs           # corpus-wide smoke: round-trip + error-count allowlist
    └── ast_log_monitor.rs  # hand-built AST for 02_log_monitor.zp top section
```

## Build & test

```
cd tools/zplus
cargo test
```

## Try the CLI

```
cd tools/zplus
cargo run --bin zplus-lex -- ../../programs/02_log_monitor.zp
```

## What's in v2

Token kinds covered by the v2 lexer:

- Trivia: `Whitespace`, `Newline`, `LineComment`
- Identifiers: ASCII `[a-zA-Z_][a-zA-Z0-9_]*`
- Numerics: `Int`, `Float`, `Hex` (`0x68`), `Duration` (ms/s/m/h/d),
  `Ratio` (`5x`), `Dimension` (`1920x1080`), `ByteSize` (`200KB`),
  `Sigma` (`2σ`), `PercentLit` (`10%`), `HexColor` (`#29ADFF` / `#444`)
- Strings: `String` (`"..."`), `TemplateString` (`"...{name}..."`)
- Sentinels: `DevNull` (`/dev/null`), `TimePast` (`t-1`, `t-2`)
- Arrows: `->`, `~>`, `-x>`, `<-`, `<->`
- Comparisons: `<`, `>`, `<=`, `>=`, `==`, `!=`
- Punctuation: `()`, `{}`, `[]`, `:`, `,`, `.`, `@`, `?`, `|`, `~`, `=`
- Arithmetic / boolean: `+`, `-`, `*`, `/`, `%`, `!`

The numeric suffix dispatch implements a **boundary check**: a suffix
character only counts if the byte after it is not an
identifier-continuation char. Without this, `0xZ` lexed as
`Ratio(0x), Ident(Z)` instead of `Int(0), Ident(xZ)`.

Deferred to v3 (corpus discoveries that need a spec decision —
TOKEN_TAXONOMY.md §13.1):

- `↑` heat operator — `programs/chirp.zp:47`
- `\"` string escapes — `programs/derez/forge_ide.zp:261`
- `─...>` long-form Flow arrow — `programs/goya_fleet.zp:217`

## Design

See `TOKEN_TAXONOMY.md` for the empirical token surface. The lexer's
arrow / number-suffix dispatch order is dictated by §14.
