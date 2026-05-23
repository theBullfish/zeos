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
    ///      → `Connect(Flow(INPUT, Merge { inputs: [], ... }))`
    ///   2. `| policy | -> DOWNSTREAM` (no input)
    ///      → `Connect(Merge { inputs: [], policy, downstream })`
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
        // Unwrap an outer `Bind(...)` wrapper if present: a merge with
        // a stmt-level shape annotation (`| -> spread : best_ask - best_bid`)
        // parses as `Connect(Bind(Merge{...}, shape))`. We coalesce the
        // inner Merge as a fragment and re-wrap with the Bind on the way
        // back out so the shape annotation is preserved.
        let (bind_outer_shape, bind_outer_span, stmt) = match stmt {
            Stmt::Connect(Chain::Bind(inner, shape, span)) => match *inner {
                Chain::Merge(_) | Chain::Flow(_, _, _) => {
                    (Some(*shape), Some(span), Stmt::Connect(*inner))
                }
                other => return Some(Stmt::Connect(Chain::Bind(Box::new(other), shape, span))),
            },
            other => (None, None, other),
        };
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
            other => {
                // Re-wrap if we had an outer Bind.
                let result = match (bind_outer_shape, bind_outer_span) {
                    (Some(shape), Some(span)) => match other {
                        Stmt::Connect(c) => Stmt::Connect(Chain::Bind(
                            Box::new(c),
                            Box::new(shape),
                            span,
                        )),
                        s => s,
                    },
                    _ => other,
                };
                return Some(result);
            }
        };

        // If the previous statement is a Connect(Merge) — bare or wrapped
        // in `Connect(Bind(Merge, _))` — fold this fragment in.
        let prev_merge: Option<&mut Merge<'src>> = match stmts.last_mut() {
            Some(Stmt::Connect(Chain::Merge(m))) => Some(m),
            Some(Stmt::Connect(Chain::Bind(inner, _, _))) => match inner.as_mut() {
                Chain::Merge(m) => Some(m),
                _ => None,
            },
            _ => None,
        };
        if let Some(prev) = prev_merge {
            if let Some(input) = maybe_input {
                prev.inputs.push(input);
            }
            prev.inputs.extend(frag_merge.inputs);
            if !matches!(frag_merge.policy, MergePolicy::All) {
                prev.policy = frag_merge.policy;
            }
            if frag_merge.downstream.is_some() {
                prev.downstream = frag_merge.downstream;
            }
            return None;
        }

        // No predecessor merge — start a new one. Re-apply outer Bind
        // wrapper if the stmt had a shape annotation.
        let mut merge = frag_merge;
        if let Some(input) = maybe_input {
            merge.inputs.insert(0, input);
        }
        let merged = Chain::Merge(merge);
        let chain = match (bind_outer_shape, bind_outer_span) {
            (Some(shape), Some(span)) => Chain::Bind(Box::new(merged), Box::new(shape), span),
            _ => merged,
        };
        Some(Stmt::Connect(chain))
    }

    fn parse_stmt(&mut self) -> Result<Stmt<'src>, ParseError> {
        // `node IDENT { ... }` — declaration form WITHOUT a colon. Parse
        // the body as a fork and synthesize a wire decl with that as the
        // chain. Same handling for `chain IDENT { ... }`.
        if self.peek_kind() == Some(TokenKind::Ident)
            && matches!(self.cur().text, "node" | "chain")
            && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
            && self.kind_at(self.pos + 2) == Some(TokenKind::LBrace)
        {
            let start = self.cur_pos();
            self.advance(); // node|chain
            let name = self.parse_path()?;
            let fork = self.parse_fork()?;
            let span = Span::new(start, self.span_of(&fork).end);
            return Ok(Stmt::Wire(WireDecl {
                name,
                args: None,
                chain: fork,
                span,
            }));
        }
        if self.looks_like_wire_decl() {
            let w = self.parse_wire_decl()?;
            Ok(Stmt::Wire(w))
        } else {
            let mut chain = self.parse_chain()?;
            // Stmt-level postfix Bind: `<chain> : <expr>` and `<chain> : { fork }`.
            // Common pattern from chains like `fused -> rank : weighted(...)`
            // and `-> input : { movement, aim, actions }`. Only fold at stmt
            // level so fork branch labels (which are inside `{}`) aren't
            // captured here.
            if self.peek_kind() == Some(TokenKind::Colon) {
                self.advance();
                self.skip_newlines();
                let rhs = self.parse_chain()?;
                let span = Span::new(self.span_of(&chain).start, self.span_of(&rhs).end);
                chain = Chain::Bind(Box::new(chain), Box::new(rhs), span);
            }
            Ok(Stmt::Connect(chain))
        }
    }

    /// Lookahead: `[node]? IDENT (DOT IDENT)* (LPAREN ... RPAREN)? COLON`.
    /// Optional `node` prefix (goya_fleet.zp uses `node fleet : ...`).
    /// Call-as-wire-decl-LHS forms like `topic("orders") :` are accepted via
    /// the optional balanced-paren chunk.
    fn looks_like_wire_decl(&self) -> bool {
        let mut p = self.pos;
        // Allow `node` keyword prefix.
        if self.kind_at(p) == Some(TokenKind::Ident)
            && self.tokens.get(p).map(|t| t.text) == Some("node")
            && self.kind_at(p + 1) == Some(TokenKind::Ident)
        {
            p += 1;
        }
        if self.kind_at(p) != Some(TokenKind::Ident) {
            return false;
        }
        p += 1;
        while self.kind_at(p) == Some(TokenKind::Dot)
            && self.kind_at(p + 1) == Some(TokenKind::Ident)
        {
            p += 2;
        }
        // Optional call args.
        if self.kind_at(p) == Some(TokenKind::LParen) {
            p += 1;
            let mut depth = 1;
            while depth > 0 && p < self.tokens.len() {
                match self.kind_at(p) {
                    Some(TokenKind::LParen) => depth += 1,
                    Some(TokenKind::RParen) => depth -= 1,
                    _ => {}
                }
                p += 1;
            }
        }
        self.kind_at(p) == Some(TokenKind::Colon)
    }

    fn parse_wire_decl(&mut self) -> Result<WireDecl<'src>, ParseError> {
        let start = self.cur_pos();
        // Skip optional `node` keyword prefix.
        if self.peek_kind() == Some(TokenKind::Ident)
            && self.cur().text == "node"
            && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
        {
            self.advance();
        }
        let name = self.parse_path()?;
        let args = if self.peek_kind() == Some(TokenKind::LParen) {
            Some(self.parse_args()?)
        } else {
            None
        };
        self.expect(TokenKind::Colon)?;
        let mut chain = self.parse_chain()?;
        // Stmt-level Bind postfix also applies to wire decls: the chain RHS
        // may end with `: <expr>` to attach a shape/binding. Same handling
        // as parse_stmt's Connect path.
        if self.peek_kind() == Some(TokenKind::Colon) {
            self.advance();
            self.skip_newlines();
            let rhs = self.parse_chain()?;
            let span = Span::new(self.span_of(&chain).start, self.span_of(&rhs).end);
            chain = Chain::Bind(Box::new(chain), Box::new(rhs), span);
        }
        let end = self.cur_pos();
        Ok(WireDecl { name, args, chain, span: Span::new(start, end) })
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

    /// Chain := Term (ChainOp Term)*
    /// ChainOp := -> | ~> | <~ | -x> | <-
    ///
    /// Multi-line chains are supported: if the next non-newline token is a
    /// chain op, newlines between the prior term and the op are skipped.
    /// (`server : net.listen(...)\n    -> log_request\n    -> rate_limit`).
    fn parse_chain(&mut self) -> Result<Chain<'src>, ParseError> {
        let mut left = self.parse_chain_term()?;
        loop {
            // Arithmetic at chain level: `a + b * c -> sink`. Encoded as
            // synthetic Calls so the chain-op loop can keep treating them
            // as values feeding into the next `->`. Tightly bound — runs
            // before the chain-op probe so multi-line chains still work.
            let arith_callee = match self.peek_kind() {
                Some(TokenKind::Plus) => Some("__add__"),
                Some(TokenKind::Minus) => Some("__sub__"),
                Some(TokenKind::Star) => Some("__mul__"),
                Some(TokenKind::Slash) => Some("__div__"),
                Some(TokenKind::Percent) => Some("__mod__"),
                _ => None,
            };
            if let Some(callee) = arith_callee {
                self.advance();
                let right = self.parse_chain_term()?;
                let span = Span::new(self.span_of(&left).start, self.span_of(&right).end);
                left = Chain::Call(Call {
                    callee: Path::one(callee, span),
                    args: vec![Arg::Positional(left), Arg::Positional(right)],
                    span,
                });
                continue;
            }
            // Chain-level comparison: `air_time < coyote_time`. Encoded as
            // BinExpr so it lands in the same shape as arg-level comparisons.
            if let Some(op) = self.peek_bin_op() {
                self.advance();
                let right = self.parse_chain_term()?;
                let span = Span::new(self.span_of(&left).start, self.span_of(&right).end);
                left = Chain::BinExpr {
                    op,
                    lhs: Box::new(left),
                    rhs: Box::new(right),
                    span,
                };
                continue;
            }
            // Look past newlines: if the next non-newline token is a chain
            // op, fold the newlines and continue. Otherwise stop here.
            let mut probe = self.pos;
            while self.kind_at(probe) == Some(TokenKind::Newline) {
                probe += 1;
            }
            let op = match self.kind_at(probe) {
                Some(TokenKind::Flow) => Some(TokenKind::Flow),
                Some(TokenKind::Tap) => Some(TokenKind::Tap),
                Some(TokenKind::Feedback) => Some(TokenKind::Feedback),
                Some(TokenKind::Sever) => Some(TokenKind::Sever),
                Some(TokenKind::BindLeft) => Some(TokenKind::BindLeft),
                // `then` keyword acts as Flow with temporal sequencing
                // semantics — but ONLY when not used as a named-arg label
                // (`then: time ascending`). Followed-by-Colon means it's
                // the name half of a named arg and stays out of chain-op
                // territory.
                Some(TokenKind::Ident)
                    if self.tokens.get(probe).map(|t| t.text) == Some("then")
                        && self.kind_at(probe + 1) != Some(TokenKind::Colon) =>
                {
                    Some(TokenKind::Flow)
                }
                _ => None,
            };
            let Some(op_kind) = op else { break };
            // Consume the skipped newlines now that we know we're chaining.
            self.pos = probe;
            self.advance();
            // Allow newline after a chain operator for multi-line chains.
            self.skip_newlines();
            let right = self.parse_chain_term()?;
            // For `<-` (and now `->` after a final IDENT atom: type-union
            // shorthand like `-> open | closed`, `-> dry | wet`), eat
            // trailing `| IDENT` and `, IDENT` segments. Without this,
            // `door_front : house.sensor(...) -> open | closed` would
            // produce a Flow followed by a stray Merge.
            let right = if matches!(op_kind, TokenKind::BindLeft) {
                let mut r = right;
                loop {
                    let kind = self.peek_kind();
                    let sep = matches!(kind, Some(TokenKind::Pipe) | Some(TokenKind::Comma));
                    if !sep || self.kind_at(self.pos + 1) != Some(TokenKind::Ident) {
                        break;
                    }
                    self.advance(); // sep
                    let extra = self.advance(); // ident — discarded
                    let new_end = extra.end;
                    let start = self.span_of(&r).start;
                    r = Chain::Call(Call {
                        callee: Path::one("__union__", Span::new(start, new_end)),
                        args: vec![Arg::Positional(r)],
                        span: Span::new(start, new_end),
                    });
                }
                self.maybe_unit_annotate(r)?
            } else if matches!(op_kind, TokenKind::Flow)
                && matches!(right, Chain::Atom(Atom::Path(_)))
                && self.peek_kind() == Some(TokenKind::Pipe)
                && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
            {
                let mut r = right;
                while self.peek_kind() == Some(TokenKind::Pipe)
                    && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
                {
                    self.advance();
                    let extra = self.advance();
                    let new_end = extra.end;
                    let start = self.span_of(&r).start;
                    r = Chain::Call(Call {
                        callee: Path::one("__union__", Span::new(start, new_end)),
                        args: vec![Arg::Positional(r)],
                        span: Span::new(start, new_end),
                    });
                }
                r
            } else {
                right
            };
            let span = Span::new(self.span_of(&left).start, self.span_of(&right).end);
            left = match op_kind {
                TokenKind::Flow => Chain::Flow(Box::new(left), Box::new(right), span),
                TokenKind::Tap => Chain::Tap(Box::new(left), Box::new(right), span),
                TokenKind::Feedback => Chain::Feedback(Box::new(left), Box::new(right), span),
                TokenKind::Sever => Chain::Sever(Box::new(left), Box::new(right), span),
                TokenKind::BindLeft => Chain::Bind(Box::new(left), Box::new(right), span),
                _ => unreachable!(),
            };
        }
        // Chain-level ternary: `cond ? then : else`. Used at top level too
        // (`vision_radius : role == "imposter" ? imposter_vision : crew_vision`).
        if self.peek_kind() == Some(TokenKind::Question) {
            self.advance();
            let then_branch = self.parse_chain()?;
            self.expect(TokenKind::Colon)?;
            let else_branch = self.parse_chain()?;
            let span = Span::new(
                self.span_of(&left).start,
                self.span_of(&else_branch).end,
            );
            left = Chain::Call(Call {
                callee: Path::one("__ternary__", span),
                args: vec![
                    Arg::Positional(left),
                    Arg::Positional(then_branch),
                    Arg::Positional(else_branch),
                ],
                span,
            });
        }
        Ok(left)
    }

    fn parse_chain_term(&mut self) -> Result<Chain<'src>, ParseError> {
        // Bare operator as a value: `reduce(+)`, `reduce(*)`, `fold(/)`.
        // When `+ - * /` is followed immediately by `)` or `,`, treat as a
        // function reference encoded as Path with the operator's name.
        if matches!(
            self.peek_kind(),
            Some(TokenKind::Plus | TokenKind::Minus | TokenKind::Star | TokenKind::Slash | TokenKind::Percent)
        ) && matches!(
            self.kind_at(self.pos + 1),
            Some(TokenKind::RParen | TokenKind::Comma | TokenKind::RBracket | TokenKind::RBrace)
        ) {
            let tok = self.advance();
            let name = match tok.kind {
                TokenKind::Plus => "+",
                TokenKind::Minus => "-",
                TokenKind::Star => "*",
                TokenKind::Slash => "/",
                TokenKind::Percent => "%",
                _ => unreachable!(),
            };
            return Ok(Chain::Atom(Atom::Path(Path::one(name, Span::new(tok.start, tok.end)))));
        }
        // Implicit-self field reference: `.field` at the start of a chain
        // term means "the current value's <field>" (used inside fork bodies).
        if self.peek_kind() == Some(TokenKind::Dot)
            && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
        {
            let dot = self.advance();
            let seg = self.expect(TokenKind::Ident)?;
            let span = Span::new(dot.start, seg.end);
            let term = Chain::Call(Call {
                callee: Path::one("__field_self__", span),
                args: vec![Arg::Positional(Chain::Atom(Atom::Path(Path::one(seg.text, Span::new(seg.start, seg.end)))))],
                span,
            });
            return self.maybe_unit_annotate(term);
        }
        // Unary `+` / `-` / `*` / `/` prefix on a chain term.
        // `* 2`, `/ count` are "lift" forms used in fold-style chains:
        // `range -> reduce(+) -> / count` means "divide by count".
        if matches!(
            self.peek_kind(),
            Some(TokenKind::Minus | TokenKind::Plus | TokenKind::Star | TokenKind::Slash)
        ) {
            let prefix_tok = self.advance();
            let inner = self.parse_chain_term()?;
            let span = Span::new(prefix_tok.start, self.span_of(&inner).end);
            let callee = match prefix_tok.kind {
                TokenKind::Minus => "__neg__",
                TokenKind::Plus => "__pos__",
                TokenKind::Star => "__mul_lift__",
                TokenKind::Slash => "__div_lift__",
                _ => unreachable!(),
            };
            let term = Chain::Call(Call {
                callee: Path::one(callee, span),
                args: vec![Arg::Positional(inner)],
                span,
            });
            return self.maybe_unit_annotate(term);
        }
        let cur = self.cur();
        let term = match cur.kind {
            TokenKind::Ident => {
                let path = self.parse_path()?;
                // Generic suffix `< ... >` — `list<waypoint @ m, speed @ m/s>`.
                // Disambiguates from comparison via lookahead: `<` followed by
                // Ident followed by one of `, > @ <` is a generic; otherwise
                // we leave the `<` alone for chain-level / arg-level handling.
                if self.peek_kind() == Some(TokenKind::Lt)
                    && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
                    && matches!(
                        self.kind_at(self.pos + 2),
                        Some(TokenKind::Comma | TokenKind::Gt | TokenKind::At | TokenKind::Lt)
                    )
                {
                    self.advance(); // <
                    let mut depth = 1usize;
                    while depth > 0 && !self.at_eof() {
                        match self.peek_kind() {
                            Some(TokenKind::Lt) => { depth += 1; self.advance(); }
                            Some(TokenKind::Gt) => { depth -= 1; self.advance(); }
                            _ => { self.advance(); }
                        }
                    }
                }
                let mut term = if self.peek_kind() == Some(TokenKind::LParen) {
                    let args = self.parse_args()?;
                    let end = self.prev_end();
                    Chain::Call(Call { callee: path.clone(), args, span: Span::new(path.span.start, end) })
                } else if self.peek_kind() == Some(TokenKind::LBrace) {
                    // `IDENT { fork_body }` — fork passed as the sole arg of
                    // the call. e.g. `transcode { resolution: 1080p, ... }`.
                    let fork = self.parse_fork()?;
                    let end = self.span_of(&fork).end;
                    Chain::Call(Call {
                        callee: path.clone(),
                        args: vec![Arg::Positional(fork)],
                        span: Span::new(path.span.start, end),
                    })
                } else {
                    Chain::Atom(Atom::Path(path))
                };
                // `Call.field` — chained field access after a call.
                while self.peek_kind() == Some(TokenKind::Dot)
                    && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
                {
                    self.advance(); // dot
                    let seg = self.expect(TokenKind::Ident)?;
                    // Encode field access as a synthetic `__field__` call so
                    // we don't need a dedicated AST variant.
                    let span = Span::new(self.span_of(&term).start, seg.end);
                    term = Chain::Call(Call {
                        callee: Path::one("__field__", span),
                        args: vec![
                            Arg::Positional(term),
                            Arg::Positional(Chain::Atom(Atom::Path(Path::one(seg.text, Span::new(seg.start, seg.end))))),
                        ],
                        span,
                    });
                }
                term
            }
            TokenKind::String => {
                self.advance();
                Chain::Atom(Atom::Literal(Literal::String(cur.text, Span::new(cur.start, cur.end))))
            }
            TokenKind::TemplateString => {
                self.advance();
                Chain::Atom(Atom::Literal(Literal::TemplateString(cur.text, Span::new(cur.start, cur.end))))
            }
            TokenKind::Int => {
                self.advance();
                let value: i64 = cur.text.parse().map_err(|e| ParseError {
                    message: format!("invalid integer literal '{}': {}", cur.text, e),
                    span: Span::new(cur.start, cur.end),
                })?;
                Chain::Atom(Atom::Literal(Literal::Int(value, Span::new(cur.start, cur.end))))
            }
            TokenKind::Float => {
                self.advance();
                let value: f64 = cur.text.parse().map_err(|e| ParseError {
                    message: format!("invalid float literal '{}': {}", cur.text, e),
                    span: Span::new(cur.start, cur.end),
                })?;
                Chain::Atom(Atom::Literal(Literal::Float(value, Span::new(cur.start, cur.end))))
            }
            TokenKind::Duration => {
                self.advance();
                let (val, unit) = parse_duration(cur.text)
                    .ok_or_else(|| ParseError {
                        message: format!("invalid duration literal '{}'", cur.text),
                        span: Span::new(cur.start, cur.end),
                    })?;
                Chain::Atom(Atom::Literal(Literal::Duration {
                    value: val, unit, span: Span::new(cur.start, cur.end),
                }))
            }
            TokenKind::Ratio => {
                self.advance();
                let val: f64 = cur.text.trim_end_matches('x').parse().unwrap_or(0.0);
                Chain::Atom(Atom::Literal(Literal::Ratio(val, Span::new(cur.start, cur.end))))
            }
            TokenKind::PercentLit => {
                self.advance();
                let val: f64 = cur.text.trim_end_matches('%').parse().unwrap_or(0.0);
                Chain::Atom(Atom::Literal(Literal::Percent(val, Span::new(cur.start, cur.end))))
            }
            TokenKind::Sigma => {
                self.advance();
                let val: f64 = cur.text.trim_end_matches('σ').parse().unwrap_or(0.0);
                Chain::Atom(Atom::Literal(Literal::Sigma(val, Span::new(cur.start, cur.end))))
            }
            TokenKind::Hex => {
                self.advance();
                let val = u64::from_str_radix(cur.text.trim_start_matches("0x"), 16).unwrap_or(0);
                Chain::Atom(Atom::Literal(Literal::Hex(val, Span::new(cur.start, cur.end))))
            }
            TokenKind::HexColor => {
                self.advance();
                Chain::Atom(Atom::Literal(Literal::HexColor(cur.text, Span::new(cur.start, cur.end))))
            }
            TokenKind::Dimension => {
                self.advance();
                let parts: Vec<&str> = cur.text.split('x').collect();
                let w: u64 = parts.first().and_then(|s| s.parse().ok()).unwrap_or(0);
                let h: u64 = parts.get(1).and_then(|s| s.parse().ok()).unwrap_or(0);
                Chain::Atom(Atom::Literal(Literal::Dimension { w, h, span: Span::new(cur.start, cur.end) }))
            }
            TokenKind::ByteSize => {
                self.advance();
                // 200KB / 2MB / 1GB / 4TB
                let (num_str, mult): (&str, u64) =
                    if let Some(s) = cur.text.strip_suffix("KB") { (s, 1024) }
                    else if let Some(s) = cur.text.strip_suffix("MB") { (s, 1024 * 1024) }
                    else if let Some(s) = cur.text.strip_suffix("GB") { (s, 1024u64.pow(3)) }
                    else if let Some(s) = cur.text.strip_suffix("TB") { (s, 1024u64.pow(4)) }
                    else { (cur.text, 1) };
                let n: u64 = num_str.parse().unwrap_or(0);
                Chain::Atom(Atom::Literal(Literal::ByteSize {
                    bytes: n * mult, span: Span::new(cur.start, cur.end),
                }))
            }
            TokenKind::DevNull => {
                self.advance();
                Chain::Atom(Atom::DevNull(Span::new(cur.start, cur.end)))
            }
            TokenKind::TimePast => {
                self.advance();
                let steps: u32 = cur.text[2..].parse().unwrap_or(0);
                Chain::Atom(Atom::TimePast { steps, span: Span::new(cur.start, cur.end) })
            }
            TokenKind::LBrace => self.parse_fork()?,
            TokenKind::LBracket => self.parse_list()?,
            TokenKind::LParen => {
                // Parenthesized chain expression `(a + b) * c` or tuple/
                // param-list `(condition, true_val, false_val)`. If a Comma
                // follows the first expression, we have a tuple — encode as
                // `__tuple__`. Otherwise it's a `__paren__` precedence wrap.
                let start = cur.start;
                self.advance();
                self.skip_newlines();
                let first = self.parse_arg_expr()?;
                self.skip_newlines();
                if self.peek_kind() == Some(TokenKind::Comma) {
                    let mut items = vec![Arg::Positional(first)];
                    while self.peek_kind() == Some(TokenKind::Comma) {
                        self.advance();
                        self.skip_newlines();
                        if self.peek_kind() == Some(TokenKind::RParen) { break; }
                        items.push(Arg::Positional(self.parse_arg_expr()?));
                        self.skip_newlines();
                    }
                    self.expect(TokenKind::RParen)?;
                    Chain::Call(Call {
                        callee: Path::one("__tuple__", Span::new(start, self.prev_end())),
                        args: items,
                        span: Span::new(start, self.prev_end()),
                    })
                } else {
                    self.expect(TokenKind::RParen)?;
                    Chain::Call(Call {
                        callee: Path::one("__paren__", Span::new(start, self.prev_end())),
                        args: vec![Arg::Positional(first)],
                        span: Span::new(start, self.prev_end()),
                    })
                }
            }
            TokenKind::Pipe => {
                // `|` standalone — start of a merge fragment with no inputs yet.
                // Forms accepted:
                //   `|`                          — default policy All, no downstream
                //   `| -> downstream`            — All policy, downstream chain
                //   `| policy | -> downstream`   — explicit policy + downstream
                //   `| policy |`                 — explicit policy, no downstream
                self.advance();
                // Try to parse a policy. If a policy parses but no closing
                // `|` follows, the policy-position content was actually the
                // merge's downstream — rewind and treat it that way.
                let policy_start = self.pos;
                let policy = self.try_parse_merge_policy()?;
                let policy = if policy.is_some() && self.peek_kind() != Some(TokenKind::Pipe) {
                    // No closing pipe — the parsed "policy" is really the
                    // downstream chain. Rewind.
                    self.pos = policy_start;
                    None
                } else {
                    policy
                };
                if policy.is_some() {
                    self.expect(TokenKind::Pipe)?;
                }
                let downstream = self.try_parse_merge_downstream()?;
                Chain::Merge(Merge {
                    inputs: Vec::new(),
                    policy: policy.unwrap_or(MergePolicy::All),
                    downstream: downstream.map(Box::new),
                    span: Span::new(cur.start, self.prev_end()),
                })
            }
            _ => return Err(ParseError {
                message: format!("unexpected token in chain term: {:?}", cur.kind),
                span: Span::new(cur.start, cur.end),
            }),
        };

        // Postfix annotations on a chain term:
        //   `<term> @ ident`           — unit annotation (separated form)
        //   `<numeric> Ident` (tight)  — unit annotation (no-space form, e.g. `60Hz`)
        self.maybe_unit_annotate(term)
    }

    /// Apply postfix annotations to a parsed term:
    ///   - `<term> @ unit`           — unit annotation
    ///   - `<numeric_literal>Ident`  — tight unit annotation (`60Hz`)
    ///   - `<term> : { fork_body }`  — record-shape binding (game_server,
    ///     chat_system, etc. use `name : { fields }` to declare structure)
    fn maybe_unit_annotate(&mut self, mut term: Chain<'src>) -> Result<Chain<'src>, ParseError> {
        loop {
            // `.field` postfix on any chain term (not just Ident paths).
            // `arr[i].field`, `call().field`, `(a + b).field`. Encoded as
            // synthetic `__field__` Call.
            if self.peek_kind() == Some(TokenKind::Dot)
                && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
            {
                self.advance(); // dot
                let seg = self.expect(TokenKind::Ident)?;
                let span = Span::new(self.span_of(&term).start, seg.end);
                term = Chain::Call(Call {
                    callee: Path::one("__field__", span),
                    args: vec![
                        Arg::Positional(term),
                        Arg::Positional(Chain::Atom(Atom::Path(Path::one(seg.text, Span::new(seg.start, seg.end))))),
                    ],
                    span,
                });
                continue;
            }
            // Postfix call: `<term>(args)` — function-call invocation on a
            // computed receiver (e.g., closures, table-of-callables).
            if self.peek_kind() == Some(TokenKind::LParen) {
                let args = self.parse_args()?;
                let end = self.prev_end();
                let span = Span::new(self.span_of(&term).start, end);
                term = Chain::Call(Call {
                    callee: Path::one("__apply__", span),
                    args: std::iter::once(Arg::Positional(term)).chain(args).collect(),
                    span,
                });
                continue;
            }
            // `@ <unit>` form. Unit can be a path (`@ percent`), a compound
            // path (`@ m/s`), or a call (`@ mde("payload.zdx")`,
            // `@ goya(0)` for hardware-pinning forms).
            if self.peek_kind() == Some(TokenKind::At) {
                self.advance();
                // Parse a path first; if a LParen follows, lift to Call.
                let mut unit_path = self.parse_path()?;
                while self.peek_kind() == Some(TokenKind::Slash)
                    && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
                {
                    self.advance();
                    let seg = self.expect(TokenKind::Ident)?;
                    let new_end = seg.end;
                    unit_path.segments.push(Ident { text: seg.text, span: Span::new(seg.start, seg.end) });
                    unit_path.span.end = new_end;
                }
                let unit_chain: Chain<'src> = if self.peek_kind() == Some(TokenKind::LParen) {
                    let args = self.parse_args()?;
                    let end = self.prev_end();
                    Chain::Call(Call {
                        callee: unit_path.clone(),
                        args,
                        span: Span::new(unit_path.span.start, end),
                    })
                } else {
                    Chain::Atom(Atom::Path(unit_path))
                };
                let span = Span::new(self.span_of(&term).start, self.span_of(&unit_chain).end);
                term = Chain::Call(Call {
                    callee: Path::one("__unit__", Span::new(span.start, span.end)),
                    args: vec![Arg::Positional(term), Arg::Positional(unit_chain)],
                    span,
                });
                continue;
            }
            // Postfix `[ index (, index)* ]` array access — `palette[0]`,
            // `screen[0,0]` (2D), `kick_pattern[step]`. Encoded as
            // `__index__(<term>, idx1, idx2, ...)`.
            if self.peek_kind() == Some(TokenKind::LBracket) {
                self.advance();
                let mut idx_args: Vec<Arg<'src>> = vec![Arg::Positional(term.clone())];
                loop {
                    self.skip_newlines();
                    if self.peek_kind() == Some(TokenKind::RBracket) { break; }
                    idx_args.push(Arg::Positional(self.parse_arg_expr()?));
                    self.skip_newlines();
                    if self.peek_kind() == Some(TokenKind::Comma) {
                        self.advance();
                    } else {
                        break;
                    }
                }
                self.skip_newlines();
                self.expect(TokenKind::RBracket)?;
                let span = Span::new(self.span_of(&term).start, self.prev_end());
                term = Chain::Call(Call {
                    callee: Path::one("__index__", span),
                    args: idx_args,
                    span,
                });
                continue;
            }
            // `...` postfix spread — `(cells...)` means "spread cells as
            // args". Three Dots in a row, no intervening whitespace.
            if self.peek_kind() == Some(TokenKind::Dot)
                && self.kind_at(self.pos + 1) == Some(TokenKind::Dot)
                && self.kind_at(self.pos + 2) == Some(TokenKind::Dot)
            {
                let d1 = self.cur();
                let d2 = self.tokens[self.pos + 1];
                let d3 = self.tokens[self.pos + 2];
                if d1.start == self.span_of(&term).end
                    && d2.start == d1.end
                    && d3.start == d2.end
                {
                    self.advance();
                    self.advance();
                    self.advance();
                    let span = Span::new(self.span_of(&term).start, d3.end);
                    term = Chain::Call(Call {
                        callee: Path::one("__spread__", span),
                        args: vec![Arg::Positional(term)],
                        span,
                    });
                    continue;
                }
            }
            // `..` range operator — `0..10`, `"1".."9"`, `"a".."z"`. Two
            // Dot tokens with no intervening whitespace form a range.
            if self.peek_kind() == Some(TokenKind::Dot)
                && self.kind_at(self.pos + 1) == Some(TokenKind::Dot)
                && (is_numeric_literal(&term) || is_string_literal(&term))
            {
                let dot1 = self.cur();
                let dot2 = self.tokens[self.pos + 1];
                if dot1.start == self.span_of(&term).end && dot2.start == dot1.end {
                    self.advance();
                    self.advance();
                    let hi = self.parse_chain_term()?;
                    let span = Span::new(self.span_of(&term).start, self.span_of(&hi).end);
                    term = Chain::Call(Call {
                        callee: Path::one("__range__", span),
                        args: vec![Arg::Positional(term), Arg::Positional(hi)],
                        span,
                    });
                    continue;
                }
            }
            // `↑` / `↓` postfix amplifier on a path/call.
            if matches!(self.peek_kind(), Some(TokenKind::HeatUp) | Some(TokenKind::HeatDown)) {
                let op_tok = self.advance();
                let callee = if op_tok.kind == TokenKind::HeatUp { "__heat_up__" } else { "__heat_down__" };
                let span = Span::new(self.span_of(&term).start, op_tok.end);
                term = Chain::Call(Call {
                    callee: Path::one(callee, span),
                    args: vec![Arg::Positional(term)],
                    span,
                });
                continue;
            }
            // `: { ... }` shape-binding form.
            if self.peek_kind() == Some(TokenKind::Colon)
                && self.kind_at(self.pos + 1) == Some(TokenKind::LBrace)
            {
                self.advance(); // colon
                let fork = self.parse_fork()?;
                let span = Span::new(self.span_of(&term).start, self.span_of(&fork).end);
                term = Chain::Bind(Box::new(term), Box::new(fork), span);
                continue;
            }
            // `Call { fork }` — postfix fork after a Call (or any term),
            // equivalent to passing the fork as the final positional arg.
            // `app("Quill Present") { content }`, `state_machine { ... }`.
            // Only kick in when we don't have `:` immediately after term —
            // that's the `: { fork }` case above.
            if self.peek_kind() == Some(TokenKind::LBrace)
                && matches!(term, Chain::Call(_))
            {
                let fork = self.parse_fork()?;
                let span = Span::new(self.span_of(&term).start, self.span_of(&fork).end);
                term = Chain::Bind(Box::new(term), Box::new(fork), span);
                continue;
            }
            // Tight numeric+Ident form: `60Hz`.
            if self.peek_kind() == Some(TokenKind::Ident) {
                let term_end = self.span_of(&term).end;
                let next = self.cur();
                if next.start == term_end && is_numeric_literal(&term) {
                    self.advance();
                    let unit = Path::one(next.text, Span::new(next.start, next.end));
                    let span = Span::new(self.span_of(&term).start, next.end);
                    term = Chain::Call(Call {
                        callee: Path::one("__unit__", span),
                        args: vec![
                            Arg::Positional(term),
                            Arg::Positional(Chain::Atom(Atom::Path(unit))),
                        ],
                        span,
                    });
                    continue;
                }
            }
            // Tight rate literal: `100/m` — numeric followed (no whitespace)
            // by `/IDENT`. Synthesizes a `__rate__` call.
            if self.peek_kind() == Some(TokenKind::Slash) && is_numeric_literal(&term) {
                let term_end = self.span_of(&term).end;
                let slash = self.cur();
                if slash.start == term_end
                    && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
                {
                    let next = self.tokens[self.pos + 1];
                    if next.start == slash.end {
                        self.advance(); // /
                        self.advance(); // ident
                        let unit = Path::one(next.text, Span::new(next.start, next.end));
                        let span = Span::new(self.span_of(&term).start, next.end);
                        term = Chain::Call(Call {
                            callee: Path::one("__rate__", span),
                            args: vec![
                                Arg::Positional(term),
                                Arg::Positional(Chain::Atom(Atom::Path(unit))),
                            ],
                            span,
                        });
                        continue;
                    }
                }
            }
            break;
        }
        Ok(term)
    }

    /// `{ branch (, branch)* }` — fork. Each branch is a chain expression,
    /// optionally labeled with `<label>: <body>`. Branches may be comma-
    /// or whitespace-separated. The `chain X { pulse beat ring log }`
    /// declaration form (30/31/32) uses whitespace separation.
    fn parse_fork(&mut self) -> Result<Chain<'src>, ParseError> {
        let start = self.cur_pos();
        self.expect(TokenKind::LBrace)?;
        let mut branches = Vec::new();
        loop {
            self.skip_newlines();
            if self.peek_kind() == Some(TokenKind::RBrace) {
                break;
            }
            let branch = self.parse_fork_branch()?;
            // Coalesce vertical-merge fragments inside fork bodies (chord
            // rule, mirroring `try_coalesce_vertical_merge` at stmt level).
            // A `INPUT -> |` or `| policy | -> DOWNSTREAM` branch folds
            // into the previous branch if that one is an unlabeled Merge.
            if let Some(out) = Self::try_coalesce_fork_merge(branch, &mut branches) {
                branches.push(out);
            }
            self.skip_newlines();
            if self.peek_kind() == Some(TokenKind::Comma) {
                self.advance();
                continue;
            }
            // Whitespace-separated branch continuation (chain X {a b c}).
            if self.peek_kind() == Some(TokenKind::RBrace) {
                break;
            }
            if self.peek_can_start_chain_term(self.pos)
                || self.peek_kind() == Some(TokenKind::Flow) // implicit-flow
            {
                continue;
            }
            break;
        }
        self.skip_newlines();
        self.expect(TokenKind::RBrace)?;
        Ok(Chain::Fork(branches, Span::new(start, self.prev_end())))
    }

    /// Coalesce a fork branch that is a vertical-merge fragment into the
    /// previous branch's `Merge` body. Returns `Some(branch)` to push, or
    /// `None` if absorbed.
    fn try_coalesce_fork_merge(
        branch: ForkBranch<'src>,
        branches: &mut Vec<ForkBranch<'src>>,
    ) -> Option<ForkBranch<'src>> {
        if branch.label.is_some() {
            return Some(branch);
        }
        let span = branch.span;
        let (maybe_input, frag_merge) = match branch.body {
            Chain::Flow(lhs, rhs, _) if matches!(rhs.as_ref(), Chain::Merge(_)) => {
                let m = match *rhs {
                    Chain::Merge(m) => m,
                    _ => unreachable!(),
                };
                (Some(*lhs), m)
            }
            Chain::Merge(m) => (None, m),
            other => {
                return Some(ForkBranch { label: None, body: other, span });
            }
        };
        if let Some(prev) = branches.last_mut() {
            if prev.label.is_none() {
                if let Chain::Merge(prev_merge) = &mut prev.body {
                    if let Some(input) = maybe_input {
                        prev_merge.inputs.push(input);
                    }
                    prev_merge.inputs.extend(frag_merge.inputs);
                    if !matches!(frag_merge.policy, MergePolicy::All) {
                        prev_merge.policy = frag_merge.policy;
                    }
                    if frag_merge.downstream.is_some() {
                        prev_merge.downstream = frag_merge.downstream;
                    }
                    return None;
                }
            }
        }
        let mut merge = frag_merge;
        if let Some(input) = maybe_input {
            merge.inputs.insert(0, input);
        }
        Some(ForkBranch {
            label: None,
            body: Chain::Merge(merge),
            span,
        })
    }

    fn parse_fork_branch(&mut self) -> Result<ForkBranch<'src>, ParseError> {
        let start = self.cur_pos();
        // Implicit-flow leading `->` branch.
        if self.peek_kind() == Some(TokenKind::Flow) {
            self.advance();
            let rest = self.parse_arg_expr()?;
            let span = Span::new(start, self.span_of(&rest).end);
            let body = Chain::Flow(
                Box::new(Chain::Atom(Atom::Path(Path::one("__from_upstream__", Span::new(start, start))))),
                Box::new(rest),
                span,
            );
            return Ok(ForkBranch { label: None, body, span });
        }
        let first = self.parse_arg_expr()?;
        // Labeled / assigned branch — accept either `label : body` or
        // `lhs = rhs` forms. The latter handles in-fork "assignment-style"
        // mutations like `target.state = dead`.
        if matches!(self.peek_kind(), Some(TokenKind::Colon | TokenKind::Eq)) {
            self.advance();
            // Body may begin on the next line — `resolve:\n   tier_prime: ...`.
            self.skip_newlines();
            // Implicit-flow body: `"NOW": -> timestamp` (quill.zp:230).
            let mut body = if self.peek_kind() == Some(TokenKind::Flow) {
                let flow_start = self.cur_pos();
                self.advance();
                let rest = self.parse_arg_expr()?;
                let span = Span::new(flow_start, self.span_of(&rest).end);
                Chain::Flow(
                    Box::new(Chain::Atom(Atom::Path(Path::one("__from_upstream__", Span::new(flow_start, flow_start))))),
                    Box::new(rest),
                    span,
                )
            } else {
                self.parse_arg_expr()?
            };
            // Type-union branch body: `label : a | b | c`.
            while self.peek_kind() == Some(TokenKind::Pipe)
                && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
            {
                self.advance();
                let extra = self.advance();
                let new_end = extra.end;
                let bstart = self.span_of(&body).start;
                body = Chain::Call(Call {
                    callee: Path::one("__union__", Span::new(bstart, new_end)),
                    args: vec![Arg::Positional(body)],
                    span: Span::new(bstart, new_end),
                });
            }
            // Bind-postfix on body: `out: health : list<card_health>`.
            // The trailing `: <expr>` attaches a shape spec to the body.
            if self.peek_kind() == Some(TokenKind::Colon) {
                self.advance();
                let rhs = self.parse_arg_expr()?;
                let bstart = self.span_of(&body).start;
                let bend = self.span_of(&rhs).end;
                body = Chain::Bind(Box::new(body), Box::new(rhs), Span::new(bstart, bend));
            }
            let span = Span::new(start, self.span_of(&body).end);
            Ok(ForkBranch { label: Some(first), body, span })
        } else {
            // Bare branch — no label.
            let span = Span::new(start, self.span_of(&first).end);
            Ok(ForkBranch { label: None, body: first, span })
        }
    }

    /// `[ a, b, c ]` — list literal. Comma-separated chain expressions.
    fn parse_list(&mut self) -> Result<Chain<'src>, ParseError> {
        let start = self.cur_pos();
        self.expect(TokenKind::LBracket)?;
        let mut items = Vec::new();
        loop {
            self.skip_newlines();
            if self.peek_kind() == Some(TokenKind::RBracket) {
                break;
            }
            items.push(self.parse_arg_expr()?);
            self.skip_newlines();
            if self.peek_kind() == Some(TokenKind::Comma) {
                self.advance();
            } else {
                break;
            }
        }
        self.skip_newlines();
        self.expect(TokenKind::RBracket)?;
        let span = Span::new(start, self.prev_end());
        // Encode lists as Fork with no-label branches for v1 — semantic layer
        // can specialize later. The shape (branches, span) is identical.
        let branches = items.into_iter().map(|c| {
            let s = self.span_of_owned(&c);
            ForkBranch { label: None, body: c, span: s }
        }).collect();
        Ok(Chain::Fork(branches, span))
    }

    fn span_of_owned(&self, c: &Chain<'src>) -> Span {
        self.span_of(c)
    }

    /// After a `|`, parse a downstream chain if one's there. The downstream
    /// can be introduced explicitly by `->` (`| -> name`) or implicitly when
    /// the next token starts a chain term (`| alert(critical: "...")`).
    fn try_parse_merge_downstream(&mut self) -> Result<Option<Chain<'src>>, ParseError> {
        if self.peek_kind() == Some(TokenKind::Flow) {
            self.advance();
            self.skip_newlines();
            return Ok(Some(self.parse_chain()?));
        }
        // Implicit downstream — only when the next token is something that
        // can start a chain term AND isn't another `|` (which would belong
        // to a separate merge fragment).
        if self.peek_can_start_chain_term(self.pos)
            && self.peek_kind() != Some(TokenKind::Pipe)
        {
            return Ok(Some(self.parse_chain()?));
        }
        Ok(None)
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
                    "merge" | "sort" => {
                        // `merge` / `sort` can appear standalone (treated as
                        // a no-op = MergePolicy::All) or with arguments. The
                        // arguments are flexible — `by:`, `method:`,
                        // `sort:` etc. — so we accept any arg list and
                        // keep only the first `by:` value as the sort key.
                        self.advance();
                        if self.peek_kind() != Some(TokenKind::LParen) {
                            return Ok(Some(MergePolicy::All));
                        }
                        self.advance(); // (
                        let mut by_path: Option<Path<'src>> = None;
                        loop {
                            self.skip_newlines();
                            if self.peek_kind() == Some(TokenKind::RParen) { break; }
                            // Try to capture `by: <path>` if present.
                            if self.peek_kind() == Some(TokenKind::Ident)
                                && self.cur().text == "by"
                                && self.kind_at(self.pos + 1) == Some(TokenKind::Colon)
                            {
                                self.advance(); // by
                                self.advance(); // :
                                by_path = Some(self.parse_path()?);
                            } else {
                                // Discard any other arg.
                                let _ = self.parse_arg()?;
                            }
                            self.skip_newlines();
                            if self.peek_kind() == Some(TokenKind::Comma) {
                                self.advance();
                                continue;
                            }
                            // Whitespace-separated continuation (mirrors parse_args).
                            if self.peek_can_start_chain_term(self.pos)
                                || matches!(self.peek_kind(), Some(TokenKind::Bang | TokenKind::Lt | TokenKind::Gt | TokenKind::Le | TokenKind::Ge | TokenKind::EqEq | TokenKind::Ne))
                            {
                                continue;
                            }
                            break;
                        }
                        self.skip_newlines();
                        self.expect(TokenKind::RParen)?;
                        Ok(Some(by_path.map(MergePolicy::By).unwrap_or(MergePolicy::All)))
                    }
                    _ => {
                        // Arbitrary Call as merge policy: `| gate(mismatch) |`.
                        // Encoded via MergePolicy::By with the call name —
                        // a custom predicate the runtime can dispatch on.
                        if self.kind_at(start_pos + 1) == Some(TokenKind::LParen) {
                            let path = self.parse_path()?;
                            // consume the call args (discarded for v1)
                            let _ = self.parse_args()?;
                            return Ok(Some(MergePolicy::By(path)));
                        }
                        Ok(None)
                    }
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

    /// Args := LPAREN (Arg ((COMMA | <whitespace>) Arg)*)? RPAREN
    /// Arg  := IDENT COLON Chain  |  Chain
    ///
    /// Whitespace separation between args is allowed for forms like
    /// `merge(sort: price descending)` and `gate(entropy 0.4)`. After
    /// each arg, if the next token can start a fresh arg, we treat it
    /// as the next arg even without an intervening comma.
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
                continue;
            }
            // No comma — but if the next token can still start an arg,
            // treat it as the next positional arg (whitespace separation).
            if self.peek_can_start_chain_term(self.pos)
                || matches!(self.peek_kind(), Some(TokenKind::Bang) | Some(TokenKind::Lt | TokenKind::Gt | TokenKind::Le | TokenKind::Ge | TokenKind::EqEq | TokenKind::Ne))
            {
                continue;
            }
            break;
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

        // Bang-prefix at start of arg: `gate(!recording)`.
        if self.peek_kind() == Some(TokenKind::Bang) {
            let start = self.cur_pos();
            self.advance();
            let rhs = self.parse_arg_expr()?;
            let span = Span::new(start, self.span_of(&rhs).end);
            // Encode as Call to a synthetic `__not__` for v1 — semantic
            // layer can specialize. Keeps AST surface minimal.
            return Ok(Arg::Positional(Chain::Call(Call {
                callee: Path::one("__not__", span),
                args: vec![Arg::Positional(rhs)],
                span,
            })));
        }

        // `N of M` quorum in arg position: `resonance(2 of 5, within: 5m)`.
        if self.peek_kind() == Some(TokenKind::Int)
            && self.kind_at(self.pos + 1) == Some(TokenKind::Ident)
            && self.tokens.get(self.pos + 1).map(|t| t.text) == Some("of")
            && self.kind_at(self.pos + 2) == Some(TokenKind::Int)
        {
            let start = self.cur_pos();
            let n_tok = self.advance();
            self.advance(); // `of`
            let m_tok = self.advance();
            let n: u32 = n_tok.text.parse().unwrap_or(0);
            let m: u32 = m_tok.text.parse().unwrap_or(0);
            let span = Span::new(start, m_tok.end);
            // Encode as a synthetic Call so it lands in the AST without a
            // dedicated variant. The semantic layer can recognize the
            // `__quorum__` callee.
            return Ok(Arg::Positional(Chain::Call(Call {
                callee: Path::one("__quorum__", span),
                args: vec![
                    Arg::Positional(Chain::Atom(Atom::Literal(Literal::Int(n as i64, Span::new(n_tok.start, n_tok.end))))),
                    Arg::Positional(Chain::Atom(Atom::Literal(Literal::Int(m as i64, Span::new(m_tok.start, m_tok.end))))),
                ],
                span,
            })));
        }

        // Named arg lookahead: IDENT (DOT IDENT)* COLON.
        if self.peek_kind() == Some(TokenKind::Ident)
            && self.peek_named_arg_colon_offset().is_some()
        {
            let start = self.cur_pos();
            let path = self.parse_path()?;
            // Squash dotted paths into a single Ident text (preserving the
            // original spelling) for the Arg::Named name. The semantic layer
            // can split on `.` if it cares about path structure.
            let name = Ident { text: path.segments.last().unwrap().text, span: path.span };
            let _ = start;
            let start = path.span.start;
            // Accept either `:` or `=` as the named-arg separator.
            // `for = 5` and `for: 5` are both seen in the corpus.
            if self.peek_kind() == Some(TokenKind::Eq) {
                self.advance();
            } else {
                self.expect(TokenKind::Colon)?;
            }
            // Unary cmp value: `sustained: > 5m`? Not seen in corpus, but
            // handle anyway by routing through parse_arg_expr.
            let value = self.parse_arg_expr()?;
            // Type-union / option-set value: `level : error | warn`,
            // `command: "set" | "del"`, `gate(0 ~ 4 | 6 ~ 9)`. Eat
            // `| <atom-ish>` segments — anything that can start a chain
            // term and ISN'T a `|` continuation marker like a merge policy.
            while self.peek_kind() == Some(TokenKind::Pipe)
                && self.peek_can_start_chain_term(self.pos + 1)
            {
                self.advance(); // pipe
                let _ = self.parse_chain_term()?; // discard alternate
            }
            let end = self.prev_end();
            Ok(Arg::Named { name, value, span: Span::new(start, end) })
        } else {
            Ok(Arg::Positional(self.parse_arg_expr()?))
        }
    }

    /// An expression usable as an arg value / arg subject. Differs from
    /// `parse_chain` in that it accepts binary comparison / fuzzy-match
    /// operators (`~`, `>`, `<`, `<=`, `>=`, `==`, `!=`) and arithmetic
    /// (`+`, `-`) — none are chain operators.
    fn parse_arg_expr(&mut self) -> Result<Chain<'src>, ParseError> {
        // `not <expr>` keyword-prefix form — `gate(branch: not "main")`.
        if self.peek_kind() == Some(TokenKind::Ident)
            && self.cur().text == "not"
            && !matches!(
                self.kind_at(self.pos + 1),
                None | Some(TokenKind::Comma | TokenKind::RParen | TokenKind::Newline)
            )
        {
            let start_tok = self.advance();
            let inner = self.parse_arg_expr()?;
            let span = Span::new(start_tok.start, self.span_of(&inner).end);
            return Ok(Chain::Call(Call {
                callee: Path::one("__not__", span),
                args: vec![Arg::Positional(inner)],
                span,
            }));
        }
        // Unary comparison at start of an inner arg-expr too:
        // `after_silence: > 6h` — value side begins with `>`.
        if let Some(op) = self.peek_unary_cmp_op() {
            let start = self.cur_pos();
            self.advance();
            let rhs = self.parse_arg_expr()?;
            let span = Span::new(start, self.span_of(&rhs).end);
            return Ok(Chain::UnaryCmp { op, rhs: Box::new(rhs), span });
        }
        let mut left = self.parse_chain()?;
        // Comparison / fuzzy-match (single op).
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
        // Pipe as logical OR in arg-expr context.
        while self.peek_kind() == Some(TokenKind::Pipe)
            && self.peek_can_start_chain_term(self.pos + 1)
        {
            self.advance();
            self.skip_newlines();
            let mut right = self.parse_arg_expr_no_pipe()?;
            // Named-arg-style alt tail: `... | timer_done: config.discuss_time`.
            // The colon here belongs to the disjunct, not to a ternary's
            // else-branch (we're in pipe-OR context, ternary handling is
            // disabled inside the no-pipe variant).
            if self.peek_kind() == Some(TokenKind::Colon) {
                self.advance();
                let value = self.parse_arg_expr_no_pipe()?;
                let r_start = self.span_of(&right).start;
                let r_end = self.span_of(&value).end;
                right = Chain::Call(Call {
                    callee: Path::one("__named__", Span::new(r_start, r_end)),
                    args: vec![Arg::Positional(right), Arg::Positional(value)],
                    span: Span::new(r_start, r_end),
                });
            }
            let span = Span::new(self.span_of(&left).start, self.span_of(&right).end);
            left = Chain::Call(Call {
                callee: Path::one("__or__", span),
                args: vec![Arg::Positional(left), Arg::Positional(right)],
                span,
            });
        }
        // Ternary: `cond ? then : else`. Encoded as `__ternary__(c, t, e)`.
        if self.peek_kind() == Some(TokenKind::Question) {
            self.advance();
            let then_branch = self.parse_arg_expr_no_pipe()?;
            self.expect(TokenKind::Colon)?;
            let else_branch = self.parse_arg_expr_no_pipe()?;
            let span = Span::new(
                self.span_of(&left).start,
                self.span_of(&else_branch).end,
            );
            left = Chain::Call(Call {
                callee: Path::one("__ternary__", span),
                args: vec![
                    Arg::Positional(left),
                    Arg::Positional(then_branch),
                    Arg::Positional(else_branch),
                ],
                span,
            });
        }
        Ok(left)
    }

    /// Same as parse_arg_expr but without the pipe-OR fold-step. Used for
    /// the right-hand side of a `|` so we don't infinite-recurse.
    fn parse_arg_expr_no_pipe(&mut self) -> Result<Chain<'src>, ParseError> {
        if self.peek_kind() == Some(TokenKind::Ident)
            && self.cur().text == "not"
            && !matches!(
                self.kind_at(self.pos + 1),
                None | Some(TokenKind::Comma | TokenKind::RParen | TokenKind::Newline)
            )
        {
            let start_tok = self.advance();
            let inner = self.parse_arg_expr_no_pipe()?;
            let span = Span::new(start_tok.start, self.span_of(&inner).end);
            return Ok(Chain::Call(Call {
                callee: Path::one("__not__", span),
                args: vec![Arg::Positional(inner)],
                span,
            }));
        }
        if let Some(op) = self.peek_unary_cmp_op() {
            let start = self.cur_pos();
            self.advance();
            let rhs = self.parse_arg_expr_no_pipe()?;
            let span = Span::new(start, self.span_of(&rhs).end);
            return Ok(Chain::UnaryCmp { op, rhs: Box::new(rhs), span });
        }
        let mut left = self.parse_chain()?;
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

    /// Returns the offset (from `self.pos`) of a Colon or Eq that turns the
    /// current token sequence into a named-arg form
    /// `IDENT (DOT IDENT)* (COLON | EQ)`. Some corpus files use `=` instead
    /// of `:` (e.g. `sustained(for = 5)` in 30_chain_native.zp:13).
    fn peek_named_arg_colon_offset(&self) -> Option<usize> {
        let mut p = self.pos;
        if self.kind_at(p) != Some(TokenKind::Ident) {
            return None;
        }
        p += 1;
        while self.kind_at(p) == Some(TokenKind::Dot)
            && self.kind_at(p + 1) == Some(TokenKind::Ident)
        {
            p += 2;
        }
        if matches!(self.kind_at(p), Some(TokenKind::Colon) | Some(TokenKind::Eq)) {
            Some(p - self.pos)
        } else {
            None
        }
    }

    /// Whether the token at `i` can start a chain term — used to decide
    /// whether `| <something>` is a type-union continuation.
    fn peek_can_start_chain_term(&self, i: usize) -> bool {
        matches!(
            self.kind_at(i),
            Some(
                TokenKind::Ident
                | TokenKind::String
                | TokenKind::TemplateString
                | TokenKind::Int
                | TokenKind::Float
                | TokenKind::Duration
                | TokenKind::Ratio
                | TokenKind::Sigma
                | TokenKind::PercentLit
                | TokenKind::Hex
                | TokenKind::HexColor
                | TokenKind::Dimension
                | TokenKind::ByteSize
                | TokenKind::DevNull
                | TokenKind::TimePast
                | TokenKind::LBrace
                | TokenKind::LBracket
                | TokenKind::LParen
                | TokenKind::Minus
                | TokenKind::Plus
                | TokenKind::Pipe
                | TokenKind::Dot
            )
        )
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
            | Chain::Feedback(_, _, s)
            | Chain::Sever(_, _, s)
            | Chain::Bind(_, _, s) => *s,
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

fn is_string_literal(c: &Chain<'_>) -> bool {
    matches!(
        c,
        Chain::Atom(Atom::Literal(Literal::String(_, _) | Literal::TemplateString(_, _)))
    )
}

fn is_numeric_literal(c: &Chain<'_>) -> bool {
    matches!(
        c,
        Chain::Atom(Atom::Literal(
            Literal::Int(_, _)
            | Literal::Float(_, _)
            | Literal::Duration { .. }
            | Literal::Ratio(_, _)
            | Literal::Sigma(_, _)
            | Literal::Percent(_, _)
            | Literal::Hex(_, _)
            | Literal::ByteSize { .. }
            | Literal::Dimension { .. }
        ))
    )
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

    /// Feedback edge `<~` — autoregressive composition. Symmetric with
    /// Tap: same syntactic shape, opposite semantic direction. Required
    /// for LLM token loops and other recurrence.
    #[test]
    fn feedback_arrow_parses() {
        let m = module("logits <~ next_token");
        let Stmt::Connect(Chain::Feedback(_, _, _)) = &m.stmts[0] else {
            panic!("expected Feedback, got {:?}", m.stmts[0]);
        };
    }

    /// Feedback can chain through a flow: `prompt -> model -> token <~ embed`.
    /// This is the minimal autoregressive loop referenced by primitive-lab L2.04.
    #[test]
    fn feedback_in_minimal_autoregressive_loop() {
        let m = module("prompt -> model -> token <~ embed");
        // Feedback is left-associative same as other chain ops, so the
        // full parse is Feedback(Flow(Flow(prompt, model), token), embed).
        let Stmt::Connect(Chain::Feedback(lhs, _rhs, _)) = &m.stmts[0] else {
            panic!("expected Feedback at root, got {:?}", m.stmts[0]);
        };
        let Chain::Flow(_, _, _) = lhs.as_ref() else {
            panic!("expected Flow on Feedback LHS, got {:?}", lhs);
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
