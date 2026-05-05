# Z+ Semantic Contracts

**The 28 synthetic `__op__` callees the parser emits, with their type signatures and runtime contracts.**

The parser encodes ~30 surface constructs as `Chain::Call` with synthetic callees (names starting and ending in `__`). This keeps the AST surface minimal — three flow operators, one Bind, one Merge, one Fork, plus literals — while preserving the source's intent in a recoverable form. This document is the spec sheet for the type checker, lowering pass, and runtime.

Each entry includes:

- **Source pattern** — the syntactic form the parser sees
- **Citation** — at least one `programs/<file>:<line>` example
- **AST** — `Chain::Call { callee: "__op__", args: [...] }`
- **Type contract** — input and output types
- **Runtime** — what the resolver does

Type variables `T`, `U` are concrete at use site; `Sig<T>` is "signal of T" (a chain-time-varying value of type T). `Bool` / `Int` / `Float` / `String` / `Path` are language primitives; `Unit(name)` is a unit-tagged numeric.

---

## Flow / wiring (these are NOT synthetic — for context)

The four real chain operators in the AST: `Flow`, `Tap`, `Sever`, `Bind`. Their type contracts live in `docs/CHAIN_CONTRACT.md`. Briefly:

- `Flow(a, b)` — `output_type(a)` must match (or convert to) `input_type(b)`. The signal flows.
- `Tap(a, b)` — read-only branch: `b` sees the same signal `a` carries, but `b`'s presence MUST NOT affect `a`'s resolution.
- `Sever(a, b)` — cuts the wire between `a` and `b`. No runtime data.
- `Bind(a, b)` — declarative binding: `a` is shaped/typed by `b`. `<-` actuator binding and `:` shape annotation both lower to this.

`Merge` and `Fork` carry chord/fastest-N policy and parallel-branch semantics respectively. They are also not synthetic.

---

## 1. Arithmetic

### `__add__`, `__sub__`, `__mul__`, `__div__`, `__mod__`

| Field | Value |
|---|---|
| Source | `a + b`, `a - b`, `a * b`, `a / b`, `a % b` |
| Example | `programs/23_supply_chain.zp:50` `on_hand + in_transit + on_order - committed -> available_to_promise(sku)` |
| AST | `Call { callee: "__add__", args: [Positional(lhs), Positional(rhs)] }` |
| Types | `T, T -> T` where `T` ∈ `{Int, Float, String (only __add__), Unit-tagged numerics with matching units}` |
| Runtime | Standard arithmetic. `__add__` on strings is concatenation. Unit-tagged values: arithmetic preserves the unit when both operands share it; mixing units is a type error. |

### `__neg__`, `__pos__`

| Field | Value |
|---|---|
| Source | `-x`, `+x` (unary chain-term prefix) |
| Example | `programs/22_precision_agriculture.zp:87` `gate(< -2σ) -> alert(warn: ...)` |
| AST | `Call { callee: "__neg__", args: [Positional(operand)] }` |
| Types | `T -> T` (numeric only) |
| Runtime | Sign flip / no-op. |

### `__mul_lift__`, `__div_lift__`

| Field | Value |
|---|---|
| Source | `* N` / `/ N` as a chain term (lifted to a chain-element). |
| Example | `programs/derez/hello_chain.zp:15` `double : input -> * 2 -> output`; `programs/quill.zp:230` `range -> reduce(+) -> / count` |
| AST | `Call { callee: "__mul_lift__", args: [Positional(operand)] }` (the `N` is the operand) |
| Types | `T -> Sig<U> -> Sig<U>` where `U` is the upstream's signal type and `T` is compatible (numeric ⊗ numeric, etc.) |
| Runtime | Construct a one-input chain element that emits `upstream * N` (or `upstream / N`) per resolve. The "lift" is structural — the literal `N` becomes a stage in the chain, not a value flowed in. |

---

## 2. Field access / indexing

### `__field__`

| Field | Value |
|---|---|
| Source | `<term>.field` postfix on any chain term. |
| Example | `programs/zeros/arm_controller.zp:51` `playback_mode -> sequence.each(delay: 50ms)` (then `.base_a`); `programs/derez/block_builder.zp` `crosshair.block.type` |
| AST | `Call { callee: "__field__", args: [Positional(receiver), Positional(Path(field_name))] }` |
| Types | `Record{...field: T...}, Path -> T`. Or `Sig<Record{...}>, Path -> Sig<T>` when receiver is a signal. |
| Runtime | Project the field at each resolution. For signal receivers, the projection re-fires each time the receiver fires. |

### `__field_self__`

| Field | Value |
|---|---|
| Source | `.field` at the start of a chain term (implicit-self). |
| Example | `programs/zeros/arm_controller.zp` `{ .base_a, .base_b, ... }` — each branch projects from the fork's upstream value. |
| AST | `Call { callee: "__field_self__", args: [Positional(Path(field_name))] }` |
| Types | `Path -> T` (the receiver is the implicit fork-branch upstream's type) |
| Runtime | Same as `__field__` but bound to the enclosing fork's input. |

### `__index__`

| Field | Value |
|---|---|
| Source | `<term>[i]` or `<term>[i, j]` postfix. |
| Example | `programs/derez/music_tracker.zp:37` `kick_pattern[step]`; `programs/shield.zp` `security_dashboard[0,0]` (2D grid) |
| AST | `Call { callee: "__index__", args: [Positional(receiver), Positional(idx0), Positional(idx1?), ...] }` |
| Types | `List<T>, Int -> T`; `Grid<T>, Int, Int -> T`; `Map<K, V>, K -> V` |
| Runtime | Index into the receiver. For signal-of-collection, re-projects per resolve. |

---

## 3. Grouping / shape

### `__paren__`

| Field | Value |
|---|---|
| Source | `(expr)` — explicit parenthesization for precedence. |
| Example | `(a + b) * c` |
| AST | `Call { callee: "__paren__", args: [Positional(inner)] }` |
| Types | `T -> T` (transparent) |
| Runtime | Identity. The wrapper exists for round-trip / source-locality, not semantics. |

### `__tuple__`

| Field | Value |
|---|---|
| Source | `(a, b, c)` — comma-separated parenthesized list. |
| Example | `programs/quill.zp:229` `"ROUND": (value, decimals) -> round(value, decimals)` (param list) |
| AST | `Call { callee: "__tuple__", args: [Positional(item0), Positional(item1), ...] }` |
| Types | `T0, T1, T2, ... -> Tuple<T0, T1, T2, ...>` |
| Runtime | Construct a positional tuple. Used as a parameter-list shape too — the lowering pass distinguishes by call context. |

### `__spread__`

| Field | Value |
|---|---|
| Source | `<term>...` postfix (three dots, no whitespace). |
| Example | `programs/quill.zp:231` `"CHAIN": (cells...) -> wire` |
| AST | `Call { callee: "__spread__", args: [Positional(operand)] }` |
| Types | `List<T> -> ...T` (variadic expansion). The `...T` form is a type-system internal — it expands at call site. |
| Runtime | Splat the operand's elements as positional args of the enclosing call. Type checker validates arity. |

---

## 4. Conditional / boolean

### `__ternary__`

| Field | Value |
|---|---|
| Source | `cond ? then : else`, both at arg-expr and chain level. |
| Example | `programs/derez/bot_trainer.zp:95` `color: sample.label == 1 ? green : red` |
| AST | `Call { callee: "__ternary__", args: [Positional(cond), Positional(then_expr), Positional(else_expr)] }` |
| Types | `Bool, T, T -> T` (the two branches' types must unify) |
| Runtime | Standard short-circuit conditional. |

### `__or__`

| Field | Value |
|---|---|
| Source | `a \| b` in arg-expr context (NOT chain context — that's `Merge`). |
| Example | `programs/competition/first_challenge.zp:106` `gate(match.state == "teleop" \| match.state == "endgame")` |
| AST | `Call { callee: "__or__", args: [Positional(lhs), Positional(rhs)] }` |
| Types | `Bool, Bool -> Bool` |
| Runtime | Short-circuit OR. Distinct from the `Merge` chord — `__or__` produces a boolean predicate; `Merge` resolves multiple incoming wires as a chord. |

### `__not__`

| Field | Value |
|---|---|
| Source | `!<expr>` (Bang prefix) or `not <expr>` keyword prefix in arg position. |
| Example | `programs/zeros/arm_controller.zp:55` `gate(!recording)`; `programs/08_cicd_pipeline.zp:13` `gate(branch: not "main")` |
| AST | `Call { callee: "__not__", args: [Positional(operand)] }` |
| Types | `Bool -> Bool` |
| Runtime | Logical negation. |

---

## 5. Call / application

### `__apply__`

| Field | Value |
|---|---|
| Source | `<term>(args)` — postfix call on any chain term (computed callees, table-of-callables). |
| Example | `programs/derez/block_builder.zp` `inventory.slot(selected_slot)` (the receiver `inventory.slot` is itself a computed expression). |
| AST | `Call { callee: "__apply__", args: [Positional(callee_expr), Positional(arg0), Positional(arg1), ...] }` |
| Types | `Fn<A, B, ... -> R>, A, B, ... -> R` |
| Runtime | Invoke the computed callee. Type checker resolves the callee's signature. |

### `__from_upstream__`

| Field | Value |
|---|---|
| Source | Implicit fork-branch `{ -> rest -> sink }` form — the `->` at the start of a branch means "from the fork's upstream." |
| Example | `programs/14_video_streaming.zp:11-17` `raw_video -> transcode { -> encode(...) -> store, -> encode(...) -> store }` |
| AST | A bare `Path::one("__from_upstream__")` Atom; appears inside `Chain::Flow(__from_upstream__, rest, _)` for each implicit-flow branch. |
| Types | The fork's upstream's output type. |
| Runtime | The chain-builder substitutes `__from_upstream__` with the fork's actual upstream wire at lowering time. |

---

## 6. Range / units

### `__range__`

| Field | Value |
|---|---|
| Source | `<lo>..<hi>` — two adjacent Dot tokens. |
| Example | `programs/multimodal/rover.zp` `map(0..1024)`; `programs/derez/forge_ide.zp` `gate(key: "ctrl+1".."ctrl+9")` |
| AST | `Call { callee: "__range__", args: [Positional(lo), Positional(hi)] }` |
| Types | `T, T -> Range<T>` where `T` ∈ `{Int, Float, Char, String}` (lexicographic for strings) |
| Runtime | Inclusive range value. Used by `map`, `gate`, etc. |

### `__rate__`

| Field | Value |
|---|---|
| Source | `<numeric>/<unit>` tight (no whitespace). |
| Example | `programs/03_http_server.zp:35` `gate(source.rate < 100/m, knee: 20/m)` |
| AST | `Call { callee: "__rate__", args: [Positional(numeric), Positional(Path(unit))] }` |
| Types | `Numeric, Path -> Rate<Unit>` |
| Runtime | Frequency literal — `100/m` is "100 per minute." |

### `__unit__`

| Field | Value |
|---|---|
| Source | `<value> @ <unit>` (with `@`) or `<numeric><unit>` (tight, like `60Hz`). The unit can be a Path (`@ percent`), a compound path (`@ m/s`), or a Call (`@ mde("model.zdx")`, `@ goya(0)`). |
| Example | `programs/16_scada_industrial.zp:25` `valve_inlet : plant.actuator(...) <- position @ percent`; `programs/19_autonomous_vehicle.zp:13` `wheel : device(...) -> speed @ m/s`; `programs/chirp.zp` `toxicity @ mde("toxic-detect.zdx")` |
| AST | `Call { callee: "__unit__", args: [Positional(value), Positional(unit_or_call)] }` |
| Types | `T, Unit -> Tagged<T, Unit>` — for path/compound forms. For Call forms (`@ mde(...)`, `@ goya(0)`), the second arg is a hardware-pin spec; output type is `T` itself with hardware-pin metadata attached. |
| Runtime | For unit tags: arithmetic preserves the unit (see `__add__`). For hardware pins: the runtime allocates the chain stage on the named device. |

### `__union__`

| Field | Value |
|---|---|
| Source | `<-` actuator-binding RHS with pipe- or comma-separated alts: `<- on \| off`, `<- brightness, color_temp`. |
| Example | `programs/11_home_automation.zp:31` `fan_bathroom : house.fan(...) <- on \| off` |
| AST | `Call { callee: "__union__", args: [Positional(option_or_more_unions)] }` (recursively wrapped) |
| Types | `T \| U \| ... -> Union<T, U, ...>` |
| Runtime | The actuator's input port accepts any of the listed alternatives; the runtime tags messages by which alt was sent. |

---

## 7. Quorum

### `__quorum__`

| Field | Value |
|---|---|
| Source | `N of M` in arg position (NOT in merge-policy position — that's first-class `MergePolicy::Quorum`). |
| Example | `programs/09_anomaly_detector.zp:50` `resonance(2 of 5, within: 5m)` |
| AST | `Call { callee: "__quorum__", args: [Positional(Int(N)), Positional(Int(M))] }` |
| Types | `Int, Int -> QuorumSpec` |
| Runtime | Resonance / convergent-evidence threshold spec. Distinct from `MergePolicy::Quorum` only in surface position — semantically identical. |

---

## 8. Heat / engagement

### `__heat_up__`, `__heat_down__`

| Field | Value |
|---|---|
| Source | `<term>↑` (U+2191) or `<term>↓` (U+2193) postfix. |
| Example | `programs/chirp.zp:47` `post ~> like(user) -> post.heat↑` |
| AST | `Call { callee: "__heat_up__", args: [Positional(operand)] }` |
| Types | `Sig<HeatGrade> -> Sig<HeatGrade>` (the upstream is a heat / grade signal) |
| Runtime | Amplifier — increments the heat grade per resolve. Logical inverse for `__heat_down__`. The corpus only uses `__heat_up__`; `__heat_down__` reserved for symmetry. |

---

## 9. Named-pair (alt-tail in pipe-OR)

### `__named__`

| Field | Value |
|---|---|
| Source | `<name>: <value>` appearing as an alternative in a pipe-OR expression. |
| Example | `programs/derez/crew_suspect.zp` `gate(all_voted \| timer_done: config.discuss_time)` |
| AST | `Call { callee: "__named__", args: [Positional(Path(name)), Positional(value)] }` |
| Types | `Path, T -> NamedPair<T>` |
| Runtime | A flagged value — the runtime treats it as "the disjunct's name is `<name>`, its value is `<value>`." Used by gates that switch on a named branch. |

---

## Type-system invariants

These are the rules the type-checker enforces over an AST that contains the synthetic ops:

1. **Flow connectivity.** For every `Flow(a, b)`, the output type of `a` must be assignable to the input type of `b`. Tap and Sever require the same.
2. **Bind asymmetry.** For `Bind(a, b)`: `a` is the receiver (actuator / wire / record); `b` is the type / shape spec. `b` does not need to "type-match" `a`'s output — instead `b` constrains what `a` accepts.
3. **Merge homogeneity.** All inputs to a `Merge` node must produce the same signal type (with possible numeric promotion). The `Merge`'s output type is the input type, modulated by the policy (e.g. `Quorum` produces `Sig<T>` if at least N inputs converge).
4. **Fork divergence.** `Fork`'s upstream feeds every branch. Each branch's input type is the upstream's output type. The fork itself does not have a single output — each branch terminates independently.
5. **Chord rule.** `Merge` MUST resolve atomically per its policy. The type-checker rejects any lowering that serializes a `Merge`'s inputs.
6. **Unit conformance.** Arithmetic on unit-tagged values must match units; otherwise the type-checker rejects with a unit-mismatch error.
7. **Hardware pin honors capability.** `__unit__` with a Call second-arg (`@ mde("...")`, `@ goya(0)`) requires the named hardware to satisfy the chain stage's signal contract from `docs/CHAIN_CONTRACT.md`.

---

## Where the synthetic ops live in the parser

| Op | parser file:line approximate | trigger |
|---|---|---|
| `__add__` / `__sub__` / `__mul__` / `__div__` / `__mod__` | parse_chain arith loop | `Plus`/`Minus`/`Star`/`Slash`/`Percent` after a chain term |
| `__neg__` / `__pos__` / `__mul_lift__` / `__div_lift__` | parse_chain_term unary prefix | `-` / `+` / `*` / `/` at chain-term start |
| `__field__` | maybe_unit_annotate | `.IDENT` postfix |
| `__field_self__` | parse_chain_term | `.IDENT` at chain-term start |
| `__index__` | maybe_unit_annotate | `[idx, ...]` postfix |
| `__paren__` / `__tuple__` | parse_chain_term LParen | `(expr)` / `(a, b, c)` |
| `__spread__` | maybe_unit_annotate | `...` postfix |
| `__ternary__` | parse_arg_expr / parse_chain tail | `? <then> : <else>` |
| `__or__` | parse_arg_expr pipe-OR loop | `\| <alt>` in arg context |
| `__not__` | parse_arg parse_arg_expr | `!` or `not` prefix |
| `__apply__` | maybe_unit_annotate | `(args)` postfix on a non-Path term |
| `__from_upstream__` | parse_fork_branch | leading `->` in a fork branch |
| `__range__` | maybe_unit_annotate | `..` between two literals |
| `__rate__` | maybe_unit_annotate | `<numeric>/<unit>` tight |
| `__unit__` | maybe_unit_annotate | `@ <unit>` postfix |
| `__union__` | parse_chain BindLeft | trailing `\| IDENT` / `, IDENT` after `<-` |
| `__quorum__` | parse_arg | `N of M` at arg start |
| `__heat_up__` / `__heat_down__` | maybe_unit_annotate | `↑` / `↓` postfix |
| `__named__` | parse_arg_expr pipe-OR loop | `<name>:<value>` in OR alt |

---

*Built 2026-05-05 from tools/zplus/src/parse.rs. Each contract is a non-binding sketch — the type-checker pass will refine them as the type system is implemented.*
