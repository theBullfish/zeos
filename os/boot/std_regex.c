/*
 * Zeos — std.regex (minimal backtracking matcher)
 */

#include "std_regex.h"

static int rx_isdigit(char c) { return c >= '0' && c <= '9'; }
static int rx_isalpha(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
static int rx_isword(char c)  { return rx_isalpha(c) || rx_isdigit(c) || c == '_'; }
static int rx_isspace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* Match a single character against a single regex atom starting at
 * pat[*ip]. On success advances *ip past the atom and returns 1.
 * Class atoms: . , \d \w \s , [abc-z] , literal. */
static int atom_match(char c, const char *pat, int *ip)
{
    char p = pat[*ip];
    if (p == 0) return 0;
    if (p == '.') { (*ip)++; return c != 0; }
    if (p == '\\') {
        char n = pat[*ip + 1];
        *ip += 2;
        if (n == 'd') return rx_isdigit(c);
        if (n == 'w') return rx_isword(c);
        if (n == 's') return rx_isspace(c);
        return c == n;
    }
    if (p == '[') {
        int j = *ip + 1;
        int neg = 0;
        if (pat[j] == '^') { neg = 1; j++; }
        int hit = 0;
        while (pat[j] && pat[j] != ']') {
            if (pat[j+1] == '-' && pat[j+2] && pat[j+2] != ']') {
                if (c >= pat[j] && c <= pat[j+2]) hit = 1;
                j += 3;
            } else {
                if (c == pat[j]) hit = 1;
                j++;
            }
        }
        if (pat[j] == ']') j++;
        *ip = j;
        return neg ? !hit : hit;
    }
    /* literal */
    (*ip)++;
    return c == p;
}

/* Length of one atom starting at pat[i]. */
static int atom_len(const char *pat, int i)
{
    if (pat[i] == '\\' && pat[i+1]) return 2;
    if (pat[i] == '[') {
        int j = i + 1;
        while (pat[j] && pat[j] != ']') j++;
        if (pat[j] == ']') j++;
        return j - i;
    }
    return 1;
}

static int match_here(const char *s, int si, int slen,
                      const char *pat, int pi);

static int match_star(int min_count, const char *s, int si, int slen,
                      const char *pat, int atom_start, int atom_l, int pi_after)
{
    /* greedy: count maximum matches first, then back off until tail matches. */
    int count = 0;
    while (si + count < slen) {
        int ip = atom_start;
        if (!atom_match(s[si + count], pat, &ip)) break;
        count++;
    }
    while (count >= min_count) {
        if (match_here(s, si + count, slen, pat, pi_after)) return 1;
        count--;
    }
    (void)atom_l;
    return 0;
}

static int match_here(const char *s, int si, int slen,
                      const char *pat, int pi)
{
    while (1) {
        if (pat[pi] == 0) return 1;
        if (pat[pi] == '$' && pat[pi+1] == 0) return si == slen;
        /* Atom boundary. */
        int atom_start = pi;
        int alen = atom_len(pat, pi);
        char q = pat[pi + alen];
        if (q == '*' || q == '+' || q == '?') {
            int min_c = (q == '+') ? 1 : 0;
            int max_c = (q == '?') ? 1 : -1;
            if (max_c == 1) {
                /* Try with one match, then with zero. */
                if (si < slen) {
                    int ip = atom_start;
                    if (atom_match(s[si], pat, &ip)) {
                        if (match_here(s, si + 1, slen, pat, pi + alen + 1)) return 1;
                    }
                }
                if (min_c == 0) {
                    return match_here(s, si, slen, pat, pi + alen + 1);
                }
                return 0;
            }
            return match_star(min_c, s, si, slen, pat,
                              atom_start, alen, pi + alen + 1);
        }
        /* Plain atom: must match exactly once. */
        if (si >= slen) return 0;
        int ip = atom_start;
        if (!atom_match(s[si], pat, &ip)) return 0;
        si++;
        pi += alen;
    }
}

int zp_regex_match_str(const char *s, const char *pattern)
{
    if (!s || !pattern) return 0;
    int slen = 0; while (s[slen]) slen++;
    if (pattern[0] == '^') {
        return match_here(s, 0, slen, pattern, 1);
    }
    for (int i = 0; i <= slen; i++) {
        if (match_here(s, i, slen, pattern, 0)) return 1;
    }
    return 0;
}

int zp_regex_find_str(const char *s, const char *pattern)
{
    if (!s || !pattern) return -1;
    int slen = 0; while (s[slen]) slen++;
    if (pattern[0] == '^') {
        return match_here(s, 0, slen, pattern, 1) ? 0 : -1;
    }
    for (int i = 0; i <= slen; i++) {
        if (match_here(s, i, slen, pattern, 0)) return i;
    }
    return -1;
}

/* Returns end offset of first match, or -1 on miss. */
static int match_extent(const char *s, int slen, const char *pattern, int *out_start)
{
    int start = (pattern[0] == '^') ? 0 : 0;
    int max_start = (pattern[0] == '^') ? 0 : slen;
    int p_off = (pattern[0] == '^') ? 1 : 0;
    for (int i = start; i <= max_start; i++) {
        if (match_here(s, i, slen, pattern, p_off)) {
            /* Re-scan greedily to find match end: try increasing
             * extents. Simplest correct: linear search the longest
             * prefix from i that still matches the WHOLE pattern. */
            int hi = slen;
            for (int j = slen; j >= i; j--) {
                /* Match against pattern with implicit anchor: temporarily
                 * substitute prefix. We approximate: if pattern matches
                 * at i and consumes through some j, j is the end. Use
                 * a marker-free re-match that tracks consumption: re-run
                 * match_here but cap slen at j. */
                if (match_here(s, i, j, pattern, p_off)) {
                    hi = j; if (out_start) *out_start = i;
                    /* Try to find tightest hi by decreasing j. */
                    while (hi > i && match_here(s, i, hi - 1, pattern, p_off)) hi--;
                    return hi;
                }
            }
            if (out_start) *out_start = i;
            return i;
        }
    }
    return -1;
}

int zp_regex_replace_str(const char *s, const char *pattern,
                         const char *repl, char *out, int out_max)
{
    if (!s || !pattern || !repl || !out || out_max <= 0) return -1;
    int slen = 0; while (s[slen]) slen++;
    int rlen = 0; while (repl[rlen]) rlen++;
    int start = -1;
    int end = match_extent(s, slen, pattern, &start);
    if (end < 0) return -1;
    int n = 0;
    for (int i = 0; i < start && n < out_max - 1; i++) out[n++] = s[i];
    for (int i = 0; i < rlen && n < out_max - 1; i++) out[n++] = repl[i];
    for (int i = end; i < slen && n < out_max - 1; i++) out[n++] = s[i];
    out[n] = 0;
    return n;
}
