/*
 * Zeos — std.json (parser + emitter)
 *
 * Strict subset:
 *   value  := object | array | string | int | true | false | null
 *   object := '{' (string ':' value (',' string ':' value)*)? '}'
 *   array  := '[' (value (',' value)*)? ']'
 *
 * Maps to Z+ values:
 *   object  -> zp_struct_alloc (anonymous type with the parsed field set)
 *   array   -> zp_array_alloc (array of string handles, or "JSON-as-str"
 *              for non-string elements — array currently only stores str
 *              handles; ints are coerced to str)
 *   string  -> zp_str_intern
 *   int     -> raw int32 (kind=ZP_VAL_INT)
 *   true    -> 1
 *   false   -> 0
 *   null    -> 0 (kind=INT)
 */

#include "std_json.h"
#include "zplus.h"
#include "kprint.h"

/* The wire packing matches zp_value but condensed to a single int32 so
 * verbs that take/return one int32 can still carry a typed value. */
#define JV_PACK(kind, handle)  (((int32_t)(kind) << 24) | ((handle) & 0xFFFFFF))
#define JV_KIND(v)             (((v) >> 24) & 0xFF)
#define JV_HANDLE(v)           ((v) & 0xFFFFFF)

struct jp_state {
    const char *s;
    int         pos;
    int         len;
    int         err;
};

static int jp_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void jp_skip_ws(struct jp_state *p)
{
    while (p->pos < p->len && jp_isspace(p->s[p->pos])) p->pos++;
}

static char jp_peek(struct jp_state *p)
{
    return (p->pos < p->len) ? p->s[p->pos] : 0;
}

static char jp_consume(struct jp_state *p)
{
    return (p->pos < p->len) ? p->s[p->pos++] : 0;
}

static int32_t jp_parse_value(struct jp_state *p);

static int jp_parse_string(struct jp_state *p, char *out, int max)
{
    if (jp_consume(p) != '"') { p->err = 1; return -1; }
    int n = 0;
    while (p->pos < p->len) {
        char c = p->s[p->pos++];
        if (c == '"') { if (n < max) out[n] = '\0'; return n; }
        if (c == '\\' && p->pos < p->len) {
            char e = p->s[p->pos++];
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            default: c = e; break;
            }
        }
        if (n < max - 1) out[n++] = c;
    }
    p->err = 1;
    return -1;
}

static int32_t jp_parse_number(struct jp_state *p)
{
    int neg = 0;
    if (jp_peek(p) == '-') { neg = 1; p->pos++; }
    int32_t v = 0;
    int saw = 0;
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
        p->pos++;
        saw = 1;
    }
    if (!saw) p->err = 1;
    /* Skip fractional / exponent (we don't support floats yet). */
    if (jp_peek(p) == '.' || jp_peek(p) == 'e' || jp_peek(p) == 'E') {
        kputs("[json.parse] warning: float truncated to int\n");
        while (p->pos < p->len) {
            char c = p->s[p->pos];
            if ((c < '0' || c > '9') && c != '.' && c != 'e' &&
                c != 'E' && c != '+' && c != '-') break;
            p->pos++;
        }
    }
    return JV_PACK(0 /*ZP_VAL_INT*/, neg ? -v : v);
}

static int32_t jp_parse_object(struct jp_state *p)
{
    if (jp_consume(p) != '{') { p->err = 1; return 0; }
    /* Build an ad-hoc struct type per parse. We register it inline so
     * field access by name works. */
    struct zp_struct_type t;
    for (int i = 0; i < ZP_MAX_NAME; i++) t.name[i] = 0;
    /* Anonymous: prefix with "_json_obj" plus a tiny seq.  Reuse if a
     * prior parse produced the same name; that's fine — we allocate a
     * fresh INSTANCE every call. */
    static int s_seq = 0;
    s_seq++;
    char *nm = t.name;
    const char *prefix = "_json_obj";
    int wp = 0;
    while (prefix[wp]) { nm[wp] = prefix[wp]; wp++; }
    /* Append seq base-10 (small). */
    if (s_seq >= 100) nm[wp++] = '0' + ((s_seq / 100) % 10);
    if (s_seq >= 10)  nm[wp++] = '0' + ((s_seq / 10)  % 10);
    nm[wp++] = '0' + (s_seq % 10);
    nm[wp]   = 0;
    t.field_count = 0;
    t.in_use = 1;

    char keybuf[32];
    int field_kinds [ZP_MAX_STRUCT_FIELDS];
    int32_t field_values[ZP_MAX_STRUCT_FIELDS];

    jp_skip_ws(p);
    if (jp_peek(p) == '}') {
        p->pos++;
        int ti = zp_struct_register(&t);
        if (ti < 0) { p->err = 1; return 0; }
        int inst = zp_struct_alloc(ti);
        return JV_PACK(2 /*ZP_VAL_STRUCT*/, inst);
    }

    while (p->pos < p->len) {
        jp_skip_ws(p);
        int klen = jp_parse_string(p, keybuf, (int)sizeof(keybuf));
        if (klen < 0) return 0;
        jp_skip_ws(p);
        if (jp_consume(p) != ':') { p->err = 1; return 0; }
        jp_skip_ws(p);
        int32_t val = jp_parse_value(p);
        if (p->err) return 0;
        if (t.field_count < ZP_MAX_STRUCT_FIELDS) {
            struct zp_struct_field *f = &t.fields[t.field_count];
            int j;
            for (j = 0; j < ZP_MAX_NAME - 1 && keybuf[j]; j++) f->name[j] = keybuf[j];
            f->name[j] = 0;
            f->offset = t.field_count;
            int kind = JV_KIND(val);
            f->kind = (kind == 1) ? 1 /*STR*/ : (kind == 2) ? 2 /*STRUCT*/ : 0 /*INT*/;
            field_kinds [t.field_count] = f->kind;
            field_values[t.field_count] = JV_HANDLE(val);
            t.field_count++;
        }
        jp_skip_ws(p);
        char nx = jp_consume(p);
        if (nx == '}') break;
        if (nx != ',') { p->err = 1; return 0; }
    }

    int ti = zp_struct_register(&t);
    if (ti < 0) { p->err = 1; return 0; }
    int inst = zp_struct_alloc(ti);
    if (inst < 0) { p->err = 1; return 0; }
    /* Populate. */
    for (int i = 0; i < t.field_count; i++) {
        if (field_kinds[i] == 1) {
            zp_struct_set_str(inst, t.fields[i].name, field_values[i]);
        } else {
            zp_struct_set_int(inst, t.fields[i].name, field_values[i]);
        }
    }
    return JV_PACK(2, inst);
}

static int32_t jp_parse_array(struct jp_state *p)
{
    if (jp_consume(p) != '[') { p->err = 1; return 0; }
    int arr = zp_array_alloc();
    jp_skip_ws(p);
    if (jp_peek(p) == ']') { p->pos++; return JV_PACK(3 /*ARRAY*/, arr); }
    while (p->pos < p->len) {
        jp_skip_ws(p);
        int32_t v = jp_parse_value(p);
        if (p->err) return 0;
        int kind = JV_KIND(v);
        int handle = JV_HANDLE(v);
        if (kind == 1) {
            zp_array_push_str(arr, handle);
        } else {
            /* Coerce non-strings to a "n" stringification so the array
             * still carries it.  Honest gap: arrays today only store
             * string handles. */
            char buf[16];
            int neg = (handle < 0);
            int32_t x = neg ? -handle : handle;
            int n = 0;
            char tmp[16];
            if (x == 0) tmp[n++] = '0';
            while (x > 0 && n < 15) { tmp[n++] = '0' + (x % 10); x /= 10; }
            int b = 0;
            if (neg) buf[b++] = '-';
            for (int j = n - 1; j >= 0; j--) buf[b++] = tmp[j];
            buf[b] = 0;
            int sh = zp_str_intern(buf, b);
            zp_array_push_str(arr, sh);
        }
        jp_skip_ws(p);
        char nx = jp_consume(p);
        if (nx == ']') break;
        if (nx != ',') { p->err = 1; return 0; }
    }
    return JV_PACK(3, arr);
}

static int32_t jp_parse_value(struct jp_state *p)
{
    jp_skip_ws(p);
    char c = jp_peek(p);
    if (c == '{') return jp_parse_object(p);
    if (c == '[') return jp_parse_array(p);
    if (c == '"') {
        char buf[256];
        int n = jp_parse_string(p, buf, (int)sizeof(buf));
        if (n < 0) return 0;
        int h = zp_str_intern(buf, n);
        return JV_PACK(1 /*STR*/, h);
    }
    if (c == '-' || (c >= '0' && c <= '9')) return jp_parse_number(p);
    if (c == 't' && p->pos + 4 <= p->len &&
        p->s[p->pos+1]=='r' && p->s[p->pos+2]=='u' && p->s[p->pos+3]=='e') {
        p->pos += 4;
        return JV_PACK(0, 1);
    }
    if (c == 'f' && p->pos + 5 <= p->len &&
        p->s[p->pos+1]=='a' && p->s[p->pos+2]=='l' &&
        p->s[p->pos+3]=='s' && p->s[p->pos+4]=='e') {
        p->pos += 5;
        return JV_PACK(0, 0);
    }
    if (c == 'n' && p->pos + 4 <= p->len &&
        p->s[p->pos+1]=='u' && p->s[p->pos+2]=='l' && p->s[p->pos+3]=='l') {
        p->pos += 4;
        return JV_PACK(0, 0);
    }
    p->err = 1;
    return 0;
}

int32_t zp_json_parse_str(const char *s)
{
    if (!s) return 0;
    struct jp_state p;
    p.s = s; p.pos = 0; p.err = 0;
    int n = 0; while (s[n]) n++;
    p.len = n;
    int32_t v = jp_parse_value(&p);
    if (p.err) {
        kputs("[json.parse] error at offset ");
        kput_dec((uint64_t)p.pos);
        kputs("\n");
        return 0;
    }
    return v;
}

/* ─── Emitter ────────────────────────────────────────────────────── */

static int em_grow_check(char *buf, int *n, int max, int need)
{
    return (*n + need) <= max;
}

static int em_putc(char *buf, int *n, int max, char c)
{
    if (!em_grow_check(buf, n, max, 1)) return -1;
    buf[(*n)++] = c;
    return 0;
}

static int em_puts(char *buf, int *n, int max, const char *s)
{
    while (*s) {
        if (em_putc(buf, n, max, *s++) < 0) return -1;
    }
    return 0;
}

static int em_int(char *buf, int *n, int max, int32_t v)
{
    if (v < 0) {
        if (em_putc(buf, n, max, '-') < 0) return -1;
        v = -v;
    }
    char tmp[16]; int tn = 0;
    if (v == 0) tmp[tn++] = '0';
    while (v > 0 && tn < 16) { tmp[tn++] = '0' + (v % 10); v /= 10; }
    for (int i = tn - 1; i >= 0; i--) {
        if (em_putc(buf, n, max, tmp[i]) < 0) return -1;
    }
    return 0;
}

static int em_quoted(char *buf, int *n, int max, const char *s, int sl)
{
    if (em_putc(buf, n, max, '"') < 0) return -1;
    for (int i = 0; i < sl; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            if (em_putc(buf, n, max, '\\') < 0) return -1;
        }
        if (c == '\n') { if (em_putc(buf, n, max, '\\') < 0) return -1; c = 'n'; }
        else if (c == '\t') { if (em_putc(buf, n, max, '\\') < 0) return -1; c = 't'; }
        if (em_putc(buf, n, max, c) < 0) return -1;
    }
    return em_putc(buf, n, max, '"');
}

static int em_value(char *buf, int *n, int max, int kind, int handle);

static int em_struct(char *buf, int *n, int max, int handle)
{
    /* Walk through the struct via field iteration.  We don't have an
     * iterator API, but the struct type slots are bounded — call the
     * known accessor for each declared field of the struct's type. */
    /* Find the type for this instance via the "_kind" sentinel: we
     * stored kind+offset in zp_struct_type's fields array. We don't
     * have a public typeof(handle), so we iterate names by trying each
     * field of every registered type — too slow. Instead, the struct
     * pool keeps type_idx; expose via zp_struct_get(handle, "*"...). */
    /* Pragmatic path: iterate up to ZP_MAX_STRUCT_FIELDS + use a
     * helper that walks the struct's type fields via zplus.h's API.
     * We add zp_struct_emit_json hook below. For now, build a generic
     * "{}" if we can't introspect — but we DO have zp_struct_get_type
     * defined in std_json_helper.c (added as a small helper inside
     * zplus.c through zp_struct_type_at()).  Punt minimal: walk via
     * zp_struct_type_count + zp_struct_type_at iteration order
     * isn't accessible publicly, so we extend zplus with a tiny
     * helper.  Defined in zplus.c (zp_struct_typeof + zp_struct_type_field). */
    extern int zp_struct_typeof(int instance_handle);
    extern int zp_struct_type_field_count(int type_idx);
    extern const char *zp_struct_type_field_name(int type_idx, int field_idx);
    extern int zp_struct_type_field_kind(int type_idx, int field_idx);

    int ti = zp_struct_typeof(handle);
    if (ti < 0) {
        if (em_puts(buf, n, max, "{}") < 0) return -1;
        return 0;
    }
    if (em_putc(buf, n, max, '{') < 0) return -1;
    int fc = zp_struct_type_field_count(ti);
    int wrote_any = 0;
    for (int f = 0; f < fc; f++) {
        const char *nm = zp_struct_type_field_name(ti, f);
        int knd = zp_struct_type_field_kind(ti, f);
        if (!nm) continue;
        if (wrote_any) {
            if (em_putc(buf, n, max, ',') < 0) return -1;
        }
        int nl = 0; while (nm[nl]) nl++;
        if (em_quoted(buf, n, max, nm, nl) < 0) return -1;
        if (em_putc(buf, n, max, ':') < 0) return -1;
        struct zp_value v;
        if (zp_struct_get(handle, nm, &v) == 0) {
            if (em_value(buf, n, max, v.kind, v.handle) < 0) return -1;
        } else {
            if (knd == 1) {
                if (em_puts(buf, n, max, "\"\"") < 0) return -1;
            } else {
                if (em_putc(buf, n, max, '0') < 0) return -1;
            }
        }
        wrote_any = 1;
    }
    if (em_putc(buf, n, max, '}') < 0) return -1;
    return 0;
}

static int em_array(char *buf, int *n, int max, int arr)
{
    if (em_putc(buf, n, max, '[') < 0) return -1;
    int len = zp_array_len(arr);
    for (int i = 0; i < len; i++) {
        if (i) { if (em_putc(buf, n, max, ',') < 0) return -1; }
        int sh = zp_array_get_str(arr, i);
        int sl = 0;
        const char *s = zp_str_get(sh, &sl);
        if (s) {
            if (em_quoted(buf, n, max, s, sl) < 0) return -1;
        } else {
            if (em_puts(buf, n, max, "null") < 0) return -1;
        }
    }
    if (em_putc(buf, n, max, ']') < 0) return -1;
    return 0;
}

static int em_value(char *buf, int *n, int max, int kind, int handle)
{
    if (kind == 0) {
        return em_int(buf, n, max, handle);
    }
    if (kind == 1) {
        int sl = 0;
        const char *s = zp_str_get(handle, &sl);
        if (!s) return em_puts(buf, n, max, "null");
        return em_quoted(buf, n, max, s, sl);
    }
    if (kind == 2) return em_struct(buf, n, max, handle);
    if (kind == 3) return em_array(buf, n, max, handle);
    return em_puts(buf, n, max, "null");
}

int zp_json_emit_value(int kind, int handle)
{
    char buf[1024];
    int n = 0;
    if (em_value(buf, &n, (int)sizeof(buf), kind, handle) < 0) {
        return -1;
    }
    return zp_str_intern(buf, n);
}
