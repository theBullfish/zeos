//! Integration test: tokenize `programs/03_http_server.zp` end-to-end.
//!
//! Same shape as `log_monitor.rs` — proves the v2 token kinds (and v1)
//! work on a second real-program fixture. `03_http_server.zp` exercises
//! `path ~ "/api/*"` (fuzzy + glob in string), `respond()` calls,
//! middleware-style pipelines.

use std::fs;
use zplus::{lex, TokenKind};

fn read_file() -> String {
    let candidates = [
        "../../programs/03_http_server.zp",
        "programs/03_http_server.zp",
    ];
    for c in candidates {
        if let Ok(s) = fs::read_to_string(c) {
            return s;
        }
    }
    panic!("could not locate programs/03_http_server.zp");
}

#[test]
fn no_error_tokens() {
    let src = read_file();
    let tokens = lex(&src);
    let errors: Vec<_> = tokens.iter().filter(|t| t.kind == TokenKind::Error).collect();
    assert!(
        errors.is_empty(),
        "lexer produced Error tokens: {:#?}",
        errors.iter().take(5).collect::<Vec<_>>()
    );
}

#[test]
fn round_trip() {
    let src = read_file();
    let reconstructed: String = lex(&src).iter().map(|t| t.text).collect();
    assert_eq!(reconstructed, src, "round-trip byte mismatch");
}
