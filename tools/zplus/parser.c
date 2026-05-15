/* zplus — recursive descent parser
 *
 * Grammar (simplified BNF):
 *
 *   program      := decl*
 *   decl         := import_decl | struct_decl | chain_decl | signal_decl
 *                 | priority_decl | pipeline_stmt
 *
 *   import_decl  := 'import' dotted_ident 'as' IDENT
 *   struct_decl  := 'struct' IDENT '{' field* '}'
 *   field        := IDENT ':' typeref  option_mod?  ','?
 *   chain_decl   := 'chain' IDENT '{' chain_item* '}'
 *   chain_item   := IDENT ':' pipeline
 *   signal_decl  := IDENT ':' pipeline        (starts with IDENT ':')
 *   priority_decl:= 'priority' ':' ('reflex'|'deliberate')
 *
 *   pipeline     := stage (edge_op stage)*
 *   edge_op      := '->' | '~>' | '-x>' | '<->'
 *   stage        := fork | merge | atom
 *   fork         := '{' pipeline (',' pipeline)* '}'
 *   merge        := '|' [merge_policy] '|'
 *   atom         := call | dotted | struct_init | literal | IDENT
 *               | 'input' | 'output' | 'on_silence' '(' ... ')'
 *               | 'on_block' | 'emit' '(' ... ')' | 'gate' '(' ... ')'
 *               | 'knee' ':' expr | 'delta' '(' ... ')' | 'tick' '(' ... ')'
 *               | 'debounce' '(' ... ')' | 'throttle' '(' ... ')'
 *               | 'rate' '(' ... ')' | 'within' '(' ... ')'
 *               | 'decay' '(' ... ')' | 'sustained' '(' ... ')'
 *
 *   call         := dotted_ident '(' arg_list? ')'
 *   dotted_ident := IDENT ('.' IDENT)*
 *   struct_init  := IDENT '{' sinit_field* '}'    [when IDENT '{' after struct name]
 *   sinit_field  := IDENT '=' expr ','?
 *
 *   arg_list     := arg (',' arg)*
 *   arg          := (IDENT ':')? expr
 *   expr         := comparison
 *   comparison   := addition (cmp_op addition)?
 *   cmp_op       := '==' | '!=' | '<' | '>' | '<=' | '>=' | '~'
 *   addition     := primary (('+' | '-') primary)*
 *   primary      := literal | dotted_ident | '.' IDENT | '(' expr ')'
 *                 | 'not' expr
 *
 *   typeref      := ('int'|'float'|'str'|'bool'|IDENT) (['|' typeref])*
 *   literal      := INT_LIT | FLOAT_LIT | STR_LIT | DURATION | MULTIPLIER
 */

#include "parser.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ── helpers ────────────────────────────────────────────────────── */

static void parse_error(parser_t *p, const char *fmt, ...) {
    if (p->nerrors >= PARSE_MAX_ERRORS) return;
    char *buf = p->errors[p->nerrors++];
    int off = snprintf(buf, 128, "%d:%d: ", p->cur.line, p->cur.col);
    va_list ap; va_start(ap, fmt); vsnprintf(buf+off, (size_t)(128-off), fmt, ap); va_end(ap);
    p->panic = 1;
}

static token_t advance(parser_t *p) {
    token_t t = p->cur;
    p->cur = lexer_next(&p->lex);
    return t;
}



static int check(parser_t *p, tok_kind_t k) { return p->cur.kind == k; }

static int match(parser_t *p, tok_kind_t k) {
    if (check(p, k)) { advance(p); return 1; } return 0;
}

static token_t expect(parser_t *p, tok_kind_t k) {
    if (check(p, k)) return advance(p);
    parse_error(p, "expected '%s', got '%s'", tok_name(k), tok_name(p->cur.kind));
    return p->cur;
}

static char *dup_tok(parser_t *p, token_t t) {
    return arena_strdup(&p->arena, t.start, t.len);
}

/* is the current token something that can start a stage? */
static int is_stage_start(parser_t *p) {
    tok_kind_t k = p->cur.kind;
    return k == TOK_IDENT || k == TOK_TK_INT || k == TOK_TK_FLOAT ||
           k == TOK_TK_STR || k == TOK_TK_BOOL ||
           k == TOK_LBRACE || k == TOK_PIPE ||
           k == TOK_GATE  || k == TOK_KNEE   || k == TOK_DELTA ||
           k == TOK_EMIT  || k == TOK_INPUT  || k == TOK_OUTPUT ||
           k == TOK_ON_SILENCE || k == TOK_ON_BLOCK ||
           k == TOK_SUSTAINED  || k == TOK_WITHIN   ||
           k == TOK_DEBOUNCE   || k == TOK_THROTTLE ||
           k == TOK_DECAY      || k == TOK_TICK      ||
           k == TOK_RATE       || k == TOK_INT_LIT   ||
           k == TOK_FLOAT_LIT  || k == TOK_STR_LIT   ||
           k == TOK_DURATION   || k == TOK_MULTIPLIER;
}

/* is the current token a flow-edge operator? */
static int is_edge_op(parser_t *p) {
    tok_kind_t k = p->cur.kind;
    return k == TOK_FLOW || k == TOK_TAP || k == TOK_SEVER || k == TOK_EXCHANGE;
}

static edge_t tok_to_edge(tok_kind_t k) {
    switch (k) {
    case TOK_TAP:      return EDGE_TAP;
    case TOK_SEVER:    return EDGE_SEVER;
    case TOK_EXCHANGE: return EDGE_EXCHANGE;
    default:           return EDGE_FLOW;
    }
}

/* ── keyword-as-identifier helpers ──────────────────────────────── */

/* True for tokens that can appear as identifiers in dotted names or labels.
 * In Z+, many stdlib namespaces (str, tap, vault, etc.) overlap with keywords.
 * Inside dotted names and labeled-arg positions, treat them as plain idents. */
static int is_ident_like(tok_kind_t k) {
    switch (k) {
    case TOK_IDENT:
    /* type keywords used as namespace prefixes: str.split, bool.parse, etc. */
    case TOK_TK_INT: case TOK_TK_FLOAT: case TOK_TK_STR: case TOK_TK_BOOL:
    /* any keyword that can appear as a module name, label, or dotted part */
    case TOK_RATE:   case TOK_TICK:     case TOK_DECAY:     case TOK_DELTA:
    case TOK_NOT:    case TOK_FOR:      case TOK_THEN:      case TOK_WITHIN:
    case TOK_INPUT:  case TOK_OUTPUT:   case TOK_EMIT:      case TOK_GATE:
    case TOK_KNEE:   case TOK_SUSTAINED:case TOK_DEBOUNCE:  case TOK_THROTTLE:
    case TOK_ON_SILENCE: case TOK_ON_BLOCK: case TOK_AS:    case TOK_PRIORITY:
    case TOK_REFLEX: case TOK_DELIBERATE:
        return 1;
    default: return 0;
    }
}

/* Consume current token as if it were an identifier; return its text */
static token_t consume_as_ident(parser_t *p) {
    if (is_ident_like(p->cur.kind)) return advance(p);
    return expect(p, TOK_IDENT); /* will error with useful message */
}

/* forward declarations */
static ast_node_t *parse_pipeline(parser_t *p);
static ast_node_t *parse_stage(parser_t *p);
static ast_node_t *parse_atom(parser_t *p);
static ast_node_t *parse_expr(parser_t *p);
static ast_node_t *parse_arg_list(parser_t *p, int *nargs_out);
static ast_node_t *parse_dotted(parser_t *p);

/* ── expression parser ──────────────────────────────────────────── */

static ast_node_t *parse_primary(parser_t *p) {
    int line = p->cur.line, col = p->cur.col;

    /* .field  — field access on implicit self */
    if (check(p, TOK_DOT)) {
        advance(p);
        token_t fname = expect(p, TOK_IDENT);
        ast_node_t *n = ast_new(&p->arena, AST_FIELD_ACCESS, line, col);
        if (n) { n->u.fld.obj = NULL; n->u.fld.field = dup_tok(p, fname); }
        return n;
    }

    if (check(p, TOK_NOT)) {
        advance(p);
        ast_node_t *operand = parse_primary(p);
        ast_node_t *n = ast_new(&p->arena, AST_UNARY, line, col);
        if (n) { n->u.unary.op = UNOP_NOT; n->u.unary.operand = operand; }
        return n;
    }

    if (check(p, TOK_LPAREN)) {
        advance(p);
        ast_node_t *inner = parse_expr(p);
        expect(p, TOK_RPAREN);
        return inner;
    }

    if (check(p, TOK_INT_LIT)) {
        token_t t = advance(p);
        ast_node_t *n = ast_new(&p->arena, AST_INT_LIT, line, col);
        if (n) n->u.ival.ival = t.val.ival;
        return n;
    }
    if (check(p, TOK_FLOAT_LIT)) {
        token_t t = advance(p);
        ast_node_t *n = ast_new(&p->arena, AST_FLOAT_LIT, line, col);
        if (n) n->u.fval.fval = t.val.fval;
        return n;
    }
    if (check(p, TOK_STR_LIT)) {
        token_t t = advance(p);
        ast_node_t *n = ast_new(&p->arena, AST_STR_LIT, line, col);
        if (n) { n->u.sval.s = arena_strdup(&p->arena, t.val.sval.p, t.val.sval.len);
                 n->u.sval.len = t.val.sval.len; }
        return n;
    }
    if (check(p, TOK_DURATION)) {
        token_t t = advance(p);
        ast_node_t *n = ast_new(&p->arena, AST_DURATION_LIT, line, col);
        if (n) n->u.ival.ival = t.val.ival;
        return n;
    }
    if (check(p, TOK_MULTIPLIER)) {
        token_t t = advance(p);
        ast_node_t *n = ast_new(&p->arena, AST_MULTIPLIER, line, col);
        if (n) n->u.ival.ival = t.val.ival;
        return n;
    }

    /* bare comparison: > expr, < expr, >= expr, etc.
     * In gate conditions, the left-hand side is the implicit signal value _v */
    {
        binop_t bop = BINOP_EQ; int is_cmp = 1;
        switch (p->cur.kind) {
        case TOK_GT: bop = BINOP_GT; break; case TOK_LT: bop = BINOP_LT; break;
        case TOK_GE: bop = BINOP_GE; break; case TOK_LE: bop = BINOP_LE; break;
        case TOK_EQ: bop = BINOP_EQ; break; case TOK_NEQ:bop = BINOP_NEQ;break;
        default:     is_cmp = 0; break;
        }
        if (is_cmp) {
            advance(p);
            ast_node_t *rhs = parse_primary(p);
            ast_node_t *lhs = ast_new(&p->arena, AST_IDENT, line, col);
            if (lhs) lhs->u.ident.name = (char*)"_v"; /* implicit signal value */
            ast_node_t *n = ast_new(&p->arena, AST_BINARY, line, col);
            if (n) { n->u.binary.op = bop; n->u.binary.left = lhs; n->u.binary.right = rhs; }
            return n;
        }
    }

    /* dotted ident (may be a call) — also accept keyword tokens as ident starts */
    if (is_ident_like(p->cur.kind)) {
        ast_node_t *d = parse_dotted(p);
        /* if followed by '(' it's a call */
        if (check(p, TOK_LPAREN)) {
            advance(p);
            int nargs = 0;
            ast_node_t *args = NULL;
            if (!check(p, TOK_RPAREN)) args = parse_arg_list(p, &nargs);
            expect(p, TOK_RPAREN);
            ast_node_t *n = ast_new(&p->arena, AST_CALL, line, col);
            if (n) { n->u.call.func = d; n->u.call.args = args; n->u.call.nargs = nargs; }
            return n;
        }
        /* if followed by '{' it's a struct init */
        if (check(p, TOK_LBRACE) && d->kind == AST_IDENT) {
            char *type_name = d->u.ident.name;
            advance(p); /* consume { */
            /* collect field = val pairs */
            ast_node_t *head = NULL, *tail = NULL; int nf = 0;
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                token_t fn = expect(p, TOK_IDENT);
                expect(p, TOK_ASSIGN);
                ast_node_t *val = parse_expr(p);
                match(p, TOK_COMMA);
                ast_node_t *f = ast_new(&p->arena, AST_FIELD, fn.line, fn.col);
                if (f) { f->u.field.name = dup_tok(p, fn); f->u.field.type = val; }
                if (!head) head = tail = f; else { tail->next = f; tail = f; } nf++;
            }
            expect(p, TOK_RBRACE);
            ast_node_t *n = ast_new(&p->arena, AST_STRUCT_INIT, line, col);
            if (n) { n->u.sinit.type_name = type_name; n->u.sinit.fields = head; n->u.sinit.nfields = nf; }
            return n;
        }
        return d;
    }

    /* fallback: unknown token — return error node */
    parse_error(p, "unexpected token '%s' in expression", tok_name(p->cur.kind));
    ast_node_t *err = ast_new(&p->arena, AST_IDENT, line, col);
    if (err) err->u.ident.name = (char*)"<err>";
    advance(p);
    return err;
}

static ast_node_t *parse_addition(parser_t *p) {
    ast_node_t *left = parse_primary(p);
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        tok_kind_t op = p->cur.kind; int line = p->cur.line, col = p->cur.col;
        advance(p);
        ast_node_t *right = parse_primary(p);
        ast_node_t *n = ast_new(&p->arena, AST_BINARY, line, col);
        if (n) { n->u.binary.op = (op == TOK_PLUS) ? BINOP_ADD : BINOP_SUB;
                 n->u.binary.left = left; n->u.binary.right = right; }
        left = n;
    }
    return left;
}

static ast_node_t *parse_expr(parser_t *p) {
    ast_node_t *left = parse_addition(p);
    /* comparison / range */
    binop_t bop = BINOP_EQ;
    int has_cmp = 1;
    switch (p->cur.kind) {
    case TOK_EQ:    bop = BINOP_EQ;    break;
    case TOK_NEQ:   bop = BINOP_NEQ;   break;
    case TOK_LT:    bop = BINOP_LT;    break;
    case TOK_GT:    bop = BINOP_GT;    break;
    case TOK_LE:    bop = BINOP_LE;    break;
    case TOK_GE:    bop = BINOP_GE;    break;
    case TOK_TILDE: bop = BINOP_RANGE; break;
    default:        has_cmp = 0; break;
    }
    if (has_cmp) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        ast_node_t *right = parse_addition(p);
        ast_node_t *n = ast_new(&p->arena, AST_BINARY, line, col);
        if (n) { n->u.binary.op = bop; n->u.binary.left = left; n->u.binary.right = right; }
        left = n;
    }
    /* | inside an expression context = binary OR (e.g. gate(level: error | warn))
     * This is distinct from pipeline-level | merge; we're already inside parens. */
    while (check(p, TOK_PIPE)) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        ast_node_t *right = parse_addition(p);
        ast_node_t *n = ast_new(&p->arena, AST_BINARY, line, col);
        if (n) { n->u.binary.op = BINOP_OR; n->u.binary.left = left; n->u.binary.right = right; }
        left = n;
    }
    return left;
}

/* ── arg list ────────────────────────────────────────────────────── */
static ast_node_t *parse_arg_list(parser_t *p, int *nout) {
    ast_node_t *head = NULL, *tail = NULL; int n = 0;
    do {
        int line = p->cur.line, col = p->cur.col;
        char *label = NULL;
        ast_node_t *val = NULL;

        /* peek: IDENT/keyword ':' or '=' means labeled arg.
         * Both forms occur in Z+: (per: 1m)  and  (for = 1) */
        if (is_ident_like(p->cur.kind)) {
            token_t saved = p->cur;
            token_t next  = lexer_peek(&p->lex);
            if (next.kind == TOK_COLON || next.kind == TOK_ASSIGN) {
                label = dup_tok(p, saved);
                advance(p); /* IDENT  */
                advance(p); /* : or = */
            }
        }

        val = parse_expr(p);
        ast_node_t *arg = ast_new(&p->arena, AST_ARG, line, col);
        if (arg) { arg->u.arg.label = label; arg->u.arg.val = val; }
        if (!head) head = tail = arg; else { tail->next = arg; tail = arg; } n++;
    } while (match(p, TOK_COMMA) && !check(p, TOK_RPAREN));
    if (nout) *nout = n;
    return head;
}

/* ── dotted ident ────────────────────────────────────────────────── */
static ast_node_t *parse_dotted(parser_t *p) {
    int line = p->cur.line, col = p->cur.col;
    /* collect IDENT ('.' IDENT)*  — accept keyword tokens as ident parts */
    char *parts[16]; int np = 0;
    token_t first = consume_as_ident(p);
    parts[np++] = dup_tok(p, first);
    while (check(p, TOK_DOT) && np < 16) {
        /* peek: next must be ident-like to be a dotted access */
        token_t nxt = lexer_peek(&p->lex);
        if (!is_ident_like(nxt.kind)) break;
        advance(p); /* consume . */
        token_t part = consume_as_ident(p);
        parts[np++] = dup_tok(p, part);
    }
    if (np == 1) {
        ast_node_t *n = ast_new(&p->arena, AST_IDENT, line, col);
        if (n) n->u.ident.name = parts[0];
        return n;
    }
    ast_node_t *n = ast_new(&p->arena, AST_DOTTED, line, col);
    if (n) {
        n->u.dotted.parts = (char**)arena_alloc(&p->arena, (size_t)(np * (int)sizeof(char*)));
        if (n->u.dotted.parts) {
            for (int i = 0; i < np; i++) n->u.dotted.parts[i] = parts[i];
        }
        n->u.dotted.nparts = np;
    }
    return n;
}

/* ── built-in signal operator atoms ─────────────────────────────── */
static ast_node_t *parse_op_node(parser_t *p, ast_kind_t kind) {
    int line = p->cur.line, col = p->cur.col;
    advance(p); /* consume the keyword */
    ast_node_t *n = ast_new(&p->arena, kind, line, col);
    if (!n) return n;
    /* most take (expr [, labeled_args...]); some are bare */
    if (check(p, TOK_LPAREN)) {
        advance(p);
        int nargs = 0;
        ast_node_t *args = NULL;
        if (!check(p, TOK_RPAREN)) args = parse_arg_list(p, &nargs);
        expect(p, TOK_RPAREN);
        n->u.op.expr  = args ? args->u.arg.val : NULL;
        n->u.op.args  = args;
        n->u.op.nargs = nargs;
    } else if (check(p, TOK_COLON)) {
        /* knee: value  or  on_block */
        advance(p);
        n->u.op.expr = parse_expr(p);
    }
    return n;
}

/* ── merge node ──────────────────────────────────────────────────── */
/* parse:  | [policy] |
 * Called when we see the second | inside a pipeline.
 * The first | was already consumed as a stage start.  */
static ast_node_t *parse_merge_inner(parser_t *p, int line, int col) {
    ast_node_t *n = ast_new(&p->arena, AST_MERGE, line, col);
    if (!n) return n;
    /* optional inline policy — only parse if it does NOT look like the start of
     * a new top-level statement.  Patterns that indicate a new statement:
     *   IDENT/keyword followed by  ->  or  :  (pipeline or signal_decl)
     * Multi-line merges leave the | bare; policy is a call or known keyword. */
    if (!check(p, TOK_PIPE)) {
        int skip_policy = 0;
        /* Skip policy when next token is an edge operator (| -> sink pattern)
         * or an EOF/block-close, or when next looks like a new top-level statement */
        if (is_edge_op(p) || p->cur.kind == TOK_EOF || p->cur.kind == TOK_RBRACE) {
            skip_policy = 1;
        } else if (is_ident_like(p->cur.kind)) {
            token_t nxt = lexer_peek(&p->lex);
            if (nxt.kind == TOK_FLOW || nxt.kind == TOK_COLON ||
                nxt.kind == TOK_TAP  || nxt.kind == TOK_SEMI  ||
                nxt.kind == TOK_EOF)
                skip_policy = 1;
        }
        if (!skip_policy)
            n->u.merge.policy = parse_atom(p);
    }
    match(p, TOK_PIPE);
    return n;
}

/* ── stage ───────────────────────────────────────────────────────── */
static ast_node_t *parse_stage(parser_t *p) {
    int line = p->cur.line, col = p->cur.col;

    /* fork: { branch, branch, ... } */
    if (check(p, TOK_LBRACE)) {
        advance(p);
        ast_node_t *branches[64]; int nb = 0;
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            if (nb < 64) branches[nb++] = parse_pipeline(p);
            if (!match(p, TOK_COMMA)) break;
        }
        expect(p, TOK_RBRACE);
        ast_node_t *n = ast_new(&p->arena, AST_FORK, line, col);
        if (n) {
            n->u.fork.branches = (ast_node_t**)arena_alloc(&p->arena,
                                     (size_t)(nb * (int)sizeof(ast_node_t*)));
            if (n->u.fork.branches)
                for (int i = 0; i < nb; i++) n->u.fork.branches[i] = branches[i];
            n->u.fork.count = nb;
        }
        return n;
    }

    /* merge: | */
    if (check(p, TOK_PIPE)) {
        advance(p);
        return parse_merge_inner(p, line, col);
    }

    /* built-in signal operators */
    switch (p->cur.kind) {
    case TOK_GATE:       return parse_op_node(p, AST_GATE);
    case TOK_KNEE:       return parse_op_node(p, AST_KNEE);
    case TOK_DELTA:      return parse_op_node(p, AST_DELTA_EXPR);
    case TOK_EMIT:       return parse_op_node(p, AST_EMIT);
    case TOK_ON_SILENCE: return parse_op_node(p, AST_ON_SILENCE);
    case TOK_ON_BLOCK:   return parse_op_node(p, AST_ON_BLOCK);
    case TOK_SUSTAINED:  return parse_op_node(p, AST_SUSTAINED);
    case TOK_WITHIN:     return parse_op_node(p, AST_WITHIN);
    case TOK_DEBOUNCE:   return parse_op_node(p, AST_DEBOUNCE);
    case TOK_THROTTLE:   return parse_op_node(p, AST_THROTTLE);
    case TOK_DECAY:      return parse_op_node(p, AST_DECAY);
    case TOK_TICK:       return parse_op_node(p, AST_TICK);
    case TOK_RATE:       return parse_op_node(p, AST_RATE);
    case TOK_INPUT: {
        advance(p);
        ast_node_t *n = ast_new(&p->arena, AST_IDENT, line, col);
        if (n) n->u.ident.name = (char*)"input";
        return n;
    }
    case TOK_OUTPUT: {
        advance(p);
        ast_node_t *n = ast_new(&p->arena, AST_IDENT, line, col);
        if (n) n->u.ident.name = (char*)"output";
        return n;
    }
    default:
        return parse_atom(p);
    }
}

static ast_node_t *parse_atom(parser_t *p) {
    return parse_primary(p);
}

/* ── pipeline ─────────────────────────────────────────────────────
 * Parses:  stage (edge_op stage)*
 * Returns AST_PIPELINE with stages[] and edges[] arrays.
 * If there's only one stage and no edges, still returns a pipeline
 * (codegen/IR can unwrap single-stage pipelines).
 */
static ast_node_t *parse_pipeline(parser_t *p) {
    int line = p->cur.line, col = p->cur.col;

#define MAX_STAGES 128
    ast_node_t *stages[MAX_STAGES];
    edge_t      edges[MAX_STAGES];  /* edges[i]: edge FROM stages[i] TO stages[i+1] */
    int ns = 0;

    stages[ns++] = parse_stage(p);

    while (is_edge_op(p) && ns < MAX_STAGES) {
        edges[ns-1] = tok_to_edge(p->cur.kind);
        advance(p); /* consume edge op */
        if (!is_stage_start(p)) break;
        stages[ns++] = parse_stage(p);
    }

    ast_node_t *n = ast_new(&p->arena, AST_PIPELINE, line, col);
    if (!n) return n;
    n->u.pipeline.stages = (ast_node_t**)arena_alloc(&p->arena,
                                (size_t)(ns * (int)sizeof(ast_node_t*)));
    n->u.pipeline.edges  = (edge_t*)arena_alloc(&p->arena,
                                (size_t)(ns * (int)sizeof(edge_t)));
    if (n->u.pipeline.stages && n->u.pipeline.edges) {
        for (int i = 0; i < ns; i++) {
            n->u.pipeline.stages[i] = stages[i];
            n->u.pipeline.edges[i]  = (i < ns-1) ? edges[i] : EDGE_FLOW;
        }
    }
    n->u.pipeline.count = ns;
    return n;
#undef MAX_STAGES
}

/* ── type reference ──────────────────────────────────────────────── */
static ast_node_t *parse_typeref(parser_t *p) {
    int line = p->cur.line, col = p->cur.col;
    char *name = NULL;
    switch (p->cur.kind) {
    case TOK_TK_INT:   name = (char*)"int";   break;
    case TOK_TK_FLOAT: name = (char*)"float"; break;
    case TOK_TK_STR:   name = (char*)"str";   break;
    case TOK_TK_BOOL:  name = (char*)"bool";  break;
    case TOK_IDENT:    name = dup_tok(p, p->cur); break;
    default:
        parse_error(p, "expected type name");
        name = (char*)"unknown";
    }
    advance(p);
    ast_node_t *n = ast_new(&p->arena, AST_TYPEREF, line, col);
    if (n) n->u.typeref.name = name;
    return n;
}

/* ── top-level declarations ──────────────────────────────────────── */

static ast_node_t *parse_import(parser_t *p) {
    int line = p->cur.line, col = p->cur.col;
    advance(p); /* consume 'import' */
    ast_node_t *mod = parse_dotted(p);
    char *mod_str = NULL;
    if (mod->kind == AST_IDENT)       mod_str = mod->u.ident.name;
    else if (mod->kind == AST_DOTTED) {
        /* flatten to "a.b.c" */
        int total = 0;
        for (int i = 0; i < mod->u.dotted.nparts; i++)
            total += (int)strlen(mod->u.dotted.parts[i]) + 1;
        char *flat = (char*)arena_alloc(&p->arena, (size_t)total);
        if (flat) {
            flat[0] = '\0';
            for (int i = 0; i < mod->u.dotted.nparts; i++) {
                if (i) strcat(flat, ".");
                strcat(flat, mod->u.dotted.parts[i]);
            }
        }
        mod_str = flat;
    }
    expect(p, TOK_AS);
    token_t alias_tok = expect(p, TOK_IDENT);
    char *alias = dup_tok(p, alias_tok);
    ast_node_t *n = ast_new(&p->arena, AST_IMPORT, line, col);
    if (n) { n->u.import.module = mod_str; n->u.import.alias = alias; }
    return n;
}

static ast_node_t *parse_struct(parser_t *p) {
    int line = p->cur.line, col = p->cur.col;
    advance(p); /* consume 'struct' */
    token_t name = expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);
    ast_node_t *head = NULL, *tail = NULL; int nf = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        int fl = p->cur.line, fc = p->cur.col;
        token_t fn = expect(p, TOK_IDENT);
        expect(p, TOK_COLON);
        ast_node_t *ty = parse_typeref(p);
        match(p, TOK_COMMA);
        ast_node_t *f = ast_new(&p->arena, AST_FIELD, fl, fc);
        if (f) { f->u.field.name = dup_tok(p, fn); f->u.field.type = ty; }
        if (!head) head = tail = f; else { tail->next = f; tail = f; } nf++;
    }
    expect(p, TOK_RBRACE);
    ast_node_t *n = ast_new(&p->arena, AST_STRUCT, line, col);
    if (n) { n->u.strct.name = dup_tok(p, name);
             n->u.strct.fields = head; n->u.strct.nfields = nf; }
    return n;
}

static ast_node_t *parse_chain(parser_t *p) {
    int line = p->cur.line, col = p->cur.col;
    advance(p); /* consume 'chain' */
    token_t name = expect(p, TOK_IDENT);
    expect(p, TOK_LBRACE);
    ast_node_t *items[256]; int ni = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && ni < 256) {
        int il = p->cur.line, ic = p->cur.col;
        /* chain_item: IDENT ':' pipeline */
        if (check(p, TOK_IDENT)) {
            token_t label = p->cur;
            token_t nxt   = lexer_peek(&p->lex);
            if (nxt.kind == TOK_COLON) {
                advance(p); /* IDENT */
                advance(p); /* :     */
                ast_node_t *pipe = parse_pipeline(p);
                ast_node_t *ci = ast_new(&p->arena, AST_CHAIN_ITEM, il, ic);
                if (ci) { ci->u.chain_item.label    = dup_tok(p, label);
                          ci->u.chain_item.pipeline = pipe; }
                items[ni++] = ci;
                continue;
            }
        }
        /* bare pipeline inside chain (ident list shorthand e.g. "chain foo { a b c }") */
        items[ni++] = parse_pipeline(p);
    }
    expect(p, TOK_RBRACE);
    ast_node_t *n = ast_new(&p->arena, AST_CHAIN, line, col);
    if (n) {
        n->u.chain.name   = dup_tok(p, name);
        n->u.chain.items  = NULL; n->u.chain.nitems = ni;
        /* store items as a linked list via next pointers */
        for (int i = ni-1; i >= 0; i--) {
            if (items[i]) { items[i]->next = n->u.chain.items; n->u.chain.items = items[i]; }
        }
    }
    return n;
}

/* ── top-level: signal decl or bare pipeline ─────────────────────── */
static ast_node_t *parse_signal_or_pipeline(parser_t *p) {
    int line = p->cur.line, col = p->cur.col;

    /* IDENT ':' pipeline  →  signal_decl (also accepts keyword-ident tokens) */
    if (is_ident_like(p->cur.kind)) {
        token_t name = p->cur;
        token_t nxt  = lexer_peek(&p->lex);
        if (nxt.kind == TOK_COLON) {
            advance(p); /* IDENT */
            advance(p); /* :     */
            ast_node_t *rhs = parse_pipeline(p);
            ast_node_t *n = ast_new(&p->arena, AST_SIGNAL_DECL, line, col);
            if (n) { n->u.sig_decl.name = dup_tok(p, name); n->u.sig_decl.rhs = rhs; }
            return n;
        }
    }

    return parse_pipeline(p);
}

/* ── program ─────────────────────────────────────────────────────── */
ast_node_t *parse(parser_t *p, const char *src, int len) {
    lexer_init(&p->lex, src, len);
    p->cur = lexer_next(&p->lex);

    ast_node_t *prog = ast_new(&p->arena, AST_PROGRAM, 1, 1);
    if (!prog) return NULL;

    ast_node_t *head = NULL, *tail = NULL; int count = 0;

    while (!check(p, TOK_EOF) && p->nerrors < PARSE_MAX_ERRORS) {
        ast_node_t *decl = NULL;

        if (check(p, TOK_ERR))    { advance(p); continue; }
        if (check(p, TOK_SEMI))   { advance(p); continue; }

        if (check(p, TOK_IMPORT)) decl = parse_import(p);
        else if (check(p, TOK_STRUCT)) decl = parse_struct(p);
        else if (check(p, TOK_CHAIN))  decl = parse_chain(p);
        else if (check(p, TOK_PRIORITY)) {
            /* priority : reflex | deliberate  — skip for now */
            advance(p); match(p, TOK_COLON); advance(p);
            continue;
        }
        else decl = parse_signal_or_pipeline(p);

        if (decl) {
            if (!head) head = tail = decl;
            else { tail->next = decl; tail = decl; }
            count++;
        }

        p->panic = 0; /* reset after each top-level decl */
    }

    prog->u.list.items = head;
    prog->u.list.count = count;
    return prog;
}

void parser_init(parser_t *p) {
    memset(p, 0, sizeof(*p));
    arena_init(&p->arena, 4 * 1024 * 1024); /* 4MB for AST nodes */
}

void parser_free(parser_t *p) {
    arena_free(&p->arena);
}
