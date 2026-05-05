//! Z+ type checker — first cut.
//!
//! Two responsibilities for v1, both ground-truth from
//! `tools/zplus/SEMANTIC_CONTRACTS.md`:
//!
//! 1. **Literal typing** — every `Atom::Literal` gets a concrete `Type`.
//!    `Int` → `Prim(Int)`, `Duration{ms}` → `Tagged<Float, ms>`, etc. Only
//!    literals are typed for now; everything else stays `Type::Unknown` and
//!    a later inference pass fills in.
//!
//! 2. **Merge arity check (the chord rule, at the type layer)** — every
//!    `Chain::Merge` gets its `inputs.len()` checked against its
//!    `MergePolicy`:
//!    - `All` / `Any` / `Within` / `By` / `Quorum::By(_)` — at least 1 input
//!    - `Quorum { n, m }` — exactly `m` inputs, with `n ≤ m`
//!    - `Fastest(n)` — at least `n` inputs
//!
//!    A merge that violates these rules cannot resolve correctly at
//!    runtime no matter what the rest of the chain looks like, so it
//!    fails type-check.
//!
//! ## Not yet
//!
//! - Type inference for non-literal terms (paths, calls, BinExpr, …)
//! - Flow connectivity (`Flow(a, b)`: `output(a) == input(b)`)
//! - Built-in environment for `gate`, `parse`, `delta`, `rate`, …
//! - Bind asymmetry, fork divergence, unit conformance
//! - Hardware-pin capability matching
//!
//! Each is queued in `STATE.md`. v1 is the foundation: the checker
//! exists, runs on a `Module`, and fails fast on chord-rule violations.

use crate::ast::*;
use crate::ty::{Prim, Type, UnitTag};
use std::collections::HashMap;

/// Built-in environment — name → signature. Pre-populated with the
/// builtins that show up in `programs/02_log_monitor.zp` and other
/// common shapes from the corpus. Each signature is a `Type::Fn` whose
/// `params` are the call's args and `returns` is the call's output type.
///
/// What's in v1: the eight transformers and sources used in
/// `02_log_monitor.zp` (fs, lines, gate, parse, delta, rate, on_silence,
/// vault.store, count, alert, last) plus a handful from other programs
/// (net.listen, tick, baseline, deviation, decay, weighted, sort).
/// Each entry is non-binding — the checker uses it for inference, not
/// strict matching. Unknown args / return types are filled with
/// `Type::Unknown` and the checker defers to the runtime.
///
/// Adding a new builtin: append to the map. The signature should match
/// `SEMANTIC_CONTRACTS.md` if the builtin appears there; otherwise pick
/// the simplest faithful shape and document why.
#[derive(Debug, Clone, Default)]
pub struct TypeEnv {
    pub bindings: HashMap<String, Type>,
}

impl TypeEnv {
    /// Empty env — useful for tests that don't want any builtins.
    pub fn empty() -> Self {
        Self { bindings: HashMap::new() }
    }

    /// Default env — pre-populated with the corpus builtins.
    pub fn default_with_builtins() -> Self {
        let mut env = Self::empty();
        let span = Span::dummy();

        // Helper: a builtin that takes any args and returns Sig<T> where
        // T is the carried type the runtime computes. v1 keeps it loose.
        let any_args_sig_unknown = || Type::Fn {
            params: vec![Type::Any(span)],
            returns: Box::new(Type::Sig(Box::new(Type::Unknown(span)), span)),
            span,
        };
        let any_args_sig_t = || Type::Fn {
            params: vec![Type::Any(span)],
            returns: Box::new(Type::Sig(Box::new(Type::Unknown(span)), span)),
            span,
        };
        let any_args_unit = || Type::Fn {
            params: vec![Type::Any(span)],
            returns: Box::new(Type::Nominal("unit".into(), span)),
            span,
        };

        // Sources — produce Sig<T>.
        env.bindings.insert("fs".into(), any_args_sig_unknown());
        env.bindings.insert("net.listen".into(), any_args_sig_unknown());
        env.bindings.insert("net.connect".into(), any_args_sig_unknown());
        env.bindings.insert("tick".into(), any_args_sig_unknown());

        // Transformers — Sig<T> → Sig<T> or Sig<T> → Sig<U>.
        env.bindings.insert("lines".into(), any_args_sig_t()); // Sig<RawBytes> → Sig<String>
        env.bindings.insert("gate".into(), any_args_sig_t());
        env.bindings.insert("parse".into(), any_args_sig_t());
        env.bindings.insert("delta".into(), any_args_sig_t());
        env.bindings.insert("rate".into(), any_args_sig_t());
        env.bindings.insert("baseline".into(), any_args_sig_t());
        env.bindings.insert("deviation".into(), any_args_sig_t());
        env.bindings.insert("decay".into(), any_args_sig_t());
        env.bindings.insert("on_silence".into(), any_args_sig_t());
        env.bindings.insert("count".into(), any_args_sig_t());
        env.bindings.insert("last".into(), any_args_sig_t());
        env.bindings.insert("weighted".into(), any_args_sig_t());
        env.bindings.insert("sort".into(), any_args_sig_t());
        env.bindings.insert("normalize".into(), any_args_sig_t());

        // Sinks — return unit.
        env.bindings.insert("vault.store".into(), any_args_unit());
        env.bindings.insert("vault.append".into(), any_args_unit());
        env.bindings.insert("vault.write".into(), any_args_unit());
        env.bindings.insert("alert".into(), any_args_unit());
        env.bindings.insert("respond".into(), any_args_unit());

        env
    }

    pub fn lookup(&self, name: &str) -> Option<&Type> {
        self.bindings.get(name)
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct TypeError {
    pub message: String,
    pub span: Span,
}

impl std::fmt::Display for TypeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "type error at {}..{}: {}", self.span.start, self.span.end, self.message)
    }
}

impl std::error::Error for TypeError {}

/// Walk a parsed `Module` and collect type errors from the merge-arity
/// check (chord rule). Every error is independent — the checker keeps
/// going past the first one so a single run surfaces every violation
/// in the file.
pub fn check_module(m: &Module<'_>) -> Vec<TypeError> {
    let mut errors = Vec::new();
    for stmt in &m.stmts {
        check_stmt(stmt, &mut errors);
    }
    errors
}

/// Run Flow connectivity on the module: for every `Flow(a, b)` (and
/// `Tap(a, b)`) inside any chain, verify `infer_chain(a)` is compatible
/// with `infer_chain(b)`. Skips when either side resolves to `Unknown`
/// — that's the "deferred to runtime" case.
///
/// This is separate from `check_module` so callers can opt in. The v1
/// merge-arity check has no env dependency; this one does.
pub fn check_flow_connectivity(m: &Module<'_>, env: &TypeEnv) -> Vec<TypeError> {
    let mut errors = Vec::new();
    for stmt in &m.stmts {
        let chain = match stmt {
            Stmt::Wire(w) => &w.chain,
            Stmt::Connect(c) => c,
        };
        walk_flows(chain, env, &mut errors);
    }
    errors
}

fn walk_flows<'a>(c: &Chain<'a>, env: &TypeEnv, errors: &mut Vec<TypeError>) {
    match c {
        Chain::Flow(a, b, span) | Chain::Tap(a, b, span) => {
            walk_flows(a, env, errors);
            walk_flows(b, env, errors);
            let out_a = infer_chain(a, env);
            let in_b = infer_chain(b, env);
            // Unknown on either side defers to runtime — the v2 inference
            // pass simply doesn't have enough info to decide.
            if matches!(out_a, Type::Unknown(_)) || matches!(in_b, Type::Unknown(_)) {
                return;
            }
            if !types_compatible(&out_a, &in_b) {
                errors.push(TypeError {
                    message: format!(
                        "flow type mismatch: upstream produces {} but downstream expects {}",
                        format_type(&out_a),
                        format_type(&in_b),
                    ),
                    span: *span,
                });
            }
        }
        Chain::Sever(a, b, _) | Chain::Bind(a, b, _) => {
            walk_flows(a, env, errors);
            walk_flows(b, env, errors);
        }
        Chain::Atom(_) => {}
        Chain::Call(call) => {
            for arg in &call.args {
                let child = match arg {
                    Arg::Positional(c) => c,
                    Arg::Named { value, .. } => value,
                };
                walk_flows(child, env, errors);
            }
        }
        Chain::Fork(branches, _) => {
            for branch in branches {
                if let Some(label) = &branch.label {
                    walk_flows(label, env, errors);
                }
                walk_flows(&branch.body, env, errors);
            }
        }
        Chain::Merge(merge) => {
            for input in &merge.inputs {
                walk_flows(input, env, errors);
            }
            if let Some(d) = &merge.downstream {
                walk_flows(d, env, errors);
            }
        }
        Chain::BinExpr { lhs, rhs, .. } => {
            walk_flows(lhs, env, errors);
            walk_flows(rhs, env, errors);
        }
        Chain::UnaryCmp { rhs, .. } => walk_flows(rhs, env, errors),
    }
}

/// Short pretty-print for a Type used in error messages. Avoids the
/// noise of `Debug` (which dumps full spans).
fn format_type(t: &Type) -> String {
    match t {
        Type::Prim(p) => format!("{:?}", p),
        Type::Sig(inner, _) => format!("Sig<{}>", format_type(inner)),
        Type::Tagged { inner, unit, .. } => format!("Tagged<{}, {:?}>", format_type(inner), unit),
        Type::Nominal(name, _) => name.clone(),
        Type::Unknown(_) => "?".into(),
        Type::Any(_) => "Any".into(),
        Type::Never(_) => "!".into(),
        Type::Range(inner, _) => format!("Range<{}>", format_type(inner)),
        Type::Tuple(items, _) => {
            let parts: Vec<String> = items.iter().map(format_type).collect();
            format!("({})", parts.join(", "))
        }
        Type::List(inner, _) => format!("List<{}>", format_type(inner)),
        Type::Fn { params, returns, .. } => {
            let p: Vec<String> = params.iter().map(format_type).collect();
            format!("Fn<{} -> {}>", p.join(", "), format_type(returns))
        }
        other => format!("{:?}", other),
    }
}

fn check_stmt(stmt: &Stmt<'_>, errors: &mut Vec<TypeError>) {
    match stmt {
        Stmt::Wire(w) => check_chain(&w.chain, errors),
        Stmt::Connect(c) => check_chain(c, errors),
    }
}

fn check_chain(c: &Chain<'_>, errors: &mut Vec<TypeError>) {
    match c {
        Chain::Atom(_) => {}
        Chain::Call(call) => {
            for arg in &call.args {
                match arg {
                    Arg::Positional(child) => check_chain(child, errors),
                    Arg::Named { value, .. } => check_chain(value, errors),
                }
            }
        }
        Chain::Flow(a, b, _) | Chain::Tap(a, b, _) | Chain::Sever(a, b, _) | Chain::Bind(a, b, _) => {
            check_chain(a, errors);
            check_chain(b, errors);
        }
        Chain::Fork(branches, _) => {
            for branch in branches {
                if let Some(label) = &branch.label {
                    check_chain(label, errors);
                }
                check_chain(&branch.body, errors);
            }
        }
        Chain::Merge(merge) => {
            check_merge(merge, errors);
            for input in &merge.inputs {
                check_chain(input, errors);
            }
            if let Some(d) = &merge.downstream {
                check_chain(d, errors);
            }
        }
        Chain::BinExpr { lhs, rhs, .. } => {
            check_chain(lhs, errors);
            check_chain(rhs, errors);
        }
        Chain::UnaryCmp { rhs, .. } => check_chain(rhs, errors),
    }
}

/// Verify the merge's input count matches its policy. The runtime can't
/// resolve a `Quorum { 2, 3 }` if only 1 input is wired; better to fail at
/// type-check than at runtime.
fn check_merge(merge: &Merge<'_>, errors: &mut Vec<TypeError>) {
    let n_inputs = merge.inputs.len();
    match &merge.policy {
        MergePolicy::Quorum { n, m } => {
            if n > m {
                errors.push(TypeError {
                    message: format!("quorum {} of {} is impossible: n must be <= m", n, m),
                    span: merge.span,
                });
            }
            if n_inputs != *m as usize {
                errors.push(TypeError {
                    message: format!(
                        "merge with policy {} of {} expects {} inputs, found {}",
                        n, m, m, n_inputs
                    ),
                    span: merge.span,
                });
            }
        }
        MergePolicy::Fastest(n) => {
            if n_inputs < *n as usize {
                errors.push(TypeError {
                    message: format!(
                        "merge with policy fastest({}) needs at least {} inputs, found {}",
                        n, n, n_inputs
                    ),
                    span: merge.span,
                });
            }
        }
        MergePolicy::All
        | MergePolicy::Any
        | MergePolicy::Within(_)
        | MergePolicy::By(_) => {
            if n_inputs == 0 {
                errors.push(TypeError {
                    message: "merge has no inputs".into(),
                    span: merge.span,
                });
            }
        }
    }
}

/// Infer the output type of a chain term. Walks the AST recursively;
/// returns `Type::Unknown` when no rule applies. The env is consulted for
/// `Atom::Path` and `Call::callee` lookups.
///
/// What v1 handles:
/// - Literals → typed via `type_of_literal`, lifted to `Sig<T>` when in a
///   chain-term position (most chain terms are signal sources).
/// - `Atom::Path` → looked up in env; otherwise `Unknown`.
/// - `Call` → looked up in env by callee path; if the callee is a
///   `Type::Fn`, its `returns` is the result type.
/// - `Flow(_, b)` → `infer(b)` — the chain's output is the rightmost
///   chain-term's output.
/// - `Tap(a, _)` → `infer(a)` — tap is read-only; the upstream's type
///   continues forward.
/// - `Merge` → carrier of the first input (homogeneity rule); falls back
///   to `Unknown` if no inputs.
/// - Everything else → `Unknown`.
pub fn infer_chain(c: &Chain<'_>, env: &TypeEnv) -> Type {
    let span = Span::dummy();
    match c {
        Chain::Atom(Atom::Literal(lit)) => {
            // Literals in chain-term position are signal sources.
            type_of_literal(lit).into_signal(span)
        }
        Chain::Atom(Atom::Path(p)) => {
            let name = p.joined();
            // A bare Path in chain-term position is implicitly applied —
            // `... -> on_silence -> ...` means "apply on_silence to the
            // upstream signal." So when the env entry is a `Fn`, return
            // its `returns` rather than the Fn itself.
            match env.lookup(&name) {
                Some(Type::Fn { returns, .. }) => (**returns).clone(),
                Some(other) => other.clone(),
                None => Type::Unknown(span),
            }
        }
        Chain::Atom(Atom::DevNull(_)) => Type::Nominal("unit".into(), span),
        Chain::Atom(Atom::TimePast { .. }) => Type::Sig(Box::new(Type::Unknown(span)), span),
        Chain::Call(call) => {
            let name = call.callee.joined();
            match env.lookup(&name) {
                Some(Type::Fn { returns, .. }) => (**returns).clone(),
                Some(other) => other.clone(),
                None => Type::Unknown(span),
            }
        }
        Chain::Flow(_, b, _) => infer_chain(b, env),
        Chain::Tap(a, _, _) => infer_chain(a, env),
        Chain::Sever(_, _, _) => Type::Nominal("unit".into(), span),
        Chain::Bind(a, _, _) => infer_chain(a, env),
        Chain::Fork(_, _) => Type::Unknown(span),
        Chain::Merge(merge) => {
            if let Some(first) = merge.inputs.first() {
                infer_chain(first, env)
            } else {
                Type::Unknown(span)
            }
        }
        Chain::BinExpr { op, .. } => match op {
            BinOp::Lt | BinOp::Gt | BinOp::Le | BinOp::Ge | BinOp::Eq | BinOp::Ne | BinOp::FuzzyMatch => {
                Type::Prim(Prim::Bool)
            }
        },
        Chain::UnaryCmp { .. } => Type::Prim(Prim::Bool),
    }
}

/// Drop one outer `Sig<...>` if present. Test-only for now — the
/// types_compatible function handles Sig-wrapper symmetry directly. Kept
/// as a public helper for future passes that need to expose the carrier.
#[cfg(test)]
fn unwrap_sig(t: &Type) -> &Type {
    match t {
        Type::Sig(inner, _) => inner,
        other => other,
    }
}

/// Whether two types should be considered compatible at a Flow boundary.
/// v1 rules:
/// - `Unknown` matches anything (deferred — checker can't decide).
/// - `Any` matches anything.
/// - `Never` matches anything.
/// - Primitive equality.
/// - Numeric promotion: `Int` flows to `Float`.
/// - `Sig<A>` matches `Sig<B>` iff `A` matches `B`.
/// - `Tagged<A, u>` matches `Tagged<B, u>` iff `A` matches `B`.
/// - `Nominal(name)` matches `Nominal(name)` (string equality, span-
///   independent).
/// - Everything else: structural equality (modulo spans), best-effort.
pub fn types_compatible(a: &Type, b: &Type) -> bool {
    // Either side Unknown / Any / Never → always compatible (deferred or
    // bottom-typed).
    if matches!(a, Type::Unknown(_) | Type::Any(_) | Type::Never(_))
        || matches!(b, Type::Unknown(_) | Type::Any(_) | Type::Never(_))
    {
        return true;
    }
    match (a, b) {
        (Type::Prim(pa), Type::Prim(pb)) => pa == pb || matches!((pa, pb), (Prim::Int, Prim::Float)),
        (Type::Sig(ia, _), Type::Sig(ib, _)) => types_compatible(ia, ib),
        (Type::Tagged { inner: ia, unit: ua, .. }, Type::Tagged { inner: ib, unit: ub, .. }) => {
            ua == ub && types_compatible(ia, ib)
        }
        (Type::Nominal(na, _), Type::Nominal(nb, _)) => na == nb,
        (Type::Range(ia, _), Type::Range(ib, _)) => types_compatible(ia, ib),
        // Sig-on-one-side wraps: `T` and `Sig<T>` are compatible at flow
        // boundaries because most builtins implicitly Sig-lift their
        // operands.
        (Type::Sig(inner, _), other) | (other, Type::Sig(inner, _)) => types_compatible(inner, other),
        _ => false,
    }
}

/// Assign a `Type` to a literal per `SEMANTIC_CONTRACTS.md`.
pub fn type_of_literal(lit: &Literal<'_>) -> Type {
    let span = match lit {
        Literal::Int(_, s)
        | Literal::Float(_, s)
        | Literal::String(_, s)
        | Literal::TemplateString(_, s)
        | Literal::Ratio(_, s)
        | Literal::Sigma(_, s)
        | Literal::Percent(_, s)
        | Literal::Hex(_, s)
        | Literal::HexColor(_, s) => *s,
        Literal::Duration { span, .. }
        | Literal::ByteSize { span, .. }
        | Literal::Dimension { span, .. } => *span,
    };
    match lit {
        Literal::Int(_, _) => Type::Prim(Prim::Int),
        Literal::Float(_, _) => Type::Prim(Prim::Float),
        Literal::String(_, _) | Literal::TemplateString(_, _) => Type::Prim(Prim::String),
        Literal::Ratio(_, _) => Type::Tagged {
            inner: Box::new(Type::Prim(Prim::Float)),
            unit: UnitTag::Simple("ratio".into()),
            span,
        },
        Literal::Sigma(_, _) => Type::Tagged {
            inner: Box::new(Type::Prim(Prim::Float)),
            unit: UnitTag::Simple("sigma".into()),
            span,
        },
        Literal::Percent(_, _) => Type::Tagged {
            inner: Box::new(Type::Prim(Prim::Float)),
            unit: UnitTag::Simple("percent".into()),
            span,
        },
        Literal::Hex(_, _) => Type::Prim(Prim::Int),
        Literal::HexColor(_, _) => Type::Nominal("color".into(), span),
        Literal::Duration { unit, .. } => Type::Tagged {
            inner: Box::new(Type::Prim(Prim::Float)),
            unit: UnitTag::Simple(format!("{}", unit)),
            span,
        },
        Literal::ByteSize { .. } => Type::Tagged {
            inner: Box::new(Type::Prim(Prim::Int)),
            unit: UnitTag::Simple("bytes".into()),
            span,
        },
        Literal::Dimension { .. } => Type::Nominal("dimension".into(), span),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ast::DurationUnit;

    fn dummy() -> Span { Span::dummy() }

    // ── literal typing ────────────────────────────────────────────

    #[test]
    fn int_literal_is_prim_int() {
        let t = type_of_literal(&Literal::Int(5, dummy()));
        assert_eq!(t, Type::Prim(Prim::Int));
    }

    #[test]
    fn duration_literal_is_tagged_float() {
        let t = type_of_literal(&Literal::Duration {
            value: 30.0,
            unit: DurationUnit::D,
            span: dummy(),
        });
        match t {
            Type::Tagged { inner, unit, .. } => {
                assert!(matches!(inner.as_ref(), Type::Prim(Prim::Float)));
                assert!(matches!(unit, UnitTag::Simple(ref s) if s == "d"));
            }
            other => panic!("expected Tagged<Float, d>, got {:?}", other),
        }
    }

    #[test]
    fn sigma_literal_carries_sigma_tag() {
        let t = type_of_literal(&Literal::Sigma(2.0, dummy()));
        match t {
            Type::Tagged { unit: UnitTag::Simple(u), .. } => assert_eq!(u, "sigma"),
            other => panic!("expected Tagged<.., sigma>, got {:?}", other),
        }
    }

    #[test]
    fn ratio_literal_carries_ratio_tag() {
        let t = type_of_literal(&Literal::Ratio(5.0, dummy()));
        match t {
            Type::Tagged { unit: UnitTag::Simple(u), .. } => assert_eq!(u, "ratio"),
            other => panic!("expected Tagged<.., ratio>, got {:?}", other),
        }
    }

    #[test]
    fn hex_color_is_nominal_color() {
        let t = type_of_literal(&Literal::HexColor("#29ADFF", dummy()));
        match t {
            Type::Nominal(name, _) => assert_eq!(name, "color"),
            other => panic!("expected Nominal('color'), got {:?}", other),
        }
    }

    // ── merge arity (the chord rule at the type layer) ────────────

    fn input_atom(text: &'static str) -> Chain<'static> {
        Chain::Atom(Atom::Path(Path::one(text, dummy())))
    }

    #[test]
    fn all_policy_with_one_input_is_ok() {
        let merge = Merge {
            inputs: vec![input_atom("a")],
            policy: MergePolicy::All,
            downstream: None,
            span: dummy(),
        };
        let mut errors = Vec::new();
        check_merge(&merge, &mut errors);
        assert!(errors.is_empty());
    }

    #[test]
    fn empty_merge_errors() {
        let merge = Merge {
            inputs: vec![],
            policy: MergePolicy::All,
            downstream: None,
            span: dummy(),
        };
        let mut errors = Vec::new();
        check_merge(&merge, &mut errors);
        assert_eq!(errors.len(), 1);
        assert!(errors[0].message.contains("no inputs"));
    }

    #[test]
    fn quorum_arity_must_match_m() {
        let merge_ok = Merge {
            inputs: vec![input_atom("a"), input_atom("b"), input_atom("c")],
            policy: MergePolicy::Quorum { n: 2, m: 3 },
            downstream: None,
            span: dummy(),
        };
        let mut e = Vec::new();
        check_merge(&merge_ok, &mut e);
        assert!(e.is_empty(), "{:?}", e);

        // 2 of 3 with only 2 inputs — error
        let merge_bad = Merge {
            inputs: vec![input_atom("a"), input_atom("b")],
            policy: MergePolicy::Quorum { n: 2, m: 3 },
            downstream: None,
            span: dummy(),
        };
        let mut e2 = Vec::new();
        check_merge(&merge_bad, &mut e2);
        assert_eq!(e2.len(), 1);
        assert!(e2[0].message.contains("expects 3 inputs"));
    }

    #[test]
    fn quorum_n_gt_m_errors() {
        let merge = Merge {
            inputs: vec![input_atom("a"), input_atom("b")],
            policy: MergePolicy::Quorum { n: 5, m: 2 },
            downstream: None,
            span: dummy(),
        };
        let mut e = Vec::new();
        check_merge(&merge, &mut e);
        // Both n>m AND arity-mismatch (m=2, inputs=2 OK, only n>m fires).
        // Actually m=2 and inputs=2 matches, so only the n>m error fires.
        assert!(e.iter().any(|err| err.message.contains("n must be <= m")));
    }

    #[test]
    fn fastest_n_min_inputs() {
        let merge_ok = Merge {
            inputs: vec![input_atom("a"), input_atom("b"), input_atom("c")],
            policy: MergePolicy::Fastest(2),
            downstream: None,
            span: dummy(),
        };
        let mut e = Vec::new();
        check_merge(&merge_ok, &mut e);
        assert!(e.is_empty());

        let merge_bad = Merge {
            inputs: vec![input_atom("a")],
            policy: MergePolicy::Fastest(2),
            downstream: None,
            span: dummy(),
        };
        let mut e2 = Vec::new();
        check_merge(&merge_bad, &mut e2);
        assert_eq!(e2.len(), 1);
        assert!(e2[0].message.contains("fastest(2)"));
    }

    /// The "structurally legal three-input vertical merge" should pass
    /// type-check. This is the canary for the `02_log_monitor.zp` shape.
    #[test]
    fn three_input_all_policy_passes() {
        let merge = Merge {
            inputs: vec![input_atom("syslog"), input_atom("app_log"), input_atom("kern_log")],
            policy: MergePolicy::All,
            downstream: Some(Box::new(input_atom("all_lines"))),
            span: dummy(),
        };
        let mut e = Vec::new();
        check_merge(&merge, &mut e);
        assert!(e.is_empty(), "{:?}", e);
    }

    // ── inference / TypeEnv ──────────────────────────────────────

    #[test]
    fn default_env_has_log_monitor_builtins() {
        let env = TypeEnv::default_with_builtins();
        for name in ["fs", "gate", "parse", "delta", "rate", "on_silence", "vault.store", "alert", "count", "last"] {
            assert!(env.lookup(name).is_some(), "missing builtin: {}", name);
        }
    }

    #[test]
    fn empty_env_returns_unknown() {
        let env = TypeEnv::empty();
        let chain = Chain::Atom(Atom::Path(Path::one("syslog", dummy())));
        assert!(matches!(infer_chain(&chain, &env), Type::Unknown(_)));
    }

    #[test]
    fn literal_in_chain_term_lifts_to_sig() {
        let env = TypeEnv::empty();
        let chain = Chain::Atom(Atom::Literal(Literal::Int(5, dummy())));
        match infer_chain(&chain, &env) {
            Type::Sig(inner, _) => assert!(matches!(inner.as_ref(), Type::Prim(Prim::Int))),
            other => panic!("expected Sig<Int>, got {:?}", other),
        }
    }

    #[test]
    fn call_uses_env_return_type() {
        let env = TypeEnv::default_with_builtins();
        let chain = Chain::Call(Call {
            callee: Path::one("fs", dummy()),
            args: vec![Arg::Positional(Chain::Atom(Atom::Literal(Literal::String(
                "\"/var/log\"",
                dummy(),
            ))))],
            span: dummy(),
        });
        // Should resolve to fs's return type — Sig<Unknown>.
        match infer_chain(&chain, &env) {
            Type::Sig(_, _) => {} // good
            other => panic!("expected Sig, got {:?}", other),
        }
    }

    #[test]
    fn flow_returns_rightmost_type() {
        let env = TypeEnv::default_with_builtins();
        // `fs("/log") -> lines` — output is whatever `lines` returns.
        let chain = Chain::Flow(
            Box::new(Chain::Call(Call {
                callee: Path::one("fs", dummy()),
                args: vec![],
                span: dummy(),
            })),
            Box::new(Chain::Atom(Atom::Path(Path::one("lines", dummy())))),
            dummy(),
        );
        // `lines` is in env as a Fn; returned type is its `returns` (Sig<Unknown>).
        match infer_chain(&chain, &env) {
            Type::Fn { .. } => {}     // env returned the fn signature directly via Path lookup
            Type::Sig(_, _) => {}     // OR Sig<...> via Fn return
            Type::Unknown(_) => {}    // OR Unknown — also acceptable for v1
            other => panic!("unexpected Flow result: {:?}", other),
        }
    }

    #[test]
    fn tap_preserves_upstream_type() {
        let env = TypeEnv::default_with_builtins();
        // `error_rate ~> live_view` — output is error_rate's type.
        let chain = Chain::Tap(
            Box::new(Chain::Atom(Atom::Path(Path::one("error_rate", dummy())))),
            Box::new(Chain::Atom(Atom::Path(Path::one("live_view", dummy())))),
            dummy(),
        );
        // error_rate isn't in env → Unknown. Tap returns the upstream type.
        assert!(matches!(infer_chain(&chain, &env), Type::Unknown(_)));
    }

    #[test]
    fn binexpr_returns_bool() {
        let env = TypeEnv::empty();
        let chain = Chain::BinExpr {
            op: BinOp::Lt,
            lhs: Box::new(Chain::Atom(Atom::Literal(Literal::Int(5, dummy())))),
            rhs: Box::new(Chain::Atom(Atom::Literal(Literal::Int(10, dummy())))),
            span: dummy(),
        };
        assert_eq!(infer_chain(&chain, &env), Type::Prim(Prim::Bool));
    }

    #[test]
    fn unary_cmp_returns_bool() {
        let env = TypeEnv::empty();
        let chain = Chain::UnaryCmp {
            op: BinOp::Gt,
            rhs: Box::new(Chain::Atom(Atom::Literal(Literal::Int(5, dummy())))),
            span: dummy(),
        };
        assert_eq!(infer_chain(&chain, &env), Type::Prim(Prim::Bool));
    }

    // ── types_compatible ─────────────────────────────────────────

    #[test]
    fn unknown_matches_anything() {
        let unk = Type::Unknown(dummy());
        let int = Type::Prim(Prim::Int);
        assert!(types_compatible(&unk, &int));
        assert!(types_compatible(&int, &unk));
    }

    #[test]
    fn int_promotes_to_float() {
        let int = Type::Prim(Prim::Int);
        let float = Type::Prim(Prim::Float);
        assert!(types_compatible(&int, &float));
    }

    #[test]
    fn sig_compatibility_is_recursive() {
        let span = dummy();
        let a = Type::Sig(Box::new(Type::Prim(Prim::Int)), span);
        let b = Type::Sig(Box::new(Type::Prim(Prim::Int)), span);
        assert!(types_compatible(&a, &b));

        let c = Type::Sig(Box::new(Type::Prim(Prim::String)), span);
        assert!(!types_compatible(&a, &c));
    }

    #[test]
    fn nominal_compares_by_name() {
        let a = Type::Nominal("pcm_source".into(), Span::new(0, 10));
        let b = Type::Nominal("pcm_source".into(), Span::new(50, 60));
        let c = Type::Nominal("tx_completion".into(), Span::new(0, 10));
        assert!(types_compatible(&a, &b));
        assert!(!types_compatible(&a, &c));
    }

    #[test]
    fn unwrap_sig_strips_one_layer() {
        let span = dummy();
        let sig_int = Type::Sig(Box::new(Type::Prim(Prim::Int)), span);
        assert_eq!(unwrap_sig(&sig_int), &Type::Prim(Prim::Int));
    }

    // ── flow connectivity ────────────────────────────────────────

    fn module_one_stmt(c: Chain<'static>) -> Module<'static> {
        Module {
            stmts: vec![Stmt::Connect(c)],
            span: dummy(),
        }
    }

    fn lit_int(n: i64) -> Chain<'static> {
        Chain::Atom(Atom::Literal(Literal::Int(n, dummy())))
    }

    #[test]
    fn flow_with_unknown_either_side_passes() {
        let env = TypeEnv::empty();
        // `unknown_path -> sink` — both sides Unknown → no error.
        let chain = Chain::Flow(
            Box::new(input_atom("unknown_a")),
            Box::new(input_atom("unknown_b")),
            dummy(),
        );
        let m = module_one_stmt(chain);
        let errs = check_flow_connectivity(&m, &env);
        assert!(errs.is_empty(), "{:?}", errs);
    }

    #[test]
    fn flow_compatible_types_pass() {
        let env = TypeEnv::default_with_builtins();
        // `fs("/log") -> lines` — fs returns Sig<Unknown>; lines lookup
        // returns its Fn signature. Sig<Unknown> vs Fn isn't directly
        // compatible, but Unknown defers, so this should pass for v1.
        let chain = Chain::Flow(
            Box::new(Chain::Call(Call {
                callee: Path::one("fs", dummy()),
                args: vec![],
                span: dummy(),
            })),
            Box::new(input_atom("lines")),
            dummy(),
        );
        let m = module_one_stmt(chain);
        let errs = check_flow_connectivity(&m, &env);
        assert!(errs.is_empty(), "{:?}", errs);
    }

    #[test]
    fn flow_int_to_string_concrete_mismatch_fires() {
        // Two literals: Sig<Int> -> Sig<String> — concrete mismatch on
        // both sides. With literal Sig-lift, this should report a
        // mismatch.
        let mut env = TypeEnv::empty();
        let span = dummy();
        // Pretend `string_sink` is a builtin that takes Sig<String> and
        // returns unit. We register it explicitly.
        env.bindings.insert(
            "string_sink".into(),
            Type::Sig(Box::new(Type::Prim(Prim::String)), span),
        );
        let chain = Chain::Flow(
            Box::new(lit_int(5)),
            Box::new(input_atom("string_sink")),
            dummy(),
        );
        let m = module_one_stmt(chain);
        let errs = check_flow_connectivity(&m, &env);
        assert_eq!(errs.len(), 1, "{:?}", errs);
        assert!(errs[0].message.contains("flow type mismatch"));
    }

    #[test]
    fn flow_int_to_float_promotes_silently() {
        let mut env = TypeEnv::empty();
        let span = dummy();
        env.bindings.insert(
            "float_sink".into(),
            Type::Sig(Box::new(Type::Prim(Prim::Float)), span),
        );
        let chain = Chain::Flow(
            Box::new(lit_int(5)),
            Box::new(input_atom("float_sink")),
            dummy(),
        );
        let m = module_one_stmt(chain);
        let errs = check_flow_connectivity(&m, &env);
        assert!(errs.is_empty(), "Int → Float should promote: {:?}", errs);
    }

    #[test]
    fn format_type_handles_common_shapes() {
        let span = dummy();
        assert_eq!(format_type(&Type::Prim(Prim::Int)), "Int");
        assert_eq!(format_type(&Type::Sig(Box::new(Type::Prim(Prim::String)), span)), "Sig<String>");
        assert_eq!(format_type(&Type::Unknown(span)), "?");
        assert_eq!(format_type(&Type::Nominal("color".into(), span)), "color");
    }
}
