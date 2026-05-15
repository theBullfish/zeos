/* zplus — lexer implementation */
#include "lexer.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

/* ── keyword table ──────────────────────────────────────────────── */
static struct { const char *word; tok_kind_t kind; } kw_table[] = {
    {"chain",       TOK_CHAIN},
    {"struct",      TOK_STRUCT},
    {"import",      TOK_IMPORT},
    {"as",          TOK_AS},
    {"gate",        TOK_GATE},
    {"knee",        TOK_KNEE},
    {"delta",       TOK_DELTA},
    {"emit",        TOK_EMIT},
    {"input",       TOK_INPUT},
    {"output",      TOK_OUTPUT},
    {"on_silence",  TOK_ON_SILENCE},
    {"on_block",    TOK_ON_BLOCK},
    {"sustained",   TOK_SUSTAINED},
    {"within",      TOK_WITHIN},
    {"then",        TOK_THEN},
    {"debounce",    TOK_DEBOUNCE},
    {"throttle",    TOK_THROTTLE},
    {"decay",       TOK_DECAY},
    {"tick",        TOK_TICK},
    {"rate",        TOK_RATE},
    {"for",         TOK_FOR},
    {"not",         TOK_NOT},
    {"priority",    TOK_PRIORITY},
    {"reflex",      TOK_REFLEX},
    {"deliberate",  TOK_DELIBERATE},
    {"int",         TOK_TK_INT},
    {"float",       TOK_TK_FLOAT},
    {"str",         TOK_TK_STR},
    {"bool",        TOK_TK_BOOL},
    {NULL,          TOK_EOF},
};

/* ── helpers ────────────────────────────────────────────────────── */
static int  at_end(lexer_t *l)        { return l->pos >= l->len; }
static char peek_ch(lexer_t *l)       { return at_end(l) ? '\0' : l->src[l->pos]; }
static char peek2(lexer_t *l)         { return (l->pos+1 < l->len) ? l->src[l->pos+1] : '\0'; }
static char peek3(lexer_t *l)         { return (l->pos+2 < l->len) ? l->src[l->pos+2] : '\0'; }

static char advance(lexer_t *l) {
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; } else { l->col++; }
    return c;
}

static void skip_whitespace_and_comments(lexer_t *l) {
    for (;;) {
        while (!at_end(l) && isspace((unsigned char)peek_ch(l)))
            advance(l);
        /* line comment */
        if (!at_end(l) && peek_ch(l) == '/' && peek2(l) == '/') {
            while (!at_end(l) && peek_ch(l) != '\n')
                advance(l);
            continue;
        }
        /* block comment */
        if (!at_end(l) && peek_ch(l) == '/' && peek2(l) == '*') {
            advance(l); advance(l);
            while (!at_end(l)) {
                if (peek_ch(l) == '*' && peek2(l) == '/') {
                    advance(l); advance(l); break;
                }
                advance(l);
            }
            continue;
        }
        break;
    }
}

static tok_kind_t lookup_keyword(const char *s, int len) {
    for (int i = 0; kw_table[i].word; i++)
        if ((int)strlen(kw_table[i].word) == len &&
            memcmp(kw_table[i].word, s, (size_t)len) == 0)
            return kw_table[i].kind;
    return TOK_IDENT;
}

/* ── main scan ──────────────────────────────────────────────────── */
static token_t scan(lexer_t *l) {
    skip_whitespace_and_comments(l);

    token_t t = {TOK_EOF, NULL, 0, l->line, l->col, {0}};

    if (at_end(l)) { t.start = l->src + l->pos; return t; }

    /* UTF-8 ↑ (U+2191: E2 86 91) */
    if ((unsigned char)l->src[l->pos] == 0xE2 &&
        l->pos+2 < l->len &&
        (unsigned char)l->src[l->pos+1] == 0x86 &&
        (unsigned char)l->src[l->pos+2] == 0x91) {
        t.kind = TOK_ARROW_UP; t.start = l->src + l->pos; t.len = 3;
        l->pos += 3; l->col += 1;
        return t;
    }

    t.start = l->src + l->pos;
    char c = advance(l);

    switch (c) {
    /* ── operators that start with - ────────────────────────────── */
    case '-':
        if (peek_ch(l) == '>') { advance(l); t.kind = TOK_FLOW;  t.len = 2; return t; }
        if (peek_ch(l) == 'x' && peek2(l) == '>') {
            advance(l); advance(l); t.kind = TOK_SEVER; t.len = 3; return t;
        }
        t.kind = TOK_MINUS; t.len = 1; return t;

    /* ── operators that start with ~ ────────────────────────────── */
    case '~':
        if (peek_ch(l) == '>') { advance(l); t.kind = TOK_TAP; t.len = 2; return t; }
        t.kind = TOK_TILDE; t.len = 1; return t;

    /* ── operators that start with < ────────────────────────────── */
    case '<':
        if (peek_ch(l) == '-' && peek2(l) == '>') {
            advance(l); advance(l); t.kind = TOK_EXCHANGE; t.len = 3; return t;
        }
        if (peek_ch(l) == '=') { advance(l); t.kind = TOK_LE;  t.len = 2; return t; }
        t.kind = TOK_LT; t.len = 1; return t;

    case '>':
        if (peek_ch(l) == '=') { advance(l); t.kind = TOK_GE; t.len = 2; return t; }
        t.kind = TOK_GT; t.len = 1; return t;

    case '=':
        if (peek_ch(l) == '=') { advance(l); t.kind = TOK_EQ;  t.len = 2; return t; }
        t.kind = TOK_ASSIGN; t.len = 1; return t;

    case '!':
        if (peek_ch(l) == '=') { advance(l); t.kind = TOK_NEQ; t.len = 2; return t; }
        t.kind = TOK_BANG; t.len = 1; return t;

    /* ── single-char ─────────────────────────────────────────────── */
    case '|': t.kind = TOK_PIPE;   t.len = 1; return t;
    case '@': t.kind = TOK_AT;     t.len = 1; return t;
    case '.': t.kind = TOK_DOT;    t.len = 1; return t;
    case ':': t.kind = TOK_COLON;  t.len = 1; return t;
    case ',': t.kind = TOK_COMMA;  t.len = 1; return t;
    case ';': t.kind = TOK_SEMI;   t.len = 1; return t;
    case '(': t.kind = TOK_LPAREN; t.len = 1; return t;
    case ')': t.kind = TOK_RPAREN; t.len = 1; return t;
    case '{': t.kind = TOK_LBRACE; t.len = 1; return t;
    case '}': t.kind = TOK_RBRACE; t.len = 1; return t;
    case '+': t.kind = TOK_PLUS;   t.len = 1; return t;
    case '*': t.kind = TOK_STAR;   t.len = 1; return t;
    case '/': t.kind = TOK_SLASH;  t.len = 1; return t;

    /* ── string literal ──────────────────────────────────────────── */
    case '"': {
        const char *inner = l->src + l->pos;
        int inner_len = 0;
        while (!at_end(l) && peek_ch(l) != '"') {
            if (peek_ch(l) == '\\') { advance(l); } /* skip escape */
            advance(l); inner_len++;
        }
        if (!at_end(l)) advance(l); /* consume closing " */
        t.kind = TOK_STR_LIT;
        t.len  = (int)(l->src + l->pos - t.start);
        t.val.sval.p   = inner;
        t.val.sval.len = inner_len;
        return t;
    }

    /* ── numeric literals ────────────────────────────────────────── */
    default:
        if (isdigit((unsigned char)c)) {
            int is_float = 0;
            while (!at_end(l) && isdigit((unsigned char)peek_ch(l))) advance(l);
            if (!at_end(l) && peek_ch(l) == '.' && isdigit((unsigned char)peek2(l))) {
                is_float = 1;
                advance(l);
                while (!at_end(l) && isdigit((unsigned char)peek_ch(l))) advance(l);
            }
            int tok_len = (int)(l->src + l->pos - t.start);

            /* duration suffix: ms h m s */
            if (!at_end(l)) {
                char s1 = peek_ch(l), s2 = peek2(l);
                if (s1 == 'm' && s2 == 's') {
                    advance(l); advance(l);
                    t.kind = TOK_DURATION;
                    t.len  = (int)(l->src + l->pos - t.start);
                    t.val.ival = atol(t.start); /* ms */
                    return t;
                }
                if (s1 == 'h' && !isalnum((unsigned char)s2)) {
                    advance(l);
                    t.kind = TOK_DURATION;
                    t.len  = (int)(l->src + l->pos - t.start);
                    t.val.ival = atol(t.start) * 3600000L;
                    return t;
                }
                if (s1 == 'm' && !isalnum((unsigned char)s2)) {
                    advance(l);
                    t.kind = TOK_DURATION;
                    t.len  = (int)(l->src + l->pos - t.start);
                    t.val.ival = atol(t.start) * 60000L;
                    return t;
                }
                if (s1 == 's' && !isalnum((unsigned char)s2)) {
                    advance(l);
                    t.kind = TOK_DURATION;
                    t.len  = (int)(l->src + l->pos - t.start);
                    t.val.ival = atol(t.start) * 1000L;
                    return t;
                }
                if (s1 == 'd' && !isalnum((unsigned char)s2)) {
                    advance(l);
                    t.kind = TOK_DURATION;
                    t.len  = (int)(l->src + l->pos - t.start);
                    t.val.ival = atol(t.start) * 86400000L;
                    return t;
                }
                /* multiplier: 5x */
                if (s1 == 'x' && !isalnum((unsigned char)s2)) {
                    advance(l);
                    t.kind = TOK_MULTIPLIER;
                    t.len  = (int)(l->src + l->pos - t.start);
                    t.val.ival = atol(t.start);
                    return t;
                }
            }

            t.len = tok_len;
            if (is_float) {
                t.kind = TOK_FLOAT_LIT;
                t.val.fval = atof(t.start);
            } else {
                t.kind = TOK_INT_LIT;
                t.val.ival = atol(t.start);
            }
            return t;
        }

        /* ── identifier / keyword ──────────────────────────────────── */
        if (isalpha((unsigned char)c) || c == '_') {
            while (!at_end(l) &&
                   (isalnum((unsigned char)peek_ch(l)) || peek_ch(l) == '_'))
                advance(l);
            t.len  = (int)(l->src + l->pos - t.start);
            t.kind = lookup_keyword(t.start, t.len);
            return t;
        }

        /* unknown */
        t.kind = TOK_ERR; t.len = 1; return t;
    }
}

/* ── public API ─────────────────────────────────────────────────── */
void lexer_init(lexer_t *l, const char *src, int len) {
    memset(l, 0, sizeof(*l));
    l->src = src; l->len = len; l->line = 1; l->col = 1;
}

token_t lexer_next(lexer_t *l) {
    if (l->_has_peek) { l->_has_peek = 0; return l->_peek; }
    return scan(l);
}

token_t lexer_peek(lexer_t *l) {
    if (!l->_has_peek) { l->_peek = scan(l); l->_has_peek = 1; }
    return l->_peek;
}

const char *tok_name(tok_kind_t k) {
    switch (k) {
    case TOK_FLOW:      return "->";
    case TOK_TAP:       return "~>";
    case TOK_SEVER:     return "-x>";
    case TOK_EXCHANGE:  return "<->";
    case TOK_LE:        return "<=";
    case TOK_GE:        return ">=";
    case TOK_EQ:        return "==";
    case TOK_NEQ:       return "!=";
    case TOK_ARROW_UP:  return "↑";
    case TOK_PIPE:      return "|";
    case TOK_AT:        return "@";
    case TOK_TILDE:     return "~";
    case TOK_DOT:       return ".";
    case TOK_COLON:     return ":";
    case TOK_ASSIGN:    return "=";
    case TOK_COMMA:     return ",";
    case TOK_SEMI:      return ";";
    case TOK_LPAREN:    return "(";
    case TOK_RPAREN:    return ")";
    case TOK_LBRACE:    return "{";
    case TOK_RBRACE:    return "}";
    case TOK_LT:        return "<";
    case TOK_GT:        return ">";
    case TOK_PLUS:      return "+";
    case TOK_MINUS:     return "-";
    case TOK_STAR:      return "*";
    case TOK_SLASH:     return "/";
    case TOK_BANG:      return "!";
    case TOK_CHAIN:     return "chain";
    case TOK_STRUCT:    return "struct";
    case TOK_IMPORT:    return "import";
    case TOK_AS:        return "as";
    case TOK_GATE:      return "gate";
    case TOK_KNEE:      return "knee";
    case TOK_DELTA:     return "delta";
    case TOK_EMIT:      return "emit";
    case TOK_INPUT:     return "input";
    case TOK_OUTPUT:    return "output";
    case TOK_ON_SILENCE:return "on_silence";
    case TOK_ON_BLOCK:  return "on_block";
    case TOK_SUSTAINED: return "sustained";
    case TOK_WITHIN:    return "within";
    case TOK_THEN:      return "then";
    case TOK_DEBOUNCE:  return "debounce";
    case TOK_THROTTLE:  return "throttle";
    case TOK_DECAY:     return "decay";
    case TOK_TICK:      return "tick";
    case TOK_RATE:      return "rate";
    case TOK_FOR:       return "for";
    case TOK_NOT:       return "not";
    case TOK_PRIORITY:  return "priority";
    case TOK_REFLEX:    return "reflex";
    case TOK_DELIBERATE:return "deliberate";
    case TOK_TK_INT:    return "int";
    case TOK_TK_FLOAT:  return "float";
    case TOK_TK_STR:    return "str";
    case TOK_TK_BOOL:   return "bool";
    case TOK_INT_LIT:   return "<int>";
    case TOK_FLOAT_LIT: return "<float>";
    case TOK_STR_LIT:   return "<string>";
    case TOK_DURATION:  return "<duration>";
    case TOK_MULTIPLIER:return "<multiplier>";
    case TOK_IDENT:     return "<ident>";
    case TOK_EOF:       return "<eof>";
    case TOK_ERR:       return "<error>";
    default:            return "?";
    }
}
