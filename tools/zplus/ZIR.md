# ZIR — the Z+ Interchange Representation (v1)

**Status:** canonical convergence format, introduced 2026-07-07 to end the Z+
engine fragmentation (three parsers, no shared target). Authored under BIBLE v1.0.

## Why this exists

Before ZIR there were **three separate Z+ brains** that did not connect:

1. **`tools/zplus` (Rust)** — the strongest lexer/parser/typechecker (~180 tests),
   but a dead-end tree-walking interpreter with **no codegen**. It could never
   produce anything the OS runs.
2. **`tools/zplus` (C)** — an AST→**SFG** (Signal Flow Graph) transpiler that emits
   C. Its `sfg.h` already says the truth: *"A Z+ program IS a signal flow graph …
   maps 1:1 to kernel `chain_create`/`chain_add_node` calls."*
3. **`kernel/boot/zplus.c`** — the OS's own, **weaker** re-parser (32-node cap)
   feeding the live kernel signal-chain engine, plus a rich stdlib verb set.

Each re-parsed Z+ from source. Fixing the language meant fixing it in three
places, and the best front-end (Rust) couldn't reach the one back-end that
matters (the kernel).

## The decision

**The Signal Flow Graph is the single model. ZIR is its serialization.**
Front-ends *emit* ZIR; back-ends *consume* it. No back-end re-parses `.zp` source.

```
                 ┌─────────────┐
   .zp  ───────▶ │  Rust FE    │ ─┐
                 │ (canonical) │  │
                 └─────────────┘  │        ┌──────────────┐
                 ┌─────────────┐  ├─ ZIR ─▶ │ kernel loader│ ─▶ signal-chain engine
   .zp  ───────▶ │   C FE      │ ─┘  JSON   │ (zplus_zir.c)│    (runs on Zeos)
                 │ (transpiler)│         └──────────────┘
                 └─────────────┘         ┌──────────────┐
                                    └───▶ │ browser / C  │ ─▶ playground
                                          └──────────────┘
```

This is a front-end/back-end split, **explicitly chosen** — not an accident of
three codebases. Any front-end that can lower to the SFG participates; any
runtime that can load ZIR runs Z+ without owning a parser.

## The format

ZIR is JSON (front-ends have no serde dependency; both hand-write it, the kernel
hand-reads it — no third-party parser on any side). One document per program.

```jsonc
{
  "zir": 1,                       // format version
  "source": "02_log_monitor.zp",  // origin filename (diagnostics only)
  "chains": [
    { "id": 0, "name": "main", "masq": "reference", "parent": -1,
      "nodes": [0,1,2] }          // node ids in resolve order
  ],
  "nodes": [
    { "id": 0, "chain": 0, "seq": 0,
      "kind": "source",           // source|processor|gate|fork|merge|tap|sink
      "verb": "emit",             // the concrete operation (maps to a kernel verb)
      "name": "emit_0",
      "sig_in": "void", "sig_out": "signal",
      "args": [ {"lit": {"int": 5}} ],
      "emit": {"int": 5},          // present on sources
      "gate": {"op": "gt", "rhs": {"int": 3}},   // present on gates
      "merge": {"policy": "fastest", "n": 2}     // present on merges
    }
  ],
  "edges": [
    { "id": 0, "kind": "flow",    // flow|tap|sever|exchange|feedback
      "from": 0, "to": 1, "sig": "signal" }
  ],
  "structs": [
    { "name": "point", "fields": [ {"name":"x","type":"int","optional":false} ] }
  ]
}
```

### Node kinds (from `sfg.h`)
`source` emits into the chain · `processor` transforms · `gate` passes/blocks on a
condition · `fork` fans one signal to N · `merge` collects N into one (carries the
chord policy) · `tap` read-only observer · `sink` terminal.

### Edge kinds (from `sfg.h` + the Rust AST feedback edge)
`flow` (`->`) · `tap` (`~>`) · `sever` (`-x>`) · `exchange` (`<->`) · `feedback`
(`<~`, sampled at next tick — never participates in the current tick's chord).

### The chord rule is preserved
A `|` merge is **one** `merge` node carrying `merge.policy`
(`all`/`any`/`quorum`/`fastest`/`within`/`by`), never N independent edges. This is
the invariant the Rust AST guards (`ast.rs` "chord rule") and ZIR carries it
verbatim so no back-end can re-linearize it.

### Paradigm-primitive fidelity (docs/SIGNAL_LOGIC.md)
ZIR must carry the signal-logic primitives as structured data, not flatten them
into opaque args — a back-end that can't see a primitive can't honor it:
- **Chord** — `merge.policy` (above).
- **Confluence** — `within` carries its **time window**:
  `{"policy":"within","window":{"duration":100,"unit":"ms"}}`. Dropping the
  window would reduce confluence to a bare AND.
- **Knee / soft gates, graded silence, grade weight, reflex/deliberate
  priority, sustained** — promoted to a first-class `node.mods` object
  (`{"knee":…,"curve":…,"on_silence":…,"priority":…,"weighted":…}`), not left
  buried in `args`.
- **Delta (higher-order)** — nested calls recurse, so `delta(delta(temp))`
  survives as `{"call":"delta","args":[{"pos":{"call":"delta",…}}]}` rather than
  collapsing to a placeholder.

These are guarded by `zir::tests::{confluence_window_survives,
higher_order_delta_survives, paradigm_modifiers_are_first_class}`.

### Verb → kernel node-type
The kernel loader (`zplus_zir.c`) maps `verb` to `enum zp_node_type`
(`emit`→`ZP_EMIT`, `gate`+`op:gt`→`ZP_GATE_GT`, `str.len`→`ZP_STR_LEN`, …). Verbs
the kernel doesn't know become pass-through processors (honest degradation, not a
silent drop — the loader records them).

## Conformance

A front-end conforms if its ZIR loads into the kernel loader without structural
error and preserves node/edge/chain counts and the chord policy. A back-end
conforms if it consumes v1 ZIR and rejects unknown `zir` versions rather than
guessing.

Reference emitters: `tools/zplus/src/zir.rs` (Rust), `tools/zplus/sfg_serialize.c`
(C). Reference consumer + host test: `kernel/boot/zplus_zir.c` +
`kernel/boot/zplus_zir_test.c`.
