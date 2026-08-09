#ifndef _ZEOSM_STDLIB_H
#define _ZEOSM_STDLIB_H

/* Minimal stdlib.h for zeosm. Only abs.c includes <stdlib.h>, solely for the
 * prototype of abs(). Kept tiny to avoid pulling a hosted libc. If the Zeos
 * kernel already provides a stdlib.h on the include path ahead of lib/zeosm,
 * that one will be used instead. */

int   abs(int);
long  labs(long);
long long llabs(long long);

#endif
