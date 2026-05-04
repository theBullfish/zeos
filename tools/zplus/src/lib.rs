//! Z+ language frontend — bootstrap compiler.
//!
//! v1 lexer scope: enough to tokenize `programs/02_log_monitor.zp` cleanly.
//! See `tools/zplus/TOKEN_TAXONOMY.md` for the full token surface.

pub mod lex;

pub use lex::{lex, Lexer, Token, TokenKind};
