/* Zeos aarch64 — freestanding C string/mem runtime (M4).
 * gcc -ffreestanding still emits calls to these; every portable Zeos module
 * needs them. Real, not stubs. */
#include <stdint.h>
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n)
{ uint8_t *dd = d; const uint8_t *ss = s; while (n--) *dd++ = *ss++; return d; }

void *memset(void *d, int c, size_t n)
{ uint8_t *dd = d; while (n--) *dd++ = (uint8_t)c; return d; }

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dd = d; const uint8_t *ss = s;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{ const uint8_t *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; x++; y++; } return 0; }

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (uint8_t)*a - (uint8_t)*b; }

int strncmp(const char *a, const char *b, size_t n)
{ while (n && *a && *a == *b) { a++; b++; n--; } return n ? ((uint8_t)*a - (uint8_t)*b) : 0; }

char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }

char *strncpy(char *d, const char *s, size_t n)
{ char *r = d; while (n && (*d++ = *s++)) n--; while (n--) *d++ = 0; return r; }

char *strchr(const char *s, int c)
{ for (; *s; s++) if (*s == (char)c) return (char *)s; return c ? 0 : (char *)s; }
