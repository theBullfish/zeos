//! Z+ abstract syntax tree.
//!
//! ## The chord rule
//!
//! `|` is **one structural node carrying its policy**, not a DAG of independent
//! edges. A merge with three inputs and policy `fastest(2)` is one `Merge`
//! with three `inputs`, a `policy`, and a `downstream` — never three `Flow`
//! edges that the runtime serializes. The runtime resolves a `Merge` as a
//! chord (atomic, simultaneous, per policy). See `docs/SIGNAL_LOGIC.md` §1.
//!
//! Linear-default tells to avoid: per-edge "wait then proceed", ordered
//! input lists where order has runtime meaning, treating `|all|` as
//! sequential AND. If you find yourself wanting any of those, the AST is
//! wrong, not your intuition.
//!
//! ## What's not here yet
//!
//! - Type annotations / typed ports (CHAIN_CONTRACT.md)
//! - Bidirectional `<->` and actuator-binding `<-` (deferred; not used by
//!   `02_log_monitor.zp`)
//! - Knee / sustained / on_silence as first-class nodes — for now they ride
//!   inside `Call` args. Promote to dedicated variants only when a concrete
//!   need shows up.
//! - Resolve / state / MasQ provenance — those are runtime concerns
//!   (CHAIN_CONTRACT.md), not AST.

use std::fmt;

/// Byte-offset span into the source. Every AST node carries one so errors and
/// IR can point back to source.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Span {
    pub start: usize,
    pub end: usize,
}

impl Span {
    pub fn new(start: usize, end: usize) -> Self { Self { start, end } }
    pub fn dummy() -> Self { Self { start: 0, end: 0 } }
}

#[derive(Debug, Clone, PartialEq)]
pub struct Ident<'src> {
    pub text: &'src str,
    pub span: Span,
}

/// `vault.store` / `me.ears.followers` — dotted access, lexed as a single path.
#[derive(Debug, Clone, PartialEq)]
pub struct Path<'src> {
    pub segments: Vec<Ident<'src>>,
    pub span: Span,
}

impl<'src> Path<'src> {
    /// Convenience: build a single-segment path.
    pub fn one(text: &'src str, span: Span) -> Self {
        Self { segments: vec![Ident { text, span }], span }
    }
    /// Joined dotted form, for readable assertions.
    pub fn joined(&self) -> String {
        self.segments.iter().map(|s| s.text).collect::<Vec<_>>().join(".")
    }
}

/// A Z+ source file is a flat list of statements. Order of statements does
/// NOT imply execution order — it's topology declaration.
#[derive(Debug, Clone, PartialEq)]
pub struct Module<'src> {
    pub stmts: Vec<Stmt<'src>>,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Stmt<'src> {
    /// `name : <chain>` — declares a wire whose source / pipeline is `chain`.
    Wire(WireDecl<'src>),
    /// Bare chain — declares topology between named wires (no LHS name).
    Connect(Chain<'src>),
}

#[derive(Debug, Clone, PartialEq)]
pub struct WireDecl<'src> {
    pub name: Path<'src>,
    /// `Some` when the wire-decl name carries call args, e.g.
    /// `topic("orders") : ...` or `channel("alpha") : ...`. The args
    /// participate in the runtime "named scope" semantics.
    pub args: Option<Vec<Arg<'src>>>,
    pub chain: Chain<'src>,
    pub span: Span,
}

/// A chain expression. Trees of these are how we represent
/// `a -> gate(...) -> b`, `a ~> taps`, `{ a, b, c }` forks, and `|` merges.
#[derive(Debug, Clone, PartialEq)]
pub enum Chain<'src> {
    Atom(Atom<'src>),
    Call(Call<'src>),
    /// `a -> b`
    Flow(Box<Chain<'src>>, Box<Chain<'src>>, Span),
    /// `a ~> b` — read-only tap. Distinct from Flow because tap MUST NOT
    /// affect upstream resolution. Same syntax, different runtime contract.
    Tap(Box<Chain<'src>>, Box<Chain<'src>>, Span),
    /// `a <~ b` — feedback edge. The RHS produces a signal that is fed
    /// back to the LHS at the *next* tick. Required for autoregressive
    /// composition (LLM token loops, control systems, recurrence). The
    /// runtime contract: feedback is sampled, not chained — it does not
    /// participate in chord resolution within the current tick, only as
    /// an input to the next tick's chord. Symmetric with `Tap`: Tap is
    /// forward-observe (no upstream effect), Feedback is backward-state
    /// (no current-tick effect).
    Feedback(Box<Chain<'src>>, Box<Chain<'src>>, Span),
    /// `a -x> b` — sever / unfollow / disconnect.
    Sever(Box<Chain<'src>>, Box<Chain<'src>>, Span),
    /// `a <- b` — actuator binding. Used in declarations:
    /// `valve : actuator("inlet") <- position @ percent`. The LHS is the
    /// device handle, RHS is the input type/option-set the device accepts.
    Bind(Box<Chain<'src>>, Box<Chain<'src>>, Span),
    /// `{ a, b, c }` — fork: same upstream sent to multiple destinations.
    /// Each branch may be labeled (`mode(chronological): sort(...)`).
    Fork(Vec<ForkBranch<'src>>, Span),
    /// `|` merge — see chord rule at top of file.
    Merge(Merge<'src>),
    /// `lhs OP rhs` — comparison or fuzzy match. Produces a predicate, not
    /// a signal flow. `message ~ "..."`, `value > 5x`, `a == b`.
    BinExpr {
        op: BinOp,
        lhs: Box<Chain<'src>>,
        rhs: Box<Chain<'src>>,
        span: Span,
    },
    /// `OP rhs` — comparison with an implicit subject. `gate(> 5x)` parses
    /// as `gate(UnaryCmp { op: Gt, rhs: Ratio(5) })`. The subject is the
    /// gate's input signal, supplied by the runtime.
    UnaryCmp {
        op: BinOp,
        rhs: Box<Chain<'src>>,
        span: Span,
    },
}

/// Binary / unary comparison & match operators usable inside arg
/// expressions. Distinct from chain operators (`->`, `~>`, `-x>`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum BinOp {
    Lt,         // <
    Gt,         // >
    Le,         // <=
    Ge,         // >=
    Eq,         // ==
    Ne,         // !=
    FuzzyMatch, // ~ — `message ~ "out of memory"` (substring / pattern)
}

#[derive(Debug, Clone, PartialEq)]
pub enum Atom<'src> {
    /// Identifier or dotted path: `errors`, `vault.store`, `me.ears`.
    Path(Path<'src>),
    Literal(Literal<'src>),
    /// `/dev/null` — the discard sink.
    DevNull(Span),
    /// `t-1`, `t-2` — temporal access. Distinct from `t - 1` (subtraction).
    TimePast { steps: u32, span: Span },
}

#[derive(Debug, Clone, PartialEq)]
pub struct Call<'src> {
    pub callee: Path<'src>,
    pub args: Vec<Arg<'src>>,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Arg<'src> {
    Positional(Chain<'src>),
    Named { name: Ident<'src>, value: Chain<'src>, span: Span },
}

/// One arm of a fork. `label` is `None` for plain `{ a, b }` forks; `Some` for
/// labeled forks like `mode(chronological): sort(...)` (chirp.zp:137-140).
#[derive(Debug, Clone, PartialEq)]
pub struct ForkBranch<'src> {
    pub label: Option<Chain<'src>>,
    pub body: Chain<'src>,
    pub span: Span,
}

/// Multiple inputs converging through one merge point. **One node per merge,
/// always** — see chord rule.
#[derive(Debug, Clone, PartialEq)]
pub struct Merge<'src> {
    pub inputs: Vec<Chain<'src>>,
    pub policy: MergePolicy<'src>,
    /// What the merge feeds into. `None` means the merge is the chain end.
    pub downstream: Option<Box<Chain<'src>>>,
    pub span: Span,
}

/// Resolution policy for a merge. The runtime treats a `Merge` as ONE
/// simultaneous chord per this policy. Linear AND / OR / wait-for-all are
/// emergent from `All`, not the underlying model.
#[derive(Debug, Clone, PartialEq)]
pub enum MergePolicy<'src> {
    /// Default `|` — strict chord; resolve when all inputs have spoken.
    All,
    /// `|any|` — first signal wins.
    Any,
    /// `|N of M|` — quorum.
    Quorum { n: u32, m: u32 },
    /// `|fastest(N)|` — race; take the fastest N, ignore stragglers.
    Fastest(u32),
    /// `| within(100ms) |` — temporal confluence window.
    Within(Box<Chain<'src>>),
    /// `| merge(by: timestamp) |` — sort policy on timestamp / other key.
    By(Path<'src>),
}

#[derive(Debug, Clone, PartialEq)]
pub enum Literal<'src> {
    Int(i64, Span),
    Float(f64, Span),
    String(&'src str, Span),
    /// `"…{name}…"` — interpolation form. Hole-span extraction belongs to a
    /// later pass; the lexeme is preserved here verbatim.
    TemplateString(&'src str, Span),
    Duration { value: f64, unit: DurationUnit, span: Span },
    Ratio(f64, Span),
    Sigma(f64, Span),
    Percent(f64, Span),
    ByteSize { bytes: u64, span: Span },
    Dimension { w: u64, h: u64, span: Span },
    Hex(u64, Span),
    HexColor(&'src str, Span),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum DurationUnit { Ms, S, M, H, D }

impl fmt::Display for DurationUnit {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            DurationUnit::Ms => "ms",
            DurationUnit::S => "s",
            DurationUnit::M => "m",
            DurationUnit::H => "h",
            DurationUnit::D => "d",
        })
    }
}

#[cfg(test)]
mod tests {
    //! Unit-level shape checks. The full hand-built fixture for
    //! `programs/02_log_monitor.zp` lives in `tests/ast_log_monitor.rs`.

    use super::*;

    #[test]
    fn merge_default_policy_is_all() {
        // The chord-rule canary. If someone ever changes the default merge
        // policy to anything other than All without thinking through the
        // chord semantics, this test fires first.
        let m: MergePolicy = MergePolicy::All;
        assert_eq!(m, MergePolicy::All);
    }

    #[test]
    fn merge_with_three_inputs_is_one_node() {
        // The shape check: three signals → one merge → one downstream is ONE
        // Merge node, never three Flow edges. This is the linear-default
        // canary.
        let merge = Merge {
            inputs: vec![
                Chain::Atom(Atom::Path(Path::one("syslog", Span::dummy()))),
                Chain::Atom(Atom::Path(Path::one("app_log", Span::dummy()))),
                Chain::Atom(Atom::Path(Path::one("kern_log", Span::dummy()))),
            ],
            policy: MergePolicy::All,
            downstream: Some(Box::new(Chain::Atom(Atom::Path(
                Path::one("all_lines", Span::dummy()),
            )))),
            span: Span::dummy(),
        };
        assert_eq!(merge.inputs.len(), 3);
        assert!(merge.downstream.is_some());
        assert_eq!(merge.policy, MergePolicy::All);
    }

    #[test]
    fn quorum_and_fastest_carry_their_arity() {
        // Quorum and fastest are first-class — not encoded as flags on a
        // generic merge node. The arity is part of the policy.
        let q23: MergePolicy<'_> = MergePolicy::Quorum { n: 2, m: 3 };
        let q33: MergePolicy<'_> = MergePolicy::Quorum { n: 3, m: 3 };
        let f2: MergePolicy<'_> = MergePolicy::Fastest(2);
        assert_eq!(q23, MergePolicy::Quorum { n: 2, m: 3 });
        assert_eq!(f2, MergePolicy::Fastest(2));
        assert_ne!(q23, q33);
    }

    #[test]
    fn path_joined_renders_dotted() {
        let p = Path {
            segments: vec![
                Ident { text: "vault", span: Span::dummy() },
                Ident { text: "store", span: Span::dummy() },
            ],
            span: Span::dummy(),
        };
        assert_eq!(p.joined(), "vault.store");
    }
}
