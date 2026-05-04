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
    Hex,           // 0x68
    Duration,    // 30d, 5m, 300ms
    Ratio,       // 5x, 2.5x
    Sigma,       // 2σ, -2σ
    PercentLit,  // 10%
    ByteSize,    // 200KB, 2MB, 1GB
    Dimension,   // 1920x1080
    HexColor,    // #29ADFF, #444
    String,      // "..." with no interpolation
    TemplateString, // "...{name}..." with at least one interpolation hole
    TimePast,    // t-1, t-2 (no whitespace; parsed as one token in temporal contexts)
    DevNull,     // /dev/null — sentinel discard sink

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
    Bang,        // ! (boolean NOT — `gate(!recording)`)
    HeatUp,      // ↑ (U+2191) — postfix amplifier; `post.heat↑`
    HeatDown,    // ↓ (U+2193) — symmetric pair, not yet seen in corpus

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

        // /dev/null — single token (taxonomy §14.4)
        if b == b'/' && self.starts_with_at(self.pos, b"/dev/null") {
            self.pos += b"/dev/null".len();
            return Some(self.tok(TokenKind::DevNull, start));
        }

        // Hex color: # followed by 1+ hex digits. Corpus uses 3-, 6-, and 8-digit
        // forms. Match greedily up to 8 hex digits.
        if b == b'#' && self.peek_at(1).map_or(false, is_hex_digit) {
            self.pos += 1; // #
            let mut count = 0;
            while count < 8 {
                match self.peek() {
                    Some(c) if is_hex_digit(c) => { self.pos += 1; count += 1; }
                    _ => break,
                }
            }
            return Some(self.tok(TokenKind::HexColor, start));
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
        if b == b'!' {
            if self.peek_at(1) == Some(b'=') {
                self.pos += 2;
                return Some(self.tok(TokenKind::Ne, start));
            }
            self.pos += 1;
            return Some(self.tok(TokenKind::Bang, start));
        }

        // ─ (U+2500 = E2 94 80) followed by 0+ more ─ then `>` is a long-form
        // Flow arrow. `intake ────────────> | ` in goya_fleet.zp:217.
        // Only matches when terminated by `>`; bare ─ runs that don't end in
        // `>` fall through to Error.
        if b == 0xE2 && self.peek_at(1) == Some(0x94) && self.peek_at(2) == Some(0x80) {
            let mut probe = self.pos + 3;
            while self.bytes.get(probe) == Some(&0xE2)
                && self.bytes.get(probe + 1) == Some(&0x94)
                && self.bytes.get(probe + 2) == Some(&0x80)
            {
                probe += 3;
            }
            if self.bytes.get(probe) == Some(&b'>') {
                self.pos = probe + 1;
                return Some(self.tok(TokenKind::Flow, start));
            }
        }

        // ↑ U+2191 = E2 86 91 ; ↓ U+2193 = E2 86 93
        if b == 0xE2 && self.peek_at(1) == Some(0x86) {
            match self.peek_at(2) {
                Some(0x91) => {
                    self.pos += 3;
                    return Some(self.tok(TokenKind::HeatUp, start));
                }
                Some(0x93) => {
                    self.pos += 3;
                    return Some(self.tok(TokenKind::HeatDown, start));
                }
                _ => {}
            }
        }

        // String literal. Supports the C-style escape set: \" \\ \n \t \r \0.
        // Detects interpolation holes `{...}` in the body and upgrades the
        // kind to TemplateString. Hole-span extraction is deferred to the
        // parser.
        if b == b'"' {
            self.pos += 1;
            let mut has_hole = false;
            while let Some(c) = self.peek() {
                self.pos += 1;
                if c == b'\\' {
                    if matches!(
                        self.peek(),
                        Some(b'"' | b'\\' | b'n' | b't' | b'r' | b'0')
                    ) {
                        self.pos += 1;
                    }
                    continue;
                }
                if c == b'"' {
                    let kind = if has_hole { TokenKind::TemplateString } else { TokenKind::String };
                    return Some(self.tok(kind, start));
                }
                if c == b'{' {
                    has_hole = true;
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

        // Identifier (with t-1 / t-2 fold-in: bare `t` immediately followed
        // by `-` and a digit becomes a single TimePast token. Taxonomy §14.3.)
        if is_ident_start(b) {
            while let Some(c) = self.peek() {
                if is_ident_continue(c) {
                    self.pos += 1;
                } else {
                    break;
                }
            }
            if &self.src[start..self.pos] == "t"
                && self.peek() == Some(b'-')
                && self.peek_at(1).map_or(false, |c| c.is_ascii_digit())
            {
                self.pos += 1; // -
                while let Some(c) = self.peek() {
                    if c.is_ascii_digit() {
                        self.pos += 1;
                    } else {
                        break;
                    }
                }
                return Some(self.tok(TokenKind::TimePast, start));
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
        // Hex literal: 0x followed by 1+ hex digits. Must be checked before the
        // Int / Ratio / Dimension paths so `0x68` doesn't lex as Int(0) +
        // Ratio(x) or similar.
        if self.peek() == Some(b'0')
            && self.peek_at(1) == Some(b'x')
            && self.peek_at(2).map_or(false, is_hex_digit)
        {
            self.pos += 2; // 0x
            while let Some(c) = self.peek() {
                if is_hex_digit(c) {
                    self.pos += 1;
                } else {
                    break;
                }
            }
            return self.tok(TokenKind::Hex, start);
        }

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

        // Suffix detection. Order matters (taxonomy §14.2):
        //   1. ByteSize (KB/MB/GB/TB) — uppercase, two-char, must precede `m` Duration
        //      otherwise `200MB` would split into Duration(200m? no, M is uppercase)
        //      — actually `M` (uppercase) is not in the Duration suffix set, so the
        //      conflict is only for lowercase `mb` if that ever appeared. Corpus is
        //      uppercase only; we match uppercase only.
        //   2. Duration ms (two-char) before single-char Duration s/m/h/d
        //   3. Dimension (`x` followed by digit) vs Ratio (`x` not followed by digit)
        //   4. PercentLit, Sigma
        // Boundary rule: a numeric suffix (`s`, `m`, `h`, `d`, `ms`, `KB`,
        // `MB`, `GB`, `TB`, `x`) only counts if the byte after the suffix is
        // not an identifier-continue char. Otherwise the "suffix" is the start
        // of an identifier and we fall back to plain Int/Float. This keeps
        // typos like `0xZ` from lexing as `Ratio(0x) + Ident(Z)`.
        if matches!(self.peek(), Some(b'K') | Some(b'M') | Some(b'G') | Some(b'T'))
            && self.peek_at(1) == Some(b'B')
            && self.at_boundary(2)
        {
            self.pos += 2;
            return self.tok(TokenKind::ByteSize, start);
        }
        if self.peek() == Some(b'm') && self.peek_at(1) == Some(b's') && self.at_boundary(2) {
            self.pos += 2;
            return self.tok(TokenKind::Duration, start);
        }
        if matches!(self.peek(), Some(b's') | Some(b'm') | Some(b'h') | Some(b'd'))
            && self.at_boundary(1)
        {
            self.pos += 1;
            return self.tok(TokenKind::Duration, start);
        }
        if self.peek() == Some(b'x') {
            // Dimension if a digit follows the x; otherwise Ratio (boundary-checked).
            if self.peek_at(1).map_or(false, |c| c.is_ascii_digit()) {
                self.pos += 1; // x
                while let Some(c) = self.peek() {
                    if c.is_ascii_digit() {
                        self.pos += 1;
                    } else {
                        break;
                    }
                }
                return self.tok(TokenKind::Dimension, start);
            }
            if self.at_boundary(1) {
                self.pos += 1;
                return self.tok(TokenKind::Ratio, start);
            }
            // x is the start of an identifier — fall through to Int/Float.
        }
        if self.peek() == Some(b'%') {
            self.pos += 1;
            return self.tok(TokenKind::PercentLit, start);
        }
        // σ encodes as UTF-8 bytes 0xCF 0x83.
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

    fn starts_with_at(&self, pos: usize, needle: &[u8]) -> bool {
        self.bytes.get(pos..pos + needle.len()) == Some(needle)
    }

    /// True if the byte at `self.pos + offset` is absent or is not an
    /// identifier-continuation character. Used to gate numeric suffixes so
    /// they don't swallow the leading char of a following identifier.
    fn at_boundary(&self, offset: usize) -> bool {
        !self.peek_at(offset).map_or(false, is_ident_continue)
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

fn is_hex_digit(b: u8) -> bool {
    b.is_ascii_digit() || (b'a'..=b'f').contains(&b) || (b'A'..=b'F').contains(&b)
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

    // ── v2 token kinds ────────────────────────────────────────────

    /// Hex literal: 0x followed by hex digits — used heavily for I²C addresses.
    #[test]
    fn hex_literal() {
        assert_eq!(non_trivia("0x68"), vec![(TokenKind::Hex, "0x68")]);
        assert_eq!(non_trivia("0xCAFEBABE"), vec![(TokenKind::Hex, "0xCAFEBABE")]);
        assert_eq!(non_trivia("0x1E"), vec![(TokenKind::Hex, "0x1E")]);
    }

    /// `0x` followed by non-hex falls back to Int(0) + Ident.
    #[test]
    fn hex_requires_hex_digit() {
        assert_eq!(
            non_trivia("0xZ"),
            vec![(TokenKind::Int, "0"), (TokenKind::Ident, "xZ")]
        );
    }

    /// HexColor: # followed by 1-8 hex digits. Corpus uses 3 (`#444`),
    /// 6 (`#29ADFF`), 8 (RGBA) forms.
    #[test]
    fn hex_color() {
        assert_eq!(non_trivia("#29ADFF"), vec![(TokenKind::HexColor, "#29ADFF")]);
        assert_eq!(non_trivia("#444"),    vec![(TokenKind::HexColor, "#444")]);
        assert_eq!(non_trivia("#FF6B35AA"), vec![(TokenKind::HexColor, "#FF6B35AA")]);
    }

    /// Bare `#` not followed by a hex digit is unrecognized — emits Error.
    /// (No corpus program does this; but the lexer must not crash.)
    #[test]
    fn lone_hash_is_error() {
        let toks = lex("# ");
        assert_eq!(toks[0].kind, TokenKind::Error);
    }

    /// Taxonomy §14.2 — `5x` is Ratio, `1920x1080` is Dimension.
    /// The `x` followed by a digit triggers Dimension.
    #[test]
    fn dimension_vs_ratio() {
        assert_eq!(non_trivia("5x"), vec![(TokenKind::Ratio, "5x")]);
        assert_eq!(non_trivia("1920x1080"), vec![(TokenKind::Dimension, "1920x1080")]);
        // edge: 5x2 is a small dimension, not Ratio + Int
        assert_eq!(non_trivia("5x2"), vec![(TokenKind::Dimension, "5x2")]);
        // edge: 5x followed by space = Ratio
        assert_eq!(
            non_trivia("5x foo"),
            vec![(TokenKind::Ratio, "5x"), (TokenKind::Ident, "foo")]
        );
    }

    /// ByteSize: 200KB, 2MB, 1GB, 4TB.
    #[test]
    fn byte_size() {
        assert_eq!(non_trivia("200KB"), vec![(TokenKind::ByteSize, "200KB")]);
        assert_eq!(non_trivia("2MB"),   vec![(TokenKind::ByteSize, "2MB")]);
        assert_eq!(non_trivia("1GB"),   vec![(TokenKind::ByteSize, "1GB")]);
        assert_eq!(non_trivia("4TB"),   vec![(TokenKind::ByteSize, "4TB")]);
    }

    /// ByteSize must not collide with Duration `ms` (different case).
    #[test]
    fn duration_ms_does_not_collide_with_byte_size() {
        assert_eq!(non_trivia("300ms"), vec![(TokenKind::Duration, "300ms")]);
        assert_eq!(non_trivia("300MB"), vec![(TokenKind::ByteSize, "300MB")]);
    }

    /// Taxonomy §14.3 — `t-1` is one TimePast token; `t - 1` (with whitespace)
    /// is three tokens; `timeline` is an Ident, not TimePast.
    #[test]
    fn time_past_one_token() {
        assert_eq!(non_trivia("t-1"), vec![(TokenKind::TimePast, "t-1")]);
        assert_eq!(non_trivia("t-2"), vec![(TokenKind::TimePast, "t-2")]);
        assert_eq!(non_trivia("t-10"), vec![(TokenKind::TimePast, "t-10")]);
    }

    #[test]
    fn time_with_whitespace_is_subtract() {
        assert_eq!(
            non_trivia("t - 1"),
            vec![
                (TokenKind::Ident, "t"),
                (TokenKind::Minus, "-"),
                (TokenKind::Int, "1"),
            ]
        );
    }

    #[test]
    fn timeline_is_identifier() {
        assert_eq!(non_trivia("timeline"), vec![(TokenKind::Ident, "timeline")]);
    }

    /// Taxonomy §14.4 — `/dev/null` is one DevNull token, not 4 tokens.
    #[test]
    fn dev_null_one_token() {
        assert_eq!(non_trivia("/dev/null"), vec![(TokenKind::DevNull, "/dev/null")]);
        assert_eq!(
            non_trivia("-> /dev/null"),
            vec![(TokenKind::Flow, "->"), (TokenKind::DevNull, "/dev/null")]
        );
    }

    /// Bare `/` not followed by `dev/null` is Slash.
    #[test]
    fn slash_alone_is_slash() {
        assert_eq!(non_trivia("/foo"),
            vec![(TokenKind::Slash, "/"), (TokenKind::Ident, "foo")]);
    }

    /// Template string: `"…{name}…"` becomes TemplateString. Plain string
    /// without holes stays String.
    #[test]
    fn template_string_with_hole() {
        assert_eq!(
            non_trivia(r#""recordings/{timestamp}.mp4""#),
            vec![(TokenKind::TemplateString, r#""recordings/{timestamp}.mp4""#)]
        );
        assert_eq!(
            non_trivia(r#""{name}.pdf""#),
            vec![(TokenKind::TemplateString, r#""{name}.pdf""#)]
        );
    }

    #[test]
    fn plain_string_without_hole_stays_string() {
        assert_eq!(
            non_trivia(r#""hello world""#),
            vec![(TokenKind::String, r#""hello world""#)]
        );
    }

    /// `//` inside a string is part of the string — strings are detected before
    /// line comments at the dispatch level.
    #[test]
    fn slashes_inside_string_are_string_content() {
        assert_eq!(
            non_trivia(r#""http://example.com""#),
            vec![(TokenKind::String, r#""http://example.com""#)]
        );
    }

    /// Bang (`!`): boolean NOT. Used as `gate(!recording)` in zeros/, derez/.
    /// Distinct from Ne (`!=`).
    #[test]
    fn bang_vs_ne() {
        assert_eq!(non_trivia("!recording"),
            vec![(TokenKind::Bang, "!"), (TokenKind::Ident, "recording")]);
        assert_eq!(non_trivia("a != b"),
            vec![(TokenKind::Ident, "a"), (TokenKind::Ne, "!="), (TokenKind::Ident, "b")]);
    }

    // ── v3 token kinds ────────────────────────────────────────────

    /// `↑` is a single token, postfix amplifier. `programs/chirp.zp:47`.
    #[test]
    fn heat_up_after_path() {
        assert_eq!(
            non_trivia("post.heat↑"),
            vec![
                (TokenKind::Ident, "post"),
                (TokenKind::Dot, "."),
                (TokenKind::Ident, "heat"),
                (TokenKind::HeatUp, "↑"),
            ]
        );
    }

    #[test]
    fn heat_down() {
        assert_eq!(non_trivia("x↓"),
            vec![(TokenKind::Ident, "x"), (TokenKind::HeatDown, "↓")]);
    }

    /// String escapes: `\"`, `\\`, `\n`, `\t`, `\r`, `\0` are all consumed
    /// as part of the string body.
    #[test]
    fn string_escapes() {
        // `\"` doesn't terminate the string
        assert_eq!(non_trivia(r#""a\"b""#),
            vec![(TokenKind::String, r#""a\"b""#)]);
        // `\n` consumed as escape, doesn't trigger the unterminated-string error
        assert_eq!(non_trivia(r#""line\n""#),
            vec![(TokenKind::String, r#""line\n""#)]);
        // `\\` is a literal backslash escape
        assert_eq!(non_trivia(r#""a\\b""#),
            vec![(TokenKind::String, r#""a\\b""#)]);
    }

    /// Forge_ide.zp pattern: nested `\"` plus `{value}` interpolation hole.
    /// Whole thing is one TemplateString.
    #[test]
    fn template_string_with_escape_and_hole() {
        assert_eq!(
            non_trivia(r#""print(\"${1:{value}}\")""#),
            vec![(TokenKind::TemplateString, r#""print(\"${1:{value}}\")""#)]
        );
    }

    /// Long-form Flow arrow: `─...>` is one Flow token.
    /// `programs/goya_fleet.zp:217`.
    #[test]
    fn long_form_flow_arrow() {
        let src = "intake ─────> sink";
        assert_eq!(
            non_trivia(src),
            vec![
                (TokenKind::Ident, "intake"),
                (TokenKind::Flow, "─────>"),
                (TokenKind::Ident, "sink"),
            ]
        );
    }

    #[test]
    fn single_dash_then_gt_is_long_form_flow() {
        assert_eq!(non_trivia("a ─> b"),
            vec![(TokenKind::Ident, "a"), (TokenKind::Flow, "─>"), (TokenKind::Ident, "b")]);
    }

    /// Bare `─` not terminated by `>` falls through to Error.
    #[test]
    fn bare_dash_is_error() {
        let toks = lex("─");
        assert!(toks.iter().any(|t| t.kind == TokenKind::Error));
    }
}
