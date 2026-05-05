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
#include "zp_runtime.h"
#include "signal.h"
#include "chain.h"
#include "chain_registry.h"
#include "hda.h"
#include "net_chain.h"
#include "block_chain.h"
#include "mde_chain.h"
#include "vault.h"
#include "kprint.h"
#include "heap.h"

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

/* ── Pass 1: string / struct / array pools ────────────────────
 *
 * Three reference-counted pools live behind a single zp_pools_init().
 * Strings dedupe on intern. Structs and arrays do not.  Cyclic struct
 * references will leak — the next pass adds a sweep collector.
 */

struct zp_str_slot {
    char    *bytes;     /* heap-allocated, NUL-terminated */
    int      len;
    int      refcount;  /* 0 == free slot */
};
static struct zp_str_slot     g_str_pool[ZP_STRING_POOL_SLOTS];

struct zp_struct_inst {
    int      type_idx;
    int      in_use;
    int      refcount;
    /* Per-field storage. ints stored in handle, strings stored as handle. */
    int      kinds [ZP_MAX_STRUCT_FIELDS];
    int32_t  values[ZP_MAX_STRUCT_FIELDS];
};
static struct zp_struct_type  g_struct_types[ZP_STRUCT_TYPE_SLOTS];
static struct zp_struct_inst  g_struct_pool[ZP_STRUCT_INST_SLOTS];

struct zp_array_slot {
    int     in_use;
    int     refcount;
    int     count;
    int     elems[16];      /* string-handle elements */
};
static struct zp_array_slot   g_array_pool[ZP_ARRAY_POOL_SLOTS];

static int g_pools_initialized = 0;
static int g_string_verb_count = 0;

void zp_pools_init(void)
{
    if (g_pools_initialized) return;
    for (int i = 0; i < ZP_STRING_POOL_SLOTS; i++) {
        g_str_pool[i].bytes = 0;
        g_str_pool[i].len = 0;
        g_str_pool[i].refcount = 0;
    }
    for (int i = 0; i < ZP_STRUCT_TYPE_SLOTS; i++) g_struct_types[i].in_use = 0;
    for (int i = 0; i < ZP_STRUCT_INST_SLOTS; i++) g_struct_pool[i].in_use = 0;
    for (int i = 0; i < ZP_ARRAY_POOL_SLOTS;  i++) g_array_pool[i].in_use = 0;
    g_pools_initialized = 1;
    /* Count of pass-1 string verbs advertised — kept in sync with the
     * ZP_STR_* enum block.  13 verbs. */
    g_string_verb_count = 13;
}

static int zp_pool_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int zp_pool_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

int zp_str_intern(const char *s, int len)
{
    if (!g_pools_initialized) zp_pools_init();
    if (!s) return -1;
    if (len < 0) len = zp_pool_strlen(s);
    if (len > ZP_MAX_STRING_BYTES) return -1;

    /* Dedupe: scan live slots for an exact match. */
    for (int i = 0; i < ZP_STRING_POOL_SLOTS; i++) {
        if (g_str_pool[i].refcount <= 0) continue;
        if (g_str_pool[i].len != len) continue;
        int eq = 1;
        for (int j = 0; j < len; j++) {
            if (g_str_pool[i].bytes[j] != s[j]) { eq = 0; break; }
        }
        if (eq) {
            g_str_pool[i].refcount++;
            return i;
        }
    }
    /* Allocate a new slot. */
    for (int i = 0; i < ZP_STRING_POOL_SLOTS; i++) {
        if (g_str_pool[i].refcount > 0) continue;
        char *buf = (char *)kmalloc((uint64_t)(len + 1));
        if (!buf) return -1;
        for (int j = 0; j < len; j++) buf[j] = s[j];
        buf[len] = '\0';
        g_str_pool[i].bytes = buf;
        g_str_pool[i].len = len;
        g_str_pool[i].refcount = 1;
        return i;
    }
    return -1;
}

const char *zp_str_get(int handle, int *out_len)
{
    if (handle < 0 || handle >= ZP_STRING_POOL_SLOTS) return 0;
    if (g_str_pool[handle].refcount <= 0) return 0;
    if (out_len) *out_len = g_str_pool[handle].len;
    return g_str_pool[handle].bytes;
}

void zp_str_addref(int handle)
{
    if (handle < 0 || handle >= ZP_STRING_POOL_SLOTS) return;
    if (g_str_pool[handle].refcount <= 0) return;
    g_str_pool[handle].refcount++;
}

void zp_str_release(int handle)
{
    if (handle < 0 || handle >= ZP_STRING_POOL_SLOTS) return;
    if (g_str_pool[handle].refcount <= 0) return;
    g_str_pool[handle].refcount--;
    if (g_str_pool[handle].refcount == 0) {
        if (g_str_pool[handle].bytes) {
            kfree(g_str_pool[handle].bytes);
            g_str_pool[handle].bytes = 0;
        }
        g_str_pool[handle].len = 0;
    }
}

int zp_string_verb_count(void)
{
    if (!g_pools_initialized) zp_pools_init();
    return g_string_verb_count;
}

int zp_struct_type_count(void)
{
    if (!g_pools_initialized) return 0;
    int n = 0;
    for (int i = 0; i < ZP_STRUCT_TYPE_SLOTS; i++)
        if (g_struct_types[i].in_use) n++;
    return n;
}

int zp_struct_find(const char *name)
{
    if (!g_pools_initialized) return -1;
    for (int i = 0; i < ZP_STRUCT_TYPE_SLOTS; i++) {
        if (!g_struct_types[i].in_use) continue;
        if (zp_pool_streq(g_struct_types[i].name, name)) return i;
    }
    return -1;
}

int zp_struct_register(const struct zp_struct_type *def)
{
    if (!g_pools_initialized) zp_pools_init();
    if (!def || !def->name[0]) return -1;
    /* Replace if exists. */
    int slot = zp_struct_find(def->name);
    if (slot < 0) {
        for (int i = 0; i < ZP_STRUCT_TYPE_SLOTS; i++) {
            if (!g_struct_types[i].in_use) { slot = i; break; }
        }
    }
    if (slot < 0) return -1;
    g_struct_types[slot] = *def;
    g_struct_types[slot].in_use = 1;
    return slot;
}

int zp_struct_alloc(int type_idx)
{
    if (!g_pools_initialized) zp_pools_init();
    if (type_idx < 0 || type_idx >= ZP_STRUCT_TYPE_SLOTS) return -1;
    if (!g_struct_types[type_idx].in_use) return -1;
    for (int i = 0; i < ZP_STRUCT_INST_SLOTS; i++) {
        if (g_struct_pool[i].in_use) continue;
        g_struct_pool[i].in_use = 1;
        g_struct_pool[i].refcount = 1;
        g_struct_pool[i].type_idx = type_idx;
        for (int f = 0; f < ZP_MAX_STRUCT_FIELDS; f++) {
            g_struct_pool[i].kinds[f]  = ZP_VAL_INT;
            g_struct_pool[i].values[f] = 0;
        }
        return i;
    }
    return -1;
}

static int struct_field_idx(int type_idx, const char *field)
{
    if (type_idx < 0 || type_idx >= ZP_STRUCT_TYPE_SLOTS) return -1;
    struct zp_struct_type *t = &g_struct_types[type_idx];
    for (int i = 0; i < t->field_count; i++) {
        if (zp_pool_streq(t->fields[i].name, field)) return i;
    }
    return -1;
}

int zp_struct_set_int(int handle, const char *field, int32_t v)
{
    if (handle < 0 || handle >= ZP_STRUCT_INST_SLOTS) return -1;
    if (!g_struct_pool[handle].in_use) return -1;
    int fi = struct_field_idx(g_struct_pool[handle].type_idx, field);
    if (fi < 0) return -1;
    g_struct_pool[handle].kinds[fi]  = ZP_VAL_INT;
    g_struct_pool[handle].values[fi] = v;
    return 0;
}

int zp_struct_set_str(int handle, const char *field, int str_handle)
{
    if (handle < 0 || handle >= ZP_STRUCT_INST_SLOTS) return -1;
    if (!g_struct_pool[handle].in_use) return -1;
    int fi = struct_field_idx(g_struct_pool[handle].type_idx, field);
    if (fi < 0) return -1;
    g_struct_pool[handle].kinds[fi]  = ZP_VAL_STR;
    g_struct_pool[handle].values[fi] = str_handle;
    zp_str_addref(str_handle);
    return 0;
}

int zp_struct_get(int handle, const char *field, struct zp_value *out)
{
    if (!out) return -1;
    if (handle < 0 || handle >= ZP_STRUCT_INST_SLOTS) return -1;
    if (!g_struct_pool[handle].in_use) return -1;
    int fi = struct_field_idx(g_struct_pool[handle].type_idx, field);
    if (fi < 0) return -1;
    out->kind   = g_struct_pool[handle].kinds[fi];
    out->handle = g_struct_pool[handle].values[fi];
    return 0;
}

int zp_array_alloc(void)
{
    if (!g_pools_initialized) zp_pools_init();
    for (int i = 0; i < ZP_ARRAY_POOL_SLOTS; i++) {
        if (g_array_pool[i].in_use) continue;
        g_array_pool[i].in_use = 1;
        g_array_pool[i].refcount = 1;
        g_array_pool[i].count = 0;
        return i;
    }
    return -1;
}

int zp_array_push_str(int arr_handle, int str_handle)
{
    if (arr_handle < 0 || arr_handle >= ZP_ARRAY_POOL_SLOTS) return -1;
    if (!g_array_pool[arr_handle].in_use) return -1;
    if (g_array_pool[arr_handle].count >= 16) return -1;
    g_array_pool[arr_handle].elems[g_array_pool[arr_handle].count++] = str_handle;
    zp_str_addref(str_handle);
    return 0;
}

int zp_array_len(int arr_handle)
{
    if (arr_handle < 0 || arr_handle >= ZP_ARRAY_POOL_SLOTS) return 0;
    if (!g_array_pool[arr_handle].in_use) return 0;
    return g_array_pool[arr_handle].count;
}

int zp_array_get_str(int arr_handle, int idx)
{
    if (arr_handle < 0 || arr_handle >= ZP_ARRAY_POOL_SLOTS) return -1;
    if (!g_array_pool[arr_handle].in_use) return -1;
    if (idx < 0 || idx >= g_array_pool[arr_handle].count) return -1;
    return g_array_pool[arr_handle].elems[idx];
}

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

    /* Leading dot identifier: `.field` is lexed as a single TOK_IDENT
     * with a leading dot.  Used for struct field access in gate(). */
    if (c == '.' && lex->pos + 1 < lex->len && zp_isalpha(lex->src[lex->pos + 1])) {
        int i = 0;
        char d = lexer_next(lex);  /* '.' */
        if (i < ZP_MAX_NAME - 1) tok->text[i++] = d;
        while (lex->pos < lex->len && zp_isident(lexer_peek(lex))) {
            d = lexer_next(lex);
            if (i < ZP_MAX_NAME - 1) tok->text[i++] = d;
        }
        tok->text[i] = '\0';
        tok->type = TOK_IDENT;
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

    /* String literal — supports \n \t \r \\ \" \0 \xNN escapes. */
    if (c == '"') {
        lexer_next(lex);  /* skip opening quote */
        int i = 0;
        while (lex->pos < lex->len && lexer_peek(lex) != '"') {
            char d = lexer_next(lex);
            if (d == '\\' && lex->pos < lex->len) {
                char e = lexer_next(lex);
                switch (e) {
                case 'n':  d = '\n'; break;
                case 't':  d = '\t'; break;
                case 'r':  d = '\r'; break;
                case '\\': d = '\\'; break;
                case '"':  d = '"';  break;
                case '0':  d = '\0'; break;
                case 'x': {
                    int hi = 0, lo = 0;
                    if (lex->pos < lex->len) {
                        char ch = lexer_next(lex);
                        if (ch >= '0' && ch <= '9') hi = ch - '0';
                        else if (ch >= 'a' && ch <= 'f') hi = 10 + (ch - 'a');
                        else if (ch >= 'A' && ch <= 'F') hi = 10 + (ch - 'A');
                    }
                    if (lex->pos < lex->len) {
                        char ch = lexer_next(lex);
                        if (ch >= '0' && ch <= '9') lo = ch - '0';
                        else if (ch >= 'a' && ch <= 'f') lo = 10 + (ch - 'a');
                        else if (ch >= 'A' && ch <= 'F') lo = 10 + (ch - 'A');
                    }
                    d = (char)((hi << 4) | lo);
                    break;
                }
                default: d = e; break;
                }
            }
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
    prog->nodes[idx].out_kind = ZP_VAL_INT;
    prog->nodes[idx].struct_type = -1;
    prog->nodes[idx].emit_handle = -1;
    prog->nodes[idx].fmt2[0] = '\0';
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
    if (zp_streq(t, "compute.run")) { *out = ZP_COMPUTE_RUN; return 1; }
    if (zp_streq(t, "str.len"))         { *out = ZP_STR_LEN;         return 1; }
    if (zp_streq(t, "str.find"))        { *out = ZP_STR_FIND;        return 1; }
    if (zp_streq(t, "str.split"))       { *out = ZP_STR_SPLIT;       return 1; }
    if (zp_streq(t, "str.replace"))     { *out = ZP_STR_REPLACE;     return 1; }
    if (zp_streq(t, "str.concat"))      { *out = ZP_STR_CONCAT;      return 1; }
    if (zp_streq(t, "str.upper"))       { *out = ZP_STR_UPPER;       return 1; }
    if (zp_streq(t, "str.lower"))       { *out = ZP_STR_LOWER;       return 1; }
    if (zp_streq(t, "str.trim"))        { *out = ZP_STR_TRIM;        return 1; }
    if (zp_streq(t, "str.starts_with")) { *out = ZP_STR_STARTS_WITH; return 1; }
    if (zp_streq(t, "str.ends_with"))   { *out = ZP_STR_ENDS_WITH;   return 1; }
    if (zp_streq(t, "str.from_int"))    { *out = ZP_STR_FROM_INT;    return 1; }
    if (zp_streq(t, "str.to_int"))      { *out = ZP_STR_TO_INT;      return 1; }
    if (zp_streq(t, "str.len_of_array")){ *out = ZP_STR_LEN_OF_ARRAY;return 1; }
    if (zp_streq(t, "sum"))             { *out = ZP_SUM;             return 1; }
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
 *
 * Pass-1 extension:
 *   gate(.field == "literal")  — struct field equality test.
 *
 * In the field-equality form, *out_val is unused; field name is written
 * into *field_buf (NULL ok, ignored), literal into *lit_buf (NULL ok),
 * and the returned type is ZP_GATE_FIELD_EQ.
 *
 * Returns the gate type, stores threshold in *out_val.
 */
static enum zp_node_type parse_gate_expr_full(struct zp_lexer *lex,
                                              int32_t *out_val,
                                              char *field_buf,
                                              char *lit_buf)
{
    struct zp_token t;
    lexer_token(lex, &t);  /* ( */
    lexer_token(lex, &t);  /* may be MINUS('.' was eaten as TOK_MINUS? no — '.' is part of TOK_IDENT) */

    /* Check for field-eq form: a leading '.' starts the field name. The
     * lexer treats '.' as part of an identifier. So the field comes through
     * as TOK_IDENT with text starting with '.'. */
    if (t.type == TOK_IDENT && t.text[0] == '.') {
        if (field_buf) {
            int j = 0;
            for (int i = 1; t.text[i] && j < ZP_MAX_NAME - 1; i++)
                field_buf[j++] = t.text[i];
            field_buf[j] = '\0';
        }
        lexer_token(lex, &t);  /* expect == */
        lexer_token(lex, &t);  /* literal */
        if (t.type == TOK_STRING && lit_buf)
            zp_strcpy(lit_buf, t.text, ZP_MAX_STRING);
        else if (t.type == TOK_NUMBER) {
            *out_val = t.num_val;
        }
        lexer_token(lex, &t);  /* ) */
        return ZP_GATE_FIELD_EQ;
    }

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

static enum zp_node_type parse_gate_expr(struct zp_lexer *lex, int32_t *out_val) __attribute__((unused));
static enum zp_node_type parse_gate_expr(struct zp_lexer *lex, int32_t *out_val)
{
    return parse_gate_expr_full(lex, out_val, 0, 0);
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

    /* ── Struct definition: struct name { field: type ... } ── */
    if (zp_streq(tok.text, "struct")) {
        struct zp_token t;
        lexer_token(lex, &t);
        if (t.type != TOK_IDENT) return -1;

        struct zp_struct_type def;
        for (int i = 0; i < ZP_MAX_NAME; i++) def.name[i] = 0;
        zp_strcpy(def.name, t.text, ZP_MAX_NAME);
        def.field_count = 0;
        def.in_use = 1;

        lexer_token(lex, &t); /* { */
        if (t.type != TOK_LBRACE) return -1;

        /* Read field decls until }. Field syntax:  field_name : type
         * type is one of: int, str, or another struct name (treated as struct). */
        lexer_token(lex, &t);
        while (t.type != TOK_RBRACE && t.type != TOK_EOF) {
            if (t.type == TOK_NEWLINE || t.type == TOK_COMMA) {
                lexer_token(lex, &t);
                continue;
            }
            if (t.type != TOK_IDENT) {
                lexer_token(lex, &t);
                continue;
            }
            if (def.field_count >= ZP_MAX_STRUCT_FIELDS) {
                /* Skip to } */
                while (t.type != TOK_RBRACE && t.type != TOK_EOF)
                    lexer_token(lex, &t);
                break;
            }
            struct zp_struct_field *f = &def.fields[def.field_count];
            zp_strcpy(f->name, t.text, ZP_MAX_NAME);
            f->offset = def.field_count;
            f->kind = ZP_VAL_INT;
            lexer_token(lex, &t); /* expect : */
            if (t.type == TOK_COLON) {
                lexer_token(lex, &t); /* type */
                if (t.type == TOK_IDENT) {
                    if (zp_streq(t.text, "int")) f->kind = ZP_VAL_INT;
                    else if (zp_streq(t.text, "str")) f->kind = ZP_VAL_STR;
                    else f->kind = ZP_VAL_STRUCT;
                }
            }
            def.field_count++;
            lexer_token(lex, &t);
        }

        zp_struct_register(&def);
        return 0;
    }

    /* ── Chain definition: chain name { step1 -> step2 -> ... } ──
     *
     * Each step is one of:
     *   name                       — bare reference to an existing node
     *   name : emit(N)             — declare a source node inline
     *   name : verb                — declare a verb-only node inline
     *   name : verb(args)          — declare a verb with args inline
     *   name : input -> verb(args) — declare a transform inline
     *   verb                       — bare verb (auto-named)
     *   verb(args)                 — bare verb with args (auto-named)
     *
     * Steps are separated by `->`. The body may span multiple lines.
     */
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
        for (int i = 0; i < ZP_MAX_CHAIN_NODES; i++)
            def->have_decl[i] = 0;

        lexer_token(lex, &t);  /* { */
        if (t.type != TOK_LBRACE)
            return -1;

        /* Skip leading newlines after { */
        lexer_token(lex, &t);
        while (t.type == TOK_NEWLINE)
            lexer_token(lex, &t);

        int auto_seq = 0;

        while (t.type != TOK_RBRACE && t.type != TOK_EOF) {
            if (t.type != TOK_IDENT) {
                /* Step boundary token (->, newline) — advance and retry. */
                lexer_token(lex, &t);
                continue;
            }
            if (def->node_count >= ZP_MAX_CHAIN_NODES) {
                /* Skip remaining body. */
                while (t.type != TOK_RBRACE && t.type != TOK_EOF)
                    lexer_token(lex, &t);
                break;
            }

            int slot = def->node_count;
            char first[ZP_MAX_NAME];
            zp_strcpy(first, t.text, ZP_MAX_NAME);

            /* Look ahead one token to disambiguate. */
            struct zp_token la;
            lexer_token(lex, &la);

            if (la.type == TOK_COLON) {
                /* name : ...  — full inline declaration. */
                zp_strcpy(def->node_names[slot], first, ZP_MAX_NAME);

                struct zp_node_decl *d = &def->decls[slot];
                d->name[0] = '\0';
                zp_strcpy(d->name, first, ZP_MAX_NAME);
                d->int_val = 0; d->int_val2 = 0; d->int_val3 = 0;
                d->fmt[0] = '\0';
                d->sig_idx = -1;
                d->chain_bind_id = -1;
                d->type = ZP_PASSTHROUGH;

                /* RHS: either `emit(N)`, `verb`, `verb(args)`, or
                 * `input -> verb...` — we walk until we hit `->`
                 * (the next chain step) or `}`. */
                struct zp_token r;
                lexer_token(lex, &r);
                int captured = 0;

                while (r.type != TOK_ARROW && r.type != TOK_RBRACE &&
                       r.type != TOK_NEWLINE && r.type != TOK_EOF) {
                    if (r.type == TOK_IDENT) {
                        if (zp_streq(r.text, "input")) {
                            /* `input -> ...` form: skip arrow and read
                             * the operator/verb that follows. */
                            struct zp_token op;
                            lexer_token(lex, &op);  /* expect -> */
                            if (op.type != TOK_ARROW) { r = op; continue; }
                            lexer_token(lex, &op);  /* operator/verb */
                            if (op.type == TOK_STAR) {
                                lexer_token(lex, &op);
                                d->type = ZP_MULTIPLY;
                                d->int_val = op.num_val;
                                captured = 1;
                            } else if (op.type == TOK_PLUS) {
                                lexer_token(lex, &op);
                                d->type = ZP_ADD;
                                d->int_val = op.num_val;
                                captured = 1;
                            } else if (op.type == TOK_MINUS) {
                                lexer_token(lex, &op);
                                d->type = ZP_SUBTRACT;
                                d->int_val = op.num_val;
                                captured = 1;
                            } else if (op.type == TOK_IDENT &&
                                       zp_streq(op.text, "gate")) {
                                int32_t th = 0;
                                enum zp_node_type gt = parse_gate_expr_full(
                                    lex, &th, d->fmt2, d->fmt);
                                d->type = gt;
                                d->int_val = th;
                                captured = 1;
                            } else if (op.type == TOK_IDENT &&
                                       zp_streq(op.text, "delta")) {
                                d->type = ZP_DELTA;
                                captured = 1;
                            } else if (op.type == TOK_IDENT &&
                                       zp_streq(op.text, "knee")) {
                                int32_t lo, hi;
                                parse_knee_args(lex, &lo, &hi);
                                d->type = ZP_KNEE;
                                d->int_val = lo;
                                d->int_val2 = hi;
                                captured = 1;
                            } else if (op.type == TOK_IDENT &&
                                       zp_streq(op.text, "sustained")) {
                                enum zp_node_type gt;
                                int32_t thresh, count;
                                parse_sustained_args(lex, &gt, &thresh, &count);
                                d->type = ZP_SUSTAINED;
                                d->int_val = thresh;
                                d->int_val2 = count;
                                d->int_val3 = (int32_t)gt;
                                captured = 1;
                            } else if (op.type == TOK_IDENT) {
                                enum zp_node_type vt;
                                if (verb_to_type(op.text, &vt)) {
                                    d->type = vt;
                                    captured = 1;
                                    /* consume verb args if present */
                                    if (lexer_peek(lex) == '(') {
                                        struct zp_token p;
                                        lexer_token(lex, &p); /* ( */
                                        /* String-arg verbs: capture first
                                         * string literal as fmt, second as fmt2. */
                                        int got_first = 0;
                                        while (p.type != TOK_RPAREN &&
                                               p.type != TOK_NEWLINE &&
                                               p.type != TOK_EOF &&
                                               p.type != TOK_RBRACE) {
                                            lexer_token(lex, &p);
                                            if (p.type == TOK_STRING) {
                                                if (!got_first) {
                                                    zp_strcpy(d->fmt, p.text, ZP_MAX_STRING);
                                                    got_first = 1;
                                                } else {
                                                    zp_strcpy(d->fmt2, p.text, ZP_MAX_STRING);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            /* fall through to outer loop to continue reading
                             * up to the step boundary `->` (which will end
                             * THIS step in the outer chain) */
                            lexer_token(lex, &r);
                            continue;
                        }

                        /* Bare verb / `emit(N | "str" | structname{...})` form. */
                        if (zp_streq(r.text, "emit")) {
                            struct zp_token p;
                            lexer_token(lex, &p);  /* ( */
                            lexer_token(lex, &p);  /* number | string | typename */
                            d->type = ZP_EMIT;
                            captured = 1;
                            if (p.type == TOK_NUMBER) {
                                d->int_val = p.num_val;
                                lexer_token(lex, &p);  /* ) */
                            } else if (p.type == TOK_STRING) {
                                int h = zp_str_intern(p.text, -1);
                                d->out_kind = ZP_VAL_STR;
                                d->emit_handle = h;
                                lexer_token(lex, &p);  /* ) */
                            } else if (p.type == TOK_IDENT) {
                                int stype = zp_struct_find(p.text);
                                lexer_token(lex, &p);  /* { */
                                if (stype >= 0 && p.type == TOK_LBRACE) {
                                    int sh = zp_struct_alloc(stype);
                                    struct zp_token f;
                                    lexer_token(lex, &f);
                                    while (f.type != TOK_RBRACE && f.type != TOK_EOF) {
                                        if (f.type == TOK_IDENT) {
                                            char fname[ZP_MAX_NAME];
                                            zp_strcpy(fname, f.text, ZP_MAX_NAME);
                                            lexer_token(lex, &f);
                                            if (f.type == TOK_NUMBER) {
                                                zp_struct_set_int(sh, fname, f.num_val);
                                            } else if (f.type == TOK_STRING) {
                                                int sv = zp_str_intern(f.text, -1);
                                                zp_struct_set_str(sh, fname, sv);
                                                zp_str_release(sv);
                                            }
                                        }
                                        lexer_token(lex, &f);
                                        if (f.type == TOK_COMMA) lexer_token(lex, &f);
                                    }
                                    d->out_kind = ZP_VAL_STRUCT;
                                    d->struct_type = stype;
                                    d->emit_handle = sh;
                                    lexer_token(lex, &p);  /* ) */
                                } else {
                                    while (p.type != TOK_RPAREN && p.type != TOK_NEWLINE &&
                                           p.type != TOK_EOF) lexer_token(lex, &p);
                                }
                            } else {
                                while (p.type != TOK_RPAREN && p.type != TOK_NEWLINE &&
                                       p.type != TOK_EOF) lexer_token(lex, &p);
                            }
                        } else if (zp_streq(r.text, "delta")) {
                            d->type = ZP_DELTA;
                            captured = 1;
                        } else if (zp_streq(r.text, "knee")) {
                            int32_t lo, hi;
                            parse_knee_args(lex, &lo, &hi);
                            d->type = ZP_KNEE;
                            d->int_val = lo;
                            d->int_val2 = hi;
                            captured = 1;
                        } else if (zp_streq(r.text, "sustained")) {
                            enum zp_node_type gt;
                            int32_t thresh, count;
                            parse_sustained_args(lex, &gt, &thresh, &count);
                            d->type = ZP_SUSTAINED;
                            d->int_val = thresh;
                            d->int_val2 = count;
                            d->int_val3 = (int32_t)gt;
                            captured = 1;
                        } else if (zp_streq(r.text, "gate")) {
                            int32_t th = 0;
                            enum zp_node_type gt = parse_gate_expr_full(
                                lex, &th, d->fmt2, d->fmt);
                            d->type = gt;
                            d->int_val = th;
                            captured = 1;
                        } else {
                            enum zp_node_type vt;
                            if (verb_to_type(r.text, &vt)) {
                                d->type = vt;
                                captured = 1;
                                if (lexer_peek(lex) == '(') {
                                    struct zp_token p;
                                    lexer_token(lex, &p); /* ( */
                                    int got_first = 0;
                                    while (p.type != TOK_RPAREN &&
                                           p.type != TOK_NEWLINE &&
                                           p.type != TOK_EOF &&
                                           p.type != TOK_RBRACE) {
                                        lexer_token(lex, &p);
                                        if (p.type == TOK_STRING) {
                                            if (!got_first) {
                                                zp_strcpy(d->fmt, p.text, ZP_MAX_STRING);
                                                got_first = 1;
                                            } else {
                                                zp_strcpy(d->fmt2, p.text, ZP_MAX_STRING);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    lexer_token(lex, &r);
                }

                def->have_decl[slot] = captured ? 1 : 0;
                def->node_count++;
                t = r;  /* continue with ARROW / RBRACE / NEWLINE */
                continue;
            }

            /* No colon — it's a bare reference or a bare verb-only node. */
            enum zp_node_type vt;
            if (verb_to_type(first, &vt)) {
                /* Auto-name: verbN where N is the slot index. */
                char autoname[ZP_MAX_NAME];
                int p = 0;
                const char *s = first;
                /* Strip leading dot-prefix-friendly chars; replace '.' with '_'
                 * so the chain node name is registry-friendly. */
                while (*s && p < ZP_MAX_NAME - 4) {
                    autoname[p++] = (*s == '.') ? '_' : *s;
                    s++;
                }
                /* Append a digit so multiple verbs of the same kind don't
                 * collide. */
                if (p < ZP_MAX_NAME - 1) {
                    int n = auto_seq++;
                    if (n >= 10) {
                        if (p < ZP_MAX_NAME - 2) {
                            autoname[p++] = '0' + (n / 10);
                            autoname[p++] = '0' + (n % 10);
                        }
                    } else {
                        autoname[p++] = '0' + n;
                    }
                }
                autoname[p] = '\0';

                zp_strcpy(def->node_names[slot], autoname, ZP_MAX_NAME);
                struct zp_node_decl *d = &def->decls[slot];
                zp_strcpy(d->name, autoname, ZP_MAX_NAME);
                d->int_val = 0; d->int_val2 = 0; d->int_val3 = 0;
                d->fmt[0] = '\0';
                d->sig_idx = -1;
                d->chain_bind_id = -1;
                d->type = vt;

                /* Consume verb args if present. */
                if (la.type == TOK_LPAREN) {
                    struct zp_token p2;
                    do { lexer_token(lex, &p2); }
                    while (p2.type != TOK_RPAREN &&
                           p2.type != TOK_NEWLINE &&
                           p2.type != TOK_EOF &&
                           p2.type != TOK_RBRACE);
                    lexer_token(lex, &t);  /* next: -> or } */
                } else {
                    t = la;
                }
                def->have_decl[slot] = 1;
                def->node_count++;
                continue;
            }

            /* Plain identifier reference (no inline decl). */
            zp_strcpy(def->node_names[slot], first, ZP_MAX_NAME);
            def->have_decl[slot] = 0;
            def->node_count++;
            t = la;
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
            /* emit(N | "str" | structname { ... }) */
            lexer_token(lex, &t);  /* ( */
            lexer_token(lex, &t);  /* number | string | ident(struct name) */
            if (t.type == TOK_NUMBER) {
                int32_t val = t.num_val;
                add_node(prog, first_name, ZP_EMIT, val, 0);
                lexer_token(lex, &t);  /* ) */
            } else if (t.type == TOK_STRING) {
                int h = zp_str_intern(t.text, -1);
                add_node(prog, first_name, ZP_EMIT, 0, 0);
                int idx = prog->node_count - 1;
                prog->nodes[idx].out_kind    = ZP_VAL_STR;
                prog->nodes[idx].emit_handle = h;
                lexer_token(lex, &t);  /* ) */
            } else if (t.type == TOK_IDENT) {
                /* struct literal: typename { field = expr, ... } */
                int stype = zp_struct_find(t.text);
                lexer_token(lex, &t); /* { */
                if (stype >= 0 && t.type == TOK_LBRACE) {
                    int sh = zp_struct_alloc(stype);
                    /* parse field bindings */
                    struct zp_token f;
                    lexer_token(lex, &f);
                    while (f.type != TOK_RBRACE && f.type != TOK_EOF) {
                        if (f.type == TOK_IDENT) {
                            char fname[ZP_MAX_NAME];
                            zp_strcpy(fname, f.text, ZP_MAX_NAME);
                            /* skip '=' (lexer doesn't tokenize bare '=') */
                            lexer_token(lex, &f);
                            if (f.type == TOK_NUMBER) {
                                zp_struct_set_int(sh, fname, f.num_val);
                            } else if (f.type == TOK_STRING) {
                                int sv = zp_str_intern(f.text, -1);
                                zp_struct_set_str(sh, fname, sv);
                                zp_str_release(sv);
                            }
                        }
                        lexer_token(lex, &f);
                        if (f.type == TOK_COMMA) lexer_token(lex, &f);
                    }
                    add_node(prog, first_name, ZP_EMIT, 0, 0);
                    int idx = prog->node_count - 1;
                    prog->nodes[idx].out_kind    = ZP_VAL_STRUCT;
                    prog->nodes[idx].struct_type = stype;
                    prog->nodes[idx].emit_handle = sh;
                    /* consume closing ) */
                    lexer_token(lex, &t);
                } else {
                    /* Bail: skip to ) */
                    while (t.type != TOK_RPAREN && t.type != TOK_NEWLINE &&
                           t.type != TOK_EOF) lexer_token(lex, &t);
                }
            } else {
                /* Unknown form: skip to ) */
                while (t.type != TOK_RPAREN && t.type != TOK_NEWLINE &&
                       t.type != TOK_EOF) lexer_token(lex, &t);
            }

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
                /* gate(> N) | gate(.field == "lit") -> output */
                int32_t threshold = 0;
                char field_buf[ZP_MAX_NAME] = {0};
                char lit_buf[ZP_MAX_STRING] = {0};
                enum zp_node_type gate_type = parse_gate_expr_full(
                    lex, &threshold, field_buf, lit_buf);
                add_node(prog, first_name, gate_type, threshold, 0);
                if (gate_type == ZP_GATE_FIELD_EQ) {
                    int idx = prog->node_count - 1;
                    zp_strcpy(prog->nodes[idx].fmt2, field_buf, ZP_MAX_STRING);
                    zp_strcpy(prog->nodes[idx].fmt,  lit_buf,   ZP_MAX_STRING);
                }
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
                        } else if (vt == ZP_COMPUTE_RUN) {
                            /* compute.run(target_node_ident [, prefer_gpu = true])
                             * Record the target name in fmt; resolved to
                             * a sig_idx during compile. prefer_gpu is
                             * stashed in int_val (0|1) and threaded into
                             * the mde_compute_request_t at submit time. */
                            lexer_token(lex, &p);
                            if (p.type == TOK_IDENT)
                                zp_strcpy(prog->nodes[prog->node_count - 1].fmt,
                                          p.text, ZP_MAX_STRING);
                            /* Scan remaining tokens for prefer_gpu = true. */
                            int seen_prefer_gpu = 0;
                            while (p.type != TOK_RPAREN && p.type != TOK_NEWLINE
                                   && p.type != TOK_EOF) {
                                lexer_token(lex, &p);
                                if (p.type == TOK_IDENT &&
                                    zp_streq(p.text, "prefer_gpu")) {
                                    seen_prefer_gpu = 1;
                                }
                                if (seen_prefer_gpu && p.type == TOK_IDENT &&
                                    zp_streq(p.text, "true")) {
                                    prog->nodes[prog->node_count - 1].int_val = 1;
                                }
                            }
                        } else if (vt == ZP_STR_SPLIT || vt == ZP_STR_FIND ||
                                   vt == ZP_STR_REPLACE || vt == ZP_STR_CONCAT ||
                                   vt == ZP_STR_STARTS_WITH || vt == ZP_STR_ENDS_WITH) {
                            /* First string arg into fmt[]. Optional second
                             * string arg into fmt2[] (for replace). */
                            lexer_token(lex, &p);
                            if (p.type == TOK_STRING)
                                zp_strcpy(prog->nodes[prog->node_count - 1].fmt,
                                          p.text, ZP_MAX_STRING);
                            lexer_token(lex, &p); /* , or ) */
                            if (p.type == TOK_COMMA) {
                                lexer_token(lex, &p);
                                if (p.type == TOK_STRING)
                                    zp_strcpy(prog->nodes[prog->node_count - 1].fmt2,
                                              p.text, ZP_MAX_STRING);
                            }
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

/* compute.run(target) — submit a synchronous compute_request through
 * CHAIN_MDE. The kernel_fn handed to MDE invokes the target Z+ node's
 * resolve, threading the input value through and capturing its output.
 * This completes the verb set so Z+ can dispatch arbitrary compute the
 * same way it dispatches audio/net/fs/vault. */
static int32_t zp_compute_target_chain[ZP_MAX_NODES]; /* target sig chain id */
static int32_t zp_compute_target_idx[ZP_MAX_NODES];   /* target sig node idx */
static int32_t zp_compute_input_val[ZP_MAX_NODES];    /* threaded value */
static int32_t zp_compute_output_val[ZP_MAX_NODES];   /* result of node */
static int32_t zp_compute_prefer_gpu[ZP_MAX_NODES];   /* 1 if compute.run(..., prefer_gpu=true) */

typedef struct {
    int      chain_id;
    int      target_idx;
    int32_t  input_val;
    int32_t  output_val;
} zp_compute_kfn_args_t;

static int zp_compute_kfn(void *args_ptr)
{
    zp_compute_kfn_args_t *a = (zp_compute_kfn_args_t *)args_ptr;
    if (!a) return -1;
    struct sig_chain *sc = sig_get_chain(a->chain_id);
    if (!sc) return -1;
    if (a->target_idx < 0 || a->target_idx >= sc->node_count) return -1;
    struct sig_node *target = &sc->nodes[a->target_idx];
    if (!target->process) return -1;

    /* Stage input/output buffers on the target node and run its
     * resolve directly. We don't propagate along edges -- compute.run
     * is a one-shot invocation, the result is read back here. */
    sig_data_write_i32(&target->input, a->input_val);
    target->output.size = 0;
    int rc = target->process(target, &target->input, &target->output);
    a->output_val = sig_data_read_i32(&target->output);
    return rc;
}

static int zp_proc_compute_run(struct sig_node *node, struct sig_data *in,
                                struct sig_data *out)
{
    int idx = node->id;
    if (idx < 0 || idx >= ZP_MAX_NODES) {
        sig_data_write_i32(out, 0);
        return 0;
    }
    int32_t v = sig_data_read_i32(in);
    zp_compute_input_val[idx] = v;
    zp_compute_output_val[idx] = 0;

    int target_chain = zp_compute_target_chain[idx];
    int target_idx   = zp_compute_target_idx[idx];
    if (target_chain < 0 || target_idx < 0) {
        /* Target unresolved (parser/compile gap). Pass-through. */
        sig_data_write_i32(out, v);
        return 0;
    }

    zp_compute_kfn_args_t args = {
        .chain_id   = target_chain,
        .target_idx = target_idx,
        .input_val  = v,
        .output_val = 0,
    };
    mde_compute_request_t req = {
        .kernel_fn    = zp_compute_kfn,
        .args         = &args,
        .rc           = 0,
        .elapsed_tsc  = 0,
        .prefer_gpu   = (idx >= 0 && idx < ZP_MAX_NODES)
                            ? zp_compute_prefer_gpu[idx] : 0,
        .backend_used = 0,
        .policy_override = -1,
        .affinity_backend_idx = -1,
    };
    int submit_rc = mde_chain_submit(&req);
    zp_compute_output_val[idx] = args.output_val;
    /* On submit failure, return 0 so the chain still flows. The
     * CHAIN_MDE vault_version + B3 capture the failure. */
    if (submit_rc < 0)
        sig_data_write_i32(out, 0);
    else
        sig_data_write_i32(out, args.output_val);
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

/* ── Pass 1: per-node side state for string/struct verbs ────────── */

static int32_t zp_emit_handle[ZP_MAX_NODES];   /* string/struct emit handle */
static int32_t zp_emit_kind[ZP_MAX_NODES];     /* ZP_VAL_* for the emit */
static int32_t zp_split_sep[ZP_MAX_NODES];     /* str.split sep handle */
static int32_t zp_field_lit[ZP_MAX_NODES];     /* gate(.f=="lit") lit handle */
static int32_t zp_struct_type_idx[ZP_MAX_NODES];
static int32_t zp_sum_acc[ZP_MAX_NODES];

/* Pass-1 emit override: when out_kind != ZP_VAL_INT, output the pool
 * handle stored at zp_emit_handle[id]. */
static int zp_proc_emit_pass1(struct sig_node *node, struct sig_data *in,
                              struct sig_data *out)
{
    (void)in;
    int idx = node->id;
    int32_t v = (idx >= 0 && idx < ZP_MAX_NODES) ? zp_emit_handle[idx] : 0;
    sig_data_write_i32(out, v);
    return 0;
}

/* str.len(s) — input handle -> int length */
static int zp_proc_str_len(struct sig_node *node, struct sig_data *in,
                            struct sig_data *out)
{
    (void)node;
    int h = sig_data_read_i32(in);
    int len = 0;
    const char *s = zp_str_get(h, &len);
    sig_data_write_i32(out, s ? len : 0);
    return 0;
}

/* str.upper / str.lower / str.trim / str.from_int / str.to_int */
static int zp_proc_str_upper(struct sig_node *node, struct sig_data *in,
                              struct sig_data *out)
{
    (void)node;
    int h = sig_data_read_i32(in);
    int len = 0;
    const char *s = zp_str_get(h, &len);
    if (!s) { sig_data_write_i32(out, -1); return 0; }
    char *buf = (char *)kmalloc((uint64_t)(len + 1));
    if (!buf) { sig_data_write_i32(out, -1); return 0; }
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[i] = c;
    }
    buf[len] = 0;
    int nh = zp_str_intern(buf, len);
    kfree(buf);
    sig_data_write_i32(out, nh);
    return 0;
}

static int zp_proc_str_lower(struct sig_node *node, struct sig_data *in,
                              struct sig_data *out)
{
    (void)node;
    int h = sig_data_read_i32(in);
    int len = 0;
    const char *s = zp_str_get(h, &len);
    if (!s) { sig_data_write_i32(out, -1); return 0; }
    char *buf = (char *)kmalloc((uint64_t)(len + 1));
    if (!buf) { sig_data_write_i32(out, -1); return 0; }
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        buf[i] = c;
    }
    buf[len] = 0;
    int nh = zp_str_intern(buf, len);
    kfree(buf);
    sig_data_write_i32(out, nh);
    return 0;
}

static int zp_proc_str_trim(struct sig_node *node, struct sig_data *in,
                             struct sig_data *out)
{
    (void)node;
    int h = sig_data_read_i32(in);
    int len = 0;
    const char *s = zp_str_get(h, &len);
    if (!s) { sig_data_write_i32(out, -1); return 0; }
    int a = 0, b = len;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\n' || s[b-1] == '\r')) b--;
    int nh = zp_str_intern(s + a, b - a);
    sig_data_write_i32(out, nh);
    return 0;
}

static int zp_proc_str_from_int(struct sig_node *node, struct sig_data *in,
                                 struct sig_data *out)
{
    (void)node;
    int32_t v = sig_data_read_i32(in);
    char buf[16];
    int neg = 0;
    int32_t u = v;
    if (v < 0) { neg = 1; u = -v; }
    int n = 0;
    if (u == 0) buf[n++] = '0';
    while (u > 0 && n < 15) { buf[n++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg && n < 15) buf[n++] = '-';
    /* reverse */
    char rev[16];
    for (int i = 0; i < n; i++) rev[i] = buf[n - 1 - i];
    rev[n] = 0;
    int nh = zp_str_intern(rev, n);
    sig_data_write_i32(out, nh);
    return 0;
}

static int zp_proc_str_to_int(struct sig_node *node, struct sig_data *in,
                               struct sig_data *out)
{
    (void)node;
    int h = sig_data_read_i32(in);
    int len = 0;
    const char *s = zp_str_get(h, &len);
    int32_t v = 0;
    int neg = 0, i = 0;
    if (s && len > 0) {
        if (s[0] == '-') { neg = 1; i = 1; }
        for (; i < len; i++) {
            if (s[i] < '0' || s[i] > '9') break;
            v = v * 10 + (s[i] - '0');
        }
        if (neg) v = -v;
    }
    sig_data_write_i32(out, v);
    return 0;
}

/* str.split(sep) — input string handle -> array handle.
 * Separator is stored in zp_split_sep[id] as a string handle. */
static int zp_proc_str_split(struct sig_node *node, struct sig_data *in,
                              struct sig_data *out)
{
    int idx = node->id;
    int h = sig_data_read_i32(in);
    int slen = 0;
    const char *s = zp_str_get(h, &slen);
    if (!s) { sig_data_write_i32(out, -1); return 0; }
    int sep_h = (idx >= 0 && idx < ZP_MAX_NODES) ? zp_split_sep[idx] : -1;
    int seplen = 0;
    const char *sep = zp_str_get(sep_h, &seplen);
    if (!sep || seplen == 0) { sig_data_write_i32(out, -1); return 0; }

    int arr = zp_array_alloc();
    if (arr < 0) { sig_data_write_i32(out, -1); return 0; }

    int start = 0;
    for (int i = 0; i + seplen <= slen; ) {
        int match = 1;
        for (int j = 0; j < seplen; j++) {
            if (s[i + j] != sep[j]) { match = 0; break; }
        }
        if (match) {
            int sh = zp_str_intern(s + start, i - start);
            zp_array_push_str(arr, sh);
            zp_str_release(sh);
            i += seplen;
            start = i;
        } else {
            i++;
        }
    }
    /* tail */
    int sh = zp_str_intern(s + start, slen - start);
    zp_array_push_str(arr, sh);
    zp_str_release(sh);
    sig_data_write_i32(out, arr);
    return 0;
}

/* str.len_of_array — input array handle -> int count of strings */
static int zp_proc_str_len_of_array(struct sig_node *node, struct sig_data *in,
                                     struct sig_data *out)
{
    (void)node;
    int h = sig_data_read_i32(in);
    sig_data_write_i32(out, zp_array_len(h));
    return 0;
}

/* str.find(haystack, needle) — needle stored in fmt at compile time -> handle */
static int zp_proc_str_find(struct sig_node *node, struct sig_data *in,
                             struct sig_data *out)
{
    int idx = node->id;
    int h = sig_data_read_i32(in);
    int hlen = 0;
    const char *hay = zp_str_get(h, &hlen);
    int neeh = (idx >= 0 && idx < ZP_MAX_NODES) ? zp_split_sep[idx] : -1;
    int nlen = 0;
    const char *nee = zp_str_get(neeh, &nlen);
    if (!hay || !nee || nlen == 0) { sig_data_write_i32(out, -1); return 0; }
    for (int i = 0; i + nlen <= hlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++)
            if (hay[i + j] != nee[j]) { match = 0; break; }
        if (match) { sig_data_write_i32(out, i); return 0; }
    }
    sig_data_write_i32(out, -1);
    return 0;
}

/* str.concat(a, b) — two-arg: input is `a` handle, b is in fmt at compile */
static int zp_proc_str_concat(struct sig_node *node, struct sig_data *in,
                               struct sig_data *out)
{
    int idx = node->id;
    int ah = sig_data_read_i32(in);
    int bh = (idx >= 0 && idx < ZP_MAX_NODES) ? zp_split_sep[idx] : -1;
    int alen = 0, blen = 0;
    const char *a = zp_str_get(ah, &alen);
    const char *b = zp_str_get(bh, &blen);
    if (!a) { a = ""; alen = 0; }
    if (!b) { b = ""; blen = 0; }
    char *buf = (char *)kmalloc((uint64_t)(alen + blen + 1));
    if (!buf) { sig_data_write_i32(out, -1); return 0; }
    for (int i = 0; i < alen; i++) buf[i] = a[i];
    for (int i = 0; i < blen; i++) buf[alen + i] = b[i];
    buf[alen + blen] = 0;
    int nh = zp_str_intern(buf, alen + blen);
    kfree(buf);
    sig_data_write_i32(out, nh);
    return 0;
}

/* str.starts_with / str.ends_with — prefix/suffix in fmt -> 0/1 */
static int zp_proc_str_starts_with(struct sig_node *node, struct sig_data *in,
                                    struct sig_data *out)
{
    int idx = node->id;
    int h = sig_data_read_i32(in);
    int slen = 0;
    const char *s = zp_str_get(h, &slen);
    int ph = (idx >= 0 && idx < ZP_MAX_NODES) ? zp_split_sep[idx] : -1;
    int plen = 0;
    const char *p = zp_str_get(ph, &plen);
    if (!s || !p || slen < plen) { sig_data_write_i32(out, 0); return 0; }
    for (int i = 0; i < plen; i++) if (s[i] != p[i]) { sig_data_write_i32(out, 0); return 0; }
    sig_data_write_i32(out, 1);
    return 0;
}

static int zp_proc_str_ends_with(struct sig_node *node, struct sig_data *in,
                                  struct sig_data *out)
{
    int idx = node->id;
    int h = sig_data_read_i32(in);
    int slen = 0;
    const char *s = zp_str_get(h, &slen);
    int ph = (idx >= 0 && idx < ZP_MAX_NODES) ? zp_split_sep[idx] : -1;
    int plen = 0;
    const char *p = zp_str_get(ph, &plen);
    if (!s || !p || slen < plen) { sig_data_write_i32(out, 0); return 0; }
    int off = slen - plen;
    for (int i = 0; i < plen; i++) if (s[off + i] != p[i]) { sig_data_write_i32(out, 0); return 0; }
    sig_data_write_i32(out, 1);
    return 0;
}

/* str.replace(old, new) — old in fmt, new in fmt2; both as string handles
 * stored at compile time (zp_split_sep + a second slot). For brevity we
 * stash new-handle in zp_field_lit (free on this verb). */
static int zp_proc_str_replace(struct sig_node *node, struct sig_data *in,
                                struct sig_data *out)
{
    int idx = node->id;
    int sh = sig_data_read_i32(in);
    int slen = 0;
    const char *s = zp_str_get(sh, &slen);
    int oh = (idx >= 0 && idx < ZP_MAX_NODES) ? zp_split_sep[idx] : -1;
    int olen = 0;
    const char *oldstr = zp_str_get(oh, &olen);
    int nh = (idx >= 0 && idx < ZP_MAX_NODES) ? zp_field_lit[idx] : -1;
    int nlen = 0;
    const char *newstr = zp_str_get(nh, &nlen);
    if (!s || !oldstr || olen == 0) { sig_data_write_i32(out, sh); return 0; }
    if (!newstr) { newstr = ""; nlen = 0; }

    /* Allocate a generous buffer (cap at 16x growth or 64KiB). */
    int cap = slen + 64; if (cap < 256) cap = 256;
    char *buf = (char *)kmalloc((uint64_t)cap);
    if (!buf) { sig_data_write_i32(out, sh); return 0; }
    int wp = 0;
    for (int i = 0; i < slen; ) {
        int match = (i + olen <= slen);
        if (match) {
            for (int j = 0; j < olen; j++)
                if (s[i + j] != oldstr[j]) { match = 0; break; }
        }
        if (match) {
            if (wp + nlen >= cap) {
                int ncap = cap * 2;
                if (ncap > ZP_MAX_STRING_BYTES) ncap = ZP_MAX_STRING_BYTES;
                char *nbuf = (char *)kmalloc((uint64_t)ncap);
                if (!nbuf) break;
                for (int k = 0; k < wp; k++) nbuf[k] = buf[k];
                kfree(buf);
                buf = nbuf;
                cap = ncap;
            }
            for (int j = 0; j < nlen; j++) buf[wp++] = newstr[j];
            i += olen;
        } else {
            if (wp + 1 >= cap) {
                int ncap = cap * 2;
                if (ncap > ZP_MAX_STRING_BYTES) ncap = ZP_MAX_STRING_BYTES;
                char *nbuf = (char *)kmalloc((uint64_t)ncap);
                if (!nbuf) break;
                for (int k = 0; k < wp; k++) nbuf[k] = buf[k];
                kfree(buf);
                buf = nbuf;
                cap = ncap;
            }
            buf[wp++] = s[i++];
        }
    }
    int rh = zp_str_intern(buf, wp);
    kfree(buf);
    sig_data_write_i32(out, rh);
    return 0;
}

/* gate(.field == "lit") — input struct handle -> handle if eq, else block */
static int zp_proc_gate_field_eq(struct sig_node *node, struct sig_data *in,
                                  struct sig_data *out)
{
    int idx = node->id;
    int sh = sig_data_read_i32(in);
    /* fmt2 = field name, fmt = literal — stored on the node decl; we copy
     * them onto the side state at compile time as user_data. */
    const char *field = (const char *)node->user_data;
    int litlen = 0;
    int lith = (idx >= 0 && idx < ZP_MAX_NODES) ? zp_field_lit[idx] : -1;
    const char *lit = zp_str_get(lith, &litlen);
    if (!field || !lit) { out->size = 0; return 1; }
    struct zp_value v;
    if (zp_struct_get(sh, field, &v) < 0) { out->size = 0; return 1; }
    if (v.kind == ZP_VAL_STR) {
        int got = 0;
        const char *fs = zp_str_get(v.handle, &got);
        if (!fs || got != litlen) { out->size = 0; return 1; }
        for (int i = 0; i < got; i++)
            if (fs[i] != lit[i]) { out->size = 0; return 1; }
        /* match — pass the struct handle through */
        sig_data_write_i32(out, sh);
        return 0;
    }
    out->size = 0;
    return 1;
}

/* sum — running accumulator. For struct inputs, pulls .value field. */
static int zp_proc_sum(struct sig_node *node, struct sig_data *in,
                        struct sig_data *out)
{
    int idx = node->id;
    int32_t v = sig_data_read_i32(in);
    /* If the upstream is a struct, treat v as struct handle and read .value */
    struct zp_value sv;
    if (zp_struct_get(v, "value", &sv) == 0 && sv.kind == ZP_VAL_INT) {
        v = sv.handle;
    }
    if (idx >= 0 && idx < ZP_MAX_NODES) {
        zp_sum_acc[idx] += v;
        sig_data_write_i32(out, zp_sum_acc[idx]);
    } else {
        sig_data_write_i32(out, v);
    }
    return 0;
}

/* tap.log: pretty-print typed values when out_kind hint is set on the
 * upstream emit. We don't track types per-edge; so we do a best-effort:
 * if the int decodes to a valid string handle, print the string;
 * if it decodes to a valid struct handle, print "<struct typename>";
 * otherwise the bare integer. The original tap.log in zp_proc_tap_log
 * remains unchanged for legacy programs (it always prints int). */

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

/*
 * Compile chain definitions from Z+ source into chain.h chains.
 *
 * Each `chain name { ... }` block becomes a real chain in the kernel
 * registry under CHAIN_CPU with MASQ_INTERNAL tier, and each node uses
 * zp_kernel_resolve_thunk so chain_resolve / chain_registry_tick walk
 * the user's verb AST. The runtime persists the AST across program
 * reloads and tears down any prior chain with the same name first.
 */
static int zp_compile_chains(struct zp_program *prog)
{
    for (int i = 0; i < prog->chain_def_count; i++) {
        struct zp_chain_def *def = &prog->chain_defs[i];

        /* Build the runtime node array from the per-step decls. Steps
         * without an inline decl fall back to a passthrough verb so the
         * chain still resolves cleanly. */
        struct zp_runtime_node rt_nodes[ZP_MAX_CHAIN_NODES];
        int rn = 0;
        for (int n = 0; n < def->node_count && n < ZP_MAX_CHAIN_NODES; n++) {
            struct zp_runtime_node *rnode = &rt_nodes[rn];
            rnode->int_val = 0;
            rnode->int_val2 = 0;
            rnode->int_val3 = 0;
            rnode->fmt[0] = '\0';
            rnode->fmt2[0] = '\0';
            rnode->out_kind = ZP_VAL_INT;
            rnode->struct_type = -1;
            rnode->emit_handle = -1;
            rnode->sustain_count = 0;
            rnode->delta_prev = 0;
            rnode->has_delta_prev = 0;
            rnode->sum_acc = 0;
            zp_strcpy(rnode->name, def->node_names[n], ZP_MAX_NAME);
            if (def->have_decl[n]) {
                struct zp_node_decl *d = &def->decls[n];
                rnode->type = d->type;
                rnode->int_val  = d->int_val;
                rnode->int_val2 = d->int_val2;
                rnode->int_val3 = d->int_val3;
                rnode->out_kind = d->out_kind;
                rnode->struct_type = d->struct_type;
                rnode->emit_handle = d->emit_handle;
                zp_strcpy(rnode->fmt, d->fmt, ZP_MAX_STRING);
                zp_strcpy(rnode->fmt2, d->fmt2, ZP_MAX_STRING);
            } else {
                /* Try to find a top-level node decl with this name and use
                 * its type. If not found, passthrough. */
                int idx = find_node(prog, def->node_names[n]);
                if (idx >= 0) {
                    struct zp_node_decl *d = &prog->nodes[idx];
                    rnode->type = d->type;
                    rnode->int_val  = d->int_val;
                    rnode->int_val2 = d->int_val2;
                    rnode->int_val3 = d->int_val3;
                    rnode->out_kind = d->out_kind;
                    rnode->struct_type = d->struct_type;
                    rnode->emit_handle = d->emit_handle;
                    zp_strcpy(rnode->fmt, d->fmt, ZP_MAX_STRING);
                    zp_strcpy(rnode->fmt2, d->fmt2, ZP_MAX_STRING);
                } else {
                    rnode->type = ZP_PASSTHROUGH;
                }
            }
            rn++;
        }

        int cid = zp_runtime_register_chain(def->name, rt_nodes, rn);
        if (cid < 0) {
            kputs("Z+ error: could not register runtime chain '");
            kputs(def->name);
            kputs("'\n");
            return -1;
        }
        def->chain_id = cid;

        kputs("  Chain '");
        kputs(def->name);
        kputs("' registered (id=");
        kput_dec((uint64_t)cid);
        kputs(", ");
        kput_dec((uint64_t)rn);
        kputs(" nodes, parent=CPU, tier=internal)\n");
    }

    return 0;
}

/* ── Public API ───────────────────────────────── */

int zp_parse(const char *source, struct zp_program *prog)
{
    zp_pools_init();
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
            if (decl->out_kind != ZP_VAL_INT) {
                proc = zp_proc_emit_pass1;
                user_data = 0;
            } else {
                proc = zp_proc_emit;
                user_data = (void *)(long)decl->int_val;
            }
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
        case ZP_COMPUTE_RUN:
            proc = zp_proc_compute_run;
            user_data = 0;
            decl->chain_bind_id = CHAIN_MDE;
            break;
        case ZP_STR_LEN:         proc = zp_proc_str_len;          break;
        case ZP_STR_FIND:        proc = zp_proc_str_find;         break;
        case ZP_STR_SPLIT:       proc = zp_proc_str_split;        break;
        case ZP_STR_REPLACE:     proc = zp_proc_str_replace;      break;
        case ZP_STR_CONCAT:      proc = zp_proc_str_concat;       break;
        case ZP_STR_UPPER:       proc = zp_proc_str_upper;        break;
        case ZP_STR_LOWER:       proc = zp_proc_str_lower;        break;
        case ZP_STR_TRIM:        proc = zp_proc_str_trim;         break;
        case ZP_STR_STARTS_WITH: proc = zp_proc_str_starts_with;  break;
        case ZP_STR_ENDS_WITH:   proc = zp_proc_str_ends_with;    break;
        case ZP_STR_FROM_INT:    proc = zp_proc_str_from_int;     break;
        case ZP_STR_TO_INT:      proc = zp_proc_str_to_int;       break;
        case ZP_STR_LEN_OF_ARRAY:proc = zp_proc_str_len_of_array; break;
        case ZP_STRUCT_LITERAL:  proc = zp_proc_emit_pass1;       break;
        case ZP_FIELD_ACCESS:    proc = zp_proc_passthrough;      break;
        case ZP_GATE_FIELD_EQ:
            proc = zp_proc_gate_field_eq;
            user_data = (void *)decl->fmt2;  /* field name */
            break;
        case ZP_SUM:             proc = zp_proc_sum;              break;
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
            if (decl->type == ZP_COMPUTE_RUN) {
                zp_compute_target_chain[idx] = -1;
                zp_compute_target_idx[idx]   = -1;
                zp_compute_input_val[idx]    = 0;
                zp_compute_output_val[idx]   = 0;
                zp_compute_prefer_gpu[idx]   = decl->int_val ? 1 : 0;
            }
            /* Pass-1: emit handles, split sep, gate-field literal, sum acc. */
            if (decl->type == ZP_EMIT) {
                zp_emit_handle[idx] = decl->emit_handle;
                zp_emit_kind[idx]   = decl->out_kind;
            }
            if (decl->type == ZP_STR_SPLIT || decl->type == ZP_STR_FIND ||
                decl->type == ZP_STR_CONCAT ||
                decl->type == ZP_STR_STARTS_WITH || decl->type == ZP_STR_ENDS_WITH ||
                decl->type == ZP_STR_REPLACE) {
                zp_split_sep[idx] = (decl->fmt[0])
                    ? zp_str_intern(decl->fmt, -1) : -1;
                if (decl->type == ZP_STR_REPLACE) {
                    zp_field_lit[idx] = (decl->fmt2[0])
                        ? zp_str_intern(decl->fmt2, -1) : -1;
                }
            }
            if (decl->type == ZP_GATE_FIELD_EQ) {
                /* fmt = literal, fmt2 = field name */
                zp_field_lit[idx] = (decl->fmt[0])
                    ? zp_str_intern(decl->fmt, -1) : -1;
            }
            if (decl->type == ZP_SUM) {
                zp_sum_acc[idx] = 0;
            }
            if (decl->type == ZP_STRUCT_LITERAL) {
                zp_emit_handle[idx] = decl->emit_handle;
                zp_emit_kind[idx]   = ZP_VAL_STRUCT;
                zp_struct_type_idx[idx] = decl->struct_type;
            }
        }
    }

    /* Resolve compute.run targets now that every node has a sig_idx. */
    for (int i = 0; i < prog->node_count; i++) {
        struct zp_node_decl *decl = &prog->nodes[i];
        if (decl->type != ZP_COMPUTE_RUN) continue;
        int self_idx = decl->sig_idx;
        if (self_idx < 0 || self_idx >= ZP_MAX_NODES) continue;
        int target = find_node(prog, decl->fmt);
        if (target < 0) {
            kputs("Z+ error: compute.run unknown target '");
            kputs(decl->fmt);
            kputs("'\n");
            return -1;
        }
        zp_compute_target_chain[self_idx] = chain;
        zp_compute_target_idx[self_idx]   = prog->nodes[target].sig_idx;
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
