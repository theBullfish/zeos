//! Integration test: run the type checker on `programs/02_log_monitor.zp`.
//!
//! v1 checker scope: literal typing + Merge arity (the chord rule). Both
//! must pass on the green-light fixture. The merges in 02_log_monitor.zp
//! all use `MergePolicy::All` (the default) plus one `Within(30s)` — none
//! have arity constraints that the inputs violate.

use std::fs;
use zplus::{check_module, parse};

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

/// Corpus-wide ratchet for checker violations. Currently 17 — all
/// `merge has no inputs` cases caused by a known parser limitation:
/// fork-body merge fragments (e.g. `13_payment_processor.zp:42-44`,
/// `11_home_automation.zp` × 6) aren't coalesced because the
/// vertical-merge coalescer in `parse_module` only runs at the top
/// level. Fixing the parser will drop this count to zero. Until then,
/// the test asserts the count doesn't INCREASE — so a real new
/// chord-rule violation (e.g. a `Quorum 2 of 3` with one input) fires
/// immediately.
#[test]
fn checker_corpus_violations_are_capped() {
    use std::path::{Path, PathBuf};

    const MAX_VIOLATIONS: usize = 17;

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
    // Also assert all current violations are the known-issue kind.
    for v in &violations {
        assert!(
            v.contains("merge has no inputs"),
            "unexpected violation kind: {}",
            v
        );
    }
}
