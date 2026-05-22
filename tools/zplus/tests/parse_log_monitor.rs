//! Integration test: parse the entire `programs/02_log_monitor.zp`
//! end-to-end. The file is the green-light target for the v1 parser
//! (TOKEN_TAXONOMY.md §16, STATE.md "Next up").
//!
//! Asserts:
//!  1. `parse(src)` returns Ok.
//!  2. The module's first three statements are wire decls for
//!     `syslog`, `app_log`, `kern_log`.
//!  3. The vertical merge that follows lowers to ONE `Merge` node
//!     (chord-rule canary).
//!  4. The `oom_alert -> | within(30s) | -> ...` block lowers to one
//!     Merge with `MergePolicy::Within(...)`.
//!  5. Tap arrows produce `Chain::Tap`.
//!  6. The total statement count is in a sensible range — too few means
//!     coalescing went wrong and lost statements; too many means it
//!     failed to coalesce and inflated them.

use std::fs;
use zplus::ast::{Atom, Chain, MergePolicy, Stmt};
use zplus::parse;

fn read_log_monitor() -> String {
    for c in ["../../programs/02_log_monitor.zp", "programs/02_log_monitor.zp"] {
        if let Ok(s) = fs::read_to_string(c) {
            return s;
        }
    }
    panic!("could not locate programs/02_log_monitor.zp");
}

#[test]
fn parses_without_error() {
    let src = read_log_monitor();
    let module = parse(&src).expect("parse failed");
    assert!(!module.stmts.is_empty(), "module has zero statements");
}

#[test]
fn first_three_stmts_are_source_wire_decls() {
    let src = read_log_monitor();
    let m = parse(&src).expect("parse failed");
    for (i, name) in ["syslog", "app_log", "kern_log"].iter().enumerate() {
        match &m.stmts[i] {
            Stmt::Wire(w) => assert_eq!(w.name.joined(), *name),
            other => panic!("stmt[{}] is not Wire: {:?}", i, other),
        }
    }
}

#[test]
fn vertical_all_lines_merge_is_one_node() {
    let src = read_log_monitor();
    let m = parse(&src).expect("parse failed");
    // Find the merge whose downstream is `all_lines`.
    let merge = m
        .stmts
        .iter()
        .find_map(|s| match s {
            Stmt::Connect(Chain::Merge(merge)) => match merge.downstream.as_deref() {
                Some(Chain::Atom(Atom::Path(p))) if p.joined() == "all_lines" => Some(merge),
                _ => None,
            },
            _ => None,
        })
        .expect("no merge with downstream 'all_lines' found");
    assert_eq!(merge.inputs.len(), 3, "all_lines merge should have 3 inputs");
    assert_eq!(merge.policy, MergePolicy::All);
}

#[test]
fn within_30s_merge_has_policy_and_two_inputs() {
    let src = read_log_monitor();
    let m = parse(&src).expect("parse failed");
    // Find the merge with Within(...) policy.
    let merge = m
        .stmts
        .iter()
        .find_map(|s| match s {
            Stmt::Connect(Chain::Merge(merge)) => match &merge.policy {
                MergePolicy::Within(_) => Some(merge),
                _ => None,
            },
            _ => None,
        })
        .expect("no merge with Within policy found");
    assert_eq!(merge.inputs.len(), 2, "expected oom_alert + conn_alert");
    assert!(merge.downstream.is_some(), "Within merge should have a downstream");
}

#[test]
fn taps_are_distinct_from_flows() {
    let src = read_log_monitor();
    let m = parse(&src).expect("parse failed");
    let tap_count = m.stmts.iter().filter(|s| match s {
        Stmt::Connect(c) => contains_tap(c),
        _ => false,
    }).count();
    // The dashboard section uses `~>` heavily — at least 3 tap statements.
    assert!(tap_count >= 3, "expected >= 3 tap statements, got {}", tap_count);
}

fn contains_tap(c: &Chain<'_>) -> bool {
    match c {
        Chain::Tap(_, _, _) => true,
        Chain::Flow(l, r, _) | Chain::Sever(l, r, _) => contains_tap(l) || contains_tap(r),
        _ => false,
    }
}

#[test]
fn statement_count_is_sensible() {
    let src = read_log_monitor();
    let m = parse(&src).expect("parse failed");
    // 02_log_monitor.zp has roughly 25 logical statements after merge
    // coalescing. Anything outside [15, 35] indicates coalescing went
    // wrong (too aggressive or too lax).
    let n = m.stmts.len();
    assert!(
        (15..=35).contains(&n),
        "statement count {} is outside expected range [15, 35]",
        n
    );
}
