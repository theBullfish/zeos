# zplus

Z+ language frontend — bootstrap compiler.

## Status

**Lexer + Parser corpus-complete.** All 68 .zp files in `programs/`
tokenize cleanly AND parse without error. Ratchet test
`parser_minimum_corpus_coverage` is locked at 68.

**AST + Type-shape designed.** `src/ast.rs` and `src/ty.rs` define the
typed surface — chord rule structural, `Merge` is one node carrying
its policy. Hand-built AST fixture for `02_log_monitor.zp` plus a
chord-rule canary test. `SEMANTIC_CONTRACTS.md` enumerates each of the
28 synthetic `__op__` callees the parser emits with a type signature
and runtime contract.

**Type checker landed.** Three independent ratchets all at zero across
the full corpus: merge arity (chord rule), Flow connectivity
(`output(a) ↔ input(b)`), named-arg type checking. Peer-through into
predicate wrappers so `on_silence(within: > 5m)` checks against the
underlying Duration. **126 tests green**.

**`zplus check` CLI** renders type errors with file:line:col header,
the source line, and a carat under the span:

```
/tmp/bad.zp:1:22: error: named arg `per` of `rate` expects duration, got Sig<String>
  | errors : raw -> rate(per: "1 minute") -> alerts
  |                      ^^^^^^^^^^^^^^^
```

**Runtime v1 landed.** Tree-walking interpreter executes Z+ programs
end-to-end. Smallest demo:

```
heartbeat : tick(rate: 1) -> print
```

```
$ zplus-run heartbeat.zp 5
[t=1] print: tick(1)
[t=2] print: tick(2)
[t=3] print: tick(3)
[t=4] print: tick(4)
[t=5] print: tick(5)
```

The runtime resolves Merge per its policy (chord rule on a single
thread), Tap is read-only, sinks (`print`, `alert`, `vault.store`)
emit captured records. Synthetic ops (`__add__`, `__neg__`, `__paren__`)
evaluate per parser contract. Unknown calls act as identity. **No
hardware, no MDE, no LLVM** — just proves the semantics in software.

**IR, codegen: not started.** The runtime is sufficient to demo and
test; an IR comes in when we want to compile to native or ship `.zpc`
bytecode.

## Layout

```
tools/zplus/
├── Cargo.toml
├── README.md
├── TOKEN_TAXONOMY.md          # the lexer spec (empirical, from 68 .zp programs)
├── SEMANTIC_CONTRACTS.md      # 28 synthetic __op__ callees + type/runtime contracts
├── src/
│   ├── lib.rs                 # crate entry — re-exports lex / parse / ast / ty
│   ├── lex.rs                 # the lexer
│   ├── parse.rs               # the parser (recursive descent, chord-rule coalescing)
│   ├── ast.rs                 # typed AST — chord rule documented at top
│   ├── ty.rs                  # the type-shape enum (no checker yet)
│   ├── bin/zplus_lex.rs       # CLI: dumps token stream
│   ├── bin/zplus_parse.rs     # CLI: parses + prints stmt count
│   ├── bin/zplus_check.rs     # CLI: parse + 3 checker passes + source-context errors
│   └── bin/zplus_run.rs       # CLI: parse → AST → Runtime → emit values
└── tests/
    ├── log_monitor.rs         # lexer fixture: programs/02_log_monitor.zp
    ├── http_server.rs         # lexer fixture: programs/03_http_server.zp
    ├── corpus.rs              # corpus-wide: round-trip + lex/parse coverage
    ├── ast_log_monitor.rs     # hand-built AST + chord-rule canary
    └── parse_log_monitor.rs   # full-file parse fixture
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
