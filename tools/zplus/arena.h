/* zplus — arena allocator (bump, no individual frees) */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *base; size_t pos, cap; } arena_t;

static inline void arena_init(arena_t *a, size_t cap) {
    a->base = (uint8_t*)malloc(cap); a->pos = 0; a->cap = cap;
}
static inline void *arena_alloc(arena_t *a, size_t size) {
    size = (size + 7) & ~(size_t)7;
    if (!a->base || a->pos + size > a->cap) return NULL;
    void *p = a->base + a->pos; a->pos += size;
    memset(p, 0, size); return p;
}
static inline char *arena_strdup(arena_t *a, const char *s, int len) {
    char *p = (char*)arena_alloc(a, (size_t)(len + 1));
    if (p) { memcpy(p, s, (size_t)len); p[len] = '\0'; }
    return p;
}
static inline void arena_reset(arena_t *a) { a->pos = 0; }
static inline void arena_free(arena_t *a)  { free(a->base); a->base = NULL; a->pos = a->cap = 0; }
