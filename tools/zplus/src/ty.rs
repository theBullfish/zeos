//! Z+ type system.
//!
//! ## What's here
//!
//! Just the type-shape enum, no checker yet. Same pattern as `ast.rs`: define
//! the typed surface before any pass starts emitting types so the shape gets
//! reviewed before code locks it in.
//!
//! ## What CHAIN_CONTRACT.md requires
//!
//! Every chain node has an input and output type. A flow `a -> b` requires
//! `output(a) == input(b)`. Type identity for hardware-class types is
//! nominal — `pcm_source`, `tx_completion` etc. are distinct types even if
//! their underlying representation is identical. For language-level
//! arithmetic and predicate types we use structural identity.
//!
//! ## What SEMANTIC_CONTRACTS.md requires
//!
//! Each of the 28 synthetic `__op__` callees has a type signature. The
//! checker resolves a call to one of them by matching against the signature
//! catalogue. Most are simple (`__not__: Bool -> Bool`); some carry unit
//! tagging (`__add__` preserves matching units); some defer (`__apply__`
//! looks up the receiver's `Fn<...>` type at the use site).
//!
//! ## What's NOT in v1
//!
//! - Generics / type parameters beyond simple inference. Z+ has implicit
//!   plurality (no `list<T>` syntax in the corpus, mostly), so the checker
//!   infers `T` per use site.
//! - Subtyping. Numeric promotion (Int → Float) is the only conversion.
//! - Effect tracking. The Tap/Sever/Merge runtime contract is enforced by
//!   the lowering pass, not the type system.
//! - Inference. v1 will require explicit annotations at chain boundaries
//!   (per CHAIN_CONTRACT.md). Inference is a v2 concern.

use crate::ast::Span;

/// A type. `Span` on each variant points back to where the type was
/// declared / inferred so error messages can locate the source.
#[derive(Debug, Clone, PartialEq)]
pub enum Type {
    /// `Int`, `Float`, `String`, `Bool` — the language primitives.
    Prim(Prim),

    /// A unit-tagged numeric: `position @ percent`, `rate @ rpm`,
    /// `2σ` carry their unit at the type level. Arithmetic preserves
    /// matching units; mixing rejects.
    Tagged { inner: Box<Type>, unit: UnitTag, span: Span },

    /// A signal — a chain-time-varying value. Every wire carries `Sig<T>`
    /// for some inner `T`. The chord rule lives at the runtime layer; the
    /// type system only tracks the carried type.
    Sig(Box<Type>, Span),

    /// A range — `0..10`, `"a".."z"`. Lo and hi share the same inner type.
    Range(Box<Type>, Span),

    /// Rate literal — `100/m`. Numerator type, unit name.
    Rate { numeric: Box<Type>, unit: UnitTag, span: Span },

    /// Tuple — `(a, b, c)`. Heterogeneous positional.
    Tuple(Vec<Type>, Span),

    /// List / collection. `T[N]` for a known-length form, `T[]` open. v1
    /// only tracks the element type.
    List(Box<Type>, Span),

    /// Grid — multi-dimensional collection. Used by `screen[0,0]` indexing.
    Grid { element: Box<Type>, dims: u8, span: Span },

    /// Map / dict. Used by `palette[key]` etc.
    Map { key: Box<Type>, value: Box<Type>, span: Span },

    /// Record / struct — labeled fields. `{ movement: vec2, aim: vec2, ... }`.
    Record { fields: Vec<RecordField>, span: Span },

    /// Union of alternatives — `<- on | off`, `level: error | warn | info`.
    Union(Vec<Type>, Span),

    /// Function — `Fn<A, B, ... -> R>`. Used by `__apply__` and by built-in
    /// callees like `gate`, `parse`, `delta`, etc.
    Fn { params: Vec<Type>, returns: Box<Type>, span: Span },

    /// Nominal type — identified by name only. Hardware-class types from
    /// CHAIN_CONTRACT.md (`pcm_source`, `tx_completion`, `block_request`),
    /// user-declared records, etc. Two nominal types are equal iff their
    /// names match.
    Nominal(String, Span),

    /// Quorum spec — `N of M` arity carrier.
    Quorum { n: u32, m: u32, span: Span },

    /// A named-pair carrier from pipe-OR alt-tail forms. `name: value`.
    Named { name: String, value: Box<Type>, span: Span },

    /// "Don't know yet." Inserted by the inference pass; resolved before
    /// the type-checker accepts a chain. v1 should never lower a chain
    /// containing `Unknown` to IR.
    Unknown(Span),

    /// Top — accepts anything. Used as the input type of pure sinks
    /// (logging, telemetry) that don't care about the wire's contents.
    Any(Span),

    /// Bottom — never produced. Output type of `drop`, `abort`, panicking
    /// branches. Convertible to any other type for assignment purposes.
    Never(Span),
}

/// Language primitives. Numeric promotion: `Int <: Float`. No other
/// implicit conversions.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Prim {
    Int,
    Float,
    String,
    Bool,
    /// Char — currently only used in range expressions like `"a".."z"`.
    Char,
}

/// A unit tag — the right-hand side of `@` or the suffix of a tight numeric
/// like `60Hz`. Compound units like `m/s` lower to `Compound(["m", "s"])`.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum UnitTag {
    Simple(String),
    /// Slash-separated compound: `m/s`, `kg/m`. The first segment is the
    /// numerator, the rest are denominators.
    Compound(Vec<String>),
    /// A hardware-pin annotation: `@ goya(0)`, `@ mde("model.zdx")`. Output
    /// type stays the same; the runtime allocates the chain stage on the
    /// named device. Captured here so the checker can dispatch on
    /// hardware-pin semantics distinctly from unit-tagged arithmetic.
    HardwarePin { device: String, args: Vec<String> },
}

#[derive(Debug, Clone, PartialEq)]
pub struct RecordField {
    pub name: String,
    pub ty: Type,
    pub span: Span,
}

impl Type {
    pub fn span(&self) -> Span {
        match self {
            Type::Prim(_) => Span::dummy(),
            Type::Tagged { span, .. }
            | Type::Sig(_, span)
            | Type::Range(_, span)
            | Type::Rate { span, .. }
            | Type::Tuple(_, span)
            | Type::List(_, span)
            | Type::Grid { span, .. }
            | Type::Map { span, .. }
            | Type::Record { span, .. }
            | Type::Union(_, span)
            | Type::Fn { span, .. }
            | Type::Nominal(_, span)
            | Type::Quorum { span, .. }
            | Type::Named { span, .. }
            | Type::Unknown(span)
            | Type::Any(span)
            | Type::Never(span) => *span,
        }
    }

    /// Wrap `self` as the carried type of a `Sig<T>`. Idempotent: `Sig<Sig<T>>`
    /// collapses to `Sig<T>` because nested signals don't make sense in the
    /// chain runtime model.
    pub fn into_signal(self, span: Span) -> Type {
        match self {
            Type::Sig(_, _) => self,
            other => Type::Sig(Box::new(other), span),
        }
    }

    /// Strip outer `Sig<...>` — what the wire carries underneath.
    pub fn carrier(&self) -> &Type {
        match self {
            Type::Sig(inner, _) => inner,
            other => other,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dummy() -> Span { Span::dummy() }

    #[test]
    fn sig_is_idempotent() {
        let t = Type::Prim(Prim::Int).into_signal(dummy()).into_signal(dummy());
        // Should not be Sig<Sig<Int>>.
        assert!(matches!(t, Type::Sig(inner, _) if matches!(*inner, Type::Prim(Prim::Int))));
    }

    #[test]
    fn carrier_strips_one_sig() {
        let t = Type::Prim(Prim::Float).into_signal(dummy());
        assert_eq!(t.carrier(), &Type::Prim(Prim::Float));
    }

    #[test]
    fn unit_tagged_arithmetic_preserves_unit() {
        // The type checker (not yet implemented) will use this rule:
        // `Tagged<Int, percent>` + `Tagged<Int, percent>` = `Tagged<Int, percent>`.
        let percent = UnitTag::Simple("percent".into());
        let lhs = Type::Tagged {
            inner: Box::new(Type::Prim(Prim::Int)),
            unit: percent.clone(),
            span: dummy(),
        };
        let rhs = lhs.clone();
        assert_eq!(lhs, rhs);
    }

    #[test]
    fn compound_unit_round_trip() {
        // `@ m/s` → Compound(["m", "s"]).
        let m_per_s = UnitTag::Compound(vec!["m".into(), "s".into()]);
        match &m_per_s {
            UnitTag::Compound(segs) => {
                assert_eq!(segs.len(), 2);
                assert_eq!(segs[0], "m");
                assert_eq!(segs[1], "s");
            }
            _ => panic!(),
        }
    }

    #[test]
    fn hardware_pin_distinct_from_simple_unit() {
        let mde = UnitTag::HardwarePin {
            device: "mde".into(),
            args: vec!["\"toxic-detect.zdx\"".into()],
        };
        let percent = UnitTag::Simple("percent".into());
        assert_ne!(mde, percent);
    }

    #[test]
    fn quorum_distinct_arity() {
        let q23 = Type::Quorum { n: 2, m: 3, span: dummy() };
        let q33 = Type::Quorum { n: 3, m: 3, span: dummy() };
        assert_ne!(q23, q33);
    }

    /// `PartialEq` on `Type` is the strict (span-included) form — useful
    /// for round-trip tests. The CHECKER will implement `type_equiv` for
    /// nominal-by-name and structural-modulo-spans equivalences. This test
    /// pins down the strict shape so a regression that changes derive(Eq)
    /// semantics (e.g. accidentally implements custom PartialEq) is loud.
    #[test]
    fn partial_eq_is_strict_with_spans() {
        let a = Type::Nominal("pcm_source".into(), Span::new(10, 20));
        let b = Type::Nominal("pcm_source".into(), Span::new(10, 20));
        let c = Type::Nominal("pcm_source".into(), Span::new(100, 200));
        assert_eq!(a, b);
        assert_ne!(a, c, "spans participate in PartialEq; type_equiv is checker's job");
    }

    #[test]
    fn record_compares_structurally() {
        let mk = |start, end| Type::Record {
            fields: vec![
                RecordField { name: "x".into(), ty: Type::Prim(Prim::Float), span: dummy() },
                RecordField { name: "y".into(), ty: Type::Prim(Prim::Float), span: dummy() },
            ],
            span: Span::new(start, end),
        };
        // Same fields, different spans, but PartialEq compares spans too —
        // documenting current behavior. The CHECKER, not PartialEq, will
        // implement the structural-equivalence rule when comparing types.
        let a = mk(0, 10);
        let b = mk(20, 30);
        assert_ne!(a, b, "PartialEq is structural+span; type-equiv is structural only — checker handles that");
    }

    /// Sanity: a `Sig<Tagged<Int, percent>>` is the type of a wire carrying
    /// percentages (e.g., a thermostat opening fraction signal).
    #[test]
    fn sig_of_tagged_is_legible() {
        let pct = Type::Tagged {
            inner: Box::new(Type::Prim(Prim::Int)),
            unit: UnitTag::Simple("percent".into()),
            span: dummy(),
        };
        let wire = pct.into_signal(dummy());
        match &wire {
            Type::Sig(inner, _) => match inner.as_ref() {
                Type::Tagged { inner: i2, unit: UnitTag::Simple(u), .. } => {
                    assert_eq!(u, "percent");
                    assert!(matches!(i2.as_ref(), Type::Prim(Prim::Int)));
                }
                _ => panic!("inner is not Tagged"),
            },
            _ => panic!("wire is not Sig"),
        }
    }
}
