//! Integration test: run the type checker on `programs/02_log_monitor.zp`.
//!
//! v1 checker scope: literal typing + Merge arity (the chord rule). Both
//! must pass on the green-light fixture. The merges in 02_log_monitor.zp
//! all use `MergePolicy::All` (the default) plus one `Within(30s)` — none
//! have arity constraints that the inputs violate.

use std::fs;
use zplus::{check_calls, check_flow_connectivity, check_module, parse, TypeEnv};

fn read_log_monitor() -> String {
    for c in ["../../programs/02_log_monitor.zp", "programs/02_log_monitor.zp"] {
        if let Ok(s) = fs::read_to_string(c) {
            return s;
        }
    }
    panic!("could not locate programs/02_log_monitor.zp");
}

#[test]
fn checker_finds_no_errors_on_log_monitor() {
    let src = read_log_monitor();
    let module = parse(&src).expect("parse failed");
    let errors = check_module(&module);
    assert!(
        errors.is_empty(),
        "checker produced errors on log_monitor: {:?}",
        errors
    );
}

/// Named-arg type checking on the green-light fixture — should pass
/// cleanly. Mismatches here mean either the env signature is wrong or
/// the inference is broken.
#[test]
fn call_args_clean_on_log_monitor() {
    let src = read_log_monitor();
    let module = parse(&src).expect("parse failed");
    let env = TypeEnv::default_with_builtins();
    let errors = check_calls(&module, &env);
    assert!(errors.is_empty(), "call-arg errors on log_monitor: {:?}", errors);
}

/// Corpus-wide ratchet for call-arg violations. Currently 0 — the
/// builtin signatures (vault.store/append ttl, tick rate, rate per,
/// baseline window, decay half_life, on_silence within, count within,
/// rewind by, net.listen port) all match how the corpus actually uses
/// them. Any new mismatch fails fast.
#[test]
fn call_args_corpus_ratchet() {
    use std::path::{Path, PathBuf};

    const MAX_CALL_VIOLATIONS: usize = 0;

    fn programs_dir() -> PathBuf {
        for c in ["../../programs", "programs"] {
            let p = Path::new(c);
            if p.is_dir() { return p.to_path_buf(); }
        }
        panic!("could not locate programs/");
    }

    fn collect(dir: &Path, out: &mut Vec<PathBuf>) {
        for entry in fs::read_dir(dir).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                collect(&path, out);
            } else if path.extension().and_then(|s| s.to_str()) == Some("zp") {
                out.push(path);
            }
        }
    }

    let env = TypeEnv::default_with_builtins();
    let root = programs_dir();
    let mut files = Vec::new();
    collect(&root, &mut files);
    let mut violations: Vec<String> = Vec::new();
    for path in &files {
        let src = fs::read_to_string(path).unwrap();
        let module = match parse(&src) {
            Ok(m) => m,
            Err(_) => continue,
        };
        for err in check_calls(&module, &env) {
            violations.push(format!("{}: {}", path.display(), err.message));
        }
    }
    assert!(
        violations.len() <= MAX_CALL_VIOLATIONS,
        "call-arg regression: {} violations (was {}). Sample:\n{}",
        violations.len(),
        MAX_CALL_VIOLATIONS,
        violations.iter().take(5).cloned().collect::<Vec<_>>().join("\n")
    );
}

/// Flow connectivity on the green-light fixture — should pass cleanly
/// with the default builtin env. Mismatches here would be real bugs in
/// either the inference or the env signatures.
#[test]
fn flow_connectivity_clean_on_log_monitor() {
    let src = read_log_monitor();
    let module = parse(&src).expect("parse failed");
    let env = TypeEnv::default_with_builtins();
    let errors = check_flow_connectivity(&module, &env);
    assert!(
        errors.is_empty(),
        "flow connectivity errors on log_monitor: {:?}",
        errors
    );
}

/// Corpus-wide ratchet for Flow connectivity. Started at 2 — both in
/// `quill.zp` lines 326-327 where `opacity(0 -> 1, ...)` and
/// `scale(0.8 -> 1, ...)` overloaded `->` for value-range semantics
/// rather than signal flow. Resolved by rewriting those uses to `..`
/// (the corpus's standard range form). Now zero. Any new mismatch
/// fails fast.
#[test]
fn flow_connectivity_corpus_ratchet() {
    use std::path::{Path, PathBuf};

    const MAX_FLOW_MISMATCHES: usize = 0;

    fn programs_dir() -> PathBuf {
        for c in ["../../programs", "programs"] {
            let p = Path::new(c);
            if p.is_dir() { return p.to_path_buf(); }
        }
        panic!("could not locate programs/");
    }

    fn collect(dir: &Path, out: &mut Vec<PathBuf>) {
        for entry in fs::read_dir(dir).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                collect(&path, out);
            } else if path.extension().and_then(|s| s.to_str()) == Some("zp") {
                out.push(path);
            }
        }
    }

    let env = TypeEnv::default_with_builtins();
    let root = programs_dir();
    let mut files = Vec::new();
    collect(&root, &mut files);
    let mut mismatches: Vec<String> = Vec::new();
    for path in &files {
        let src = fs::read_to_string(path).unwrap();
        let module = match parse(&src) {
            Ok(m) => m,
            Err(_) => continue,
        };
        for err in check_flow_connectivity(&module, &env) {
            mismatches.push(format!("{}: {}", path.display(), err.message));
        }
    }
    assert!(
        mismatches.len() <= MAX_FLOW_MISMATCHES,
        "flow connectivity regression: {} mismatches (was {}). Sample:\n{}",
        mismatches.len(),
        MAX_FLOW_MISMATCHES,
        mismatches.iter().take(5).cloned().collect::<Vec<_>>().join("\n")
    );
}

/// Corpus-wide ratchet for checker violations. Started at 17 (parser
/// limitation: fork-body merge fragments + top-level type-union shorthand
/// produced empty merges). Driven to 0 by extending merge coalescing into
/// fork bodies, into Bind-wrapped statements, and adding type-union eat
/// on `->` Flow RHS for `IDENT | IDENT` shorthand. The corpus now has
/// zero chord-rule violations. New regressions fail fast.
#[test]
fn checker_corpus_violations_are_capped() {
    use std::path::{Path, PathBuf};

    const MAX_VIOLATIONS: usize = 0;

    fn programs_dir() -> PathBuf {
        for c in ["../../programs", "programs"] {
            let p = Path::new(c);
            if p.is_dir() {
                return p.to_path_buf();
            }
        }
        panic!("could not locate programs/");
    }

    fn collect(dir: &Path, out: &mut Vec<PathBuf>) {
        for entry in fs::read_dir(dir).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                collect(&path, out);
            } else if path.extension().and_then(|s| s.to_str()) == Some("zp") {
                out.push(path);
            }
        }
    }

    let root = programs_dir();
    let mut files = Vec::new();
    collect(&root, &mut files);
    let mut violations: Vec<String> = Vec::new();
    for path in &files {
        let src = fs::read_to_string(path).unwrap();
        let module = match parse(&src) {
            Ok(m) => m,
            Err(_) => continue,
        };
        for err in check_module(&module) {
            violations.push(format!("{}: {}", path.display(), err.message));
        }
    }
    assert!(
        violations.len() <= MAX_VIOLATIONS,
        "checker corpus regression: {} violations (was {}). \
         Sample:\n{}",
        violations.len(),
        MAX_VIOLATIONS,
        violations.iter().take(5).cloned().collect::<Vec<_>>().join("\n")
    );
}
