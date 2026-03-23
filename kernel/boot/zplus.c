/*
 * Zeos — Z+ Interpreter (Minimal)
 *
 * Parses Tier 3 Z+ syntax and maps it to the signal chain engine.
 *
 * This is deliberately simple. It handles the constructs needed
 * to run hello_chain.zp and signal_playground.zp. As the language
 * spec solidifies, this grows.
 *
 * The interpreter works in three phases:
 *   1. PARSE  — tokenize source, extract node decls and edges
 *   2. COMPILE — create signal chain nodes and wire them
 *   3. EXECUTE — inject data into sources and resolve
 */

#include "zplus.h"
#include "signal.h"
#include "kprint.h"

/* ── String utilities ─────────────────────────── */

static int zp_strlen(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int zp_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void zp_strcpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int zp_isdigit(char c) { return c >= '0' && c <= '9'; }
static int zp_isalpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int zp_isalnum(char c) { return zp_isalpha(c) || zp_isdigit(c); }

/* ── Tokenizer ────────────────────────────────── */

enum zp_token_type {
    TOK_IDENT,      /* variable name */
    TOK_NUMBER,     /* integer literal */
    TOK_STRING,     /* "quoted string" */
    TOK_COLON,      /* : */
    TOK_ARROW,      /* -> */
    TOK_TAP,        /* ~> */
    TOK_STAR,       /* * */
    TOK_PLUS,       /* + */
    TOK_MINUS,      /* - */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_COMMA,      /* , */
    TOK_GT,         /* > */
    TOK_LT,         /* < */
    TOK_EQ,         /* == */
    TOK_GTE,        /* >= */
    TOK_LTE,        /* <= */
    TOK_NEWLINE,    /* end of line */
    TOK_EOF,        /* end of input */
};

struct zp_token {
    enum zp_token_type type;
    char    text[ZP_MAX_NAME];
    int32_t num_val;
};

struct zp_lexer {
    const char *src;
    int         pos;
    int         len;
};

static void lexer_init(struct zp_lexer *lex, const char *src)
{
    lex->src = src;
    lex->pos = 0;
    lex->len = zp_strlen(src);
}

static char lexer_peek(struct zp_lexer *lex)
{
    if (lex->pos >= lex->len) return '\0';
    return lex->src[lex->pos];
}

static char lexer_next(struct zp_lexer *lex)
{
    if (lex->pos >= lex->len) return '\0';
    return lex->src[lex->pos++];
}

static void lexer_skip_spaces(struct zp_lexer *lex)
{
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (c == ' ' || c == '\t' || c == '\r')
            lex->pos++;
        else
            break;
    }
}

static void lexer_skip_comment(struct zp_lexer *lex)
{
    /* Skip // comment to end of line */
    while (lex->pos < lex->len && lex->src[lex->pos] != '\n')
        lex->pos++;
}

static int lexer_token(struct zp_lexer *lex, struct zp_token *tok)
{
    lexer_skip_spaces(lex);

    if (lex->pos >= lex->len) {
        tok->type = TOK_EOF;
        tok->text[0] = '\0';
        return 0;
    }

    char c = lexer_peek(lex);

    /* Comments */
    if (c == '/' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '/') {
        lexer_skip_comment(lex);
        /* Treat rest-of-line comment as newline */
        if (lexer_peek(lex) == '\n') {
            lexer_next(lex);
        }
        tok->type = TOK_NEWLINE;
        tok->text[0] = '\n';
        tok->text[1] = '\0';
        return 0;
    }

    /* Newline */
    if (c == '\n') {
        lexer_next(lex);
        tok->type = TOK_NEWLINE;
        tok->text[0] = '\n';
        tok->text[1] = '\0';
        return 0;
    }

    /* Arrow -> */
    if (c == '-' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '>') {
        lex->pos += 2;
        tok->type = TOK_ARROW;
        tok->text[0] = '-';
        tok->text[1] = '>';
        tok->text[2] = '\0';
        return 0;
    }

    /* Tap ~> */
    if (c == '~' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '>') {
        lex->pos += 2;
        tok->type = TOK_TAP;
        tok->text[0] = '~';
        tok->text[1] = '>';
        tok->text[2] = '\0';
        return 0;
    }

    /* Comparison operators (must check two-char before single-char) */
    if (c == '>' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '=') {
        lex->pos += 2;
        tok->type = TOK_GTE;
        tok->text[0] = '>'; tok->text[1] = '='; tok->text[2] = '\0';
        return 0;
    }
    if (c == '<' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '=') {
        lex->pos += 2;
        tok->type = TOK_LTE;
        tok->text[0] = '<'; tok->text[1] = '='; tok->text[2] = '\0';
        return 0;
    }
    if (c == '=' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '=') {
        lex->pos += 2;
        tok->type = TOK_EQ;
        tok->text[0] = '='; tok->text[1] = '='; tok->text[2] = '\0';
        return 0;
    }

    /* Single-char tokens */
    switch (c) {
    case ':': lexer_next(lex); tok->type = TOK_COLON;  tok->text[0] = ':'; tok->text[1] = '\0'; return 0;
    case '*': lexer_next(lex); tok->type = TOK_STAR;   tok->text[0] = '*'; tok->text[1] = '\0'; return 0;
    case '+': lexer_next(lex); tok->type = TOK_PLUS;   tok->text[0] = '+'; tok->text[1] = '\0'; return 0;
    case '-': lexer_next(lex); tok->type = TOK_MINUS;  tok->text[0] = '-'; tok->text[1] = '\0'; return 0;
    case '(': lexer_next(lex); tok->type = TOK_LPAREN; tok->text[0] = '('; tok->text[1] = '\0'; return 0;
    case ')': lexer_next(lex); tok->type = TOK_RPAREN; tok->text[0] = ')'; tok->text[1] = '\0'; return 0;
    case '{': lexer_next(lex); tok->type = TOK_LBRACE; tok->text[0] = '{'; tok->text[1] = '\0'; return 0;
    case '}': lexer_next(lex); tok->type = TOK_RBRACE; tok->text[0] = '}'; tok->text[1] = '\0'; return 0;
    case ',': lexer_next(lex); tok->type = TOK_COMMA;  tok->text[0] = ','; tok->text[1] = '\0'; return 0;
    case '>': lexer_next(lex); tok->type = TOK_GT;     tok->text[0] = '>'; tok->text[1] = '\0'; return 0;
    case '<': lexer_next(lex); tok->type = TOK_LT;     tok->text[0] = '<'; tok->text[1] = '\0'; return 0;
    }

    /* Number */
    if (zp_isdigit(c)) {
        int i = 0;
        int32_t val = 0;
        while (lex->pos < lex->len && zp_isdigit(lexer_peek(lex))) {
            char d = lexer_next(lex);
            val = val * 10 + (d - '0');
            if (i < ZP_MAX_NAME - 1)
                tok->text[i++] = d;
        }
        tok->text[i] = '\0';
        tok->type = TOK_NUMBER;
        tok->num_val = val;
        return 0;
    }

    /* Identifier */
    if (zp_isalpha(c)) {
        int i = 0;
        while (lex->pos < lex->len && zp_isalnum(lexer_peek(lex))) {
            char d = lexer_next(lex);
            if (i < ZP_MAX_NAME - 1)
                tok->text[i++] = d;
        }
        tok->text[i] = '\0';
        tok->type = TOK_IDENT;
        return 0;
    }

    /* String literal */
    if (c == '"') {
        lexer_next(lex);  /* skip opening quote */
        int i = 0;
        while (lex->pos < lex->len && lexer_peek(lex) != '"') {
            char d = lexer_next(lex);
            if (i < ZP_MAX_NAME - 1)
                tok->text[i++] = d;
        }
        tok->text[i] = '\0';
        if (lexer_peek(lex) == '"')
            lexer_next(lex);  /* skip closing quote */
        tok->type = TOK_STRING;
        return 0;
    }

    /* Unknown character — skip it */
    lexer_next(lex);
    return lexer_token(lex, tok);
}

/* ── Parser ───────────────────────────────────── */

static int find_node(struct zp_program *prog, const char *name)
{
    for (int i = 0; i < prog->node_count; i++) {
        if (zp_streq(prog->nodes[i].name, name))
            return i;
    }
    return -1;
}

static int add_node(struct zp_program *prog, const char *name, enum zp_node_type type,
                    int32_t val, const char *fmt)
{
    if (prog->node_count >= ZP_MAX_NODES)
        return -1;

    int idx = prog->node_count++;
    zp_strcpy(prog->nodes[idx].name, name, ZP_MAX_NAME);
    prog->nodes[idx].type = type;
    prog->nodes[idx].int_val = val;
    prog->nodes[idx].sig_idx = -1;
    if (fmt)
        zp_strcpy(prog->nodes[idx].fmt, fmt, ZP_MAX_STRING);
    else
        prog->nodes[idx].fmt[0] = '\0';

    return idx;
}

static int add_edge(struct zp_program *prog, const char *src, const char *dst)
{
    if (prog->edge_count >= ZP_MAX_EDGES)
        return -1;

    int idx = prog->edge_count++;
    zp_strcpy(prog->edges[idx].src, src, ZP_MAX_NAME);
    zp_strcpy(prog->edges[idx].dst, dst, ZP_MAX_NAME);
    return idx;
}

/*
 * Parse a gate() expression: gate(> N), gate(< N), gate(== N), etc.
 * Returns the gate type, stores threshold in *out_val.
 */
static enum zp_node_type parse_gate_expr(struct zp_lexer *lex, int32_t *out_val)
{
    struct zp_token t;
    lexer_token(lex, &t);  /* ( */
    lexer_token(lex, &t);  /* comparison operator */

    enum zp_node_type gate_type = ZP_GATE_GT;

    if (t.type == TOK_GT)       gate_type = ZP_GATE_GT;
    else if (t.type == TOK_LT)  gate_type = ZP_GATE_LT;
    else if (t.type == TOK_EQ)  gate_type = ZP_GATE_EQ;
    else if (t.type == TOK_GTE) gate_type = ZP_GATE_GTE;
    else if (t.type == TOK_LTE) gate_type = ZP_GATE_LTE;

    lexer_token(lex, &t);  /* number (threshold) */
    *out_val = t.num_val;

    lexer_token(lex, &t);  /* ) */
    return gate_type;
}

/*
 * Parse one line of Z+ source.
 *
 * Patterns:
 *   name : emit(N)                        — source node
 *   name : input -> * N -> output         — multiply transform
 *   name : input -> + N -> output         — add transform
 *   name : input -> - N -> output         — subtract transform
 *   name : input -> print("...")          — display sink
 *   name : input -> gate(> N) -> output   — gate (conditional pass)
 *   name : input -> delta -> output       — change detection
 *   a -> b -> c                           — wiring
 *   a -> {b, c}                           — fork (one to many)
 *   a ~> b                                — tap (read-only wire)
 */
static int parse_line(struct zp_lexer *lex, struct zp_program *prog)
{
    struct zp_token tok;
    lexer_token(lex, &tok);

    /* Skip blank lines */
    if (tok.type == TOK_NEWLINE || tok.type == TOK_EOF)
        return 0;

    /* Must start with identifier */
    if (tok.type != TOK_IDENT)
        return 0;  /* Skip lines we can't parse */

    char first_name[ZP_MAX_NAME];
    zp_strcpy(first_name, tok.text, ZP_MAX_NAME);

    /* Peek at next token */
    struct zp_token next;
    lexer_token(lex, &next);

    if (next.type == TOK_COLON) {
        /* NODE DECLARATION: name : ... */
        struct zp_token t;
        lexer_token(lex, &t);

        if (t.type == TOK_IDENT && zp_streq(t.text, "emit")) {
            /* emit(N) */
            lexer_token(lex, &t);  /* ( */
            lexer_token(lex, &t);  /* number */
            int32_t val = t.num_val;
            lexer_token(lex, &t);  /* ) */

            add_node(prog, first_name, ZP_EMIT, val, 0);

        } else if (t.type == TOK_IDENT && zp_streq(t.text, "input")) {
            /* input -> OP ... */
            lexer_token(lex, &t);  /* -> */
            if (t.type != TOK_ARROW) goto skip_line;

            lexer_token(lex, &t);  /* operator, keyword, or function */

            if (t.type == TOK_STAR) {
                /* * N -> output */
                lexer_token(lex, &t);  /* number */
                int32_t val = t.num_val;
                add_node(prog, first_name, ZP_MULTIPLY, val, 0);
                lexer_token(lex, &t);  /* -> */
                lexer_token(lex, &t);  /* output */

            } else if (t.type == TOK_PLUS) {
                /* + N -> output */
                lexer_token(lex, &t);  /* number */
                int32_t val = t.num_val;
                add_node(prog, first_name, ZP_ADD, val, 0);
                lexer_token(lex, &t);  /* -> */
                lexer_token(lex, &t);  /* output */

            } else if (t.type == TOK_MINUS) {
                /* - N -> output */
                lexer_token(lex, &t);  /* number */
                int32_t val = t.num_val;
                add_node(prog, first_name, ZP_SUBTRACT, val, 0);
                lexer_token(lex, &t);  /* -> */
                lexer_token(lex, &t);  /* output */

            } else if (t.type == TOK_IDENT && zp_streq(t.text, "print")) {
                /* print("...") */
                lexer_token(lex, &t);  /* ( */
                lexer_token(lex, &t);  /* string */
                char fmt[ZP_MAX_STRING];
                zp_strcpy(fmt, t.text, ZP_MAX_STRING);
                lexer_token(lex, &t);  /* ) */

                add_node(prog, first_name, ZP_PRINT, 0, fmt);

            } else if (t.type == TOK_IDENT && zp_streq(t.text, "gate")) {
                /* gate(> N) -> output */
                int32_t threshold;
                enum zp_node_type gate_type = parse_gate_expr(lex, &threshold);
                add_node(prog, first_name, gate_type, threshold, 0);
                lexer_token(lex, &t);  /* -> */
                lexer_token(lex, &t);  /* output */

            } else if (t.type == TOK_IDENT && zp_streq(t.text, "delta")) {
                /* delta -> output */
                add_node(prog, first_name, ZP_DELTA, 0, 0);
                lexer_token(lex, &t);  /* -> */
                lexer_token(lex, &t);  /* output */
            }

        } else if (t.type == TOK_NUMBER) {
            /* name : N  (shorthand for emit) */
            add_node(prog, first_name, ZP_EMIT, t.num_val, 0);
        }

        /* Skip to end of line */
        goto skip_line;

    } else if (next.type == TOK_ARROW || next.type == TOK_TAP) {
        /* WIRING: a -> b [-> c ...] or a -> {b, c} or a ~> b */
        char prev_name[ZP_MAX_NAME];
        zp_strcpy(prev_name, first_name, ZP_MAX_NAME);
        int is_tap = (next.type == TOK_TAP);

        struct zp_token t;
        lexer_token(lex, &t);

        /* Check for fork: a -> {b, c, d} */
        if (t.type == TOK_LBRACE) {
            /* Parse comma-separated list of identifiers */
            lexer_token(lex, &t);
            while (t.type == TOK_IDENT) {
                if (is_tap) {
                    /* Tap creates a passthrough node to avoid modifying the source */
                    char tap_name[ZP_MAX_NAME];
                    zp_strcpy(tap_name, "_tap_", ZP_MAX_NAME);
                    /* Append destination name */
                    int len = 5;
                    const char *dn = t.text;
                    while (*dn && len < ZP_MAX_NAME - 1)
                        tap_name[len++] = *dn++;
                    tap_name[len] = '\0';
                    add_node(prog, tap_name, ZP_PASSTHROUGH, 0, 0);
                    add_edge(prog, prev_name, tap_name);
                    add_edge(prog, tap_name, t.text);
                } else {
                    add_edge(prog, prev_name, t.text);
                }

                lexer_token(lex, &t);  /* , or } */
                if (t.type == TOK_COMMA)
                    lexer_token(lex, &t);  /* next identifier */
            }
            /* t should be TOK_RBRACE here */
            return 0;
        }

        /* Regular chain: a -> b -> c */
        while (t.type == TOK_IDENT) {
            if (is_tap) {
                /* Tap: insert a passthrough node */
                char tap_name[ZP_MAX_NAME];
                zp_strcpy(tap_name, "_tap_", ZP_MAX_NAME);
                int len = 5;
                const char *dn = t.text;
                while (*dn && len < ZP_MAX_NAME - 1)
                    tap_name[len++] = *dn++;
                tap_name[len] = '\0';
                add_node(prog, tap_name, ZP_PASSTHROUGH, 0, 0);
                add_edge(prog, prev_name, tap_name);
                add_edge(prog, tap_name, t.text);
            } else {
                add_edge(prog, prev_name, t.text);
            }
            zp_strcpy(prev_name, t.text, ZP_MAX_NAME);

            lexer_token(lex, &t);  /* -> or ~> or end */
            if (t.type == TOK_ARROW) {
                is_tap = 0;
            } else if (t.type == TOK_TAP) {
                is_tap = 1;
            } else {
                break;
            }
            lexer_token(lex, &t);  /* next identifier or { */

            /* Check for fork mid-chain */
            if (t.type == TOK_LBRACE) {
                lexer_token(lex, &t);
                while (t.type == TOK_IDENT) {
                    add_edge(prog, prev_name, t.text);
                    lexer_token(lex, &t);
                    if (t.type == TOK_COMMA)
                        lexer_token(lex, &t);
                }
                return 0;
            }
        }

        return 0;
    }

skip_line:
    /* Consume remaining tokens until newline or EOF */
    {
        struct zp_token t;
        do {
            lexer_token(lex, &t);
        } while (t.type != TOK_NEWLINE && t.type != TOK_EOF);
    }
    return 0;
}

/* ── Signal chain process functions ───────────── */

/* Emit: produce a constant value */
static int zp_proc_emit(struct sig_node *node, struct sig_data *in,
                         struct sig_data *out)
{
    (void)in;
    int32_t val = (int32_t)(long)node->user_data;
    out->data[0] = val & 0xFF;
    out->data[1] = (val >> 8) & 0xFF;
    out->data[2] = (val >> 16) & 0xFF;
    out->data[3] = (val >> 24) & 0xFF;
    out->size = 4;
    out->type = 1;  /* int32 */
    return 0;
}

/* Read a 32-bit int from sig_data */
static int32_t sig_data_read_i32(struct sig_data *d)
{
    return (int32_t)(d->data[0] | (d->data[1] << 8) |
                     (d->data[2] << 16) | (d->data[3] << 24));
}

/* Write a 32-bit int to sig_data */
static void sig_data_write_i32(struct sig_data *d, int32_t val)
{
    d->data[0] = val & 0xFF;
    d->data[1] = (val >> 8) & 0xFF;
    d->data[2] = (val >> 16) & 0xFF;
    d->data[3] = (val >> 24) & 0xFF;
    d->size = 4;
    d->type = 1;
}

/* Multiply: input * constant */
static int zp_proc_multiply(struct sig_node *node, struct sig_data *in,
                             struct sig_data *out)
{
    int32_t val = sig_data_read_i32(in);
    int32_t factor = (int32_t)(long)node->user_data;
    sig_data_write_i32(out, val * factor);
    return 0;
}

/* Add: input + constant */
static int zp_proc_add(struct sig_node *node, struct sig_data *in,
                        struct sig_data *out)
{
    int32_t val = sig_data_read_i32(in);
    int32_t addend = (int32_t)(long)node->user_data;
    sig_data_write_i32(out, val + addend);
    return 0;
}

/* Subtract: input - constant */
static int zp_proc_subtract(struct sig_node *node, struct sig_data *in,
                              struct sig_data *out)
{
    int32_t val = sig_data_read_i32(in);
    int32_t sub = (int32_t)(long)node->user_data;
    sig_data_write_i32(out, val - sub);
    return 0;
}

/* Gate (greater than): pass if input > threshold, else block */
static int zp_proc_gate_gt(struct sig_node *node, struct sig_data *in,
                            struct sig_data *out)
{
    int32_t val = sig_data_read_i32(in);
    int32_t threshold = (int32_t)(long)node->user_data;
    if (val > threshold) {
        sig_data_write_i32(out, val);
        return 0;
    }
    /* Block: return error to stop propagation */
    out->size = 0;
    return 1;
}

/* Gate (less than) */
static int zp_proc_gate_lt(struct sig_node *node, struct sig_data *in,
                            struct sig_data *out)
{
    int32_t val = sig_data_read_i32(in);
    int32_t threshold = (int32_t)(long)node->user_data;
    if (val < threshold) {
        sig_data_write_i32(out, val);
        return 0;
    }
    out->size = 0;
    return 1;
}

/* Gate (equal) */
static int zp_proc_gate_eq(struct sig_node *node, struct sig_data *in,
                            struct sig_data *out)
{
    int32_t val = sig_data_read_i32(in);
    int32_t threshold = (int32_t)(long)node->user_data;
    if (val == threshold) {
        sig_data_write_i32(out, val);
        return 0;
    }
    out->size = 0;
    return 1;
}

/* Gate (greater than or equal) */
static int zp_proc_gate_gte(struct sig_node *node, struct sig_data *in,
                             struct sig_data *out)
{
    int32_t val = sig_data_read_i32(in);
    int32_t threshold = (int32_t)(long)node->user_data;
    if (val >= threshold) {
        sig_data_write_i32(out, val);
        return 0;
    }
    out->size = 0;
    return 1;
}

/* Gate (less than or equal) */
static int zp_proc_gate_lte(struct sig_node *node, struct sig_data *in,
                             struct sig_data *out)
{
    int32_t val = sig_data_read_i32(in);
    int32_t threshold = (int32_t)(long)node->user_data;
    if (val <= threshold) {
        sig_data_write_i32(out, val);
        return 0;
    }
    out->size = 0;
    return 1;
}

/*
 * Delta: output = input - previous_input (change detection)
 * Uses user_data to store the previous value.
 * First call outputs 0 (no previous value).
 */
static int32_t delta_history[ZP_MAX_NODES];
static int delta_has_prev[ZP_MAX_NODES];

static int zp_proc_delta(struct sig_node *node, struct sig_data *in,
                          struct sig_data *out)
{
    int32_t val = sig_data_read_i32(in);
    int idx = node->id;

    if (idx < ZP_MAX_NODES && delta_has_prev[idx]) {
        int32_t prev = delta_history[idx];
        sig_data_write_i32(out, val - prev);
    } else {
        sig_data_write_i32(out, 0);
    }

    if (idx < ZP_MAX_NODES) {
        delta_history[idx] = val;
        delta_has_prev[idx] = 1;
    }
    return 0;
}

/* Passthrough: copy input to output unchanged */
static int zp_proc_passthrough(struct sig_node *node, struct sig_data *in,
                                struct sig_data *out)
{
    (void)node;
    sig_data_write_i32(out, sig_data_read_i32(in));
    return 0;
}

/* Print: display the value with format string */
static int zp_proc_print(struct sig_node *node, struct sig_data *in,
                          struct sig_data *out)
{
    (void)out;
    int32_t val = sig_data_read_i32(in);

    /* Walk the format string, replacing {value} with the number */
    const char *fmt = (const char *)node->user_data;
    if (!fmt) {
        kput_dec(val);
        kputs("\n");
        return 0;
    }

    kputs("  ");
    while (*fmt) {
        if (*fmt == '{') {
            /* Check for {value} */
            const char *p = fmt + 1;
            if (p[0] == 'v' && p[1] == 'a' && p[2] == 'l' &&
                p[3] == 'u' && p[4] == 'e' && p[5] == '}') {
                kput_dec(val);
                fmt = p + 6;
                continue;
            }
        }
        kputc(*fmt);
        fmt++;
    }
    kputs("\n");
    return 0;
}

/* ── Public API ───────────────────────────────── */

int zp_parse(const char *source, struct zp_program *prog)
{
    prog->node_count = 0;
    prog->edge_count = 0;
    prog->chain_id = -1;

    struct zp_lexer lex;
    lexer_init(&lex, source);

    while (lex.pos < lex.len) {
        parse_line(&lex, prog);
    }

    return 0;
}

int zp_compile(struct zp_program *prog)
{
    /* Create a signal chain */
    int chain = sig_chain_create("zplus");
    if (chain < 0) {
        kputs("Z+ error: could not create signal chain\n");
        return -1;
    }
    prog->chain_id = chain;

    /* Create signal nodes for each parsed node */
    for (int i = 0; i < prog->node_count; i++) {
        struct zp_node_decl *decl = &prog->nodes[i];
        sig_process_fn proc = 0;
        void *user_data = 0;

        switch (decl->type) {
        case ZP_EMIT:
            proc = zp_proc_emit;
            user_data = (void *)(long)decl->int_val;
            break;
        case ZP_MULTIPLY:
            proc = zp_proc_multiply;
            user_data = (void *)(long)decl->int_val;
            break;
        case ZP_ADD:
            proc = zp_proc_add;
            user_data = (void *)(long)decl->int_val;
            break;
        case ZP_SUBTRACT:
            proc = zp_proc_subtract;
            user_data = (void *)(long)decl->int_val;
            break;
        case ZP_PRINT:
            proc = zp_proc_print;
            /* Store format string pointer as user_data.
             * Safe because the program struct outlives execution. */
            user_data = (void *)decl->fmt;
            break;
        case ZP_NEGATE:
            proc = zp_proc_multiply;
            user_data = (void *)(long)-1;
            break;
        case ZP_GATE_GT:
            proc = zp_proc_gate_gt;
            user_data = (void *)(long)decl->int_val;
            break;
        case ZP_GATE_LT:
            proc = zp_proc_gate_lt;
            user_data = (void *)(long)decl->int_val;
            break;
        case ZP_GATE_EQ:
            proc = zp_proc_gate_eq;
            user_data = (void *)(long)decl->int_val;
            break;
        case ZP_GATE_GTE:
            proc = zp_proc_gate_gte;
            user_data = (void *)(long)decl->int_val;
            break;
        case ZP_GATE_LTE:
            proc = zp_proc_gate_lte;
            user_data = (void *)(long)decl->int_val;
            break;
        case ZP_DELTA:
            proc = zp_proc_delta;
            user_data = 0;
            break;
        case ZP_PASSTHROUGH:
            proc = zp_proc_passthrough;
            user_data = 0;
            break;
        }

        int idx = sig_node_add(chain, decl->name, proc, user_data);
        if (idx < 0) {
            kputs("Z+ error: could not add node '");
            kputs(decl->name);
            kputs("'\n");
            return -1;
        }
        decl->sig_idx = idx;
    }

    /* Create edges */
    for (int i = 0; i < prog->edge_count; i++) {
        int src = find_node(prog, prog->edges[i].src);
        int dst = find_node(prog, prog->edges[i].dst);

        if (src < 0) {
            kputs("Z+ error: unknown node '");
            kputs(prog->edges[i].src);
            kputs("'\n");
            return -1;
        }
        if (dst < 0) {
            kputs("Z+ error: unknown node '");
            kputs(prog->edges[i].dst);
            kputs("'\n");
            return -1;
        }

        int r = sig_edge_add(chain, prog->nodes[src].sig_idx,
                                     prog->nodes[dst].sig_idx);
        if (r < 0) {
            kputs("Z+ error: could not wire ");
            kputs(prog->edges[i].src);
            kputs(" -> ");
            kputs(prog->edges[i].dst);
            kputs("\n");
            return -1;
        }
    }

    return chain;
}

int zp_execute(struct zp_program *prog)
{
    if (prog->chain_id < 0)
        return -1;

    /* Inject data into all source (emit) nodes */
    for (int i = 0; i < prog->node_count; i++) {
        if (prog->nodes[i].type == ZP_EMIT) {
            struct sig_data trigger = {.size = 0, .type = 0};
            sig_inject(prog->chain_id, prog->nodes[i].sig_idx, &trigger);
        }
    }

    /* Resolve */
    int fired = sig_resolve(prog->chain_id);
    return fired;
}

int zp_run(const char *source)
{
    struct zp_program prog;

    kputs("\n  Z+ interpreter v0.2\n");
    kputs("  Parsing...\n");

    if (zp_parse(source, &prog) < 0) {
        kputs("  Parse error.\n\n");
        return -1;
    }

    kputs("  Parsed ");
    kput_dec(prog.node_count);
    kputs(" nodes, ");
    kput_dec(prog.edge_count);
    kputs(" edges.\n");

    kputs("  Compiling...\n");

    if (zp_compile(&prog) < 0) {
        kputs("  Compile error.\n\n");
        return -1;
    }

    kputs("  Running...\n\n");

    int fired = zp_execute(&prog);

    kputs("\n  ");
    kput_dec(fired);
    kputs(" nodes fired.\n");

    /* Show timing */
    struct sig_chain *c = sig_get_chain(prog.chain_id);
    if (c) {
        kputs("  Chain resolved in ");
        kput_dec(c->tsc_end - c->tsc_start);
        kputs(" TSC cycles.\n");

        kputs("\n  Node timing:\n");
        for (int i = 0; i < c->node_count; i++) {
            struct sig_node *n = &c->nodes[i];
            kputs("    ");
            kputs(n->name);
            kputs(": ");
            kput_dec(n->tsc_end - n->tsc_start);
            kputs(" cycles\n");
        }
    }

    kputs("\n");
    return fired;
}
