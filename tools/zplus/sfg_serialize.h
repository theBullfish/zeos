/* zplus — SFG -> ZIR serialization.
 *
 * Emits a Signal Flow Graph as ZIR v1 JSON (see ZIR.md), the same interchange
 * format the Rust front-end (`src/zir.rs`) emits and the kernel loader
 * (`kernel/boot/zplus_zir.c`) consumes. This is what makes the C transpiler a
 * conforming *front-end* to the shared pipeline, not a parallel dead-end.
 */
#pragma once
#include <stdio.h>
#include "sfg.h"

/* Write `g` as ZIR v1 JSON to `out`. */
void sfg_to_zir(sfg_program_t *g, FILE *out);
