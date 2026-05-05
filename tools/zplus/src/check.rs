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

/// Walk a parsed `Module` and collect type errors. Every error is independent
/// — the checker keeps going past the first one so a single run surfaces
/// every violation in the file.
pub fn check_module(m: &Module<'_>) -> Vec<TypeError> {
    let mut errors = Vec::new();
    for stmt in &m.stmts {
        check_stmt(stmt, &mut errors);
    }
    errors
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
}
