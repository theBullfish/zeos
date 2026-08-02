/*
 * Zeos — ZIR (Z+ Interchange Representation) loader.  See zplus_zir.h.
 *
 * A self-contained JSON reader + ZIR->zp_program translator. No libc, no
 * allocator: only the tiny static helpers below and caller-provided storage,
 * so this exact file compiles in the kernel and in the host unit test.
 */
#include "zplus_zir.h"

/* ── tiny dependency-free helpers ─────────────────────────────────────── */

static int zir_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int zir_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static void zir_copy(char *dst, const char *src, int dstsz) {
    int i = 0;
    for (; src[i] && i < dstsz - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ── cursor over the JSON text ────────────────────────────────────────── */

typedef struct { const char *p; const char *end; int err; } cur_t;

/* What we pull out of a JSON value we care about (emit / gate / gate.rhs). */
typedef struct {
    int  has_int; long ival;
    int  has_str; char sval[ZP_MAX_STRING];
    int  has_op;  char op[8];
} pulled_t;

static void skip_ws(cur_t *c) { while (c->p < c->end && zir_is_ws(*c->p)) c->p++; }

static int peek(cur_t *c) { skip_ws(c); return (c->p < c->end) ? *c->p : 0; }

/* Required match: consumes `ch` or flags an error. Use where the grammar
 * demands the character (':', container open/close). */
static int eat(cur_t *c, char ch) {
    skip_ws(c);
    if (c->p < c->end && *c->p == ch) { c->p++; return 1; }
    c->err = 1;
    return 0;
}

/* Optional match: consumes `ch` if present, never errors. Use in list-loop
 * conditions where "no separator" is the normal terminator. */
static int accept(cur_t *c, char ch) {
    skip_ws(c);
    if (c->p < c->end && *c->p == ch) { c->p++; return 1; }
    return 0;
}

/* Parse a JSON string literal into dst (basic escapes). Leaves cursor past the
 * closing quote. */
static int parse_string(cur_t *c, char *dst, int dstsz) {
    skip_ws(c);
    if (c->p >= c->end || *c->p != '"') { c->err = 1; return 0; }
    c->p++;
    int n = 0;
    while (c->p < c->end && *c->p != '"') {
        char ch = *c->p++;
        if (ch == '\\' && c->p < c->end) {
            char e = *c->p++;
            switch (e) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '"': ch = '"';  break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/';  break;
                case 'u': /* skip 4 hex, emit placeholder */
                    for (int k = 0; k < 4 && c->p < c->end; k++) c->p++;
                    ch = '?';
                    break;
                default: ch = e; break;
            }
        }
        if (dst && n < dstsz - 1) dst[n++] = ch;
    }
    if (c->p >= c->end) { c->err = 1; return 0; }
    c->p++; /* closing quote */
    if (dst) dst[n] = 0;
    return 1;
}

/* Parse a JSON number as a long (truncates floats). */
static int parse_number(cur_t *c, long *out) {
    skip_ws(c);
    int neg = 0;
    if (c->p < c->end && (*c->p == '-' || *c->p == '+')) { neg = (*c->p == '-'); c->p++; }
    long v = 0; int any = 0;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') { v = v * 10 + (*c->p - '0'); c->p++; any = 1; }
    if (c->p < c->end && *c->p == '.') { /* consume fractional part, ignore */
        c->p++;
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') c->p++;
    }
    if (!any) { c->err = 1; return 0; }
    *out = neg ? -v : v;
    return 1;
}

static void skip_value(cur_t *c);   /* fwd */

/* Pull fields of interest from a value; recurse into "rhs". Non-objects are
 * captured as scalars (a bare number/string). */
static void pull_value(cur_t *c, pulled_t *out) {
    int ch = peek(c);
    if (ch == '{') {
        c->p++; /* '{' */
        if (peek(c) == '}') { c->p++; return; }
        do {
            char key[32];
            if (!parse_string(c, key, sizeof key)) return;
            if (!eat(c, ':')) return;
            if (zir_streq(key, "int") || zir_streq(key, "steps") || zir_streq(key, "hex")) {
                long v; if (parse_number(c, &v)) { out->has_int = 1; out->ival = v; }
            } else if (zir_streq(key, "str") || zir_streq(key, "template") || zir_streq(key, "color")) {
                if (parse_string(c, out->sval, (int)sizeof out->sval)) out->has_str = 1;
            } else if (zir_streq(key, "op")) {
                if (parse_string(c, out->op, (int)sizeof out->op)) out->has_op = 1;
            } else if (zir_streq(key, "rhs")) {
                pull_value(c, out); /* rhs's int/str populate ival/sval */
            } else {
                skip_value(c);
            }
        } while (accept(c, ','));
        eat(c, '}');
    } else if (ch == '"') {
        if (parse_string(c, out->sval, (int)sizeof out->sval)) out->has_str = 1;
    } else if ((ch >= '0' && ch <= '9') || ch == '-') {
        long v; if (parse_number(c, &v)) { out->has_int = 1; out->ival = v; }
    } else {
        skip_value(c);
    }
}

static void skip_value(cur_t *c) {
    int ch = peek(c);
    if (ch == '{' || ch == '[') {
        char open = (char)ch, close = (open == '{') ? '}' : ']';
        c->p++;
        int depth = 1;
        while (c->p < c->end && depth > 0) {
            char x = *c->p;
            if (x == '"') { parse_string(c, 0, 0); continue; }
            if (x == open) depth++;
            else if (x == close) depth--;
            c->p++;
        }
    } else if (ch == '"') {
        parse_string(c, 0, 0);
    } else if ((ch >= '0' && ch <= '9') || ch == '-') {
        long v; parse_number(c, &v);
    } else { /* true/false/null */
        while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']') c->p++;
    }
}

/* ── verb -> kernel node type ─────────────────────────────────────────── */

enum zp_node_type zir_verb_to_type(const char *verb, const char *gate_op, int *known) {
    if (known) *known = 1;
    if (zir_streq(verb, "emit"))    return ZP_EMIT;
    if (zir_streq(verb, "input"))   return ZP_PASSTHROUGH;
    if (zir_streq(verb, "output"))  return ZP_PASSTHROUGH;
    if (zir_streq(verb, "print"))   return ZP_PRINT;
    if (zir_streq(verb, "tap.log")) return ZP_TAP_LOG;
    if (zir_streq(verb, "delta"))   return ZP_DELTA;
    if (zir_streq(verb, "sum"))     return ZP_SUM;
    if (zir_streq(verb, "knee"))    return ZP_KNEE;
    if (zir_streq(verb, "sustained")) return ZP_SUSTAINED;
    if (zir_streq(verb, "str.len"))     return ZP_STR_LEN;
    if (zir_streq(verb, "str.find"))    return ZP_STR_FIND;
    if (zir_streq(verb, "str.split"))   return ZP_STR_SPLIT;
    if (zir_streq(verb, "str.concat"))  return ZP_STR_CONCAT;
    if (zir_streq(verb, "str.upper"))   return ZP_STR_UPPER;
    if (zir_streq(verb, "str.lower"))   return ZP_STR_LOWER;
    if (zir_streq(verb, "vault.put"))   return ZP_VAULT_PUT;
    if (zir_streq(verb, "vault.get"))   return ZP_VAULT_GET;
    if (zir_streq(verb, "time.now"))    return ZP_TIME_NOW;
    if (zir_streq(verb, "crypto.hmac_sha256")) return ZP_CRYPTO_HMAC;
    if (zir_streq(verb, "json.parse"))  return ZP_JSON_PARSE;
    if (zir_streq(verb, "json.emit"))   return ZP_JSON_EMIT;
    if (zir_streq(verb, "regex.match")) return ZP_REGEX_MATCH;
    if (zir_streq(verb, "fs.read"))     return ZP_FS_READ;
    if (zir_streq(verb, "fs.write"))    return ZP_FS_WRITE;
    if (zir_streq(verb, "net.send"))    return ZP_NET_SEND;
    if (zir_streq(verb, "net.recv"))    return ZP_NET_RECV;
    if (zir_streq(verb, "gate")) {
        if (gate_op) {
            if (zir_streq(gate_op, "gt")) return ZP_GATE_GT;
            if (zir_streq(gate_op, "lt")) return ZP_GATE_LT;
            if (zir_streq(gate_op, "eq")) return ZP_GATE_EQ;
            if (zir_streq(gate_op, "ge")) return ZP_GATE_GTE;
            if (zir_streq(gate_op, "le")) return ZP_GATE_LTE;
        }
        /* '~' fuzzy-match gate and bare gates have no scalar kernel node type */
        if (known) *known = 0;
        return ZP_PASSTHROUGH;
    }
    /* unknown verb — honest degradation to passthrough */
    if (known) *known = 0;
    return ZP_PASSTHROUGH;
}

/* ── the loader ───────────────────────────────────────────────────────── */

#define ZIR_MAX_RAW_EDGES ZP_MAX_EDGES

static void zero_prog(struct zp_program *p) {
    char *b = (char *)p;
    for (unsigned i = 0; i < sizeof *p; i++) b[i] = 0;
    p->chain_id = -1;
}

/* Parse one node object into prog->nodes[idx]; records its name for edges. */
static void load_node(cur_t *c, struct zp_program *prog, zir_load_result_t *res) {
    char verb[ZP_MAX_NAME] = {0};
    char name[ZP_MAX_NAME] = {0};
    pulled_t emit = {0};
    pulled_t gate = {0};
    int have_emit = 0, have_gate = 0;

    if (!eat(c, '{')) return;
    if (peek(c) != '}') {
        do {
            char key[32];
            if (!parse_string(c, key, sizeof key)) { c->err = 1; return; }
            if (!eat(c, ':')) return;
            if (zir_streq(key, "verb")) {
                parse_string(c, verb, sizeof verb);
            } else if (zir_streq(key, "name")) {
                parse_string(c, name, sizeof name);
            } else if (zir_streq(key, "emit")) {
                pull_value(c, &emit); have_emit = 1;
            } else if (zir_streq(key, "gate")) {
                pull_value(c, &gate); have_gate = 1;
            } else {
                skip_value(c);
            }
        } while (accept(c, ','));
    }
    eat(c, '}');
    if (c->err) return;

    if (prog->node_count >= ZP_MAX_NODES) { res->truncated++; return; }

    struct zp_node_decl *nd = &prog->nodes[prog->node_count];
    int known = 1;
    const char *op = (have_gate && gate.has_op) ? gate.op : 0;
    nd->type = zir_verb_to_type(verb, op, &known);
    if (!known) res->unmapped++;
    zir_copy(nd->name, name[0] ? name : verb, ZP_MAX_NAME);
    nd->sig_idx = -1;
    nd->chain_bind_id = -1;
    nd->out_kind = ZP_VAL_INT;
    nd->struct_type = -1;
    nd->emit_handle = -1;

    /* scalar payloads: emit value, or gate threshold */
    if (have_emit) {
        if (emit.has_int) nd->int_val = (int)emit.ival;
        if (emit.has_str) { zir_copy(nd->fmt, emit.sval, ZP_MAX_STRING); nd->out_kind = ZP_VAL_STR; }
    }
    if (have_gate) {
        if (gate.has_int) nd->int_val = (int)gate.ival;
        if (gate.has_str) zir_copy(nd->fmt, gate.sval, ZP_MAX_STRING);
    }

    prog->node_count++;
    res->nodes_loaded++;
}

static void load_nodes_array(cur_t *c, struct zp_program *prog, zir_load_result_t *res) {
    if (!eat(c, '[')) return;
    if (peek(c) == ']') { c->p++; return; }
    do {
        load_node(c, prog, res);
        if (c->err) return;
    } while (accept(c, ','));
    eat(c, ']');
}

/* Edges reference node ids; collected raw here and resolved to names after all
 * nodes are loaded (id == node index). */
static void load_edges_array(cur_t *c, zir_load_result_t *res,
                             int from_ids[], int to_ids[], int *nedges) {
    if (!eat(c, '[')) return;
    if (peek(c) == ']') { c->p++; return; }
    do {
        int from = -1, to = -1;
        if (!eat(c, '{')) return;
        if (peek(c) != '}') {
            do {
                char key[16];
                if (!parse_string(c, key, sizeof key)) { c->err = 1; return; }
                if (!eat(c, ':')) return;
                if (zir_streq(key, "from")) { long v; parse_number(c, &v); from = (int)v; }
                else if (zir_streq(key, "to")) { long v; parse_number(c, &v); to = (int)v; }
                else skip_value(c);
            } while (accept(c, ','));
        }
        eat(c, '}');
        if (c->err) return;
        if (*nedges < ZIR_MAX_RAW_EDGES) {
            from_ids[*nedges] = from; to_ids[*nedges] = to; (*nedges)++;
        } else {
            res->truncated++;
        }
    } while (accept(c, ','));
    eat(c, ']');
}

int zir_load(const char *json, struct zp_program *prog, zir_load_result_t *res) {
    /* zero outputs */
    { char *b = (char *)res; for (unsigned i = 0; i < sizeof *res; i++) b[i] = 0; }
    zero_prog(prog);

    if (!json) { zir_copy(res->error, "null json", sizeof res->error); return -1; }

    cur_t c;
    c.p = json;
    /* find end */
    const char *e = json; while (*e) e++;
    c.end = e;
    c.err = 0;

    int from_ids[ZIR_MAX_RAW_EDGES];
    int to_ids[ZIR_MAX_RAW_EDGES];
    int nedges = 0;

    if (!eat(&c, '{')) { zir_copy(res->error, "expected object", sizeof res->error); return -1; }

    if (peek(&c) != '}') {
        do {
            char key[32];
            if (!parse_string(&c, key, sizeof key)) { zir_copy(res->error, "bad key", sizeof res->error); return -1; }
            if (!eat(&c, ':')) { zir_copy(res->error, "expected ':'", sizeof res->error); return -1; }
            if (zir_streq(key, "zir")) {
                long v; parse_number(&c, &v); res->version = (int)v;
            } else if (zir_streq(key, "nodes")) {
                load_nodes_array(&c, prog, res);
            } else if (zir_streq(key, "edges")) {
                load_edges_array(&c, res, from_ids, to_ids, &nedges);
            } else {
                skip_value(&c);
            }
            if (c.err) { zir_copy(res->error, "parse error", sizeof res->error); return -1; }
        } while (accept(&c, ','));
    }

    if (res->version != ZIR_SUPPORTED_VERSION) {
        zir_copy(res->error, "unsupported zir version", sizeof res->error);
        return -1;
    }

    /* resolve edge ids -> node names */
    for (int i = 0; i < nedges; i++) {
        int f = from_ids[i], t = to_ids[i];
        if (f < 0 || f >= prog->node_count || t < 0 || t >= prog->node_count) continue;
        if (prog->edge_count >= ZP_MAX_EDGES) { res->truncated++; continue; }
        struct zp_edge *ed = &prog->edges[prog->edge_count];
        zir_copy(ed->src, prog->nodes[f].name, ZP_MAX_NAME);
        zir_copy(ed->dst, prog->nodes[t].name, ZP_MAX_NAME);
        prog->edge_count++;
        res->edges_loaded++;
    }

    res->ok = 1;
    return 0;
}

/*
 * Live entry point: load ZIR text, compile it through the kernel's Z+ engine,
 * and drive one resolution pass. Returns nodes fired (>=0), or -1 if the ZIR
 * failed to load. This is the call site that makes the loader LIVE (a boot
 * selftest calls it — see tests.c), so it is not dead code.
 *
 * HONEST STATUS: zp_compile targets the sig_chain engine today, so this does
 * NOT yet get MasQ tier / vault_version / B3 (see the header's KNOWN GAPS).
 * It proves the .zp -> ZIR -> kernel ingestion path executes end to end.
 */
int zir_run(const char *json) {
    static struct zp_program prog;   /* static: keep this off the kernel stack */
    zir_load_result_t res;
    if (zir_load(json, &prog, &res) != 0) return -1;
    (void)zp_compile(&prog);
    return zp_execute(&prog);
}
