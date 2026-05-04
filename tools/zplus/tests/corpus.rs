//! Corpus-wide smoke tests.
//!
//! Two assertions across every `programs/**/*.zp` file:
//!
//!  1. **Round-trip:** the concatenation of token texts equals the source
//!     byte-for-byte. This is independent of whether the lexer recognized
//!     every token — `Error` tokens still carry their text — so it's a
//!     hard guarantee that the lexer never loses input.
//!
//!  2. **Zero-error allowlist:** files known to fully tokenize cleanly
//!     are asserted to produce zero `Error` tokens. Files that exercise
//!     deferred lexical surface (the `↑` heat operator, `─...>` long-form
//!     arrows, `\"` string escapes) are tracked in `EXPECTED_ERRORS` with
//!     the count we expect — so a regression that breaks a "clean" file
//!     fails fast, while the deferred items don't block CI.
//!
//! When a v3 lexer feature ships and brings a file's error count to zero,
//! delete its entry here.

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

use zplus::{lex, TokenKind};

/// Files known to use lexical surface the v2 lexer doesn't yet handle.
/// Keep this list sorted; entries should match exactly the count produced
/// by the current lexer so unrelated regressions are caught immediately.
fn expected_errors() -> HashMap<&'static str, usize> {
    HashMap::from([
        // ↑ heat operator — `programs/chirp.zp:47` `post.heat↑`
        ("chirp.zp", 1),
        // String escapes `\"` in LSP-style snippet templates
        ("derez/forge_ide.zp", 4),
        // `─...>` decorative long-form arrow
        ("goya_fleet.zp", 20),
    ])
}

fn programs_dir() -> PathBuf {
    for c in ["../../programs", "programs"] {
        let p = Path::new(c);
        if p.is_dir() {
            return p.to_path_buf();
        }
    }
    panic!("could not locate programs/ directory")
}

fn collect_zp_files(dir: &Path, out: &mut Vec<PathBuf>) {
    for entry in fs::read_dir(dir).expect("read_dir") {
        let entry = entry.expect("dir entry");
        let path = entry.path();
        if path.is_dir() {
            collect_zp_files(&path, out);
        } else if path.extension().and_then(|s| s.to_str()) == Some("zp") {
            out.push(path);
        }
    }
}

/// Path relative to the programs/ root, with forward slashes — used as the
/// allowlist key.
fn relative_key(path: &Path, root: &Path) -> String {
    path.strip_prefix(root)
        .unwrap_or(path)
        .components()
        .map(|c| c.as_os_str().to_string_lossy().into_owned())
        .collect::<Vec<_>>()
        .join("/")
}

#[test]
fn every_program_round_trips() {
    let root = programs_dir();
    let mut files = Vec::new();
    collect_zp_files(&root, &mut files);
    assert!(!files.is_empty(), "no .zp files found under {:?}", root);

    let mut failures = Vec::new();
    for path in &files {
        let src = fs::read_to_string(path).expect("read source");
        let reconstructed: String = lex(&src).iter().map(|t| t.text).collect();
        if reconstructed != src {
            failures.push(format!("{}: byte mismatch", path.display()));
        }
    }
    assert!(
        failures.is_empty(),
        "{} file(s) failed round-trip:\n{}",
        failures.len(),
        failures.join("\n")
    );
}

#[test]
fn error_counts_match_allowlist() {
    let root = programs_dir();
    let mut files = Vec::new();
    collect_zp_files(&root, &mut files);

    let allowlist = expected_errors();
    let mut surprises = Vec::new();

    for path in &files {
        let key = relative_key(path, &root);
        let src = fs::read_to_string(path).expect("read source");
        let actual = lex(&src).iter().filter(|t| t.kind == TokenKind::Error).count();
        let expected = allowlist.get(key.as_str()).copied().unwrap_or(0);
        if actual != expected {
            surprises.push(format!(
                "{}: expected {} error tokens, got {}",
                key, expected, actual
            ));
        }
    }

    assert!(
        surprises.is_empty(),
        "lexer error-count regressions:\n{}",
        surprises.join("\n")
    );
}
