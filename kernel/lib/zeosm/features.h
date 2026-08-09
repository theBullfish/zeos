#ifndef _ZEOSM_FEATURES_H
#define _ZEOSM_FEATURES_H

/* Self-contained replacement for musl's features.h for the Zeos freestanding
 * libm (zeosm). Provides only the macros the vendored math sources need. */

#if __STDC_VERSION__ >= 199901L
#define __restrict restrict
#define __inline inline
#elif !defined(__GNUC__)
#define __restrict
#define __inline
#endif

#if __STDC_VERSION__ >= 201112L
#elif defined(__GNUC__)
#define _Noreturn __attribute__((__noreturn__))
#else
#define _Noreturn
#endif

#define weak __attribute__((__weak__))
#define hidden __attribute__((__visibility__("hidden")))
#define weak_alias(old, new) \
	extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))

#endif
