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

    /// If `stmt` is a vertical-merge fragment, fold it into a running
    /// `Merge` in `stmts`. Two fragment shapes:
    ///   1. `INPUT -> |` / `INPUT -> | -> DOWNSTREAM`
    ///        → `Connect(Flow(INPUT, Merge { inputs: [], ... }))`
    ///   2. `| policy | -> DOWNSTREAM` (no input)
    ///        → `Connect(Merge { inputs: [], policy, downstream })`
    ///
    /// Returns `Some(stmt)` if a brand-new statement should be pushed, or
    /// `None` if the stmt was absorbed into an already-present Merge.
    ///
    /// Rule: any fragment immediately after a `Connect(Merge)` coalesces —
    /// the policy / downstream / inputs of the new fragment are merged in.
    /// Non-fragment statements (including blank-line-separated ones) end
    /// the running merge group naturally because the next iteration will
    /// see no `Connect(Merge)` to coalesce with.
    fn try_coalesce_vertical_merge(
        &mut self,
        stmt: Stmt<'src>,
        stmts: &mut Vec<Stmt<'src>>,
    ) -> Option<Stmt<'src>> {
        let (maybe_input, frag_merge) = match stmt {
            Stmt::Connect(Chain::Flow(lhs, rhs, _))
                if matches!(rhs.as_ref(), Chain::Merge(_)) =>
            {
                let m = match *rhs {
                    Chain::Merge(m) => m,
                    _ => unreachable!(),
                };
                (Some(*lhs), m)
            }
            Stmt::Connect(Chain::Merge(m)) => (None, m),
            other => return Some(other),
        };

        // If the previous statement is a Connect(Merge), fold this fragment in.
        if let Some(Stmt::Connect(Chain::Merge(prev))) = stmts.last_mut() {
            if let Some(input) = maybe_input {
                prev.inputs.push(input);
            }
            // Carry over any inputs the new fragment had collected.
            prev.inputs.extend(frag_merge.inputs);
            // Adopt non-default policy if the fragment specifies one.
            if !matches!(frag_merge.policy, MergePolicy::All) {
                prev.policy = frag_merge.policy;
            }
            // Adopt downstream if the fragment specifies one.
            if frag_merge.downstream.is_some() {
                prev.downstream = frag_merge.downstream;
            }
            return None;
        }

        // No predecessor merge — start a new one.
        let mut merge = frag_merge;
        if let Some(input) = maybe_input {
            merge.inputs.insert(0, input);
        }
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
                // Forms accepted:
                //   `|`                          — default policy All, no downstream
                //   `| -> downstream`            — All policy, downstream chain
                //   `| policy | -> downstream`   — explicit policy + downstream
                //   `| policy |`                 — explicit policy, no downstream
                self.advance();
                let policy = self.try_parse_merge_policy()?;
                if policy.is_some() {
                    self.expect(TokenKind::Pipe)?;
                }
                let downstream = self.try_parse_merge_downstream()?;
                Ok(Chain::Merge(Merge {
                    inputs: Vec::new(),
                    policy: policy.unwrap_or(MergePolicy::All),
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
            // The downstream is itself a chain — it may chain further with
            // `-> next -> next` etc.
            Ok(Some(self.parse_chain()?))
        } else {
            Ok(None)
        }
    }

    /// Parses the policy specifier between `|` markers, if present.
    /// Forms:
    ///   - `all`, `any`
    ///   - `N of M` — quorum
    ///   - `fastest(N)`
    ///   - `within(<chain>)` — temporal confluence
    ///   - `merge(by: <path>)` — sort policy
    fn try_parse_merge_policy(&mut self) -> Result<Option<MergePolicy<'src>>, ParseError> {
        let start_pos = self.pos;
        match self.peek_kind() {
            Some(TokenKind::Ident) => {
                let cur = self.cur();
                match cur.text {
                    "all" => { self.advance(); Ok(Some(MergePolicy::All)) }
                    "any" => { self.advance(); Ok(Some(MergePolicy::Any)) }
                    "fastest" => {
                        self.advance();
                        self.expect(TokenKind::LParen)?;
                        let n_tok = self.expect(TokenKind::Int)?;
                        let n: u32 = n_tok.text.parse().map_err(|_| ParseError {
                            message: format!("invalid fastest count: {}", n_tok.text),
                            span: Span::new(n_tok.start, n_tok.end),
                        })?;
                        self.expect(TokenKind::RParen)?;
                        Ok(Some(MergePolicy::Fastest(n)))
                    }
                    "within" => {
                        self.advance();
                        self.expect(TokenKind::LParen)?;
                        let inner = self.parse_chain()?;
                        self.expect(TokenKind::RParen)?;
                        Ok(Some(MergePolicy::Within(Box::new(inner))))
                    }
                    "merge" => {
                        self.advance();
                        self.expect(TokenKind::LParen)?;
                        // expect `by: <path>`
                        let by_tok = self.expect(TokenKind::Ident)?;
                        if by_tok.text != "by" {
                            return Err(ParseError {
                                message: format!("expected 'by', got '{}'", by_tok.text),
                                span: Span::new(by_tok.start, by_tok.end),
                            });
                        }
                        self.expect(TokenKind::Colon)?;
                        let path = self.parse_path()?;
                        self.expect(TokenKind::RParen)?;
                        Ok(Some(MergePolicy::By(path)))
                    }
                    _ => Ok(None),
                }
            }
            Some(TokenKind::Int) => {
                // `N of M` quorum
                let n_tok = self.advance();
                if self.peek_kind() == Some(TokenKind::Ident) && self.cur().text == "of" {
                    self.advance();
                    let m_tok = self.expect(TokenKind::Int)?;
                    let n: u32 = n_tok.text.parse().unwrap_or(0);
                    let m: u32 = m_tok.text.parse().unwrap_or(0);
                    Ok(Some(MergePolicy::Quorum { n, m }))
                } else {
                    // Not a policy — back up
                    self.pos = start_pos;
                    Ok(None)
                }
            }
            _ => Ok(None),
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
        // Unary comparison at start of arg: `gate(> 5x, knee: 2x)`.
        if let Some(op) = self.peek_unary_cmp_op() {
            let start = self.cur_pos();
            self.advance();
            let rhs = self.parse_arg_expr()?;
            let span = Span::new(start, self.span_of(&rhs).end);
            return Ok(Arg::Positional(Chain::UnaryCmp {
                op,
                rhs: Box::new(rhs),
                span,
            }));
        }

        // Named arg lookahead: IDENT COLON.
        if self.peek_kind() == Some(TokenKind::Ident)
            && self.kind_at(self.pos + 1) == Some(TokenKind::Colon)
        {
            let start = self.cur_pos();
            let tok = self.advance();
            let name = Ident { text: tok.text, span: Span::new(tok.start, tok.end) };
            self.expect(TokenKind::Colon)?;
            // Unary cmp value: `sustained: > 5m`? Not seen in corpus, but
            // handle anyway by routing through parse_arg_expr.
            let value = self.parse_arg_expr()?;
            // Type-union value form: `level : error | warn | info | debug`
            // — eat additional `| IDENT` segments and discard for now.
            while self.peek_kind() == Some(TokenKind::Pipe)
                && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
            {
                self.advance(); // pipe
                self.advance(); // ident — discarded for v1
            }
            let end = self.prev_end();
            Ok(Arg::Named { name, value, span: Span::new(start, end) })
        } else {
            Ok(Arg::Positional(self.parse_arg_expr()?))
        }
    }

    /// An expression usable as an arg value / arg subject. Differs from
    /// `parse_chain` in that it accepts binary comparison / fuzzy-match
    /// operators (`~`, `>`, `<`, `<=`, `>=`, `==`, `!=`) — these are NOT
    /// chain operators, so the chain parser proper won't fold them.
    fn parse_arg_expr(&mut self) -> Result<Chain<'src>, ParseError> {
        let mut left = self.parse_chain()?;
        // After a chain term, check for a single comparison/match operator.
        // We handle the simple form `a OP b` only — no precedence chains
        // like `a OP b OP c`. Corpus doesn't use those.
        if let Some(op) = self.peek_bin_op() {
            self.advance();
            let right = self.parse_chain()?;
            let span = Span::new(self.span_of(&left).start, self.span_of(&right).end);
            left = Chain::BinExpr {
                op,
                lhs: Box::new(left),
                rhs: Box::new(right),
                span,
            };
        }
        Ok(left)
    }

    fn peek_bin_op(&self) -> Option<BinOp> {
        match self.peek_kind()? {
            TokenKind::Tilde => Some(BinOp::FuzzyMatch),
            TokenKind::Lt => Some(BinOp::Lt),
            TokenKind::Gt => Some(BinOp::Gt),
            TokenKind::Le => Some(BinOp::Le),
            TokenKind::Ge => Some(BinOp::Ge),
            TokenKind::EqEq => Some(BinOp::Eq),
            TokenKind::Ne => Some(BinOp::Ne),
            _ => None,
        }
    }

    /// Comparison ops valid as a unary (subject elided). FuzzyMatch is not
    /// in the corpus as a unary form.
    fn peek_unary_cmp_op(&self) -> Option<BinOp> {
        match self.peek_kind()? {
            TokenKind::Lt => Some(BinOp::Lt),
            TokenKind::Gt => Some(BinOp::Gt),
            TokenKind::Le => Some(BinOp::Le),
            TokenKind::Ge => Some(BinOp::Ge),
            TokenKind::EqEq => Some(BinOp::Eq),
            TokenKind::Ne => Some(BinOp::Ne),
            _ => None,
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
            Chain::BinExpr { span, .. } | Chain::UnaryCmp { span, .. } => *span,
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

    /// Fuzzy match in arg: `gate(message ~ "out of memory")`.
    /// Note Flow is left-associative: `a -> b -> c` is `Flow(Flow(a,b), c)`.
    #[test]
    fn fuzzy_match_arg() {
        let m = module(r#"errors -> gate(message ~ "out of memory") -> oom_alert"#);
        // Connect(Flow(Flow(errors, gate(...)), oom_alert))
        let Stmt::Connect(Chain::Flow(left, _oom, _)) = &m.stmts[0] else { panic!() };
        let Chain::Flow(_errors, gate_call, _) = left.as_ref() else { panic!() };
        let Chain::Call(call) = gate_call.as_ref() else { panic!() };
        assert_eq!(call.callee.joined(), "gate");
        assert_eq!(call.args.len(), 1);
        let Arg::Positional(Chain::BinExpr { op, .. }) = &call.args[0] else {
            panic!("expected BinExpr arg, got {:?}", call.args[0]);
        };
        assert_eq!(*op, BinOp::FuzzyMatch);
    }

    /// Unary comparison shorthand: `gate(> 5x, knee: 2x)`.
    #[test]
    fn unary_comparison_arg() {
        let m = module("delta(error_rate) -> gate(> 5x, knee: 2x) -> spike_alert");
        // Connect(Flow(Flow(delta_call, gate_call), spike_alert))
        let Stmt::Connect(Chain::Flow(left, _spike, _)) = &m.stmts[0] else { panic!() };
        let Chain::Flow(_delta, gate_call, _) = left.as_ref() else { panic!() };
        let Chain::Call(call) = gate_call.as_ref() else { panic!() };
        assert_eq!(call.callee.joined(), "gate");
        assert_eq!(call.args.len(), 2);
        // First arg: UnaryCmp(Gt, Ratio(5))
        let Arg::Positional(Chain::UnaryCmp { op, rhs, .. }) = &call.args[0] else {
            panic!("expected UnaryCmp, got {:?}", call.args[0]);
        };
        assert_eq!(*op, BinOp::Gt);
        let Chain::Atom(Atom::Literal(Literal::Ratio(v, _))) = rhs.as_ref() else { panic!() };
        assert_eq!(*v, 5.0);
        // Second arg: knee: 2x
        let Arg::Named { name, value, .. } = &call.args[1] else { panic!() };
        assert_eq!(name.text, "knee");
        let Chain::Atom(Atom::Literal(Literal::Ratio(v, _))) = value else { panic!() };
        assert_eq!(*v, 2.0);
    }

    /// Merge with `within(N)` policy. `programs/02_log_monitor.zp:50-54`.
    #[test]
    fn merge_with_within_policy() {
        let src = "oom_alert -> |
| within(30s) | -> crash_pattern -> alert(critical: \"service crash loop\")
conn_alert -> |
";
        let m = module(src);
        assert_eq!(m.stmts.len(), 1, "expected 1 coalesced stmt, got {}", m.stmts.len());
        let Stmt::Connect(Chain::Merge(merge)) = &m.stmts[0] else { panic!() };
        // Inputs: oom_alert, conn_alert (the policy line has no input).
        let names: Vec<_> = merge.inputs.iter().map(|c| match c {
            Chain::Atom(Atom::Path(p)) => p.joined(),
            other => panic!("unexpected input: {:?}", other),
        }).collect();
        assert_eq!(names, vec!["oom_alert", "conn_alert"]);
        // Policy: Within(Duration(30s))
        let MergePolicy::Within(inner) = &merge.policy else {
            panic!("expected Within policy, got {:?}", merge.policy);
        };
        let Chain::Atom(Atom::Literal(Literal::Duration { value, unit, .. })) = inner.as_ref() else {
            panic!("within arg is not Duration: {:?}", inner);
        };
        assert_eq!(*value, 30.0);
        assert_eq!(*unit, DurationUnit::S);
        // Downstream: should chain through crash_pattern -> alert(...)
        assert!(merge.downstream.is_some());
    }

    /// Quorum policy: `| 2 of 3 |`.
    #[test]
    fn merge_with_quorum_policy() {
        let m = module("a -> | 2 of 3 | -> result");
        let Stmt::Connect(Chain::Merge(merge)) = &m.stmts[0] else { panic!() };
        assert_eq!(merge.policy, MergePolicy::Quorum { n: 2, m: 3 });
    }

    /// Fastest policy: `| fastest(2) |`.
    #[test]
    fn merge_with_fastest_policy() {
        let m = module("a -> | fastest(2) | -> result");
        let Stmt::Connect(Chain::Merge(merge)) = &m.stmts[0] else { panic!() };
        assert_eq!(merge.policy, MergePolicy::Fastest(2));
    }

    /// Any policy: `| any |`.
    #[test]
    fn merge_with_any_policy() {
        let m = module("a -> | any | -> result");
        let Stmt::Connect(Chain::Merge(merge)) = &m.stmts[0] else { panic!() };
        assert_eq!(merge.policy, MergePolicy::Any);
    }
}
