#ifndef _ZEOSM_FENV_H
#define _ZEOSM_FENV_H

/* Minimal fenv.h stub for zeosm.
 *
 * Only lrint.c includes <fenv.h>. On x86_64 (LONG_MAX == 0x7fffffffffffffff)
 * the slow, fenv-using code path in lrint.c is guarded by
 *   #if LONG_MAX < 1U<<53 && defined(FE_INEXACT)
 * which is false, so no fenv symbols (fetestexcept/feclearexcept/FE_INEXACT)
 * are referenced. This header therefore only needs to exist; it deliberately
 * does NOT define FE_INEXACT so the simple `return rint(x)` path is used.
 *
 * If a future consumer needs real floating-point environment control, replace
 * this with a proper x86_64 fenv implementation. */

#endif
