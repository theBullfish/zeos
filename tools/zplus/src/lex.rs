//! Z+ lexer (v1).
//!
//! Token surface tracks `tools/zplus/TOKEN_TAXONOMY.md`. v1 covers what
//! `programs/02_log_monitor.zp` exercises: arrows, identifiers, integers,
//! float, duration / ratio / sigma / percent suffixed numerics, strings,
//! line comments, the punctuation set, and comparison operators. Hex
//! literals, hex colors, dimensions, byte sizes, template-string
//! interpolation, `t-1` / `t-2`, `/dev/null` and `↑` are deferred to v2.
//!
//! Unicode handling: source is UTF-8. ASCII fast path; `σ` (U+03C3, two
//! bytes in UTF-8) recognized as a numeric suffix.

use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum TokenKind {
    // Trivia
    Whitespace,
    Newline,
    LineComment,

    // Literals / identifiers
    Ident,
    Int,
    Float,
    Duration,    // 30d, 5m, 300ms
    Ratio,       // 5x, 2.5x
    Sigma,       // 2σ, -2σ
    PercentLit,  // 10%
    String,      // "..."

    // Multi-char arrows
    Sever,       // -x>
    Flow,        // ->
    Tap,         // ~>
    Bidir,       // <->
    BindLeft,    // <-

    // Comparisons
    Le,          // <=
    Ge,          // >=
    EqEq,        // ==
    Ne,          // !=
    Lt,          // <
    Gt,          // >

    // Punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Colon,
    Comma,
    Dot,
    At,
    Question,
    Pipe,
    Tilde,
    Eq,          // =

    // Arithmetic
    Plus,
    Minus,
    Star,
    Slash,
    Percent,

    // Sentinel: lexer could not recognize the input here.
    Error,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Token<'a> {
    pub kind: TokenKind,
    pub start: usize,
    pub end: usize,
    pub text: &'a str,
}

impl<'a> fmt::Display for Token<'a> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}({:?}) @ {}..{}", self.kind, self.text, self.start, self.end)
    }
}

/// Tokenize `src` end-to-end. Trivia tokens (whitespace, newlines, line
/// comments) are included so the stream can round-trip back to source.
pub fn lex(src: &str) -> Vec<Token<'_>> {
    let mut lx = Lexer::new(src);
    let mut out = Vec::new();
    while let Some(tok) = lx.next_token() {
        out.push(tok);
    }
    out
}

pub struct Lexer<'a> {
    src: &'a str,
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Lexer<'a> {
    pub fn new(src: &'a str) -> Self {
        Self { src, bytes: src.as_bytes(), pos: 0 }
    }

    pub fn next_token(&mut self) -> Option<Token<'a>> {
        if self.pos >= self.bytes.len() {
            return None;
        }
        let start = self.pos;
        let b = self.bytes[start];

        // Newlines first (significant per taxonomy §12)
        if b == b'\n' {
            self.pos += 1;
            return Some(self.tok(TokenKind::Newline, start));
        }
        if b == b'\r' {
            // \r or \r\n collapse into one Newline token
            self.pos += 1;
            if self.peek() == Some(b'\n') {
                self.pos += 1;
            }
            return Some(self.tok(TokenKind::Newline, start));
        }

        // Other whitespace
        if b == b' ' || b == b'\t' {
            while let Some(c) = self.peek() {
                if c == b' ' || c == b'\t' {
                    self.pos += 1;
                } else {
                    break;
                }
            }
            return Some(self.tok(TokenKind::Whitespace, start));
        }

        // Line comment: // ... up to (but not including) newline
        if b == b'/' && self.peek_at(1) == Some(b'/') {
            self.pos += 2;
            while let Some(c) = self.peek() {
                if c == b'\n' || c == b'\r' {
                    break;
                }
                self.pos += 1;
            }
            return Some(self.tok(TokenKind::LineComment, start));
        }

        // Multi-char operators — order matters (taxonomy §14.1)
        // -x> must be checked before -> ; <-> before <- ; etc.
        if b == b'-' {
            if self.peek_at(1) == Some(b'x') && self.peek_at(2) == Some(b'>') {
                self.pos += 3;
                return Some(self.tok(TokenKind::Sever, start));
            }
            if self.peek_at(1) == Some(b'>') {
                self.pos += 2;
                return Some(self.tok(TokenKind::Flow, start));
            }
            self.pos += 1;
            return Some(self.tok(TokenKind::Minus, start));
        }
        if b == b'<' {
            if self.peek_at(1) == Some(b'-') && self.peek_at(2) == Some(b'>') {
                self.pos += 3;
                return Some(self.tok(TokenKind::Bidir, start));
            }
            if self.peek_at(1) == Some(b'-') {
                self.pos += 2;
                return Some(self.tok(TokenKind::BindLeft, start));
            }
            if self.peek_at(1) == Some(b'=') {
                self.pos += 2;
                return Some(self.tok(TokenKind::Le, start));
            }
            self.pos += 1;
            return Some(self.tok(TokenKind::Lt, start));
        }
        if b == b'>' {
            if self.peek_at(1) == Some(b'=') {
                self.pos += 2;
                return Some(self.tok(TokenKind::Ge, start));
            }
            self.pos += 1;
            return Some(self.tok(TokenKind::Gt, start));
        }
        if b == b'~' {
            if self.peek_at(1) == Some(b'>') {
                self.pos += 2;
                return Some(self.tok(TokenKind::Tap, start));
            }
            self.pos += 1;
            return Some(self.tok(TokenKind::Tilde, start));
        }
        if b == b'=' {
            if self.peek_at(1) == Some(b'=') {
                self.pos += 2;
                return Some(self.tok(TokenKind::EqEq, start));
            }
            self.pos += 1;
            return Some(self.tok(TokenKind::Eq, start));
        }
        if b == b'!' && self.peek_at(1) == Some(b'=') {
            self.pos += 2;
            return Some(self.tok(TokenKind::Ne, start));
        }

        // String literal — no escape handling in v1 (corpus has no escapes)
        if b == b'"' {
            self.pos += 1;
            while let Some(c) = self.peek() {
                self.pos += 1;
                if c == b'"' {
                    return Some(self.tok(TokenKind::String, start));
                }
                if c == b'\n' {
                    // Unterminated string — bail out as Error spanning what we read.
                    return Some(Token {
                        kind: TokenKind::Error,
                        start,
                        end: self.pos,
                        text: &self.src[start..self.pos],
                    });
                }
            }
            return Some(Token {
                kind: TokenKind::Error,
                start,
                end: self.pos,
                text: &self.src[start..self.pos],
            });
        }

        // Numeric literal (with possible suffix: ms|s|m|h|d|x|%|σ)
        if b.is_ascii_digit() {
            return Some(self.lex_number(start));
        }

        // Identifier
        if is_ident_start(b) {
            while let Some(c) = self.peek() {
                if is_ident_continue(c) {
                    self.pos += 1;
                } else {
                    break;
                }
            }
            return Some(self.tok(TokenKind::Ident, start));
        }

        // Single-char punctuation
        let kind = match b {
            b'(' => TokenKind::LParen,
            b')' => TokenKind::RParen,
            b'{' => TokenKind::LBrace,
            b'}' => TokenKind::RBrace,
            b'[' => TokenKind::LBracket,
            b']' => TokenKind::RBracket,
            b':' => TokenKind::Colon,
            b',' => TokenKind::Comma,
            b'.' => TokenKind::Dot,
            b'@' => TokenKind::At,
            b'?' => TokenKind::Question,
            b'|' => TokenKind::Pipe,
            b'+' => TokenKind::Plus,
            b'*' => TokenKind::Star,
            b'/' => TokenKind::Slash,
            b'%' => TokenKind::Percent,
            _ => {
                // Unknown byte — emit Error token for the next unicode char so we
                // don't split a multi-byte sequence.
                let advance = utf8_char_len(b);
                self.pos += advance;
                return Some(Token {
                    kind: TokenKind::Error,
                    start,
                    end: self.pos,
                    text: &self.src[start..self.pos],
                });
            }
        };
        self.pos += 1;
        Some(self.tok(kind, start))
    }

    fn lex_number(&mut self, start: usize) -> Token<'a> {
        // integer part
        while let Some(c) = self.peek() {
            if c.is_ascii_digit() {
                self.pos += 1;
            } else {
                break;
            }
        }
        // optional fractional part — but only if followed by a digit, so that
        // `1.5x` is a Float-then-suffix while `vault.store` keeps the `.` as Dot.
        let mut is_float = false;
        if self.peek() == Some(b'.') && self.peek_at(1).map_or(false, |c| c.is_ascii_digit()) {
            is_float = true;
            self.pos += 1; // .
            while let Some(c) = self.peek() {
                if c.is_ascii_digit() {
                    self.pos += 1;
                } else {
                    break;
                }
            }
        }

        // suffix detection (order matters: ms before m)
        // duration suffixes
        if self.peek() == Some(b'm') && self.peek_at(1) == Some(b's') {
            self.pos += 2;
            return self.tok(TokenKind::Duration, start);
        }
        if matches!(self.peek(), Some(b's') | Some(b'm') | Some(b'h') | Some(b'd')) {
            self.pos += 1;
            return self.tok(TokenKind::Duration, start);
        }
        if self.peek() == Some(b'x') {
            self.pos += 1;
            return self.tok(TokenKind::Ratio, start);
        }
        if self.peek() == Some(b'%') {
            self.pos += 1;
            return self.tok(TokenKind::PercentLit, start);
        }
        // σ is U+03C3, UTF-8 bytes 0xCE 0xC3? Actually 0xCF 0x83. Verify.
        // σ encodes as 0xCF 0x83 in UTF-8.
        if self.peek() == Some(0xCF) && self.peek_at(1) == Some(0x83) {
            self.pos += 2;
            return self.tok(TokenKind::Sigma, start);
        }

        if is_float {
            self.tok(TokenKind::Float, start)
        } else {
            self.tok(TokenKind::Int, start)
        }
    }

    fn tok(&self, kind: TokenKind, start: usize) -> Token<'a> {
        Token { kind, start, end: self.pos, text: &self.src[start..self.pos] }
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn peek_at(&self, offset: usize) -> Option<u8> {
        self.bytes.get(self.pos + offset).copied()
    }
}

fn is_ident_start(b: u8) -> bool {
    b == b'_' || b.is_ascii_alphabetic()
}

fn is_ident_continue(b: u8) -> bool {
    b == b'_' || b.is_ascii_alphanumeric()
}

fn utf8_char_len(first_byte: u8) -> usize {
    if first_byte < 0x80 {
        1
    } else if first_byte < 0xC0 {
        1 // continuation byte; advance by 1 to make progress
    } else if first_byte < 0xE0 {
        2
    } else if first_byte < 0xF0 {
        3
    } else {
        4
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn kinds(src: &str) -> Vec<TokenKind> {
        lex(src).into_iter().map(|t| t.kind).collect()
    }

    fn non_trivia(src: &str) -> Vec<(TokenKind, &str)> {
        lex(src)
            .into_iter()
            .filter(|t| !matches!(t.kind, TokenKind::Whitespace | TokenKind::Newline))
            .map(|t| (t.kind, t.text))
            .collect()
    }

    #[test]
    fn empty_source() {
        assert!(lex("").is_empty());
    }

    #[test]
    fn single_flow_arrow() {
        let toks = non_trivia("a -> b");
        assert_eq!(
            toks,
            vec![
                (TokenKind::Ident, "a"),
                (TokenKind::Flow, "->"),
                (TokenKind::Ident, "b"),
            ]
        );
    }

    /// Taxonomy §14.1 — the multi-char arrow precedence hazard.
    /// `-x>` must NOT lex as `MINUS IDENT GT`; `<->` must NOT lex as `BIND_LEFT GT`.
    #[test]
    fn arrow_precedence() {
        assert_eq!(non_trivia("a -x> b"),
            vec![(TokenKind::Ident, "a"), (TokenKind::Sever, "-x>"), (TokenKind::Ident, "b")]);
        assert_eq!(non_trivia("a <-> b"),
            vec![(TokenKind::Ident, "a"), (TokenKind::Bidir, "<->"), (TokenKind::Ident, "b")]);
        assert_eq!(non_trivia("a <- b"),
            vec![(TokenKind::Ident, "a"), (TokenKind::BindLeft, "<-"), (TokenKind::Ident, "b")]);
        assert_eq!(non_trivia("a ~> b"),
            vec![(TokenKind::Ident, "a"), (TokenKind::Tap, "~>"), (TokenKind::Ident, "b")]);
    }

    /// Taxonomy §14.2 — number+suffix tokens must be one lexeme.
    #[test]
    fn number_with_suffix() {
        assert_eq!(non_trivia("30d"),  vec![(TokenKind::Duration, "30d")]);
        assert_eq!(non_trivia("300ms"), vec![(TokenKind::Duration, "300ms")]);
        assert_eq!(non_trivia("5m"),   vec![(TokenKind::Duration, "5m")]);
        assert_eq!(non_trivia("5x"),   vec![(TokenKind::Ratio, "5x")]);
        assert_eq!(non_trivia("10%"),  vec![(TokenKind::PercentLit, "10%")]);
        assert_eq!(non_trivia("2σ"),   vec![(TokenKind::Sigma, "2σ")]);
    }

    /// `vault.store` keeps the `.` as Dot — fractional part needs a digit after the dot.
    #[test]
    fn dot_after_int_is_dot() {
        let toks = non_trivia("vault.store");
        assert_eq!(toks, vec![
            (TokenKind::Ident, "vault"),
            (TokenKind::Dot, "."),
            (TokenKind::Ident, "store"),
        ]);
    }

    #[test]
    fn float_literal() {
        assert_eq!(non_trivia("0.3"), vec![(TokenKind::Float, "0.3")]);
    }

    #[test]
    fn line_comment_then_newline() {
        let kinds = kinds("// hello\nx");
        assert_eq!(kinds, vec![TokenKind::LineComment, TokenKind::Newline, TokenKind::Ident]);
    }

    /// Taxonomy §10 SECTION_DIVIDER — `// ──` is a line comment.
    #[test]
    fn section_divider_is_line_comment() {
        let kinds = kinds("// ── SOURCES ─────");
        assert_eq!(kinds, vec![TokenKind::LineComment]);
    }

    #[test]
    fn string_literal() {
        let toks = non_trivia(r#""hello world""#);
        assert_eq!(toks, vec![(TokenKind::String, r#""hello world""#)]);
    }

    #[test]
    fn unterminated_string_is_error() {
        let toks: Vec<TokenKind> = lex("\"abc\n").into_iter().map(|t| t.kind).collect();
        assert_eq!(toks[0], TokenKind::Error);
    }

    /// Mini fragment from `programs/02_log_monitor.zp:8`.
    #[test]
    fn fragment_log_monitor_source_line() {
        let src = r#"syslog    : fs("/var/log/syslog")    -> lines"#;
        let toks: Vec<TokenKind> = non_trivia(src).into_iter().map(|(k, _)| k).collect();
        assert_eq!(toks, vec![
            TokenKind::Ident,    // syslog
            TokenKind::Colon,
            TokenKind::Ident,    // fs
            TokenKind::LParen,
            TokenKind::String,
            TokenKind::RParen,
            TokenKind::Flow,
            TokenKind::Ident,    // lines
        ]);
    }

    /// Mini fragment from `programs/02_log_monitor.zp:43`.
    #[test]
    fn fragment_gate_with_ratio_and_knee() {
        let src = "delta(error_rate) -> gate(> 5x, knee: 2x) -> spike_alert";
        let toks: Vec<TokenKind> = non_trivia(src).into_iter().map(|(k, _)| k).collect();
        assert_eq!(toks, vec![
            TokenKind::Ident,    // delta
            TokenKind::LParen,
            TokenKind::Ident,    // error_rate
            TokenKind::RParen,
            TokenKind::Flow,
            TokenKind::Ident,    // gate
            TokenKind::LParen,
            TokenKind::Gt,
            TokenKind::Ratio,    // 5x
            TokenKind::Comma,
            TokenKind::Ident,    // knee
            TokenKind::Colon,
            TokenKind::Ratio,    // 2x
            TokenKind::RParen,
            TokenKind::Flow,
            TokenKind::Ident,    // spike_alert
        ]);
    }

    /// Mini fragment from `programs/02_log_monitor.zp:35`.
    #[test]
    fn fragment_fuzzy_match_in_gate() {
        let src = r#"errors -> gate(message ~ "out of memory") -> oom_alert"#;
        let toks: Vec<TokenKind> = non_trivia(src).into_iter().map(|(k, _)| k).collect();
        assert_eq!(toks, vec![
            TokenKind::Ident,
            TokenKind::Flow,
            TokenKind::Ident,
            TokenKind::LParen,
            TokenKind::Ident,
            TokenKind::Tilde,
            TokenKind::String,
            TokenKind::RParen,
            TokenKind::Flow,
            TokenKind::Ident,
        ]);
    }

    /// Round-trip: the concatenation of all token texts equals the source
    /// (because trivia tokens are retained).
    #[test]
    fn round_trip_simple() {
        let src = "a -> b ~> c // comment\nd | e";
        let reconstructed: String = lex(src).iter().map(|t| t.text).collect();
        assert_eq!(reconstructed, src);
    }
}
