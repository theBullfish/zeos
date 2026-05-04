//! Integration test: tokenize `programs/02_log_monitor.zp` end-to-end.
//!
//! Asserts:
//!  1. The lexer produces no `Error` tokens against this real-program input.
//!  2. The token stream round-trips back to the original source byte-for-byte.
//!  3. Sanity checks: the file uses every operator class the v1 lexer claims
//!     to support (Flow, Tap, Pipe, Tilde, Gt, Duration, Ratio, String,
//!     LineComment, Ident).
//!
//! See `tools/zplus/TOKEN_TAXONOMY.md` §16 for why this file is the green-light
//! target.

use std::fs;
use zplus::{lex, TokenKind};

fn read_log_monitor() -> String {
    // Path is relative to the package root (where Cargo runs tests).
    let candidates = [
        "../../programs/02_log_monitor.zp",
        "programs/02_log_monitor.zp",
    ];
    for c in candidates {
        if let Ok(s) = fs::read_to_string(c) {
            return s;
        }
    }
    panic!("could not locate programs/02_log_monitor.zp from {:?}", std::env::current_dir());
}

#[test]
fn no_error_tokens() {
    let src = read_log_monitor();
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
    let src = read_log_monitor();
    let reconstructed: String = lex(&src).iter().map(|t| t.text).collect();
    assert_eq!(reconstructed, src, "round-trip byte mismatch");
}

#[test]
fn covers_expected_token_classes() {
    let src = read_log_monitor();
    let kinds: std::collections::HashSet<TokenKind> =
        lex(&src).into_iter().map(|t| t.kind).collect();

    let required = [
        TokenKind::Flow,
        TokenKind::Tap,
        TokenKind::Pipe,
        TokenKind::Tilde,
        TokenKind::Gt,
        TokenKind::Duration,
        TokenKind::Ratio,
        TokenKind::String,
        TokenKind::LineComment,
        TokenKind::Ident,
        TokenKind::Colon,
        TokenKind::Comma,
        TokenKind::LParen,
        TokenKind::RParen,
        TokenKind::Dot,
    ];
    for r in required {
        assert!(kinds.contains(&r), "expected token class {:?} not produced for log monitor", r);
    }
}

#[test]
fn nontrivial_token_count() {
    let src = read_log_monitor();
    let tokens = lex(&src);
    // 02_log_monitor.zp is ~97 lines including comments; should produce
    // hundreds of tokens. Lower bound is conservative.
    assert!(
        tokens.len() > 200,
        "expected >200 tokens for log monitor, got {}",
        tokens.len()
    );
}
