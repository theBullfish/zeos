#ifndef _ZEOSM_LIMITS_H
#define _ZEOSM_LIMITS_H

/* Minimal freestanding <limits.h> for zeosm (x86_64, LP64).
 *
 * Only lrint.c includes <limits.h> (for LONG_MAX). The compiler's own
 * limits.h chains into the hosted glibc headers on this build host, which is
 * not available in the Zeos freestanding environment, so we provide the
 * standard integer limits directly. Values are the C standard LP64 minimums
 * for x86_64. */

#define CHAR_BIT   8
#define SCHAR_MIN  (-128)
#define SCHAR_MAX  127
#define UCHAR_MAX  255

#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN   0
#define CHAR_MAX   UCHAR_MAX
#else
#define CHAR_MIN   SCHAR_MIN
#define CHAR_MAX   SCHAR_MAX
#endif

#define SHRT_MIN   (-1-0x7fff)
#define SHRT_MAX   0x7fff
#define USHRT_MAX  0xffff

#define INT_MIN    (-1-0x7fffffff)
#define INT_MAX    0x7fffffff
#define UINT_MAX   0xffffffffU

#define LONG_MIN   (-1L-0x7fffffffffffffffL)
#define LONG_MAX   0x7fffffffffffffffL
#define ULONG_MAX  0xffffffffffffffffUL

#define LLONG_MIN  (-1LL-0x7fffffffffffffffLL)
#define LLONG_MAX  0x7fffffffffffffffLL
#define ULLONG_MAX 0xffffffffffffffffULL

#define MB_LEN_MAX 4

#endif
