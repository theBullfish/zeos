//! Z+ parser. Tokens → AST.
//!
//! Strategy: recursive descent. Newlines are statement separators at
//! paren-depth zero; inside parens / braces they're whitespace. Whitespace
//! and line-comment tokens are filtered out of the input stream entirely.
//!
//! The chord rule from `ast.rs` lives here at the parser level too: the
//! vertical `-> |` block lowering produces ONE `Merge` node, never a list
//! of `Flow` edges. See `parse_module` for the merge-coalescing logic.
//!
//! v1 scope: enough to parse `programs/02_log_monitor.zp` end to end.
//! Errors are fail-fast (`Result<Module, ParseError>`) — recovery comes
//! when the corpus surfaces a real need.

use crate::ast::*;
use crate::lex::{lex as lex_source, Token, TokenKind};

#[derive(Debug, Clone, PartialEq)]
pub struct ParseError {
    pub message: String,
    pub span: Span,
}

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "parse error at {}..{}: {}", self.span.start, self.span.end, self.message)
    }
}

impl std::error::Error for ParseError {}

pub fn parse(src: &str) -> Result<Module<'_>, ParseError> {
    let mut p = Parser::new(src);
    p.parse_module()
}

pub struct Parser<'src> {
    tokens: Vec<Token<'src>>,
    pos: usize,
    src_len: usize,
}

impl<'src> Parser<'src> {
    pub fn new(src: &'src str) -> Self {
        let tokens: Vec<Token<'src>> = lex_source(src)
            .into_iter()
            .filter(|t| !matches!(t.kind, TokenKind::Whitespace | TokenKind::LineComment))
            .collect();
        Self { tokens, pos: 0, src_len: src.len() }
    }

    pub fn parse_module(&mut self) -> Result<Module<'src>, ParseError> {
        let start = self.cur_pos();
        self.skip_newlines();
        let mut stmts = Vec::new();

        while !self.at_eof() {
            let stmt = self.parse_stmt()?;

            // Vertical merge coalescing. If this stmt is a `Connect(Flow(x, Pipe))`
            // — i.e., `x -> |` with no downstream — and the next stmt is also of
            // that shape OR is `Connect(Flow(x, Merge { downstream: Some, ... }))`,
            // collapse them into one Merge node. The chord rule.
            if let Some(coalesced) = self.try_coalesce_vertical_merge(stmt, &mut stmts) {
                stmts.push(coalesced);
            }
            self.skip_newlines();
        }

        Ok(Module { stmts, span: Span::new(start, self.src_len) })
    }

    /// If `stmt` is a vertical-merge fragment (`x -> |` or `x -> | -> y`),
    /// see if it pairs with already-pushed fragments OR upcoming fragments
    /// to form one `Merge` node. Returns `Some(stmt)` to push, or `None`
    /// if the stmt was absorbed into something already in `stmts`.
    fn try_coalesce_vertical_merge(
        &mut self,
        stmt: Stmt<'src>,
        stmts: &mut Vec<Stmt<'src>>,
    ) -> Option<Stmt<'src>> {
        // Detect: Connect(Flow(input, MergePlaceholder { downstream: Option<...> }))
        // We model the parse output of `x -> |` as Connect(Flow(x, Merge{inputs:[], policy:All, downstream:None}))
        // and `x -> | -> y` as Connect(Flow(x, Merge{inputs:[], policy:All, downstream:Some(y)}))
        // The coalesce step folds the input into the Merge's `inputs` list.
        let frag = match &stmt {
            Stmt::Connect(Chain::Flow(_lhs, rhs, _)) => match rhs.as_ref() {
                Chain::Merge(_) => true,
                _ => false,
            },
            _ => false,
        };
        if !frag {
            return Some(stmt);
        }

        // Extract the fragment's components.
        let (input, mut merge) = match stmt {
            Stmt::Connect(Chain::Flow(lhs, rhs, _)) => match *rhs {
                Chain::Merge(m) => (*lhs, m),
                _ => unreachable!(),
            },
            _ => unreachable!(),
        };

        // Look back: is the last pushed stmt also a merge fragment with no
        // downstream yet committed AND no inputs collected yet?
        if let Some(Stmt::Connect(Chain::Merge(prev))) = stmts.last_mut() {
            // Append this input.
            prev.inputs.push(input);
            // If the new fragment provides a downstream, commit it.
            if merge.downstream.is_some() {
                prev.downstream = merge.downstream;
            }
            return None;
        }

        // Else: start a new Merge. Move the input into the merge's inputs.
        merge.inputs.insert(0, input);
        Some(Stmt::Connect(Chain::Merge(merge)))
    }

    fn parse_stmt(&mut self) -> Result<Stmt<'src>, ParseError> {
        if self.looks_like_wire_decl() {
            let w = self.parse_wire_decl()?;
            Ok(Stmt::Wire(w))
        } else {
            let chain = self.parse_chain()?;
            Ok(Stmt::Connect(chain))
        }
    }

    /// Lookahead: `IDENT (DOT IDENT)* COLON`.
    fn looks_like_wire_decl(&self) -> bool {
        let mut p = self.pos;
        if self.kind_at(p) != Some(TokenKind::Ident) {
            return false;
        }
        p += 1;
        while self.kind_at(p) == Some(TokenKind::Dot)
            && self.kind_at(p + 1) == Some(TokenKind::Ident)
        {
            p += 2;
        }
        self.kind_at(p) == Some(TokenKind::Colon)
    }

    fn parse_wire_decl(&mut self) -> Result<WireDecl<'src>, ParseError> {
        let start = self.cur_pos();
        let name = self.parse_path()?;
        self.expect(TokenKind::Colon)?;
        let chain = self.parse_chain()?;
        let end = self.cur_pos();
        Ok(WireDecl { name, chain, span: Span::new(start, end) })
    }

    fn parse_path(&mut self) -> Result<Path<'src>, ParseError> {
        let first = self.expect(TokenKind::Ident)?;
        let start = first.start;
        let mut segments = vec![Ident { text: first.text, span: Span::new(first.start, first.end) }];
        while self.peek_kind() == Some(TokenKind::Dot)
            && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
        {
            self.advance(); // dot
            let seg = self.expect(TokenKind::Ident)?;
            segments.push(Ident { text: seg.text, span: Span::new(seg.start, seg.end) });
        }
        let end = segments.last().unwrap().span.end;
        Ok(Path { segments, span: Span::new(start, end) })
    }

    /// Chain := Term (FlowOp Term)*
    /// FlowOp := -> | ~> | -x>
    fn parse_chain(&mut self) -> Result<Chain<'src>, ParseError> {
        let mut left = self.parse_chain_term()?;
        loop {
            let op = match self.peek_kind() {
                Some(TokenKind::Flow) => Some(TokenKind::Flow),
                Some(TokenKind::Tap) => Some(TokenKind::Tap),
                Some(TokenKind::Sever) => Some(TokenKind::Sever),
                _ => None,
            };
            let Some(op_kind) = op else { break };
            self.advance();
            // Allow newline after a chain operator for multi-line chains.
            self.skip_newlines();
            let right = self.parse_chain_term()?;
            let span = Span::new(self.span_of(&left).start, self.span_of(&right).end);
            left = match op_kind {
                TokenKind::Flow => Chain::Flow(Box::new(left), Box::new(right), span),
                TokenKind::Tap => Chain::Tap(Box::new(left), Box::new(right), span),
                TokenKind::Sever => Chain::Sever(Box::new(left), Box::new(right), span),
                _ => unreachable!(),
            };
        }
        Ok(left)
    }

    fn parse_chain_term(&mut self) -> Result<Chain<'src>, ParseError> {
        let cur = self.cur();
        match cur.kind {
            TokenKind::Ident => {
                let path = self.parse_path()?;
                if self.peek_kind() == Some(TokenKind::LParen) {
                    let args = self.parse_args()?;
                    let end = self.prev_end();
                    Ok(Chain::Call(Call { callee: path.clone(), args, span: Span::new(path.span.start, end) }))
                } else {
                    Ok(Chain::Atom(Atom::Path(path)))
                }
            }
            TokenKind::String => {
                self.advance();
                Ok(Chain::Atom(Atom::Literal(Literal::String(cur.text, Span::new(cur.start, cur.end)))))
            }
            TokenKind::TemplateString => {
                self.advance();
                Ok(Chain::Atom(Atom::Literal(Literal::TemplateString(cur.text, Span::new(cur.start, cur.end)))))
            }
            TokenKind::Int => {
                self.advance();
                let value: i64 = cur.text.parse().map_err(|e| ParseError {
                    message: format!("invalid integer literal '{}': {}", cur.text, e),
                    span: Span::new(cur.start, cur.end),
                })?;
                Ok(Chain::Atom(Atom::Literal(Literal::Int(value, Span::new(cur.start, cur.end)))))
            }
            TokenKind::Float => {
                self.advance();
                let value: f64 = cur.text.parse().map_err(|e| ParseError {
                    message: format!("invalid float literal '{}': {}", cur.text, e),
                    span: Span::new(cur.start, cur.end),
                })?;
                Ok(Chain::Atom(Atom::Literal(Literal::Float(value, Span::new(cur.start, cur.end)))))
            }
            TokenKind::Duration => {
                self.advance();
                let (val, unit) = parse_duration(cur.text)
                    .ok_or_else(|| ParseError {
                        message: format!("invalid duration literal '{}'", cur.text),
                        span: Span::new(cur.start, cur.end),
                    })?;
                Ok(Chain::Atom(Atom::Literal(Literal::Duration {
                    value: val, unit, span: Span::new(cur.start, cur.end),
                })))
            }
            TokenKind::Ratio => {
                self.advance();
                let val: f64 = cur.text.trim_end_matches('x').parse().unwrap_or(0.0);
                Ok(Chain::Atom(Atom::Literal(Literal::Ratio(val, Span::new(cur.start, cur.end)))))
            }
            TokenKind::PercentLit => {
                self.advance();
                let val: f64 = cur.text.trim_end_matches('%').parse().unwrap_or(0.0);
                Ok(Chain::Atom(Atom::Literal(Literal::Percent(val, Span::new(cur.start, cur.end)))))
            }
            TokenKind::Sigma => {
                self.advance();
                let val: f64 = cur.text.trim_end_matches('σ').parse().unwrap_or(0.0);
                Ok(Chain::Atom(Atom::Literal(Literal::Sigma(val, Span::new(cur.start, cur.end)))))
            }
            TokenKind::DevNull => {
                self.advance();
                Ok(Chain::Atom(Atom::DevNull(Span::new(cur.start, cur.end))))
            }
            TokenKind::TimePast => {
                self.advance();
                // text is "t-1", "t-2", etc.
                let steps: u32 = cur.text[2..].parse().unwrap_or(0);
                Ok(Chain::Atom(Atom::TimePast { steps, span: Span::new(cur.start, cur.end) }))
            }
            TokenKind::Pipe => {
                // `|` standalone — start of a merge fragment with no inputs yet.
                // The actual inputs come from previous Flow chains via the
                // vertical-merge coalescing pass in parse_module.
                self.advance();
                let downstream = self.try_parse_merge_downstream()?;
                Ok(Chain::Merge(Merge {
                    inputs: Vec::new(),
                    policy: MergePolicy::All,
                    downstream: downstream.map(Box::new),
                    span: Span::new(cur.start, self.prev_end()),
                }))
            }
            _ => Err(ParseError {
                message: format!("unexpected token in chain term: {:?}", cur.kind),
                span: Span::new(cur.start, cur.end),
            }),
        }
    }

    /// After a `|`, if the very next token is `->`, parse the downstream chain.
    /// Otherwise the merge has no downstream (yet — coalescing may attach one).
    fn try_parse_merge_downstream(&mut self) -> Result<Option<Chain<'src>>, ParseError> {
        if self.peek_kind() == Some(TokenKind::Flow) {
            self.advance();
            self.skip_newlines();
            Ok(Some(self.parse_chain_term()?))
        } else {
            Ok(None)
        }
    }

    /// Args := LPAREN (Arg (COMMA Arg)*)? RPAREN
    /// Arg  := IDENT COLON Chain  |  Chain
    fn parse_args(&mut self) -> Result<Vec<Arg<'src>>, ParseError> {
        self.expect(TokenKind::LParen)?;
        let mut args = Vec::new();
        loop {
            self.skip_newlines();
            if self.peek_kind() == Some(TokenKind::RParen) {
                break;
            }
            args.push(self.parse_arg()?);
            self.skip_newlines();
            if self.peek_kind() == Some(TokenKind::Comma) {
                self.advance();
            } else {
                break;
            }
        }
        self.skip_newlines();
        self.expect(TokenKind::RParen)?;
        Ok(args)
    }

    fn parse_arg(&mut self) -> Result<Arg<'src>, ParseError> {
        // Named arg lookahead: IDENT COLON (and not part of a wire decl —
        // arg names don't have dotted paths in any corpus citation we've
        // seen, so the simple check is enough).
        if self.peek_kind() == Some(TokenKind::Ident)
            && self.kind_at(self.pos + 1) == Some(TokenKind::Colon)
        {
            let start = self.cur_pos();
            let tok = self.advance();
            let name = Ident { text: tok.text, span: Span::new(tok.start, tok.end) };
            self.expect(TokenKind::Colon)?;
            let value = self.parse_chain()?;
            // Type-union value form: `level : error | warn | info | debug`
            // — eat additional `| IDENT` segments and discard for now (the
            // semantic layer can reconstruct from value if needed).
            while self.peek_kind() == Some(TokenKind::Pipe)
                && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
            {
                self.advance(); // pipe
                self.advance(); // ident — discarded for v1
            }
            let end = self.prev_end();
            Ok(Arg::Named { name, value, span: Span::new(start, end) })
        } else {
            Ok(Arg::Positional(self.parse_chain()?))
        }
    }

    // ── helpers ────────────────────────────────────────────────────

    fn at_eof(&self) -> bool {
        self.pos >= self.tokens.len()
    }

    fn cur(&self) -> Token<'src> {
        self.tokens.get(self.pos).copied().unwrap_or(Token {
            kind: TokenKind::Error,
            start: self.src_len,
            end: self.src_len,
            text: "",
        })
    }

    fn cur_pos(&self) -> usize {
        self.cur().start
    }

    fn prev_end(&self) -> usize {
        if self.pos == 0 { 0 } else { self.tokens[self.pos - 1].end }
    }

    fn peek_kind(&self) -> Option<TokenKind> {
        self.tokens.get(self.pos).map(|t| t.kind)
    }

    fn kind_at(&self, i: usize) -> Option<TokenKind> {
        self.tokens.get(i).map(|t| t.kind)
    }

    fn advance(&mut self) -> Token<'src> {
        let t = self.cur();
        self.pos += 1;
        t
    }

    fn expect(&mut self, kind: TokenKind) -> Result<Token<'src>, ParseError> {
        let cur = self.cur();
        if cur.kind == kind {
            self.advance();
            Ok(cur)
        } else {
            Err(ParseError {
                message: format!("expected {:?}, got {:?} ({:?})", kind, cur.kind, cur.text),
                span: Span::new(cur.start, cur.end),
            })
        }
    }

    fn skip_newlines(&mut self) {
        while self.peek_kind() == Some(TokenKind::Newline) {
            self.advance();
        }
    }

    fn span_of(&self, c: &Chain<'src>) -> Span {
        match c {
            Chain::Atom(a) => match a {
                Atom::Path(p) => p.span,
                Atom::Literal(l) => literal_span(l),
                Atom::DevNull(s) | Atom::TimePast { span: s, .. } => *s,
            },
            Chain::Call(c) => c.span,
            Chain::Flow(_, _, s)
            | Chain::Tap(_, _, s)
            | Chain::Sever(_, _, s) => *s,
            Chain::Fork(_, s) => *s,
            Chain::Merge(m) => m.span,
        }
    }
}

fn literal_span(l: &Literal<'_>) -> Span {
    match l {
        Literal::Int(_, s)
        | Literal::Float(_, s)
        | Literal::String(_, s)
        | Literal::TemplateString(_, s)
        | Literal::Ratio(_, s)
        | Literal::Sigma(_, s)
        | Literal::Percent(_, s)
        | Literal::Hex(_, s)
        | Literal::HexColor(_, s) => *s,
        Literal::Duration { span, .. }
        | Literal::ByteSize { span, .. }
        | Literal::Dimension { span, .. } => *span,
    }
}

fn parse_duration(text: &str) -> Option<(f64, DurationUnit)> {
    let (num, unit) = if let Some(stripped) = text.strip_suffix("ms") {
        (stripped, DurationUnit::Ms)
    } else if let Some(stripped) = text.strip_suffix('s') {
        (stripped, DurationUnit::S)
    } else if let Some(stripped) = text.strip_suffix('m') {
        (stripped, DurationUnit::M)
    } else if let Some(stripped) = text.strip_suffix('h') {
        (stripped, DurationUnit::H)
    } else if let Some(stripped) = text.strip_suffix('d') {
        (stripped, DurationUnit::D)
    } else {
        return None;
    };
    Some((num.parse().ok()?, unit))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn module(src: &'static str) -> Module<'static> {
        parse(src).expect("parse failed")
    }

    #[test]
    fn empty_source_is_empty_module() {
        let m = module("");
        assert!(m.stmts.is_empty());
    }

    #[test]
    fn single_wire_decl() {
        let m = module(r#"syslog : fs("/var/log/syslog") -> lines"#);
        assert_eq!(m.stmts.len(), 1);
        let Stmt::Wire(w) = &m.stmts[0] else { panic!() };
        assert_eq!(w.name.joined(), "syslog");
        let Chain::Flow(from, to, _) = &w.chain else { panic!("not Flow") };
        let Chain::Call(call) = from.as_ref() else { panic!("not Call") };
        assert_eq!(call.callee.joined(), "fs");
        assert_eq!(call.args.len(), 1);
        let Chain::Atom(Atom::Path(p)) = to.as_ref() else { panic!() };
        assert_eq!(p.joined(), "lines");
    }

    #[test]
    fn three_wire_decls() {
        let src = r#"syslog : fs("/var/log/syslog") -> lines
app_log : fs("/var/log/app/*.log") -> lines
kern_log : fs("/var/log/kern.log") -> lines
"#;
        let m = module(src);
        assert_eq!(m.stmts.len(), 3);
        for (i, name) in ["syslog", "app_log", "kern_log"].iter().enumerate() {
            let Stmt::Wire(w) = &m.stmts[i] else { panic!() };
            assert_eq!(w.name.joined(), *name);
        }
    }

    /// The chord rule canary at the parser level. The vertical merge MUST
    /// produce ONE `Merge` node, not three `Flow` edges.
    #[test]
    fn vertical_merge_coalesces_to_one_node() {
        let src = "syslog -> |
app_log -> | -> all_lines
kern_log -> |
";
        let m = module(src);
        // Should be exactly ONE statement, not three.
        assert_eq!(
            m.stmts.len(),
            1,
            "vertical merge produced {} stmts; expected 1 (chord-rule violation)",
            m.stmts.len()
        );
        let Stmt::Connect(Chain::Merge(merge)) = &m.stmts[0] else {
            panic!("merge wasn't coalesced — chord rule violated: {:?}", m.stmts[0]);
        };
        assert_eq!(merge.inputs.len(), 3);
        assert_eq!(merge.policy, MergePolicy::All);
        let downstream = merge.downstream.as_ref().expect("no downstream");
        let Chain::Atom(Atom::Path(p)) = downstream.as_ref() else { panic!() };
        assert_eq!(p.joined(), "all_lines");
    }

    /// Named arg with type-union value: `level : error | warn | info | debug`.
    /// The pipe-separated alternatives are absorbed by the parser for v1.
    #[test]
    fn named_arg_with_type_union_value() {
        let src = r#"all_lines -> parse(timestamp, level : error | warn | info | debug, source, message) -> entries"#;
        let m = module(src);
        assert_eq!(m.stmts.len(), 1);
    }

    #[test]
    fn dotted_path_in_call() {
        let m = module("entries -> vault.store(ttl: 30d)");
        assert_eq!(m.stmts.len(), 1);
        let Stmt::Connect(chain) = &m.stmts[0] else { panic!() };
        let Chain::Flow(_, to, _) = chain else { panic!() };
        let Chain::Call(call) = to.as_ref() else { panic!() };
        assert_eq!(call.callee.joined(), "vault.store");
        assert_eq!(call.args.len(), 1);
        let Arg::Named { name, value, .. } = &call.args[0] else { panic!() };
        assert_eq!(name.text, "ttl");
        let Chain::Atom(Atom::Literal(Literal::Duration { value: v, unit, .. })) = value else {
            panic!("ttl value is not a Duration literal: {:?}", value);
        };
        assert_eq!(*v, 30.0);
        assert_eq!(*unit, DurationUnit::D);
    }

    #[test]
    fn tap_arrow_distinct_from_flow() {
        let m = module("error_rate ~> live_rate_view");
        let Stmt::Connect(Chain::Tap(_, _, _)) = &m.stmts[0] else {
            panic!("expected Tap, got {:?}", m.stmts[0]);
        };
    }
}
