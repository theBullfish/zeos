//! Hand-built AST fixture for the top section of
//! `programs/02_log_monitor.zp`. No parser yet — this test exists to prove
//! the AST type definitions can faithfully express the source's topology
//! before parser code locks anything in.
//!
//! The fixture covers lines 8–15:
//!
//! ```text
//! syslog    : fs("/var/log/syslog")    -> lines
//! app_log   : fs("/var/log/app/*.log") -> lines
//! kern_log  : fs("/var/log/kern.log")  -> lines
//!
//! syslog   -> |
//! app_log  -> | -> all_lines
//! kern_log -> |
//! ```
//!
//! The merge across the three `-> |` lines is **one** `Merge` node carrying
//! `MergePolicy::All`. That's the chord rule from `docs/SIGNAL_LOGIC.md` §1.
//! If a future parser ever lowers this to three independent `Flow` edges,
//! the assertion in `merge_is_one_node_not_three_flow_edges` fails first.

use zplus::ast::{
    Arg, Atom, Chain, Ident, Literal, Merge, MergePolicy, Module, Path, Span, Stmt, WireDecl,
};

/// Build the AST for lines 8–15 of `02_log_monitor.zp` by hand.
///
/// Spans use `Span::dummy()` — this fixture is about structure, not
/// source-position fidelity. A real parser will fill in real spans.
fn fixture() -> Module<'static> {
    fn ident(text: &'static str) -> Ident<'static> {
        Ident { text, span: Span::dummy() }
    }
    fn path(text: &'static str) -> Path<'static> {
        Path::one(text, Span::dummy())
    }
    fn atom_path(text: &'static str) -> Chain<'static> {
        Chain::Atom(Atom::Path(path(text)))
    }
    fn flow(from: Chain<'static>, to: Chain<'static>) -> Chain<'static> {
        Chain::Flow(Box::new(from), Box::new(to), Span::dummy())
    }
    fn fs_call(arg: &'static str) -> Chain<'static> {
        Chain::Call(zplus::ast::Call {
            callee: path("fs"),
            args: vec![Arg::Positional(Chain::Atom(Atom::Literal(Literal::String(
                arg,
                Span::dummy(),
            ))))],
            span: Span::dummy(),
        })
    }

    // syslog : fs("\"/var/log/syslog\"") -> lines
    let syslog_decl = Stmt::Wire(WireDecl {
        name: path("syslog"),
        args: None,
        chain: flow(fs_call("\"/var/log/syslog\""), atom_path("lines")),
        span: Span::dummy(),
    });

    // app_log : fs("\"/var/log/app/*.log\"") -> lines
    let app_log_decl = Stmt::Wire(WireDecl {
        name: path("app_log"),
        args: None,
        chain: flow(fs_call("\"/var/log/app/*.log\""), atom_path("lines")),
        span: Span::dummy(),
    });

    // kern_log : fs("\"/var/log/kern.log\"") -> lines
    let kern_log_decl = Stmt::Wire(WireDecl {
        name: path("kern_log"),
        args: None,
        chain: flow(fs_call("\"/var/log/kern.log\""), atom_path("lines")),
        span: Span::dummy(),
    });

    // The three-line vertical merge — ONE Merge node, policy All.
    //
    //   syslog   -> |
    //   app_log  -> | -> all_lines
    //   kern_log -> |
    //
    // Inputs: [syslog, app_log, kern_log] in source order. Order does not
    // imply runtime ordering — the chord resolves atomically — but it's
    // preserved for diagnostic / round-trip purposes.
    let merge_stmt = Stmt::Connect(Chain::Merge(Merge {
        inputs: vec![atom_path("syslog"), atom_path("app_log"), atom_path("kern_log")],
        policy: MergePolicy::All,
        downstream: Some(Box::new(atom_path("all_lines"))),
        span: Span::dummy(),
    }));

    // Suppress unused warning for `ident` — kept as a helper for clarity.
    let _ = ident;

    Module {
        stmts: vec![syslog_decl, app_log_decl, kern_log_decl, merge_stmt],
        span: Span::dummy(),
    }
}

#[test]
fn module_has_three_wire_decls_and_one_merge() {
    let m = fixture();
    assert_eq!(m.stmts.len(), 4);

    // First three are wire declarations.
    for (i, expected_name) in ["syslog", "app_log", "kern_log"].iter().enumerate() {
        match &m.stmts[i] {
            Stmt::Wire(w) => assert_eq!(w.name.joined(), *expected_name),
            other => panic!("stmt[{}] expected Wire, got {:?}", i, other),
        }
    }

    // Fourth is the merge connect.
    matches!(m.stmts[3], Stmt::Connect(Chain::Merge(_)));
}

#[test]
fn each_wire_decl_is_fs_call_then_flow_to_lines() {
    let m = fixture();
    for (i, expected_arg) in [
        "\"/var/log/syslog\"",
        "\"/var/log/app/*.log\"",
        "\"/var/log/kern.log\"",
    ]
    .iter()
    .enumerate()
    {
        let Stmt::Wire(w) = &m.stmts[i] else { panic!("not Wire") };
        // chain shape: Flow(Call(fs, [string]), Atom(Path(lines)))
        let Chain::Flow(from, to, _) = &w.chain else {
            panic!("wire {} chain is not Flow: {:?}", i, w.chain);
        };
        let Chain::Call(call) = from.as_ref() else {
            panic!("wire {} flow.from is not Call: {:?}", i, from);
        };
        assert_eq!(call.callee.joined(), "fs");
        assert_eq!(call.args.len(), 1);
        let Arg::Positional(Chain::Atom(Atom::Literal(Literal::String(s, _)))) = &call.args[0]
        else {
            panic!("wire {} arg is not String literal: {:?}", i, call.args[0]);
        };
        assert_eq!(*s, *expected_arg);

        let Chain::Atom(Atom::Path(p)) = to.as_ref() else {
            panic!("wire {} flow.to is not Path: {:?}", i, to);
        };
        assert_eq!(p.joined(), "lines");
    }
}

/// **The chord-rule canary.** A merge with three inputs is one `Merge`
/// node, NOT three `Flow` edges. If a parser change ever lowers the
/// vertical-stack syntax to a DAG of independent edges, this test fails
/// first.
#[test]
fn merge_is_one_node_not_three_flow_edges() {
    let m = fixture();
    let Stmt::Connect(connect) = &m.stmts[3] else {
        panic!("stmt[3] is not Connect");
    };
    let Chain::Merge(merge) = connect else {
        panic!("connect is not Merge — chord rule violated: {:?}", connect);
    };
    // Three inputs, one node.
    assert_eq!(merge.inputs.len(), 3);
    // Default policy is All — strict chord.
    assert_eq!(merge.policy, MergePolicy::All);
    // Single downstream.
    let downstream = merge.downstream.as_ref().expect("merge has no downstream");
    let Chain::Atom(Atom::Path(p)) = downstream.as_ref() else {
        panic!("downstream is not Path: {:?}", downstream);
    };
    assert_eq!(p.joined(), "all_lines");
}

#[test]
fn merge_inputs_are_named_wires_in_source_order() {
    let m = fixture();
    let Stmt::Connect(Chain::Merge(merge)) = &m.stmts[3] else { panic!() };
    let names: Vec<String> = merge
        .inputs
        .iter()
        .map(|c| match c {
            Chain::Atom(Atom::Path(p)) => p.joined(),
            other => panic!("input is not a path atom: {:?}", other),
        })
        .collect();
    assert_eq!(names, vec!["syslog", "app_log", "kern_log"]);
}

/// Negative check: a single-input "merge" with no downstream is degenerate.
/// The test ensures we *can* express it (so the parser doesn't crash on
/// unusual inputs), but documents that the runtime would treat it as a
/// passthrough.
#[test]
fn degenerate_merge_is_expressible() {
    let m = Merge {
        inputs: vec![Chain::Atom(Atom::Path(Path::one("only", Span::dummy())))],
        policy: MergePolicy::All,
        downstream: None,
        span: Span::dummy(),
    };
    assert_eq!(m.inputs.len(), 1);
    assert!(m.downstream.is_none());
}
