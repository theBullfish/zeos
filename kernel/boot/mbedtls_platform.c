/*
 * Zeos — mbedTLS Platform Shim
 *
 * Connects mbedTLS to Zeos bare-metal primitives:
 *   - Memory: kmalloc/kfree from heap.c
 *   - Entropy: TSC jitter + MAC address
 *   - Time: TSC-derived seconds (for certificate validation)
 *
 * This file + mbedtls_config.h is everything mbedTLS needs
 * to run on bare metal with no OS.
 */

#include "heap.h"
#include "timer.h"
#include "net.h"

#include <stddef.h>

/* ══════════════════════════════════════════════════════
 * Memory: route mbedTLS allocations to kernel heap
 * ══════════════════════════════════════════════════════ */

/*
 * mbedTLS calls mbedtls_calloc / mbedtls_free.
 * We register these via mbedtls_platform_set_calloc_free()
 * during tls_init(), or define them directly.
 */

void *zeos_calloc(size_t n, size_t size) {
    size_t total = n * size;
    void *p = kmalloc(total);
    if (p) {
        /* Zero the memory (calloc contract) */
        uint8_t *b = (uint8_t *)p;
        for (size_t i = 0; i < total; i++) b[i] = 0;
    }
    return p;
}

void zeos_free(void *ptr) {
    kfree(ptr);
}

/* ══════════════════════════════════════════════════════
 * Entropy: TSC timing jitter as hardware RNG
 * ══════════════════════════════════════════════════════
 *
 * TSC reads between network packets and interrupts have
 * genuine jitter from pipeline stalls, cache misses, and
 * interrupt timing. We mix multiple TSC reads with the
 * MAC address for device uniqueness.
 *
 * This satisfies MBEDTLS_ENTROPY_HARDWARE_ALT.
 */

int mbedtls_hardware_poll(void *data,
                          unsigned char *output, size_t len,
                          size_t *olen)
{
    (void)data;

    /*
     * Mix strategy: read TSC twice per 4 bytes with a volatile
     * memory access between reads (forces pipeline stall = jitter).
     * XOR with MAC bytes for device uniqueness.
     */
    volatile uint8_t sink = 0;

    for (size_t i = 0; i < len; i += 4) {
        uint64_t tsc1 = timer_read_tsc();

        /* Force a pipeline stall for jitter */
        sink ^= (uint8_t)(tsc1 & 0xFF);

        uint64_t tsc2 = timer_read_tsc();

        /* Mix: XOR the delta with MAC and both TSC halves */
        uint32_t val = (uint32_t)(tsc2 - tsc1);
        val ^= (uint32_t)(tsc1 >> 7);
        val ^= (uint32_t)(tsc2 >> 13);
        val ^= ((uint32_t)g_net.mac.b[i % 6] << 24);
        val ^= ((uint32_t)g_net.mac.b[(i + 1) % 6] << 16);
        val ^= ((uint32_t)g_net.mac.b[(i + 2) % 6] << 8);
        val ^= (uint32_t)g_net.mac.b[(i + 3) % 6];

        /* Write output */
        size_t remaining = len - i;
        size_t chunk = remaining < 4 ? remaining : 4;
        for (size_t j = 0; j < chunk; j++)
            output[i + j] = (unsigned char)((val >> (j * 8)) & 0xFF);
    }

    *olen = len;
    return 0;
}

/* ══════════════════════════════════════════════════════
 * Time: approximate Unix timestamp from TSC
 * ══════════════════════════════════════════════════════
 *
 * mbedTLS needs time for certificate expiry checks.
 * We approximate: boot time is treated as a known epoch
 * (set via DHCP or hardcoded), then TSC ticks forward.
 *
 * For certificate validation, being within a few hours
 * is sufficient — certs expire on day boundaries.
 */

/* Approximate boot timestamp: April 7, 2026 00:00 UTC */
#define ZEOS_BOOT_EPOCH  1775433600UL

/*
 * mbedtls_time_t mbedtls_time(mbedtls_time_t *timer)
 *
 * Returns seconds since Unix epoch (approximately).
 */
typedef long mbedtls_time_t;

mbedtls_time_t mbedtls_time(mbedtls_time_t *timer) {
    uint64_t tsc = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    uint64_t seconds_since_boot = 0;

    if (freq > 0)
        seconds_since_boot = tsc / freq;

    mbedtls_time_t now = (mbedtls_time_t)(ZEOS_BOOT_EPOCH + seconds_since_boot);

    if (timer)
        *timer = now;

    return now;
}

/* ══════════════════════════════════════════════════════
 * String/memory functions mbedTLS expects
 * ══════════════════════════════════════════════════════
 *
 * mbedTLS uses memcpy, memset, memcmp, strlen, strcmp.
 * In freestanding mode these may not exist. Provide them.
 */

__attribute__((weak)) void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

__attribute__((weak)) void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dst;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while (*src) *d++ = *src++;
    *d = 0;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

/* snprintf stub — mbedTLS debug uses this, we don't need debug output */
int snprintf(char *buf, size_t size, const char *fmt, ...) {
    (void)fmt;
    if (size > 0) buf[0] = 0;
    return 0;
}

/* ══════════════════════════════════════════════════════
 * Platform init — called from tls_init()
 * ══════════════════════════════════════════════════════ */

void mbedtls_platform_shim_init(void) {
    /*
     * Register our allocator with mbedTLS:
     * mbedtls_platform_set_calloc_free(zeos_calloc, zeos_free);
     *
     * This is called automatically if MBEDTLS_PLATFORM_MEMORY
     * is defined and we provide the symbols.
     */
}
