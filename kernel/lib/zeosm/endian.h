#ifndef _ZEOSM_ENDIAN_H
#define _ZEOSM_ENDIAN_H

/* Minimal endian.h for zeosm. libm.h only needs __BYTE_ORDER vs
 * __LITTLE_ENDIAN / __BIG_ENDIAN to select the long double (ldshape) layout.
 * Values are taken from the compiler's own byte-order builtins. */

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define __BYTE_ORDER __BIG_ENDIAN
#else
#define __BYTE_ORDER __LITTLE_ENDIAN
#endif
#else
/* Zeos targets x86_64: little endian. */
#define __BYTE_ORDER __LITTLE_ENDIAN
#endif

#define BIG_ENDIAN    __BIG_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define PDP_ENDIAN    __PDP_ENDIAN
#define BYTE_ORDER    __BYTE_ORDER

#endif
