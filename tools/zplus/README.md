# zplus

Z+ language frontend — bootstrap compiler.

## Status

**v1 lexer landed.** Tokenizes `programs/02_log_monitor.zp` end-to-end with
round-trip equality. Parser, AST, IR not started.

## Layout

```
tools/zplus/
├── Cargo.toml
├── README.md
├── TOKEN_TAXONOMY.md      # the lexer spec (empirical, from 68 .zp programs)
├── src/
│   ├── lib.rs             # crate entry — re-exports lex API
│   ├── lex.rs             # the lexer
│   └── bin/zplus_lex.rs   # CLI: dumps token stream for a file
└── tests/
    └── log_monitor.rs     # green-light integration test
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

## What's in v1

Token kinds covered by the v1 lexer:

- Trivia: `Whitespace`, `Newline`, `LineComment`
- Identifiers: ASCII `[a-zA-Z_][a-zA-Z0-9_]*`
- Literals: `Int`, `Float`, `Duration` (ms/s/m/h/d), `Ratio` (`5x`),
  `Sigma` (`2σ`), `PercentLit` (`10%`), `String`
- Arrows: `->`, `~>`, `-x>`, `<-`, `<->`
- Comparisons: `<`, `>`, `<=`, `>=`, `==`, `!=`
- Punctuation: `()`, `{}`, `[]`, `:`, `,`, `.`, `@`, `?`, `|`, `~`, `=`
- Arithmetic: `+`, `-`, `*`, `/`, `%`

Defers v2 (not exercised by `02_log_monitor.zp`):

- `Hex` (`0x68`), `HexColor` (`#29ADFF`), `Dimension` (`1920x1080`),
  `ByteSize` (`200KB`)
- `t-1` / `t-2` temporal-past tokens
- `/dev/null` as a single token
- Template-string interpolation `"…{name}…"`
- `↑` / `↓` heat operators

These have unit tests waiting to be written; see `TOKEN_TAXONOMY.md`
§13–§14 for the precise rules.

## Design

See `TOKEN_TAXONOMY.md` for the empirical token surface. The lexer's
arrow / number-suffix dispatch order is dictated by §14.
