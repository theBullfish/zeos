/* zplus — codegen: SFG → C source */
#pragma once
#include "sfg.h"
#include <stdio.h>

typedef struct {
    char msg[256];
} cg_error_t;

#define CG_MAX_ERRORS 32

typedef struct {
    cg_error_t errors[CG_MAX_ERRORS];
    int        nerrors;
} cg_result_t;

/* Emit a compilable C translation unit to `out`.
 * The output calls chain_create / chain_add_node using the kernel ABI
 * from kernel/boot/chain.h.
 *
 * Emitted files:
 *   <stem>.zp.c   — resolve functions + zp_<stem>_init()
 *   <stem>.zp.h   — declaration of zp_<stem>_init() + struct typedefs
 *
 * This function writes only the .c portion to `out`.
 * Call codegen_emit_header() separately for the .h.  */
void codegen_emit(sfg_program_t *g, FILE *out, cg_result_t *res);
void codegen_emit_header(sfg_program_t *g, FILE *out);
