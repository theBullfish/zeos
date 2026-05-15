/* zplus — parser interface */
#pragma once
#include "lexer.h"
#include "ast.h"
#include "arena.h"

#define PARSE_MAX_ERRORS 32

typedef struct {
    lexer_t     lex;
    arena_t     arena;
    token_t     cur;
    char        errors[PARSE_MAX_ERRORS][128];
    int         nerrors;
    int         panic;   /* error recovery: skip to next sync point */
} parser_t;

/* Returns AST_PROGRAM node, or NULL on total failure.
 * Errors are in p->errors[0..p->nerrors-1]. */
ast_node_t *parse(parser_t *p, const char *src, int len);

void        parser_init(parser_t *p);
void        parser_free(parser_t *p);
