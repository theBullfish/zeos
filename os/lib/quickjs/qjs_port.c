/*
 * qjs_port.c — freestanding libc glue for QuickJS inside the Zeos kernel.
 *
 * QuickJS + vendored musl-libm (lib/zeosm) reference a small set of libc
 * symbols Zeos doesn't otherwise export. This file provides exactly those:
 *   - heap: malloc/realloc/free/malloc_usable_size over kmalloc/kfree
 *           (8-byte size header so realloc/usable_size work)
 *   - a couple of mem/str functions Zeos lacks (memchr, strrchr)
 *   - stdio sinks routed to the serial console (printf/fprintf/fwrite/…)
 *   - time (clock_gettime/gettimeofday/localtime_r) off the TSC
 *   - abort/__assert_fail -> panic
 *   - pthread mutex/cond -> no-ops (the JS runtime is single-threaded here)
 *
 * memcpy/memset/memmove/memcmp/strchr/strcmp/strlen/snprintf/vsnprintf are
 * already provided elsewhere in the kernel (builtins + mbedtls_platform.c);
 * we do NOT redefine them.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

/* Zeos primitives (declared locally to avoid header coupling). */
extern void  *kmalloc(uint64_t size);
extern void   kfree(void *ptr);
extern void   kputc(char c);
extern void   kputs(const char *s);
extern void   panic(const char *msg);
extern uint64_t timer_read_tsc(void);
extern uint64_t timer_tsc_freq(void);
extern void  *memcpy(void *d, const void *s, unsigned long n);
extern void  *memset(void *d, int c, unsigned long n);
extern int    vsnprintf(char *buf, unsigned long n, const char *fmt, va_list ap);

/* ── heap: 16-byte header keeps kmalloc's alignment + stores the size ── */
#define QJS_HDR 16

void *malloc(size_t n)
{
    if (n == 0) n = 1;
    uint64_t *p = (uint64_t *)kmalloc((uint64_t)n + QJS_HDR);
    if (!p) return NULL;
    p[0] = (uint64_t)n;
    return (void *)((char *)p + QJS_HDR);
}

void free(void *ptr)
{
    if (!ptr) return;
    kfree((void *)((char *)ptr - QJS_HDR));
}

size_t malloc_usable_size(void *ptr)
{
    if (!ptr) return 0;
    uint64_t *p = (uint64_t *)((char *)ptr - QJS_HDR);
    return (size_t)p[0];
}

void *realloc(void *ptr, size_t n)
{
    if (!ptr) return malloc(n);
    if (n == 0) { free(ptr); return NULL; }
    size_t old = malloc_usable_size(ptr);
    void *np = malloc(n);
    if (!np) return NULL;
    memcpy(np, ptr, old < n ? old : n);
    free(ptr);
    return np;
}

/* ── mem/str functions Zeos doesn't already export ── */
void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) break;
    }
    return (char *)last;
}

/* ── stdio -> serial ── */
int fputc(int c, FILE *stream)      { (void)stream; kputc((char)c); return c; }
int putchar(int c)                  { kputc((char)c); return c; }

size_t fwrite(const void *ptr, size_t sz, size_t nm, FILE *stream)
{
    (void)stream;
    const char *s = (const char *)ptr;
    size_t total = sz * nm;
    for (size_t i = 0; i < total; i++) kputc(s[i]);
    return nm;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    (void)stream;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kputs(buf);
    return n;
}

int printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kputs(buf);
    return n;
}

/* QuickJS references the `stdout` object (only in diagnostic paths). */
FILE *stdout = NULL;
FILE *stderr = NULL;

/* ── time off the TSC ── (clock_gettime is provided by mbedtls_platform.c) ── */
int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    uint64_t f = timer_tsc_freq(); if (!f) f = 2000000000ULL;
    uint64_t t = timer_read_tsc();
    tv->tv_sec  = (time_t)(t / f);
    tv->tv_usec = (long)((t % f) * 1000000ULL / f);
    return 0;
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    (void)t;
    memset(out, 0, sizeof(*out));
    out->tm_year = 126;   /* 2026, placeholder until a real RTC-backed Date */
    out->tm_mday = 1;
    return out;
}

/* ── fatal ── (__assert_fail is provided by mbedtls_platform.c) ── */
void abort(void)
{
    panic("quickjs: abort()");
    for (;;) { }
}

/* ── pthread: single-threaded no-ops (names only; no <pthread.h> types) ── */
int pthread_mutex_lock(void *m)     { (void)m; return 0; }
int pthread_mutex_unlock(void *m)   { (void)m; return 0; }
int pthread_cond_init(void *c, void *a)        { (void)c; (void)a; return 0; }
int pthread_cond_destroy(void *c)              { (void)c; return 0; }
int pthread_cond_signal(void *c)               { (void)c; return 0; }
int pthread_cond_wait(void *c, void *m)        { (void)c; (void)m; return 0; }
int pthread_cond_timedwait(void *c, void *m, const void *ts) { (void)c; (void)m; (void)ts; return 0; }
