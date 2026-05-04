/*
 * Zeos — Z+ Interpreter
 *
 * Parses Tier 3 Z+ syntax and maps it to the signal chain engine.
 *
 * Z+ now binds directly to the live kernel chain graph.
 * audio.* -> CHAIN_AUDIO, net.* -> CHAIN_NET_TX/RX, fs.* -> CHAIN_BLOCK,
 * vault.* -> vault_save_config / vault_load_config. Programs are
 * first-class chain consumers — Z+ is the surface that talks to the
 * actual registered chain graph, not a generic DSL.
 *
 * The interpreter works in three phases:
 *   1. PARSE  — tokenize source, extract node decls and edges
 *   2. COMPILE — create signal chain nodes and wire them
 *   3. EXECUTE — inject data into sources and resolve (multi-tick when
 *                a sustained() node is present so its consecutive-tick
 *                condition can fire)
 */

#include "zplus.h"
#include "signal.h"
#include "chain.h"
#include "chain_registry.h"
#include "hda.h"
#include "net_chain.h"
#include "block_chain.h"
#include "vault.h"
#include "kprint.h"

/* The audio chain consumes a staged pcm_request from hda.c. It's declared
 * static there; we use the module-level shim hda_play_pcm() to stage and
 * resolve, which is the canonical compat-shim pattern from the chain
 * contract. zp_proc_audio_play() does the same work without the imperative
 * wrapper — it stages a tiny PCM blip (square wave) and calls
 * chain_resolve(CHAIN_AUDIO) directly. The chain's vault_version bumps
 * automatically on stream start (per the MasQ provenance rule). */

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
/* Identifier extension: allow '.' so verbs like audio.play / net.send /
 * vault.get / tap.log lex as a single TOK_IDENT. */
static int zp_isident(char c) { return zp_isalnum(c) || c == '.'; }

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

    /* Identifier (may contain '.') */
    if (zp_isalpha(c)) {
        int i = 0;
        while (lex->pos < lex->len && zp_isident(lexer_peek(lex))) {
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
    prog->nodes[idx].int_val2 = 0;
    prog->nodes[idx].int_val3 = 0;
    prog->nodes[idx].sig_idx = -1;
    prog->nodes[idx].chain_bind_id = -1;
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

/* Map a TOK_IDENT verb name (after lexing with '.' allowed) to a node type.
 * Returns 1 on hit, 0 if unknown. */
static int verb_to_type(const char *t, enum zp_node_type *out)
{
    if (zp_streq(t, "audio.play"))  { *out = ZP_AUDIO_PLAY; return 1; }
    if (zp_streq(t, "net.send"))    { *out = ZP_NET_SEND;   return 1; }
    if (zp_streq(t, "net.recv"))    { *out = ZP_NET_RECV;   return 1; }
    if (zp_streq(t, "fs.read"))     { *out = ZP_FS_READ;    return 1; }
    if (zp_streq(t, "fs.write"))    { *out = ZP_FS_WRITE;   return 1; }
    if (zp_streq(t, "vault.put"))   { *out = ZP_VAULT_PUT;  return 1; }
    if (zp_streq(t, "vault.get"))   { *out = ZP_VAULT_GET;  return 1; }
    if (zp_streq(t, "tap.log"))     { *out = ZP_TAP_LOG;    return 1; }
    return 0;
}

/* Parse `(low = N, high = M)` for knee. */
static void parse_knee_args(struct zp_lexer *lex, int32_t *out_low, int32_t *out_high)
{
    struct zp_token t;
    *out_low = 0; *out_high = 100;
    lexer_token(lex, &t); /* ( */
    if (t.type != TOK_LPAREN) return;
    while (1) {
        lexer_token(lex, &t);
        if (t.type == TOK_RPAREN || t.type == TOK_EOF || t.type == TOK_NEWLINE) break;
        if (t.type == TOK_IDENT) {
            char key[ZP_MAX_NAME];
            zp_strcpy(key, t.text, ZP_MAX_NAME);
            lexer_token(lex, &t); /* '=' would be EQ but we lex == only; '=' is unknown */
            /* Our lexer doesn't tokenize bare '='; it skipped. Just read a number. */
            if (t.type != TOK_NUMBER) lexer_token(lex, &t);
            if (t.type == TOK_NUMBER) {
                if (zp_streq(key, "low"))       *out_low  = t.num_val;
                else if (zp_streq(key, "high")) *out_high = t.num_val;
            }
        }
        lexer_token(lex, &t); /* , or ) */
        if (t.type == TOK_RPAREN) break;
    }
}

/* Parse `(> N, for = M)` for sustained. Returns gate-style comparator type
 * via *out_gtype, threshold in *out_thresh, count in *out_count. */
static void parse_sustained_args(struct zp_lexer *lex,
                                 enum zp_node_type *out_gtype,
                                 int32_t *out_thresh,
                                 int32_t *out_count)
{
    struct zp_token t;
    *out_gtype = ZP_GATE_GT;
    *out_thresh = 0;
    *out_count = 1;
    lexer_token(lex, &t); /* ( */
    if (t.type != TOK_LPAREN) return;
    lexer_token(lex, &t); /* comparator */
    if      (t.type == TOK_GT)  *out_gtype = ZP_GATE_GT;
    else if (t.type == TOK_LT)  *out_gtype = ZP_GATE_LT;
    else if (t.type == TOK_EQ)  *out_gtype = ZP_GATE_EQ;
    else if (t.type == TOK_GTE) *out_gtype = ZP_GATE_GTE;
    else if (t.type == TOK_LTE) *out_gtype = ZP_GATE_LTE;
    lexer_token(lex, &t); /* threshold number */
    if (t.type == TOK_NUMBER) *out_thresh = t.num_val;
    lexer_token(lex, &t); /* , */
    /* expect for = N */
    while (t.type != TOK_RPAREN && t.type != TOK_EOF && t.type != TOK_NEWLINE) {
        if (t.type == TOK_IDENT && zp_streq(t.text, "for")) {
            lexer_token(lex, &t); /* maybe number or skipped '=' */
            if (t.type != TOK_NUMBER) lexer_token(lex, &t);
            if (t.type == TOK_NUMBER) *out_count = t.num_val;
        }
        lexer_token(lex, &t);
    }
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

    /* ── Chain definition: chain name { node1 -> node2 -> ... } ── */
    if (zp_streq(tok.text, "chain")) {
        if (prog->chain_def_count >= ZP_MAX_CHAINS)
            return -1;

        struct zp_token t;
        lexer_token(lex, &t);  /* chain name */
        if (t.type != TOK_IDENT)
            return -1;

        struct zp_chain_def *def = &prog->chain_defs[prog->chain_def_count];
        zp_strcpy(def->name, t.text, ZP_MAX_NAME);
        def->node_count = 0;
        def->chain_id = -1;

        lexer_token(lex, &t);  /* { */
        if (t.type != TOK_LBRACE)
            return -1;

        /* Skip newlines after { */
        lexer_token(lex, &t);
        while (t.type == TOK_NEWLINE)
            lexer_token(lex, &t);

        /* Parse node1 -> node2 -> node3 ... until } */
        while (t.type != TOK_RBRACE && t.type != TOK_EOF) {
            if (t.type == TOK_IDENT) {
                if (def->node_count < ZP_MAX_CHAIN_NODES) {
                    zp_strcpy(def->node_names[def->node_count], t.text, ZP_MAX_NAME);
                    def->node_count++;
                }
            }
            /* Skip arrows and newlines between nodes */
            lexer_token(lex, &t);
            while (t.type == TOK_ARROW || t.type == TOK_NEWLINE)
                lexer_token(lex, &t);
        }

        prog->chain_def_count++;
        return 0;
    }

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

            } else if (t.type == TOK_IDENT && zp_streq(t.text, "knee")) {
                /* knee(low = N, high = M) [-> output]   (output skipped by skip_line) */
                int32_t lo, hi;
                parse_knee_args(lex, &lo, &hi);
                add_node(prog, first_name, ZP_KNEE, lo, 0);
                prog->nodes[prog->node_count - 1].int_val2 = hi;

            } else if (t.type == TOK_IDENT && zp_streq(t.text, "sustained")) {
                /* sustained(> N, for = K) [-> output] */
                enum zp_node_type gt;
                int32_t thresh, count;
                parse_sustained_args(lex, &gt, &thresh, &count);
                add_node(prog, first_name, ZP_SUSTAINED, thresh, 0);
                prog->nodes[prog->node_count - 1].int_val2 = count;
                prog->nodes[prog->node_count - 1].int_val3 = (int32_t)gt;

            } else if (t.type == TOK_IDENT) {
                /* Verb dispatch: audio.play / net.* / fs.* / vault.* / tap.log */
                enum zp_node_type vt;
                if (verb_to_type(t.text, &vt)) {
                    add_node(prog, first_name, vt, 0, 0);
                    /* fs.read/write and vault.put/get accept args.
                     * Probe by char in source — we don't pre-consume the
                     * next token because skip_line will eat to newline. */
                    if (lexer_peek(lex) == '(') {
                        struct zp_token p;
                        lexer_token(lex, &p); /* ( */
                        if (vt == ZP_VAULT_PUT || vt == ZP_VAULT_GET) {
                            lexer_token(lex, &p); /* key string */
                            if (p.type == TOK_STRING)
                                zp_strcpy(prog->nodes[prog->node_count - 1].fmt,
                                          p.text, ZP_MAX_STRING);
                            while (p.type != TOK_RPAREN && p.type != TOK_NEWLINE
                                   && p.type != TOK_EOF)
                                lexer_token(lex, &p);
                        } else if (vt == ZP_FS_READ || vt == ZP_FS_WRITE) {
                            int32_t a = 0, b = 0, c = 0;
                            lexer_token(lex, &p);
                            if (p.type == TOK_NUMBER) a = p.num_val;
                            lexer_token(lex, &p); /* , */
                            lexer_token(lex, &p);
                            if (p.type == TOK_NUMBER) b = p.num_val;
                            lexer_token(lex, &p); /* , */
                            lexer_token(lex, &p);
                            if (p.type == TOK_NUMBER) c = p.num_val;
                            prog->nodes[prog->node_count - 1].int_val  = a;
                            prog->nodes[prog->node_count - 1].int_val2 = b;
                            prog->nodes[prog->node_count - 1].int_val3 = c;
                            while (p.type != TOK_RPAREN && p.type != TOK_NEWLINE
                                   && p.type != TOK_EOF)
                                lexer_token(lex, &p);
                        } else {
                            while (p.type != TOK_RPAREN && p.type != TOK_NEWLINE
                                   && p.type != TOK_EOF)
                                lexer_token(lex, &p);
                        }
                    }
                }
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

/* Knee: smooth ramp. Below low -> 0. Above high -> 100. Linear in between.
 * low/high stored in user_data low24:high8? Use globals indexed by node id. */
static int32_t knee_low[ZP_MAX_NODES];
static int32_t knee_high[ZP_MAX_NODES];

static int zp_proc_knee(struct sig_node *node, struct sig_data *in,
                         struct sig_data *out)
{
    int32_t v = sig_data_read_i32(in);
    int idx = node->id;
    int32_t lo = (idx < ZP_MAX_NODES) ? knee_low[idx]  : 0;
    int32_t hi = (idx < ZP_MAX_NODES) ? knee_high[idx] : 100;
    int32_t r;
    if (v <= lo)        r = 0;
    else if (v >= hi)   r = 100;
    else if (hi == lo)  r = 100;
    else                r = ((v - lo) * 100) / (hi - lo);
    sig_data_write_i32(out, r);
    return 0;
}

/* Sustained: fires only after input has held the comparator condition for
 * N consecutive ticks. Internal counter. When threshold reached, output
 * passes the input value through; otherwise output 0 (no fire). */
static int32_t sustained_thresh[ZP_MAX_NODES];
static int32_t sustained_target[ZP_MAX_NODES];
static int32_t sustained_count[ZP_MAX_NODES];
static int32_t sustained_gate[ZP_MAX_NODES];   /* gate type */

static int sustained_cmp(int32_t v, int32_t t, int32_t gt)
{
    switch ((enum zp_node_type)gt) {
    case ZP_GATE_GT:  return v >  t;
    case ZP_GATE_LT:  return v <  t;
    case ZP_GATE_EQ:  return v == t;
    case ZP_GATE_GTE: return v >= t;
    case ZP_GATE_LTE: return v <= t;
    default:          return v >  t;
    }
}

static int zp_proc_sustained(struct sig_node *node, struct sig_data *in,
                              struct sig_data *out)
{
    int32_t v = sig_data_read_i32(in);
    int idx = node->id;
    if (idx >= ZP_MAX_NODES) { out->size = 0; return 1; }
    int32_t t = sustained_thresh[idx];
    int32_t target = sustained_target[idx];
    int32_t gt = sustained_gate[idx];

    if (sustained_cmp(v, t, gt))
        sustained_count[idx]++;
    else
        sustained_count[idx] = 0;

    if (sustained_count[idx] >= target) {
        sig_data_write_i32(out, v);
        return 0;
    }
    /* Not yet sustained — block downstream */
    out->size = 0;
    return 1;
}

/* audio.play — bridge to CHAIN_AUDIO. Generates a short square-wave blip
 * (200 samples @ 8 kHz, 440 Hz) and stages it through the canonical
 * pcm_request path. The compat shim hda_play_pcm() does exactly this:
 * stage the request, call chain_resolve(CHAIN_AUDIO). The chain's
 * vault_version bumps inside the pcm_source / hardware_dma resolves per
 * the MasQ provenance rule. */
static int zp_proc_audio_play(struct sig_node *node, struct sig_data *in,
                               struct sig_data *out)
{
    (void)node; (void)in;
    static int16_t blip[400];
    static int initialized = 0;
    if (!initialized) {
        for (int i = 0; i < 400; i++)
            blip[i] = (i & 0x10) ? 12000 : -12000;  /* coarse 440-ish square */
        initialized = 1;
    }
    int rc = hda_play_pcm(blip, 400, 8000);
    sig_data_write_i32(out, rc == 0 ? 1 : 0);
    return 0;
}

/* net.send — bridge to CHAIN_NET_TX. Sends a tiny placeholder ethernet
 * frame (broadcast, EtherType 0x88B5 reserved-experimental). Returns 1 if
 * the chain accepted the frame, 0 otherwise. */
static int zp_proc_net_send(struct sig_node *node, struct sig_data *in,
                             struct sig_data *out)
{
    (void)node; (void)in;
    uint8_t frame[64];
    for (int i = 0; i < 6; i++) frame[i] = 0xff;        /* dst broadcast */
    for (int i = 6; i < 12; i++) frame[i] = 0x00;        /* src placeholder */
    frame[12] = 0x88; frame[13] = 0xB5;                  /* experimental */
    for (int i = 14; i < 64; i++) frame[i] = (uint8_t)i;
    int rc = net_chain_send(frame, sizeof(frame));
    sig_data_write_i32(out, rc == 0 ? 1 : 0);
    return 0;
}

/* net.recv — bridge to CHAIN_NET_RX. Pulls one frame if present, emits
 * its byte length. Zero if queue empty (chain still resolves). */
static int zp_proc_net_recv(struct sig_node *node, struct sig_data *in,
                             struct sig_data *out)
{
    (void)node; (void)in;
    uint8_t buf[1600];
    int len = net_chain_recv(buf, sizeof(buf));
    sig_data_write_i32(out, len > 0 ? len : 0);
    return 0;
}

/* fs.read(drive, lba, count) — bridge to CHAIN_BLOCK with op=READ. Reads
 * into a small static scratch buffer (up to one block). Output = bytes
 * actually moved (count*512 on success, 0 on failure). */
static uint8_t zp_fs_scratch[4096];
static int32_t zp_fs_drive[ZP_MAX_NODES];
static int32_t zp_fs_lba[ZP_MAX_NODES];
static int32_t zp_fs_count[ZP_MAX_NODES];

static int zp_proc_fs_read(struct sig_node *node, struct sig_data *in,
                            struct sig_data *out)
{
    (void)in;
    int idx = node->id;
    int32_t drive = (idx < ZP_MAX_NODES) ? zp_fs_drive[idx] : 0;
    int32_t lba   = (idx < ZP_MAX_NODES) ? zp_fs_lba[idx]   : 0;
    int32_t count = (idx < ZP_MAX_NODES) ? zp_fs_count[idx] : 1;
    if (count > 8) count = 8;
    if (count <= 0) count = 1;
    int rc = block_chain_submit(drive, (uint64_t)lba, (uint32_t)count,
                                BLOCK_OP_READ, zp_fs_scratch);
    sig_data_write_i32(out, rc == 0 ? count * 512 : 0);
    return 0;
}

static int zp_proc_fs_write(struct sig_node *node, struct sig_data *in,
                             struct sig_data *out)
{
    (void)in;
    int idx = node->id;
    int32_t drive = (idx < ZP_MAX_NODES) ? zp_fs_drive[idx] : 0;
    int32_t lba   = (idx < ZP_MAX_NODES) ? zp_fs_lba[idx]   : 0;
    int32_t count = (idx < ZP_MAX_NODES) ? zp_fs_count[idx] : 1;
    if (count > 8) count = 8;
    if (count <= 0) count = 1;
    /* Source buffer = scratch (whatever a previous fs.read left). */
    int rc = block_chain_submit(drive, (uint64_t)lba, (uint32_t)count,
                                BLOCK_OP_WRITE, zp_fs_scratch);
    sig_data_write_i32(out, rc == 0 ? count * 512 : 0);
    return 0;
}

/* vault.put / vault.get — vault config key/value store bridge. Key is in
 * node->user_data as a const char* (the parsed string literal). Value for
 * put is the input int32. */
static int zp_proc_vault_put(struct sig_node *node, struct sig_data *in,
                              struct sig_data *out)
{
    int32_t v = sig_data_read_i32(in);
    const char *key = (const char *)node->user_data;
    if (!key || !*key) { sig_data_write_i32(out, 0); return 0; }
    int rc = vault_save_config(key, &v, sizeof(v));
    sig_data_write_i32(out, rc > 0 ? v : 0);
    return 0;
}

static int zp_proc_vault_get(struct sig_node *node, struct sig_data *in,
                              struct sig_data *out)
{
    (void)in;
    const char *key = (const char *)node->user_data;
    int32_t v = 0;
    if (key && *key) vault_load_config(key, &v, sizeof(v));
    sig_data_write_i32(out, v);
    return 0;
}

/* tap.log — pure observer. Logs the value to kprint and passes it through
 * unchanged so the chain can keep flowing. */
static int zp_proc_tap_log(struct sig_node *node, struct sig_data *in,
                            struct sig_data *out)
{
    int32_t v = sig_data_read_i32(in);
    kputs("    [tap.log ");
    kputs(node->name);
    kputs("] ");
    kput_dec((uint64_t)(uint32_t)v);
    kputs("\n");
    sig_data_write_i32(out, v);
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

/* ── Chain node resolve (placeholder for user-defined nodes) ── */

static void zp_chain_node_resolve(chain_node_t *self, void *input, void *output)
{
    (void)input;
    (void)output;
    kputs("  resolved: ");
    kputs(self->name);
    kputs("\n");
}

/*
 * Compile chain definitions from Z+ source into chain.h chains.
 * Called from zp_compile after signal chain compilation.
 */
static int zp_compile_chains(struct zp_program *prog)
{
    for (int i = 0; i < prog->chain_def_count; i++) {
        struct zp_chain_def *def = &prog->chain_defs[i];

        int cid = chain_create(def->name, -1, MASQ_REFERENCE);
        if (cid < 0) {
            kputs("Z+ error: could not create chain '");
            kputs(def->name);
            kputs("'\n");
            return -1;
        }
        def->chain_id = cid;

        for (int n = 0; n < def->node_count; n++) {
            int nid = chain_add_node(cid, def->node_names[n],
                                     "any", "any",
                                     zp_chain_node_resolve);
            if (nid < 0) {
                kputs("Z+ error: could not add node '");
                kputs(def->node_names[n]);
                kputs("' to chain '");
                kputs(def->name);
                kputs("'\n");
                return -1;
            }
        }

        kputs("  Chain '");
        kputs(def->name);
        kputs("' created (id=");
        kput_dec((uint64_t)cid);
        kputs(", ");
        kput_dec((uint64_t)def->node_count);
        kputs(" nodes)\n");
    }

    return 0;
}

/* ── Public API ───────────────────────────────── */

int zp_parse(const char *source, struct zp_program *prog)
{
    prog->node_count = 0;
    prog->edge_count = 0;
    prog->chain_id = -1;
    prog->chain_def_count = 0;

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
        case ZP_KNEE:
            proc = zp_proc_knee;
            user_data = 0;
            break;
        case ZP_SUSTAINED:
            proc = zp_proc_sustained;
            user_data = 0;
            break;
        case ZP_AUDIO_PLAY:
            proc = zp_proc_audio_play;
            user_data = 0;
            decl->chain_bind_id = CHAIN_AUDIO;
            break;
        case ZP_NET_SEND:
            proc = zp_proc_net_send;
            user_data = 0;
            decl->chain_bind_id = CHAIN_NET_TX;
            break;
        case ZP_NET_RECV:
            proc = zp_proc_net_recv;
            user_data = 0;
            decl->chain_bind_id = CHAIN_NET_RX;
            break;
        case ZP_FS_READ:
            proc = zp_proc_fs_read;
            user_data = 0;
            decl->chain_bind_id = CHAIN_BLOCK;
            break;
        case ZP_FS_WRITE:
            proc = zp_proc_fs_write;
            user_data = 0;
            decl->chain_bind_id = CHAIN_BLOCK;
            break;
        case ZP_VAULT_PUT:
            proc = zp_proc_vault_put;
            user_data = (void *)decl->fmt;
            break;
        case ZP_VAULT_GET:
            proc = zp_proc_vault_get;
            user_data = (void *)decl->fmt;
            break;
        case ZP_TAP_LOG:
            proc = zp_proc_tap_log;
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

        /* Seed per-node state arrays for nodes that need extra params. */
        if (idx < ZP_MAX_NODES) {
            if (decl->type == ZP_KNEE) {
                knee_low[idx]  = decl->int_val;
                knee_high[idx] = decl->int_val2;
            } else if (decl->type == ZP_SUSTAINED) {
                sustained_thresh[idx] = decl->int_val;
                sustained_target[idx] = decl->int_val2;
                sustained_gate[idx]   = decl->int_val3;
                sustained_count[idx]  = 0;
            } else if (decl->type == ZP_FS_READ || decl->type == ZP_FS_WRITE) {
                zp_fs_drive[idx] = decl->int_val;
                zp_fs_lba[idx]   = decl->int_val2;
                zp_fs_count[idx] = decl->int_val3;
            }
            /* Reset delta history for this node so re-runs start clean. */
            if (decl->type == ZP_DELTA) {
                delta_history[idx] = 0;
                delta_has_prev[idx] = 0;
            }
        }
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

    /* Compile chain definitions (chain.h chains) */
    if (prog->chain_def_count > 0) {
        if (zp_compile_chains(prog) < 0)
            return -1;
    }

    return chain;
}

int zp_execute(struct zp_program *prog)
{
    if (prog->chain_id < 0)
        return -1;

    /* If the program has a sustained node with target N, the chain has to
     * tick at least N+1 times before the sustain fires. Walk the program
     * to compute the longest target and run that many ticks. Default 1. */
    int max_ticks = 1;
    for (int i = 0; i < prog->node_count; i++) {
        if (prog->nodes[i].type == ZP_SUSTAINED) {
            int t = prog->nodes[i].int_val2 + 1;
            if (t > max_ticks) max_ticks = t;
        }
    }
    if (max_ticks > 16) max_ticks = 16; /* safety cap */

    int total_fired = 0;
    for (int tick = 0; tick < max_ticks; tick++) {
        /* Inject data into all source (emit) nodes each tick */
        for (int i = 0; i < prog->node_count; i++) {
            if (prog->nodes[i].type == ZP_EMIT) {
                struct sig_data trigger = {.size = 0, .type = 0};
                sig_inject(prog->chain_id, prog->nodes[i].sig_idx, &trigger);
            }
        }
        int fired = sig_resolve(prog->chain_id);
        if (fired > 0) total_fired += fired;
        if (max_ticks > 1) {
            kputs("    tick ");
            kput_dec((uint64_t)tick);
            kputs(": ");
            kput_dec((uint64_t)fired);
            kputs(" fired\n");
        }
    }
    return total_fired;
}

int zp_run(const char *source)
{
    struct zp_program prog;

    kputs("\n  Z+ interpreter v0.3\n");
    kputs("  Parsing...\n");

    if (zp_parse(source, &prog) < 0) {
        kputs("  Parse error.\n\n");
        return -1;
    }

    kputs("  Parsed ");
    kput_dec(prog.node_count);
    kputs(" nodes, ");
    kput_dec(prog.edge_count);
    kputs(" edges");
    if (prog.chain_def_count > 0) {
        kputs(", ");
        kput_dec(prog.chain_def_count);
        kputs(" chain(s)");
    }
    kputs(".\n");

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

void zp_list_chains(void)
{
    int count = chain_count();
    if (count == 0) {
        kputs("No chains active.\n");
        return;
    }

    kputs("\n  Active chains (chain.h):\n\n");
    for (int i = 0; i < MAX_CHAINS; i++) {
        chain_t *c = chain_get(i);
        if (!c) continue;

        kputs("  [");
        kput_dec((uint64_t)c->id);
        kputs("] ");
        kputs(c->name);
        kputs("  (");
        kput_dec((uint64_t)c->node_count);
        kputs(" nodes, status=");
        switch (c->status) {
        case CHAIN_LIVE:     kputs("live");     break;
        case CHAIN_PAUSED:   kputs("paused");   break;
        case CHAIN_ERROR:    kputs("error");    break;
        case CHAIN_DETACHED: kputs("detached"); break;
        }
        kputs(")\n");
    }
    kputs("\n  Total: ");
    kput_dec((uint64_t)count);
    kputs(" chain(s)\n");
    kputs("  Use 'inspect <id>' for details.\n\n");
}

void zp_inspect_chain(int chain_id)
{
    chain_t *c = chain_get(chain_id);
    if (!c) {
        kputs("Chain ");
        kput_dec((uint64_t)chain_id);
        kputs(" not found.\n");
        return;
    }

    kputs("\n");
    chain_dump(chain_id);
    kputs("\n");
}
