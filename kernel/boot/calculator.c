/*
 * Calculator. Three modes (standard / scientific / programmer),
 * recursive-descent parser, history panel. Programmer mode displays
 * result simultaneously in DEC/HEX/OCT/BIN.
 *
 * Design:
 *   - Single global g_calc state. Singleton overlay window like image_viewer.
 *   - Recursive-descent parser for + - * / ( ) ^ unary-minus, named funcs
 *     (sin/cos/tan/log/ln/exp/sqrt/abs), factorial (postfix !), percent
 *     (postfix %), and programmer-mode integer ops (& | ^ ~ << >>) layered
 *     onto the same grammar. Programmer-mode evaluation happens in int64,
 *     all other modes in double.
 *   - Each keystroke triggers a re-parse for the live preview. Errors are
 *     swallowed silently (the preview just disappears) — only Enter logs
 *     a final failure into history.
 *   - History is a fixed ring of 8 entries. Up/Down navigates; click
 *     recalls the expression into the input.
 *
 * Math kernel:
 *   - sin/cos/tan via Taylor series with range reduction to [-pi/4, pi/4].
 *   - log (base-10) and ln via ln(x) = 2*atanh((x-1)/(x+1)) using a
 *     ratio-of-Chebyshev-fast power series. exp via Taylor on reduced arg.
 *   - sqrt via Newton iteration (8 steps from a halved exponent seed).
 *   - All freestanding — no libc.
 */

#include "calculator.h"
#include "fb.h"
#include "font.h"
#include "theme.h"
#include "compositor.h"
#include "ui_dirty.h"
#include "ui_undo.h"
#include "ui_hover.h"
#include "kprint.h"

/* ── Layout ── */

#define CALC_W            560
#define CALC_H            520
#define CALC_TITLE_H      32
#define CALC_TAB_H        28
#define CALC_DISPLAY_H    72
#define CALC_PAD          10
#define CALC_HIST_W       180
#define CALC_HIST_LINES   8
#define CALC_INPUT_MAX    192
#define CALC_PREVIEW_MAX  96
#define CALC_ERR_MAX      64
#define CALC_WIN_ID       4242  /* dirty registry id */

/* ── State ── */

typedef struct {
    char   expr[CALC_INPUT_MAX];
    double result_d;
    int64_t result_i;
    int    is_int;        /* programmer mode result */
    int    ok;
} calc_hist_t;

typedef struct {
    int  active;
    int  wx, wy;
    calc_mode_t mode;

    /* Editor */
    char input[CALC_INPUT_MAX];
    int  input_len;
    int  cursor;          /* cursor index into input */

    /* Live preview */
    char preview[CALC_PREVIEW_MAX];
    int  preview_ok;
    double preview_d;
    int64_t preview_i;
    int  preview_is_int;

    /* History (newest at index 0) */
    calc_hist_t hist[CALC_HIST_LINES];
    int  hist_count;
    int  hist_browse;     /* -1 = editing fresh, else index into hist */

    /* Programmer-mode bit toggles operate on this 64-bit working register.
     * Initialized from the last result on every eval. */
    uint64_t bits;

    /* Hover registration tokens (registered on layout). */
    uint64_t hover_close;
    uint64_t hover_tab[3];

    /* Mouse-up suppression for click-to-recall. */
    int  pending_recall;
    int  pending_recall_idx;

    /* Undo on input field. */
    undo_buffer_t undo;
    int undo_inited;
} calc_t;

static calc_t g_calc;
static uint32_t g_total_opens = 0;
static uint32_t g_total_evals_ok = 0;
static uint32_t g_total_evals_fail = 0;

/* ── Tiny helpers (no libc) ── */

static int c_strlen(const char *s) { int n = 0; if (!s) return 0; while (s[n]) n++; return n; }
static void c_strncpy(char *dst, const char *src, int max) {
    int i = 0; if (!dst || max <= 0) return;
    if (src) while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static int c_streq(const char *a, const char *b) {
    int i = 0; while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == 0 && b[i] == 0;
}
static int c_isdigit(int c) { return c >= '0' && c <= '9'; }
static int c_isalpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int c_isxdigit(int c) {
    return c_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int c_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* ── Math kernel (freestanding) ── */

#define C_PI       3.14159265358979323846
#define C_PI_2     1.57079632679489661923
#define C_PI_4     0.78539816339744830962
#define C_LN10     2.30258509299404568402
#define C_HUGE     1.0e308

static double c_fabs(double x) { return x < 0 ? -x : x; }

static double c_floor(double x) {
    /* Range-limited; for our use cases x is always finite reasonable. */
    int64_t i = (int64_t)x;
    double d = (double)i;
    if (x < 0 && d != x) d -= 1.0;
    return d;
}

static double c_sqrt(double x) {
    if (x < 0) return 0.0 / 0.0; /* NaN */
    if (x == 0) return 0;
    /* Seed via halving the exponent — works fine for normal doubles. */
    double r = x;
    if (r > 1) r = r / 2;
    else        r = (r + 1) * 0.5;
    for (int i = 0; i < 32; i++) {
        double n = 0.5 * (r + x / r);
        if (n == r) break;
        r = n;
    }
    return r;
}

/* exp via range reduction: exp(x) = 2^k * exp(r), r in [-ln2/2, ln2/2]. */
static double c_exp(double x) {
    if (x > 709.0)  return C_HUGE;
    if (x < -745.0) return 0.0;
    double LN2 = 0.69314718055994530942;
    double k_d = c_floor(x / LN2 + 0.5);
    int    k   = (int)k_d;
    double r   = x - k_d * LN2;
    /* Taylor — 14 terms is overkill but cheap. */
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 18; n++) {
        term *= r / n;
        sum  += term;
    }
    /* Multiply by 2^k. */
    double pow2 = 1.0;
    if (k >= 0) for (int i = 0; i < k;  i++) pow2 *= 2.0;
    else        for (int i = 0; i < -k; i++) pow2 *= 0.5;
    return sum * pow2;
}

static double c_ln(double x) {
    if (x <= 0) return 0.0 / 0.0;
    /* Reduce: x = m * 2^k, m in [1, 2). */
    int k = 0;
    while (x >= 2.0) { x *= 0.5; k++; }
    while (x < 1.0)  { x *= 2.0; k--; }
    /* ln(m) = 2 * sum_{n>=0} 1/(2n+1) * t^(2n+1), t = (m-1)/(m+1). */
    double t = (x - 1.0) / (x + 1.0);
    double t2 = t * t;
    double term = t, sum = t;
    for (int n = 1; n < 40; n++) {
        term *= t2;
        sum  += term / (2 * n + 1);
    }
    double LN2 = 0.69314718055994530942;
    return 2.0 * sum + k * LN2;
}

static double c_log10(double x) { return c_ln(x) / C_LN10; }

static double c_pow(double a, double b) {
    if (a == 0) return b == 0 ? 1.0 : 0.0;
    if (b == 0) return 1.0;
    /* Integer fast-path for common case (squares, cubes…). */
    if (b == c_floor(b) && c_fabs(b) < 1024) {
        int n = (int)b;
        double r = 1.0;
        double base = a;
        if (n < 0) { base = 1.0 / a; n = -n; }
        while (n) {
            if (n & 1) r *= base;
            base *= base;
            n >>= 1;
        }
        return r;
    }
    /* General: a^b = exp(b * ln(a)). a must be > 0 here for ln. */
    if (a < 0) return 0.0 / 0.0;
    return c_exp(b * c_ln(a));
}

/* Sin/cos via reduction to [-pi/4, pi/4] then Taylor. */
static double c_sin_kernel(double x) {
    double x2 = x * x;
    /* 8-term Taylor — accurate to ~1e-16 in [-pi/4, pi/4]. */
    double r = x;
    double term = x;
    int sign = -1;
    for (int n = 1; n < 8; n++) {
        term *= x2 / ((2 * n) * (2 * n + 1));
        r += sign * term;
        sign = -sign;
    }
    return r;
}
static double c_cos_kernel(double x) {
    double x2 = x * x;
    double r = 1.0;
    double term = 1.0;
    int sign = -1;
    for (int n = 1; n < 9; n++) {
        term *= x2 / ((2 * n - 1) * (2 * n));
        r += sign * term;
        sign = -sign;
    }
    return r;
}
static void c_sincos_reduce(double x, double *s, double *c) {
    /* Reduce x to [-pi, pi] then to one of 4 quadrants of size pi/2. */
    double k = c_floor(x / (2 * C_PI) + 0.5);
    x -= k * 2 * C_PI;
    int neg_s = 0, neg_c = 0, swap = 0;
    if (x < 0) { x = -x; neg_s = 1; }
    if (x > C_PI)        { x = 2*C_PI - x; neg_s ^= 1; }
    if (x > C_PI_2)      { x = C_PI - x;   neg_c = 1; }
    if (x > C_PI_4)      { x = C_PI_2 - x; swap = 1; }
    double sk = c_sin_kernel(x);
    double ck = c_cos_kernel(x);
    if (swap) { double t = sk; sk = ck; ck = t; }
    if (neg_s) sk = -sk;
    if (neg_c) ck = -ck;
    *s = sk; *c = ck;
}
static double c_sin(double x) { double s, c; c_sincos_reduce(x, &s, &c); return s; }
static double c_cos(double x) { double s, c; c_sincos_reduce(x, &s, &c); return c; }
static double c_tan(double x) {
    double s, c;
    c_sincos_reduce(x, &s, &c);
    if (c_fabs(c) < 1e-300) return c < 0 ? -C_HUGE : C_HUGE;
    return s / c;
}

static double c_factorial(double x) {
    if (x < 0 || x != c_floor(x) || x > 170) return 0.0 / 0.0;
    double r = 1.0;
    for (int i = 2; i <= (int)x; i++) r *= i;
    return r;
}

/* ── Number formatting ── */

static int c_u64_dec(uint64_t v, char *out, int max) {
    char tmp[32]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 32) { tmp[n++] = '0' + (v % 10); v /= 10; }
    int o = 0;
    while (n > 0 && o < max - 1) out[o++] = tmp[--n];
    out[o] = '\0';
    return o;
}
static int c_i64_dec(int64_t v, char *out, int max) {
    if (v < 0 && max > 1) {
        out[0] = '-';
        return 1 + c_u64_dec((uint64_t)(-v), out + 1, max - 1);
    }
    return c_u64_dec((uint64_t)v, out, max);
}

static int c_u64_hex(uint64_t v, char *out, int max) {
    char tmp[20]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 20) {
        int d = v & 0xF;
        tmp[n++] = (d < 10) ? '0' + d : 'A' + d - 10;
        v >>= 4;
    }
    int o = 0;
    while (n > 0 && o < max - 1) out[o++] = tmp[--n];
    out[o] = '\0';
    return o;
}
static int c_u64_oct(uint64_t v, char *out, int max) {
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 24) {
        tmp[n++] = '0' + (v & 0x7);
        v >>= 3;
    }
    int o = 0;
    while (n > 0 && o < max - 1) out[o++] = tmp[--n];
    out[o] = '\0';
    return o;
}
static int c_u64_bin(uint64_t v, char *out, int max) {
    char tmp[68]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 64) {
        tmp[n++] = '0' + (int)(v & 1);
        v >>= 1;
    }
    int o = 0;
    while (n > 0 && o < max - 1) out[o++] = tmp[--n];
    out[o] = '\0';
    return o;
}

/* Format a double. If it's integral and small, print as int; else print
 * with up to 12 fractional digits, trimming trailing zeros. */
static int c_fmt_double(double v, char *out, int max) {
    if (v != v) { c_strncpy(out, "NaN", max); return 3; }
    if (v > 1e308) { c_strncpy(out, "inf", max); return 3; }
    if (v < -1e308) { c_strncpy(out, "-inf", max); return 4; }

    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    /* Integer fast-path. */
    if (v == c_floor(v) && v < 1e18) {
        int o = 0;
        if (neg && o < max - 1) out[o++] = '-';
        o += c_u64_dec((uint64_t)v, out + o, max - o);
        return o;
    }
    /* Use scientific if magnitude is extreme. */
    int use_sci = (v != 0 && (v >= 1e16 || v < 1e-4));
    int o = 0;
    if (neg && o < max - 1) out[o++] = '-';
    if (use_sci) {
        /* Normalise to [1, 10). */
        int e = 0;
        while (v >= 10.0) { v /= 10.0; e++; }
        while (v <  1.0)  { v *= 10.0; e--; }
        /* Print with 10 fractional digits. */
        int ip = (int)v;
        if (o < max - 1) out[o++] = '0' + ip;
        if (o < max - 1) out[o++] = '.';
        v -= ip;
        for (int i = 0; i < 10 && o < max - 1; i++) {
            v *= 10.0;
            int d = (int)v;
            if (d < 0) d = 0;
            if (d > 9) d = 9;
            out[o++] = '0' + d;
            v -= d;
        }
        /* Trim trailing zeros. */
        while (o > 0 && out[o - 1] == '0') o--;
        if (o > 0 && out[o - 1] == '.') o--;
        if (o < max - 1) out[o++] = 'e';
        if (e < 0) { if (o < max - 1) out[o++] = '-'; e = -e; }
        else       { if (o < max - 1) out[o++] = '+'; }
        char eb[8]; int en = c_u64_dec((uint64_t)e, eb, 8);
        for (int i = 0; i < en && o < max - 1; i++) out[o++] = eb[i];
        out[o] = '\0';
        return o;
    }
    /* Plain decimal. */
    uint64_t ip = (uint64_t)v;
    o += c_u64_dec(ip, out + o, max - o);
    double frac = v - (double)ip;
    if (frac > 0 && o < max - 1) {
        out[o++] = '.';
        for (int i = 0; i < 12 && o < max - 1; i++) {
            frac *= 10.0;
            int d = (int)frac;
            if (d < 0) d = 0;
            if (d > 9) d = 9;
            out[o++] = '0' + d;
            frac -= d;
        }
        while (o > 0 && out[o - 1] == '0') o--;
        if (o > 0 && out[o - 1] == '.') o--;
    }
    out[o] = '\0';
    return o;
}

/* ── Recursive-descent parser ──
 *
 * Grammar (programmer-mode adds the bitwise/shift operators):
 *   expr      = bor
 *   bor       = bxor ( '|' bxor )*
 *   bxor      = band ( '^^'  band )*       // '^^' avoids clash with power
 *   band      = shift ( '&' shift )*
 *   shift     = add ( ('<<'|'>>') add )*
 *   add       = mul ( ('+'|'-') mul )*
 *   mul       = unary ( ('*'|'/'|'%') unary )*
 *   unary     = ('-'|'+'|'~'|'!') unary | power
 *   power     = postfix ( '^' unary )?
 *   postfix   = atom ( '!' | '%' )*
 *   atom      = number | hex | oct | bin | '(' expr ')' | ident '(' expr ')'
 *
 * Standard/Scientific modes parse '^' as power, no bitwise ops accepted.
 * Programmer mode coerces operands to int64 for bitwise/shift; '%' is mod.
 *
 * We use double-typed values throughout, then on programmer mode we
 * round-trip int64<->double to keep the parser unified. The 64-bit precision
 * of int64 fits in double for inputs up to ~2^53; programmer-mode toggles
 * use the calculator's `bits` register which is true uint64_t.
 */

typedef struct {
    const char *p;
    const char *end;
    calc_mode_t mode;
    int err;
    char errmsg[CALC_ERR_MAX];
} parser_t;

static void p_set_err(parser_t *ps, const char *m) {
    if (ps->err) return;
    ps->err = 1;
    c_strncpy(ps->errmsg, m, CALC_ERR_MAX);
}
static void p_skip_ws(parser_t *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t')) ps->p++;
}
static int p_peek(parser_t *ps) { return ps->p < ps->end ? (unsigned char)*ps->p : 0; }
static int p_peek2(parser_t *ps) { return ps->p + 1 < ps->end ? (unsigned char)ps->p[1] : 0; }

static double parse_expr(parser_t *ps);

static double parse_number(parser_t *ps) {
    /* Detect base prefixes. */
    if (p_peek(ps) == '0' && (p_peek2(ps) == 'x' || p_peek2(ps) == 'X')) {
        ps->p += 2;
        uint64_t v = 0;
        int any = 0;
        while (ps->p < ps->end && c_isxdigit((unsigned char)*ps->p)) {
            int c = c_tolower((unsigned char)*ps->p);
            int d = c_isdigit(c) ? c - '0' : c - 'a' + 10;
            v = (v << 4) | (uint64_t)d;
            ps->p++; any = 1;
        }
        if (!any) p_set_err(ps, "expected hex digits");
        return (double)(int64_t)v;
    }
    if (p_peek(ps) == '0' && (p_peek2(ps) == 'b' || p_peek2(ps) == 'B')) {
        ps->p += 2;
        uint64_t v = 0;
        int any = 0;
        while (ps->p < ps->end && (*ps->p == '0' || *ps->p == '1')) {
            v = (v << 1) | (uint64_t)(*ps->p - '0');
            ps->p++; any = 1;
        }
        if (!any) p_set_err(ps, "expected binary digits");
        return (double)(int64_t)v;
    }
    if (p_peek(ps) == '0' && (p_peek2(ps) == 'o' || p_peek2(ps) == 'O')) {
        ps->p += 2;
        uint64_t v = 0;
        int any = 0;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '7') {
            v = (v << 3) | (uint64_t)(*ps->p - '0');
            ps->p++; any = 1;
        }
        if (!any) p_set_err(ps, "expected octal digits");
        return (double)(int64_t)v;
    }

    /* Decimal / float. */
    double v = 0;
    int any = 0;
    while (ps->p < ps->end && c_isdigit((unsigned char)*ps->p)) {
        v = v * 10.0 + (*ps->p - '0');
        ps->p++; any = 1;
    }
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        double f = 0.1;
        while (ps->p < ps->end && c_isdigit((unsigned char)*ps->p)) {
            v += (*ps->p - '0') * f;
            f *= 0.1;
            ps->p++; any = 1;
        }
    }
    /* Optional exponent */
    if (any && ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        int sign = 1;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) {
            if (*ps->p == '-') sign = -1;
            ps->p++;
        }
        int exp = 0, ea = 0;
        while (ps->p < ps->end && c_isdigit((unsigned char)*ps->p)) {
            exp = exp * 10 + (*ps->p - '0');
            ps->p++; ea = 1;
        }
        if (!ea) p_set_err(ps, "bad exponent");
        else     v *= c_pow(10.0, sign * exp);
    }
    if (!any) p_set_err(ps, "expected number");
    return v;
}

static double parse_atom(parser_t *ps) {
    p_skip_ws(ps);
    if (ps->err) return 0;
    int c = p_peek(ps);
    if (c == 0) { p_set_err(ps, "unexpected end"); return 0; }
    if (c == '(') {
        ps->p++;
        double v = parse_expr(ps);
        p_skip_ws(ps);
        if (p_peek(ps) != ')') { p_set_err(ps, "missing ')'"); return 0; }
        ps->p++;
        return v;
    }
    if (c_isalpha(c)) {
        char name[16]; int n = 0;
        while (ps->p < ps->end && (c_isalpha((unsigned char)*ps->p) ||
                                   c_isdigit((unsigned char)*ps->p))) {
            if (n < 15) name[n++] = c_tolower((unsigned char)*ps->p);
            ps->p++;
        }
        name[n] = '\0';
        /* Constants. */
        if (c_streq(name, "pi"))  return C_PI;
        if (c_streq(name, "e"))   return 2.71828182845904523536;
        if (c_streq(name, "ans")) {
            /* Last result. */
            if (g_calc.hist_count > 0) {
                if (g_calc.hist[0].is_int) return (double)g_calc.hist[0].result_i;
                return g_calc.hist[0].result_d;
            }
            return 0;
        }
        /* Function call. */
        p_skip_ws(ps);
        if (p_peek(ps) != '(') { p_set_err(ps, "expected '(' after function"); return 0; }
        ps->p++;
        double a = parse_expr(ps);
        p_skip_ws(ps);
        if (p_peek(ps) != ')') { p_set_err(ps, "missing ')'"); return 0; }
        ps->p++;
        if (c_streq(name, "sin")) return c_sin(a);
        if (c_streq(name, "cos")) return c_cos(a);
        if (c_streq(name, "tan")) return c_tan(a);
        if (c_streq(name, "log")) {
            if (a <= 0) { p_set_err(ps, "log domain"); return 0; }
            return c_log10(a);
        }
        if (c_streq(name, "ln"))  {
            if (a <= 0) { p_set_err(ps, "ln domain"); return 0; }
            return c_ln(a);
        }
        if (c_streq(name, "exp")) return c_exp(a);
        if (c_streq(name, "sqrt")){
            if (a < 0) { p_set_err(ps, "sqrt domain"); return 0; }
            return c_sqrt(a);
        }
        if (c_streq(name, "abs")) return c_fabs(a);
        p_set_err(ps, "unknown function");
        return 0;
    }
    if (c_isdigit(c) || c == '.') {
        return parse_number(ps);
    }
    p_set_err(ps, "unexpected character");
    return 0;
}

static double parse_postfix(parser_t *ps) {
    double v = parse_atom(ps);
    while (!ps->err) {
        p_skip_ws(ps);
        int c = p_peek(ps);
        if (c == '!') {
            /* Don't confuse with logical-NOT — that's a unary prefix. */
            ps->p++;
            v = c_factorial(v);
            if (v != v) { p_set_err(ps, "factorial domain"); return 0; }
            continue;
        }
        if (c == '%' && ps->mode != CALC_PROGRAMMER) {
            /* Postfix percent: 50% → 0.5. Programmer mode reserves '%'
             * as the modulo binary op (handled in parse_mul). */
            ps->p++;
            v = v / 100.0;
            continue;
        }
        break;
    }
    return v;
}

static double parse_unary(parser_t *ps) {
    p_skip_ws(ps);
    int c = p_peek(ps);
    if (c == '-') { ps->p++; return -parse_unary(ps); }
    if (c == '+') { ps->p++; return  parse_unary(ps); }
    if (c == '~' && ps->mode == CALC_PROGRAMMER) {
        ps->p++;
        double v = parse_unary(ps);
        return (double)(~(int64_t)v);
    }
    return parse_postfix(ps);
}

static double parse_power(parser_t *ps) {
    double a = parse_unary(ps);
    p_skip_ws(ps);
    if (!ps->err && p_peek(ps) == '^' && ps->mode != CALC_PROGRAMMER) {
        ps->p++;
        double b = parse_unary(ps);
        return c_pow(a, b);
    }
    return a;
}

static double parse_mul(parser_t *ps) {
    double a = parse_power(ps);
    while (!ps->err) {
        p_skip_ws(ps);
        int c = p_peek(ps);
        if (c == '*') { ps->p++; a *= parse_power(ps); }
        else if (c == '/') {
            ps->p++;
            double b = parse_power(ps);
            if (b == 0) { p_set_err(ps, "divide by zero"); return 0; }
            if (ps->mode == CALC_PROGRAMMER) {
                int64_t ai = (int64_t)a, bi = (int64_t)b;
                a = (double)(ai / bi);
            } else {
                a /= b;
            }
        }
        else if (c == '%' && ps->mode == CALC_PROGRAMMER) {
            ps->p++;
            double b = parse_power(ps);
            int64_t bi = (int64_t)b;
            if (bi == 0) { p_set_err(ps, "mod by zero"); return 0; }
            int64_t ai = (int64_t)a;
            a = (double)(ai % bi);
        }
        else break;
    }
    return a;
}

static double parse_add(parser_t *ps) {
    double a = parse_mul(ps);
    while (!ps->err) {
        p_skip_ws(ps);
        int c = p_peek(ps);
        if (c == '+') { ps->p++; a += parse_mul(ps); }
        else if (c == '-') { ps->p++; a -= parse_mul(ps); }
        else break;
    }
    return a;
}

static double parse_shift(parser_t *ps) {
    double a = parse_add(ps);
    if (ps->mode != CALC_PROGRAMMER) return a;
    while (!ps->err) {
        p_skip_ws(ps);
        int c = p_peek(ps), c2 = p_peek2(ps);
        if (c == '<' && c2 == '<') {
            ps->p += 2;
            double b = parse_add(ps);
            int sh = (int)b & 63;
            a = (double)((int64_t)((uint64_t)(int64_t)a << sh));
        } else if (c == '>' && c2 == '>') {
            ps->p += 2;
            double b = parse_add(ps);
            int sh = (int)b & 63;
            a = (double)((int64_t)((uint64_t)(int64_t)a >> sh));
        } else break;
    }
    return a;
}

static double parse_band(parser_t *ps) {
    double a = parse_shift(ps);
    if (ps->mode != CALC_PROGRAMMER) return a;
    while (!ps->err) {
        p_skip_ws(ps);
        if (p_peek(ps) == '&' && p_peek2(ps) != '&') {
            ps->p++;
            double b = parse_shift(ps);
            a = (double)((int64_t)a & (int64_t)b);
        } else break;
    }
    return a;
}

static double parse_bxor(parser_t *ps) {
    double a = parse_band(ps);
    if (ps->mode != CALC_PROGRAMMER) return a;
    while (!ps->err) {
        p_skip_ws(ps);
        /* In programmer mode '^' is XOR (no power conflict because power
         * is rejected in unary above). */
        if (p_peek(ps) == '^') {
            ps->p++;
            double b = parse_band(ps);
            a = (double)((int64_t)a ^ (int64_t)b);
        } else break;
    }
    return a;
}

static double parse_bor(parser_t *ps) {
    double a = parse_bxor(ps);
    if (ps->mode != CALC_PROGRAMMER) return a;
    while (!ps->err) {
        p_skip_ws(ps);
        if (p_peek(ps) == '|' && p_peek2(ps) != '|') {
            ps->p++;
            double b = parse_bxor(ps);
            a = (double)((int64_t)a | (int64_t)b);
        } else break;
    }
    return a;
}

static double parse_expr(parser_t *ps) { return parse_bor(ps); }

/* ── Public eval API ── */

int calculator_eval(const char *expr, calc_mode_t mode,
                    double *out, char *err, int err_max)
{
    if (!expr || !*expr) { if (err) c_strncpy(err, "empty", err_max); return 0; }
    parser_t ps;
    ps.p = expr;
    ps.end = expr + c_strlen(expr);
    ps.mode = mode;
    ps.err = 0;
    ps.errmsg[0] = '\0';

    double v = parse_expr(&ps);
    p_skip_ws(&ps);
    if (!ps.err && ps.p != ps.end) p_set_err(&ps, "trailing garbage");
    if (ps.err) {
        if (err) c_strncpy(err, ps.errmsg, err_max);
        return 0;
    }
    if (out) *out = v;
    return 1;
}

/* ── History ── */

static void hist_push(const char *expr, double dv, int64_t iv, int is_int, int ok) {
    /* Shift down. */
    for (int i = CALC_HIST_LINES - 1; i > 0; i--) {
        g_calc.hist[i] = g_calc.hist[i - 1];
    }
    calc_hist_t *h = &g_calc.hist[0];
    c_strncpy(h->expr, expr, CALC_INPUT_MAX);
    h->result_d = dv;
    h->result_i = iv;
    h->is_int   = is_int;
    h->ok       = ok;
    if (g_calc.hist_count < CALC_HIST_LINES) g_calc.hist_count++;
}

/* ── Live preview & evaluate ── */

static void calc_recompute_preview(void) {
    g_calc.preview_ok = 0;
    g_calc.preview[0] = '\0';
    if (g_calc.input_len == 0) return;

    double v;
    char err[CALC_ERR_MAX];
    int ok = calculator_eval(g_calc.input, g_calc.mode, &v, err, CALC_ERR_MAX);
    if (!ok) return;
    g_calc.preview_ok = 1;
    if (g_calc.mode == CALC_PROGRAMMER) {
        g_calc.preview_is_int = 1;
        g_calc.preview_i = (int64_t)v;
        c_i64_dec(g_calc.preview_i, g_calc.preview, CALC_PREVIEW_MAX);
    } else {
        g_calc.preview_is_int = 0;
        g_calc.preview_d = v;
        c_fmt_double(v, g_calc.preview, CALC_PREVIEW_MAX);
    }
}

static void calc_commit_eval(void) {
    if (g_calc.input_len == 0) return;
    double v;
    char err[CALC_ERR_MAX];
    int ok = calculator_eval(g_calc.input, g_calc.mode, &v, err, CALC_ERR_MAX);
    if (ok) {
        if (g_calc.mode == CALC_PROGRAMMER) {
            int64_t iv = (int64_t)v;
            hist_push(g_calc.input, (double)iv, iv, 1, 1);
            g_calc.bits = (uint64_t)iv;
        } else {
            hist_push(g_calc.input, v, 0, 0, 1);
        }
        g_total_evals_ok++;
    } else {
        hist_push(g_calc.input, 0, 0, 0, 0);
        g_total_evals_fail++;
    }
    g_calc.hist_browse = -1;
    /* Clear input on Enter. */
    g_calc.input[0] = '\0';
    g_calc.input_len = 0;
    g_calc.cursor = 0;
    calc_recompute_preview();
    dirty_clear(CALC_WIN_ID);
}

/* ── Input editing ── */

static void calc_input_insert(char c) {
    if (g_calc.input_len >= CALC_INPUT_MAX - 1) return;
    /* Shift right from cursor. */
    for (int i = g_calc.input_len; i > g_calc.cursor; i--) {
        g_calc.input[i] = g_calc.input[i - 1];
    }
    g_calc.input[g_calc.cursor] = c;
    g_calc.input_len++;
    g_calc.cursor++;
    g_calc.input[g_calc.input_len] = '\0';
    dirty_register(CALC_WIN_ID);
    calc_recompute_preview();
}

static void calc_input_backspace(void) {
    if (g_calc.cursor == 0) return;
    for (int i = g_calc.cursor - 1; i < g_calc.input_len; i++) {
        g_calc.input[i] = g_calc.input[i + 1];
    }
    g_calc.input_len--;
    g_calc.cursor--;
    if (g_calc.input_len == 0) dirty_clear(CALC_WIN_ID);
    calc_recompute_preview();
}

static void calc_input_clear(void) {
    g_calc.input[0] = '\0';
    g_calc.input_len = 0;
    g_calc.cursor = 0;
    g_calc.preview[0] = '\0';
    g_calc.preview_ok = 0;
    dirty_clear(CALC_WIN_ID);
}

static void calc_recall_hist(int idx) {
    if (idx < 0 || idx >= g_calc.hist_count) return;
    c_strncpy(g_calc.input, g_calc.hist[idx].expr, CALC_INPUT_MAX);
    g_calc.input_len = c_strlen(g_calc.input);
    g_calc.cursor = g_calc.input_len;
    calc_recompute_preview();
    dirty_register(CALC_WIN_ID);
}

/* ── Layout helpers ── */

static void calc_layout(void) {
    int sw = (int)fb_width(), sh = (int)fb_height();
    g_calc.wx = (sw - CALC_W) / 2;
    g_calc.wy = (sh - CALC_H) / 2;
    if (g_calc.wx < 0) g_calc.wx = 0;
    if (g_calc.wy < 0) g_calc.wy = 0;
}

static int calc_in_window(int x, int y) {
    return (x >= g_calc.wx && x < g_calc.wx + CALC_W &&
            y >= g_calc.wy && y < g_calc.wy + CALC_H);
}
static int calc_in_close_btn(int x, int y) {
    int bx = g_calc.wx + CALC_W - 30, by = g_calc.wy + 6;
    return (x >= bx && x < bx + 24 && y >= by && y < by + 20);
}
static int calc_tab_rect(int idx, int *bx, int *by, int *bw, int *bh) {
    int tab_w = 100;
    *bx = g_calc.wx + CALC_PAD + idx * (tab_w + 4);
    *by = g_calc.wy + CALC_TITLE_H + 2;
    *bw = tab_w;
    *bh = CALC_TAB_H - 4;
    return 1;
}
static int calc_in_tab(int idx, int x, int y) {
    int bx, by, bw, bh;
    calc_tab_rect(idx, &bx, &by, &bw, &bh);
    return (x >= bx && x < bx + bw && y >= by && y < by + bh);
}

/* Keypad button grid. Rows depend on mode; we render 6 rows of 5 buttons
 * for STANDARD/SCIENTIFIC, with SCIENTIFIC swapping the leftmost column for
 * sin/cos/tan/log/ln. PROGRAMMER also has 64 bit toggles above the keypad. */

#define BTN_COLS 5
#define BTN_ROWS 6

typedef struct {
    const char *label;
    int        kind;   /* 0=insert literal, 1=clear, 2=back, 3=eq, 4=insert function with '(' */
    const char *text;  /* what to insert (kind 0/4) */
} calc_btn_t;

static const calc_btn_t BTN_STD[BTN_ROWS][BTN_COLS] = {
    {{"C",1,""},{"(",0,"("},{")",0,")"},{"sqrt",4,"sqrt"},{"\xe2\x90\xa3",2,""}},
    {{"7",0,"7"},{"8",0,"8"},{"9",0,"9"},{"/",0,"/"},{"%",0,"%"}},
    {{"4",0,"4"},{"5",0,"5"},{"6",0,"6"},{"*",0,"*"},{"^",0,"^"}},
    {{"1",0,"1"},{"2",0,"2"},{"3",0,"3"},{"-",0,"-"},{"ans",0,"ans"}},
    {{"0",0,"0"},{".",0,"."},{"+",0,"+"},{"=",3,""},{"pi",0,"pi"}},
    {{"e",0,"e"},{"!",0,"!"},{"abs",4,"abs"},{",",0,","},{" ",0," "}},
};

static const calc_btn_t BTN_SCI[BTN_ROWS][BTN_COLS] = {
    {{"C",1,""},{"(",0,"("},{")",0,")"},{"^",0,"^"},{"\xe2\x90\xa3",2,""}},
    {{"sin",4,"sin"},{"cos",4,"cos"},{"tan",4,"tan"},{"log",4,"log"},{"ln",4,"ln"}},
    {{"7",0,"7"},{"8",0,"8"},{"9",0,"9"},{"/",0,"/"},{"exp",4,"exp"}},
    {{"4",0,"4"},{"5",0,"5"},{"6",0,"6"},{"*",0,"*"},{"sqrt",4,"sqrt"}},
    {{"1",0,"1"},{"2",0,"2"},{"3",0,"3"},{"-",0,"-"},{"!",0,"!"}},
    {{"0",0,"0"},{".",0,"."},{"+",0,"+"},{"=",3,""},{"pi",0,"pi"}},
};

static const calc_btn_t BTN_PRG[BTN_ROWS][BTN_COLS] = {
    {{"C",1,""},{"(",0,"("},{")",0,")"},{"~",0,"~"},{"\xe2\x90\xa3",2,""}},
    {{"A",0,"A"},{"B",0,"B"},{"C",0,"C"},{"<<",0,"<<"},{">>",0,">>"}},
    {{"D",0,"D"},{"E",0,"E"},{"F",0,"F"},{"&",0,"&"},{"|",0,"|"}},
    {{"7",0,"7"},{"8",0,"8"},{"9",0,"9"},{"^",0,"^"},{"%",0,"%"}},
    {{"4",0,"4"},{"5",0,"5"},{"6",0,"6"},{"*",0,"*"},{"/",0,"/"}},
    {{"1",0,"1"},{"2",0,"2"},{"3",0,"3"},{"-",0,"-"},{"+",0,"+"}},
};

/* Note: programmer mode "C" hex digit clashes with "C" clear; we render
 * clear on row 0 col 0 (kind 1) and the hex C separately on row 1 col 2. */

static const calc_btn_t *btn_grid_for_mode(calc_mode_t m) {
    switch (m) {
        case CALC_SCIENTIFIC: return &BTN_SCI[0][0];
        case CALC_PROGRAMMER: return &BTN_PRG[0][0];
        case CALC_STANDARD:
        default:              return &BTN_STD[0][0];
    }
}

static void calc_btn_rect(int row, int col, int *bx, int *by, int *bw, int *bh) {
    int grid_x = g_calc.wx + CALC_PAD;
    int grid_w = CALC_W - CALC_HIST_W - 3 * CALC_PAD;
    int grid_y = g_calc.wy + CALC_TITLE_H + CALC_TAB_H + CALC_DISPLAY_H + CALC_PAD;
    int grid_h = CALC_H - (CALC_TITLE_H + CALC_TAB_H + CALC_DISPLAY_H + CALC_PAD * 2);
    if (g_calc.mode == CALC_PROGRAMMER) grid_h -= 110; /* room for bit panel */
    int gap = 4;
    int cell_w = (grid_w - gap * (BTN_COLS - 1)) / BTN_COLS;
    int cell_h = (grid_h - gap * (BTN_ROWS - 1)) / BTN_ROWS;
    *bx = grid_x + col * (cell_w + gap);
    *by = grid_y + row * (cell_h + gap);
    *bw = cell_w;
    *bh = cell_h;
}

static int calc_in_btn(int row, int col, int x, int y) {
    int bx, by, bw, bh;
    calc_btn_rect(row, col, &bx, &by, &bw, &bh);
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

/* Bit toggle row layout (programmer mode only, 64 bits, 2 rows of 32). */
static void calc_bit_rect(int bit, int *bx, int *by, int *bw, int *bh) {
    int row = bit < 32 ? 0 : 1;
    int col = bit & 31;
    int panel_x = g_calc.wx + CALC_PAD;
    int panel_w = CALC_W - CALC_HIST_W - 3 * CALC_PAD;
    int panel_y = g_calc.wy + CALC_H - CALC_PAD - 56;
    int cell_w = panel_w / 32;
    int cell_h = 22;
    /* bit 0 displayed rightmost. */
    int draw_col = 31 - col;
    *bx = panel_x + draw_col * cell_w;
    *by = panel_y + row * (cell_h + 4);
    *bw = cell_w - 2;
    *bh = cell_h;
}
static int calc_in_bit(int bit, int x, int y) {
    int bx, by, bw, bh;
    calc_bit_rect(bit, &bx, &by, &bw, &bh);
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

/* History line rect. */
static int calc_hist_line_rect(int idx, int *bx, int *by, int *bw, int *bh) {
    int hx = g_calc.wx + CALC_W - CALC_HIST_W - CALC_PAD;
    int hy = g_calc.wy + CALC_TITLE_H + CALC_TAB_H + CALC_PAD;
    *bx = hx;
    *by = hy + 24 + idx * 36;  /* 24px header + per-line height */
    *bw = CALC_HIST_W;
    *bh = 32;
    return 1;
}
static int calc_in_hist(int idx, int x, int y) {
    int bx, by, bw, bh;
    calc_hist_line_rect(idx, &bx, &by, &bw, &bh);
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

/* ── Hover registration ── */

static void calc_register_hover_zones(void) {
    /* Clear and re-register on every layout change. We don't persist
     * tokens across layout; the hover registry tolerates redundant
     * unregisters via tokens going stale (we only register fresh). */
    if (g_calc.hover_close) hover_unregister(g_calc.hover_close);
    for (int i = 0; i < 3; i++) if (g_calc.hover_tab[i]) hover_unregister(g_calc.hover_tab[i]);
    g_calc.hover_close = 0;
    for (int i = 0; i < 3; i++) g_calc.hover_tab[i] = 0;

    if (!g_calc.active) return;

    int bx = g_calc.wx + CALC_W - 30, by = g_calc.wy + 6;
    g_calc.hover_close = hover_register(bx, by, 24, 20,
                                        HOVER_CURSOR_POINTER, 0, 0);
    for (int i = 0; i < 3; i++) {
        int tx, ty, tw, th;
        calc_tab_rect(i, &tx, &ty, &tw, &th);
        g_calc.hover_tab[i] = hover_register(tx, ty, tw, th,
                                             HOVER_CURSOR_POINTER, 0, 0);
    }
    /* Register button grid + bit panel + history rows as pointer zones. */
    for (int r = 0; r < BTN_ROWS; r++) {
        for (int c = 0; c < BTN_COLS; c++) {
            int rx, ry, rw, rh;
            calc_btn_rect(r, c, &rx, &ry, &rw, &rh);
            hover_register(rx, ry, rw, rh, HOVER_CURSOR_POINTER, 0, 0);
        }
    }
    if (g_calc.mode == CALC_PROGRAMMER) {
        for (int b = 0; b < 64; b++) {
            int rx, ry, rw, rh;
            calc_bit_rect(b, &rx, &ry, &rw, &rh);
            hover_register(rx, ry, rw, rh, HOVER_CURSOR_POINTER, 0, 0);
        }
    }
    for (int i = 0; i < g_calc.hist_count; i++) {
        int rx, ry, rw, rh;
        calc_hist_line_rect(i, &rx, &ry, &rw, &rh);
        hover_register(rx, ry, rw, rh, HOVER_CURSOR_POINTER, 0, 0);
    }
}

/* ── Public API ── */

void calculator_open(void)
{
    if (g_calc.active) {
        compositor_dirty(g_calc.wx, g_calc.wy, CALC_W, CALC_H);
        return;
    }
    g_calc.active = 1;
    g_calc.mode = CALC_STANDARD;
    g_calc.input[0] = '\0';
    g_calc.input_len = 0;
    g_calc.cursor = 0;
    g_calc.preview[0] = '\0';
    g_calc.preview_ok = 0;
    g_calc.hist_browse = -1;
    g_calc.bits = 0;
    if (!g_calc.undo_inited) {
        undo_init(&g_calc.undo, 0, 0);
        g_calc.undo_inited = 1;
    }
    calc_layout();
    calc_register_hover_zones();
    g_total_opens++;
    compositor_dirty(g_calc.wx, g_calc.wy, CALC_W, CALC_H);
}

static void calc_close_modal_cb(int wid, dirty_response_t r, void *ctx) {
    (void)wid; (void)ctx;
    if (r == DIRTY_CANCEL) return;
    /* Save is treated like Discard — no persistence layer for the calc;
     * the user gets to keep the input in history if they prefer Save. */
    if (r == DIRTY_SAVE && g_calc.input_len > 0) {
        calc_commit_eval();
    }
    g_calc.active = 0;
    g_calc.input[0] = '\0';
    g_calc.input_len = 0;
    g_calc.cursor = 0;
    dirty_clear(CALC_WIN_ID);
    int sw = (int)fb_width(), sh = (int)fb_height();
    compositor_dirty(0, 0, sw, sh);
}

void calculator_close(void)
{
    if (!g_calc.active) return;
    if (dirty_get(CALC_WIN_ID) && g_calc.input_len > 0) {
        dirty_open_close_modal(CALC_WIN_ID,
                               "Discard unfinished expression?",
                               calc_close_modal_cb, 0);
        return;
    }
    g_calc.active = 0;
    int sw = (int)fb_width(), sh = (int)fb_height();
    compositor_dirty(0, 0, sw, sh);
}

int calculator_active(void) { return g_calc.active; }

void calculator_set_mode(calc_mode_t mode) {
    if (mode != CALC_STANDARD && mode != CALC_SCIENTIFIC && mode != CALC_PROGRAMMER) return;
    g_calc.mode = mode;
    calc_recompute_preview();
    calc_register_hover_zones();
    compositor_dirty(g_calc.wx, g_calc.wy, CALC_W, CALC_H);
}
calc_mode_t calculator_get_mode(void) { return g_calc.mode; }

/* ── Input ── */

static void calc_apply_button(const calc_btn_t *b) {
    switch (b->kind) {
    case 0:  /* literal insert */
        for (int i = 0; b->text[i]; i++) calc_input_insert(b->text[i]);
        break;
    case 1:  /* clear */
        calc_input_clear();
        break;
    case 2:  /* backspace */
        calc_input_backspace();
        break;
    case 3:  /* equals */
        calc_commit_eval();
        break;
    case 4:  /* function */
        for (int i = 0; b->text[i]; i++) calc_input_insert(b->text[i]);
        calc_input_insert('(');
        break;
    }
    compositor_dirty(g_calc.wx, g_calc.wy, CALC_W, CALC_H);
}

int calculator_key(int ascii) {
    if (!g_calc.active) return 0;
    /* Esc */
    if (ascii == 27) { calculator_close(); return 1; }
    /* Enter (CR or LF) */
    if (ascii == '\n' || ascii == '\r') { calc_commit_eval(); compositor_dirty(g_calc.wx, g_calc.wy, CALC_W, CALC_H); return 1; }
    /* Backspace */
    if (ascii == 8 || ascii == 127) { calc_input_backspace(); compositor_dirty(g_calc.wx, g_calc.wy, CALC_W, CALC_H); return 1; }
    /* Mode switch via Ctrl+1/2/3 — keyboard.c routes Ctrl+digit through
     * keybinds normally; we accept bare 1/2/3 only when input is empty
     * AND the user just opened the calc. Better: dedicated ascii guards
     * via Ctrl modifier — we expose them via direct ASCII codes 0x11/0x12/0x13
     * which the keybinds layer can map. For now treat F1/F2/F3-equivalent
     * ascii (0x11/0x12/0x13) as mode switches. */
    if (ascii == 0x11) { calculator_set_mode(CALC_STANDARD);   return 1; }
    if (ascii == 0x12) { calculator_set_mode(CALC_SCIENTIFIC); return 1; }
    if (ascii == 0x13) { calculator_set_mode(CALC_PROGRAMMER); return 1; }

    /* Printable: digits, operators, parens, alpha (for hex digits / function names if user
     * types them by hand), '.', whitespace. */
    if (ascii >= 32 && ascii < 127) {
        calc_input_insert((char)ascii);
        compositor_dirty(g_calc.wx, g_calc.wy, CALC_W, CALC_H);
        return 1;
    }
    return 1;  /* swallow */
}

int calculator_key_arrow(int dir) {
    if (!g_calc.active) return 0;
    if (dir == 0) {  /* left */
        if (g_calc.cursor > 0) g_calc.cursor--;
    } else if (dir == 1) {  /* right */
        if (g_calc.cursor < g_calc.input_len) g_calc.cursor++;
    } else if (dir == 2) {  /* up — older history */
        if (g_calc.hist_count == 0) return 1;
        if (g_calc.hist_browse < g_calc.hist_count - 1) {
            g_calc.hist_browse++;
            calc_recall_hist(g_calc.hist_browse);
        }
    } else if (dir == 3) {  /* down — newer history */
        if (g_calc.hist_browse > 0) {
            g_calc.hist_browse--;
            calc_recall_hist(g_calc.hist_browse);
        } else if (g_calc.hist_browse == 0) {
            g_calc.hist_browse = -1;
            calc_input_clear();
        }
    }
    compositor_dirty(g_calc.wx, g_calc.wy, CALC_W, CALC_H);
    return 1;
}

int calculator_mouse_down(int x, int y, int button) {
    if (!g_calc.active) return 0;
    (void)button;
    if (!calc_in_window(x, y)) {
        /* Clicking outside the window is a soft close attempt — but unlike
         * image_viewer we DON'T close on outside-click because the user
         * may have unsaved input. Just consume to keep focus. */
        return 1;
    }
    if (calc_in_close_btn(x, y)) {
        calculator_close();
        return 1;
    }
    /* Tabs */
    for (int i = 0; i < 3; i++) {
        if (calc_in_tab(i, x, y)) {
            calculator_set_mode((calc_mode_t)i);
            return 1;
        }
    }
    /* Buttons */
    const calc_btn_t *grid = btn_grid_for_mode(g_calc.mode);
    for (int r = 0; r < BTN_ROWS; r++) {
        for (int c = 0; c < BTN_COLS; c++) {
            if (calc_in_btn(r, c, x, y)) {
                const calc_btn_t *b = &grid[r * BTN_COLS + c];
                calc_apply_button(b);
                return 1;
            }
        }
    }
    /* Bit toggles (programmer mode). */
    if (g_calc.mode == CALC_PROGRAMMER) {
        for (int b = 0; b < 64; b++) {
            if (calc_in_bit(b, x, y)) {
                g_calc.bits ^= ((uint64_t)1 << b);
                /* Replace input with the new value as decimal so user can
                 * continue editing. */
                char tmp[32];
                c_i64_dec((int64_t)g_calc.bits, tmp, sizeof(tmp));
                c_strncpy(g_calc.input, tmp, CALC_INPUT_MAX);
                g_calc.input_len = c_strlen(g_calc.input);
                g_calc.cursor = g_calc.input_len;
                dirty_register(CALC_WIN_ID);
                calc_recompute_preview();
                compositor_dirty(g_calc.wx, g_calc.wy, CALC_W, CALC_H);
                return 1;
            }
        }
    }
    /* History click → recall on mouse-up to feel responsive without
     * stealing on accidental drags. We commit immediately here for simplicity. */
    for (int i = 0; i < g_calc.hist_count; i++) {
        if (calc_in_hist(i, x, y)) {
            calc_recall_hist(i);
            compositor_dirty(g_calc.wx, g_calc.wy, CALC_W, CALC_H);
            return 1;
        }
    }
    return 1;
}

int calculator_mouse_up(int x, int y, int button) {
    if (!g_calc.active) return 0;
    (void)x; (void)y; (void)button;
    return 1;
}

int calculator_mouse_move(int x, int y) {
    if (!g_calc.active) return 0;
    (void)x; (void)y;
    return 0;  /* let hover_dispatch run for cursor changes */
}

/* ── Drawing ── */

static const char *mode_label(calc_mode_t m) {
    switch (m) {
        case CALC_STANDARD:   return "Standard";
        case CALC_SCIENTIFIC: return "Scientific";
        case CALC_PROGRAMMER: return "Programmer";
    }
    return "?";
}

static void draw_button(int bx, int by, int bw, int bh, const char *label, uint32_t bg, uint32_t fg) {
    fb_rect(bx, by, bw, bh, bg);
    fb_rect_outline(bx, by, bw, bh, COLOR_SEPARATOR, 1);
    int tw = c_strlen(label) * 8;
    fb_text(bx + (bw - tw) / 2, by + (bh - 8) / 2, label, fg);
}

void calculator_draw(void)
{
    if (!g_calc.active) return;
    calc_layout();

    int sw = (int)fb_width(), sh = (int)fb_height();
    fb_rect_blend(0, 0, sw, sh, 0xC0000000);

    /* Card */
    fb_rect(g_calc.wx, g_calc.wy, CALC_W, CALC_H, COLOR_SURFACE_HIGH);
    fb_rect_outline(g_calc.wx, g_calc.wy, CALC_W, CALC_H, COLOR_SEPARATOR, 1);

    /* Title bar */
    fb_rect(g_calc.wx, g_calc.wy, CALC_W, CALC_TITLE_H, COLOR_SURFACE_TOP);
    fb_text(g_calc.wx + CALC_PAD, g_calc.wy + 9, "Calculator", COLOR_ON_SURFACE);
    /* Close [X] */
    int cbx = g_calc.wx + CALC_W - 30, cby = g_calc.wy + 6;
    fb_rect(cbx, cby, 24, 20, COLOR_SURFACE_HIGH);
    fb_rect_outline(cbx, cby, 24, 20, COLOR_SEPARATOR, 1);
    fb_text(cbx + 8, cby + 3, "X", COLOR_ON_SURFACE);

    /* Tabs */
    for (int i = 0; i < 3; i++) {
        int tx, ty, tw, th;
        calc_tab_rect(i, &tx, &ty, &tw, &th);
        uint32_t bg = (g_calc.mode == (calc_mode_t)i) ? COLOR_PRIMARY : COLOR_SURFACE;
        uint32_t fg = (g_calc.mode == (calc_mode_t)i) ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_2;
        draw_button(tx, ty, tw, th, mode_label((calc_mode_t)i), bg, fg);
    }

    /* Display panel */
    int dx = g_calc.wx + CALC_PAD;
    int dy = g_calc.wy + CALC_TITLE_H + CALC_TAB_H + 4;
    int dw = CALC_W - CALC_HIST_W - 3 * CALC_PAD;
    int dh = CALC_DISPLAY_H - 8;
    fb_rect(dx, dy, dw, dh, COLOR_SURFACE);
    fb_rect_outline(dx, dy, dw, dh, COLOR_SEPARATOR, 1);

    /* Expression line (cursor as '|'). */
    {
        char line[CALC_INPUT_MAX + 4];
        int o = 0, i;
        for (i = 0; i < g_calc.input_len && o < (int)sizeof(line) - 4; i++) {
            if (i == g_calc.cursor && o < (int)sizeof(line) - 1) line[o++] = '|';
            line[o++] = g_calc.input[i];
        }
        if (g_calc.cursor == g_calc.input_len && o < (int)sizeof(line) - 1) line[o++] = '|';
        if (o == 0) {
            const char *ph = "Type an expression…";
            for (i = 0; ph[i] && o < (int)sizeof(line) - 1; i++) line[o++] = ph[i];
            line[o] = '\0';
            fb_text(dx + 8, dy + 8, line, COLOR_ON_SURFACE_3);
        } else {
            line[o] = '\0';
            fb_text(dx + 8, dy + 8, line, COLOR_ON_SURFACE);
        }
    }

    /* Preview line */
    if (g_calc.preview_ok) {
        char buf[160];
        int o = 0;
        const char *eq = "= ";
        for (int i = 0; eq[i] && o < 158; i++) buf[o++] = eq[i];
        for (int i = 0; g_calc.preview[i] && o < 158; i++) buf[o++] = g_calc.preview[i];
        buf[o] = '\0';
        fb_text(dx + 8, dy + 30, buf, COLOR_PRIMARY);
    } else if (g_calc.input_len > 0) {
        fb_text(dx + 8, dy + 30, "= …", COLOR_ON_SURFACE_3);
    }

    /* Programmer mode: bases line under the preview. */
    if (g_calc.mode == CALC_PROGRAMMER && g_calc.preview_ok) {
        char hex[24], oct[28], bin[72];
        c_u64_hex((uint64_t)g_calc.preview_i, hex, sizeof(hex));
        c_u64_oct((uint64_t)g_calc.preview_i, oct, sizeof(oct));
        c_u64_bin((uint64_t)g_calc.preview_i, bin, sizeof(bin));
        char line[200];
        int o = 0;
        const char *h = "HEX 0x";
        for (int i = 0; h[i] && o < 198; i++) line[o++] = h[i];
        for (int i = 0; hex[i] && o < 198; i++) line[o++] = hex[i];
        const char *sep = "  OCT 0o";
        for (int i = 0; sep[i] && o < 198; i++) line[o++] = sep[i];
        for (int i = 0; oct[i] && o < 198; i++) line[o++] = oct[i];
        line[o] = '\0';
        fb_text(dx + 8, dy + 50, line, COLOR_ON_SURFACE_2);
    }

    /* Keypad */
    const calc_btn_t *grid = btn_grid_for_mode(g_calc.mode);
    for (int r = 0; r < BTN_ROWS; r++) {
        for (int c = 0; c < BTN_COLS; c++) {
            int bx, by, bw, bh;
            calc_btn_rect(r, c, &bx, &by, &bw, &bh);
            const calc_btn_t *b = &grid[r * BTN_COLS + c];
            uint32_t bg = COLOR_SURFACE;
            uint32_t fg = COLOR_ON_SURFACE;
            if (b->kind == 3) { bg = COLOR_PRIMARY; fg = COLOR_ON_SURFACE; }
            else if (b->kind == 1 || b->kind == 2) { bg = COLOR_SURFACE_TOP; }
            draw_button(bx, by, bw, bh, b->label, bg, fg);
        }
    }

    /* Programmer bit panel */
    if (g_calc.mode == CALC_PROGRAMMER) {
        uint64_t v = (uint64_t)(int64_t)(g_calc.preview_ok ? g_calc.preview_i :
                                         (g_calc.hist_count > 0 ? g_calc.hist[0].result_i : 0));
        if (g_calc.bits) v = g_calc.bits;  /* show explicit toggles */
        for (int b = 0; b < 64; b++) {
            int bx, by, bw, bh;
            calc_bit_rect(b, &bx, &by, &bw, &bh);
            int set = (v >> b) & 1;
            uint32_t bg = set ? COLOR_PRIMARY : COLOR_SURFACE;
            uint32_t fg = set ? COLOR_ON_SURFACE : COLOR_ON_SURFACE_3;
            char lbl[2] = { (char)('0' + set), 0 };
            draw_button(bx, by, bw, bh, lbl, bg, fg);
            /* Bit index marker every 8 bits, drawn under the toggle. */
            if ((b & 7) == 0) {
                char idx[4];
                c_u64_dec((uint64_t)b, idx, sizeof(idx));
                fb_text(bx, by + bh + 1, idx, COLOR_ON_SURFACE_4);
            }
        }
    }

    /* History panel */
    {
        int hx = g_calc.wx + CALC_W - CALC_HIST_W - CALC_PAD;
        int hy = g_calc.wy + CALC_TITLE_H + CALC_TAB_H + CALC_PAD;
        int hh = CALC_H - (CALC_TITLE_H + CALC_TAB_H + CALC_PAD * 2);
        fb_rect(hx, hy, CALC_HIST_W, hh, COLOR_SURFACE);
        fb_rect_outline(hx, hy, CALC_HIST_W, hh, COLOR_SEPARATOR, 1);
        fb_text(hx + 8, hy + 6, "History", COLOR_ON_SURFACE_2);
        for (int i = 0; i < g_calc.hist_count; i++) {
            int bx, by, bw, bh;
            calc_hist_line_rect(i, &bx, &by, &bw, &bh);
            calc_hist_t *h = &g_calc.hist[i];
            char res[64];
            if (!h->ok) c_strncpy(res, "error", sizeof(res));
            else if (h->is_int) c_i64_dec(h->result_i, res, sizeof(res));
            else c_fmt_double(h->result_d, res, sizeof(res));
            /* Truncate expression to fit. */
            char expr[CALC_HIST_W / 8 + 1];
            int max = (int)sizeof(expr) - 1;
            int o = 0;
            for (int j = 0; h->expr[j] && o < max; j++) expr[o++] = h->expr[j];
            expr[o] = '\0';
            fb_text(bx + 6, by + 2, expr, COLOR_ON_SURFACE);
            char rline[80];
            int ro = 0;
            const char *eq = "= ";
            for (int j = 0; eq[j] && ro < 78; j++) rline[ro++] = eq[j];
            for (int j = 0; res[j] && ro < 78; j++) rline[ro++] = res[j];
            rline[ro] = '\0';
            fb_text(bx + 6, by + 18, rline,
                    h->ok ? COLOR_PRIMARY : COLOR_DANGER);
            fb_rect_outline(bx, by, bw, bh, COLOR_SEPARATOR, 1);
        }
    }

    /* Footer hint */
    {
        const char *hint = "Enter eval  Esc close  ^A/^B/^C [Std/Sci/Prg]  Up/Dn history";
        int hw = c_strlen(hint) * 8;
        fb_text(g_calc.wx + CALC_W - CALC_PAD - hw,
                g_calc.wy + CALC_H - 14, hint, COLOR_ON_SURFACE_3);
    }
}

/* ── Counters ── */

uint32_t calculator_total_opens(void)      { return g_total_opens; }
uint32_t calculator_total_evals_ok(void)   { return g_total_evals_ok; }
uint32_t calculator_total_evals_fail(void) { return g_total_evals_fail; }
