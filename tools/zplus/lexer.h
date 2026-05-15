/* zplus — lexer: all Z+ token types */
#pragma once
#include <stdint.h>

typedef enum {
    /* ── multi-char operators ─────────────────────────────────────── */
    TOK_FLOW,        /* ->   signal flows forward                     */
    TOK_TAP,         /* ~>   observe without affecting                 */
    TOK_SEVER,       /* -x>  cut a wire                               */
    TOK_EXCHANGE,    /* <->  bidirectional request/response            */
    TOK_LE,          /* <=                                            */
    TOK_GE,          /* >=                                            */
    TOK_EQ,          /* ==                                            */
    TOK_NEQ,         /* !=                                            */
    TOK_ARROW_UP,    /* ↑   UTF-8 U+2191 (heat/increment)             */

    /* ── single-char operators ────────────────────────────────────── */
    TOK_PIPE,        /* |   merge                                     */
    TOK_AT,          /* @   pin to hardware / unit annotation          */
    TOK_TILDE,       /* ~   range (0.6 ~ 0.9)                         */
    TOK_DOT,         /* .   field access                              */
    TOK_COLON,       /* :   type annotation / label                   */
    TOK_ASSIGN,      /* =   assignment                                */
    TOK_COMMA,       /* ,                                             */
    TOK_SEMI,        /* ;                                             */
    TOK_LPAREN,      /* (                                             */
    TOK_RPAREN,      /* )                                             */
    TOK_LBRACE,      /* {                                             */
    TOK_RBRACE,      /* }                                             */
    TOK_LT,          /* <                                             */
    TOK_GT,          /* >                                             */
    TOK_PLUS,        /* +                                             */
    TOK_MINUS,       /* -   (also start of -> -x>)                    */
    TOK_STAR,        /* *                                             */
    TOK_SLASH,       /* /                                             */
    TOK_BANG,        /* !                                             */

    /* ── keywords ────────────────────────────────────────────────── */
    TOK_CHAIN,       /* chain                                         */
    TOK_STRUCT,      /* struct                                        */
    TOK_IMPORT,      /* import                                        */
    TOK_AS,          /* as                                            */
    TOK_GATE,        /* gate                                          */
    TOK_KNEE,        /* knee                                          */
    TOK_DELTA,       /* delta                                         */
    TOK_EMIT,        /* emit                                          */
    TOK_INPUT,       /* input                                         */
    TOK_OUTPUT,      /* output                                        */
    TOK_ON_SILENCE,  /* on_silence                                    */
    TOK_ON_BLOCK,    /* on_block                                      */
    TOK_SUSTAINED,   /* sustained                                     */
    TOK_WITHIN,      /* within                                        */
    TOK_THEN,        /* then                                          */
    TOK_DEBOUNCE,    /* debounce                                      */
    TOK_THROTTLE,    /* throttle                                      */
    TOK_DECAY,       /* decay                                         */
    TOK_TICK,        /* tick                                          */
    TOK_RATE,        /* rate                                          */
    TOK_FOR,         /* for                                           */
    TOK_NOT,         /* not                                           */
    TOK_PRIORITY,    /* priority                                      */
    TOK_REFLEX,      /* reflex                                        */
    TOK_DELIBERATE,  /* deliberate                                    */

    /* ── type keywords ───────────────────────────────────────────── */
    TOK_TK_INT,      /* int                                           */
    TOK_TK_FLOAT,    /* float                                         */
    TOK_TK_STR,      /* str                                           */
    TOK_TK_BOOL,     /* bool                                          */

    /* ── literals ────────────────────────────────────────────────── */
    TOK_INT_LIT,     /* 42                                            */
    TOK_FLOAT_LIT,   /* 3.14                                          */
    TOK_STR_LIT,     /* "hello"  (val.sval points into source)        */
    TOK_DURATION,    /* 5m 30s 1h 500ms  (val.ival = milliseconds)    */
    TOK_MULTIPLIER,  /* 5x       (val.ival = multiplier integer)      */

    /* ── identifier ──────────────────────────────────────────────── */
    TOK_IDENT,       /* anything else that looks like a name          */

    /* ── meta ────────────────────────────────────────────────────── */
    TOK_EOF,
    TOK_ERR,
} tok_kind_t;

typedef struct {
    tok_kind_t  kind;
    const char *start;   /* pointer into source buffer */
    int         len;     /* byte length in source      */
    int         line;
    int         col;
    union {
        long    ival;    /* TOK_INT_LIT, TOK_DURATION (ms), TOK_MULTIPLIER */
        double  fval;    /* TOK_FLOAT_LIT                                   */
        struct { const char *p; int len; } sval; /* TOK_STR_LIT (raw, no quotes) */
    } val;
} token_t;

typedef struct {
    const char *src;
    int         pos;
    int         len;
    int         line;
    int         col;
    token_t     _peek;
    int         _has_peek;
} lexer_t;

void        lexer_init(lexer_t *l, const char *src, int len);
token_t     lexer_next(lexer_t *l);
token_t     lexer_peek(lexer_t *l);
const char *tok_name(tok_kind_t k);
