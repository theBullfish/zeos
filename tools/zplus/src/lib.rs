//! Z+ language frontend — bootstrap compiler.
//!
//! Lexer (v2): tokenizes 65/68 `.zp` programs with zero Error tokens; full
//! corpus round-trips byte-for-byte. See `tools/zplus/TOKEN_TAXONOMY.md`.
//!
//! AST: typed enums in `ast` reflecting chord/fastest-N — `Merge` is one
//! node carrying its policy, never a DAG of independent edges. Hand-built
//! fixture for `programs/02_log_monitor.zp` lives in
//! `tests/ast_log_monitor.rs`.
//!
//! Parser: not started.

pub mod ast;
pub mod check;
pub mod lex;
pub mod parse;
pub mod ty;

pub use check::{
    check_module, infer_chain, type_of_literal, types_compatible, TypeEnv, TypeError,
};
pub use lex::{lex, Lexer, Token, TokenKind};
pub use parse::{parse, ParseError};
