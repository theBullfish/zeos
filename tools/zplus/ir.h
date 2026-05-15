/* zplus — IR lowering: AST → Signal Flow Graph */
#pragma once
#include "ast.h"
#include "sfg.h"

typedef struct {
    char msg[256];
    int  line, col;
} ir_error_t;

#define IR_MAX_ERRORS 64

typedef struct {
    ir_error_t errors[IR_MAX_ERRORS];
    int        nerrors;
} ir_result_t;

/* Lower an AST_PROGRAM node into a fresh SFG.
 * arena is used for SFG allocations (can be the same as parser's arena).
 * source_name is the .zp filename (for generated identifiers).
 * Returns NULL on fatal error; check res->nerrors for diagnostics. */
sfg_program_t *ir_lower(ast_node_t *program, arena_t *arena,
                        const char *source_name, ir_result_t *res);
