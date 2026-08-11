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

/* mbedTLS allocator hooks. Routes to the shared kernel heap (heap.c).
 * Earlier in the integration we used a dedicated 4 MB region here because
 * the kernel heap had a split_block underflow bug that corrupted its
 * freelist under mbedtls's mixed alloc/free pattern. With heap.c fixed
 * (commit 1df9b0f), the dedicated region is no longer needed. */

void *zeos_calloc(size_t n, size_t size) {
    size_t total = n * size;
    if (total == 0) return 0;
    if (n != 0 && total / n != size) return 0;  /* overflow */
    void *p = kmalloc(total);
    if (p) {
        uint8_t *b = (uint8_t *)p;
        for (size_t i = 0; i < total; i++) b[i] = 0;
    }
    return p;
}

void zeos_free(void *ptr) {
    if (ptr) kfree(ptr);
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

/* RDRAND: NIST SP 800-90B-compliant DRNG present on Ivy Bridge+ Intel and
 * Excavator+ AMD x86_64. Detected via CPUID.01:ECX bit 30. When available,
 * we use it as the primary entropy source and mix in TSC jitter as
 * defense-in-depth against silicon-level attacks. */

static int rdrand_supported(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    cached = (ecx & (1u << 30)) ? 1 : 0;
    return cached;
}

static int rdrand64(uint64_t *out) {
    /* RDRAND can transiently fail under load; spec says retry up to 10 times. */
    for (int i = 0; i < 10; i++) {
        uint64_t v;
        unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return 0; }
    }
    return -1;
}

int mbedtls_hardware_poll(void *data,
                          unsigned char *output, size_t len,
                          size_t *olen)
{
    (void)data;

    /* Fast path: RDRAND, with TSC jitter mixed in for defense-in-depth. */
    if (rdrand_supported()) {
        size_t i = 0;
        while (i < len) {
            uint64_t r;
            if (rdrand64(&r) != 0) break;
            r ^= timer_read_tsc();
            size_t chunk = len - i < 8 ? len - i : 8;
            for (size_t j = 0; j < chunk; j++)
                output[i + j] = (unsigned char)((r >> (j * 8)) & 0xFF);
            i += chunk;
        }
        if (i == len) {
            *olen = len;
            return 0;
        }
        /* RDRAND mid-stream failure: fall through to TSC-only path. */
    }

    /*
     * TSC-jitter fallback: read TSC twice per 4 bytes with a volatile
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

/* Wall clock from CMOS/RTC (Motorola MC146818 register file at port 0x70/0x71).
 * Read once at boot, then add seconds_since_boot from TSC. This gives mbedtls
 * a real Unix timestamp for cert validity checks (notBefore/notAfter). */

typedef long mbedtls_time_t;

static inline uint8_t cmos_in(uint8_t reg) {
    uint8_t v;
    __asm__ volatile("outb %1, $0x70; inb $0x71, %0" : "=a"(v) : "a"(reg));
    return v;
}

static int bcd2bin(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }

static uint64_t cmos_unix_epoch(void) {
    /* Read all fields twice with UIP-clear gate around each read; accept
     * only when both reads agree. Without this, an RTC update can begin
     * between the UIP check and the field reads, returning e.g.
     * seconds=59 with minutes=N+1. */
    uint8_t s, mn, h, d, mo, y, status_b;
    uint8_t s2, mn2, h2, d2, mo2, y2;
    int attempts = 0;
    do {
        if (++attempts > 8) break;  /* CMOS broken / VM weirdness — accept last read */
        while (cmos_in(0x0A) & 0x80) { }
        s  = cmos_in(0x00); mn = cmos_in(0x02); h  = cmos_in(0x04);
        d  = cmos_in(0x07); mo = cmos_in(0x08); y  = cmos_in(0x09);
        while (cmos_in(0x0A) & 0x80) { }
        s2 = cmos_in(0x00); mn2 = cmos_in(0x02); h2 = cmos_in(0x04);
        d2 = cmos_in(0x07); mo2 = cmos_in(0x08); y2 = cmos_in(0x09);
    } while (s != s2 || mn != mn2 || h != h2 || d != d2 || mo != mo2 || y != y2);
    status_b = cmos_in(0x0B);

    if (!(status_b & 0x04)) {  /* values are BCD */
        s  = bcd2bin(s);
        mn = bcd2bin(mn);
        h  = (h & 0x80) ? bcd2bin(h & 0x7F) : bcd2bin(h);
        d  = bcd2bin(d);
        mo = bcd2bin(mo);
        y  = bcd2bin(y);
    }
    if (!(status_b & 0x02)) {  /* 12-hour mode */
        if (h & 0x80) h = ((h & 0x7F) + 12) % 24;
    }

    int year = 2000 + y;  /* CMOS reports 2-digit year */

    /* Days-from-epoch using Howard Hinnant's algorithm. */
    int yy = year - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;

    return (uint64_t)days * 86400 + (uint64_t)h * 3600 + (uint64_t)mn * 60 + s;
}

static uint64_t g_boot_epoch = 0;
static uint64_t g_boot_tsc   = 0;

static void mbedtls_time_init(void) {
    if (g_boot_epoch) return;
    g_boot_epoch = cmos_unix_epoch();
    g_boot_tsc   = timer_read_tsc();
    /* Sanity floor: if CMOS returned something obviously bogus
     * (pre-2024 or post-2099), fall back to a hardcoded floor so
     * cert validity checks don't all spuriously fail. */
    if (g_boot_epoch < 1704067200UL || g_boot_epoch > 4102444800UL) {
        g_boot_epoch = 1775433600UL;  /* 2026-04-07 00:00 UTC */
    }
}

/* libc time interface — required when MBEDTLS_PLATFORM_C is on and platform_util
 * compiles its time helpers. We forward to zeos_time and provide a minimal
 * gmtime_r that decomposes a Unix timestamp into struct tm. */

struct timespec_t {
    long tv_sec;
    long tv_nsec;
};

struct tm_t {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

extern long zeos_time(long *);

long time(long *t) {
    long v = zeos_time(0);
    if (t) *t = v;
    return v;
}

int clock_gettime(int clk_id, struct timespec_t *tp) {
    (void)clk_id;
    if (!tp) return -1;
    tp->tv_sec = zeos_time(0);
    tp->tv_nsec = 0;
    return 0;
}

void *gmtime_r(const long *tt, struct tm_t *tm_buf) {
    if (!tt || !tm_buf) return 0;
    long t = *tt;
    long days = t / 86400;
    long sec  = t - days * 86400;
    if (sec < 0) { sec += 86400; days -= 1; }
    tm_buf->tm_hour = (int)(sec / 3600);
    tm_buf->tm_min  = (int)((sec % 3600) / 60);
    tm_buf->tm_sec  = (int)(sec % 60);
    tm_buf->tm_wday = (int)((days + 4) % 7);
    if (tm_buf->tm_wday < 0) tm_buf->tm_wday += 7;
    /* Hinnant's days-from-civil inverse */
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (unsigned)((5 * doy + 2) / 153);
    unsigned d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y += 1;
    tm_buf->tm_year = (int)(y - 1900);
    tm_buf->tm_mon  = (int)(m - 1);
    tm_buf->tm_mday = (int)d;
    tm_buf->tm_yday = (int)doy;
    tm_buf->tm_isdst = 0;
    tm_buf->tm_gmtoff = 0;
    tm_buf->tm_zone = "UTC";
    return tm_buf;
}

mbedtls_time_t zeos_time(mbedtls_time_t *timer) {
    if (!g_boot_epoch) mbedtls_time_init();
    uint64_t freq = timer_tsc_freq();
    uint64_t since_boot = 0;
    if (freq > 0) since_boot = (timer_read_tsc() - g_boot_tsc) / freq;
    mbedtls_time_t now = (mbedtls_time_t)(g_boot_epoch + since_boot);
    if (timer) *timer = now;
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

/* Minimal snprintf/vsnprintf. Handles the subset mbedTLS debug + error
 * formatting actually emits: %c %s %d %i %u %x %X %p with optional width,
 * '0'/' ' padding, '-' left-align, length modifiers l/ll/z/j/t. No floats,
 * no '*' width, no precision. Truncates safely at size-1, always
 * NUL-terminates when size > 0, returns characters that would have been
 * written (POSIX semantics). */

typedef __builtin_va_list zeos_va_list;

static void z_putc(char *buf, size_t size, size_t *pos, char c) {
    if (*pos + 1 < size) buf[*pos] = c;
    (*pos)++;
}

static void z_puts(char *buf, size_t size, size_t *pos,
                   const char *s, int width, int left, char pad) {
    int n = 0;
    while (s[n]) n++;
    if (!left) for (int i = n; i < width; i++) z_putc(buf, size, pos, pad);
    for (int i = 0; i < n; i++) z_putc(buf, size, pos, s[i]);
    if (left)  for (int i = n; i < width; i++) z_putc(buf, size, pos, ' ');
}

static void z_putu(char *buf, size_t size, size_t *pos,
                   unsigned long long v, unsigned base, int upper,
                   int width, int left, char pad) {
    char tmp[32];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) {
        unsigned d = (unsigned)(v % base);
        tmp[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= base;
    }
    int width_used = n;
    if (!left) for (int i = n; i < width; i++) { z_putc(buf, size, pos, pad); width_used++; }
    while (n--) z_putc(buf, size, pos, tmp[n]);
    if (left) for (int i = width_used; i < width; i++) z_putc(buf, size, pos, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, zeos_va_list ap) {
    size_t pos = 0;
    while (*fmt) {
        if (*fmt != '%') { z_putc(buf, size, &pos, *fmt++); continue; }
        fmt++;
        int left = 0, width = 0, lng = 0;
        char pad = ' ';
        while (*fmt == '-' || *fmt == '0' || *fmt == ' ' || *fmt == '+' || *fmt == '#') {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0' && !left) pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h' || *fmt == 'j' || *fmt == 't') {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 2;
            fmt++;
        }
        char c = *fmt++;
        size_t before = pos;
        switch (c) {
            case '%': z_putc(buf, size, &pos, '%'); break;
            case 'c': z_putc(buf, size, &pos, (char)__builtin_va_arg(ap, int)); break;
            case 's': {
                const char *s = __builtin_va_arg(ap, const char *);
                z_puts(buf, size, &pos, s ? s : "(null)", width, left, pad);
                width = 0;
                break;
            }
            case 'd': case 'i': {
                long long v = (lng >= 2) ? __builtin_va_arg(ap, long long)
                            : (lng == 1) ? __builtin_va_arg(ap, long)
                                         : __builtin_va_arg(ap, int);
                if (v < 0) { z_putc(buf, size, &pos, '-'); v = -v; if (width) width--; }
                z_putu(buf, size, &pos, (unsigned long long)v, 10, 0, width, left, pad);
                width = 0;
                break;
            }
            case 'u': {
                unsigned long long v = (lng >= 2) ? __builtin_va_arg(ap, unsigned long long)
                                     : (lng == 1) ? __builtin_va_arg(ap, unsigned long)
                                                  : __builtin_va_arg(ap, unsigned int);
                z_putu(buf, size, &pos, v, 10, 0, width, left, pad);
                width = 0;
                break;
            }
            case 'x': case 'X': {
                unsigned long long v = (lng >= 2) ? __builtin_va_arg(ap, unsigned long long)
                                     : (lng == 1) ? __builtin_va_arg(ap, unsigned long)
                                                  : __builtin_va_arg(ap, unsigned int);
                z_putu(buf, size, &pos, v, 16, c == 'X', width, left, pad);
                width = 0;
                break;
            }
            case 'p': {
                void *p = __builtin_va_arg(ap, void *);
                z_putc(buf, size, &pos, '0'); z_putc(buf, size, &pos, 'x');
                z_putu(buf, size, &pos, (unsigned long long)(uintptr_t)p, 16, 0, 0, 0, ' ');
                break;
            }
            default:
                z_putc(buf, size, &pos, '%'); z_putc(buf, size, &pos, c);
                break;
        }
        if (left && width > 0) {
            size_t wrote = pos - before;
            for (size_t i = wrote; i < (size_t)width; i++) z_putc(buf, size, &pos, ' ');
        }
    }
    if (size > 0) buf[pos < size ? pos : size - 1] = 0;
    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    zeos_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

/* String functions used by x509 / pem code paths */

char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char *)s;
        s++;
    }
    return ch == 0 ? (char *)s : (void *)0;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return (void *)0;
}

/* Securely zero memory — used by mbedtls platform_util */
void *explicit_bzero(void *s, size_t n) {
    volatile uint8_t *p = (volatile uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = 0;
    return s;
}

/* IPv4/IPv6 textual to binary — mbedtls x509 uses this for IP-SAN matching.
 * AF_INET=2, AF_INET6=10 on Linux. Returns 1 on success, 0 on bad input. */
int inet_pton(int af, const char *src, void *dst) {
    if (!src || !dst) return 0;
    if (af == 2) {
        /* IPv4 dotted-quad */
        unsigned char *out = (unsigned char *)dst;
        for (int i = 0; i < 4; i++) {
            unsigned v = 0;
            int digits = 0;
            while (*src >= '0' && *src <= '9') {
                v = v * 10 + (unsigned)(*src - '0');
                src++; digits++;
                if (digits > 3 || v > 255) return 0;
            }
            if (digits == 0) return 0;
            out[i] = (unsigned char)v;
            if (i < 3) { if (*src != '.') return 0; src++; }
        }
        return *src == 0 ? 1 : 0;
    }
    if (af == 10) {
        /* IPv6 colon-hex with optional :: compression */
        unsigned char out[16];
        for (int i = 0; i < 16; i++) out[i] = 0;
        int idx = 0, zero_run = -1;
        if (src[0] == ':' && src[1] == ':') { zero_run = 0; src += 2; if (!*src) goto done; }
        while (*src && idx < 16) {
            unsigned v = 0;
            int digits = 0;
            while ((*src >= '0' && *src <= '9') ||
                   (*src >= 'a' && *src <= 'f') ||
                   (*src >= 'A' && *src <= 'F')) {
                unsigned d = (*src >= '0' && *src <= '9') ? (unsigned)(*src - '0')
                           : (*src >= 'a' && *src <= 'f') ? (unsigned)(*src - 'a' + 10)
                                                          : (unsigned)(*src - 'A' + 10);
                v = (v << 4) | d;
                src++; digits++;
                if (digits > 4) return 0;
            }
            if (digits == 0) return 0;
            if (idx >= 15) return 0;
            out[idx++] = (unsigned char)(v >> 8);
            out[idx++] = (unsigned char)(v & 0xff);
            if (*src == 0) break;
            if (*src != ':') return 0;
            src++;
            if (*src == ':') {
                if (zero_run >= 0) return 0;
                zero_run = idx;
                src++;
                if (*src == 0) break;
            }
        }
        if (zero_run >= 0) {
            int filled = idx;
            int gap = 16 - filled;
            for (int i = filled - 1; i >= zero_run; i--) out[i + gap] = out[i];
            for (int i = zero_run; i < zero_run + gap; i++) out[i] = 0;
        } else if (idx != 16) {
            return 0;
        }
done:
        for (int i = 0; i < 16; i++) ((unsigned char *)dst)[i] = out[i];
        return 1;
    }
    return 0;
}

/* Math + assert shims — pulled in by stb_truetype paths inside font.c that
 * the STBTT_* macro overrides don't cover. Pre-existing undefined refs;
 * landing them here keeps the kernel's freestanding libc surface in one
 * place. */

extern void kputs(const char *s);
static void zeos_libc_panic(const char *what) {
    kputs("PANIC: ");
    kputs(what);
    kputs("\n");
    for (;;) __asm__ volatile("cli; hlt");
}

void __assert_fail(const char *expr, const char *file, unsigned line, const char *fn) {
    (void)file; (void)line; (void)fn;
    zeos_libc_panic(expr ? expr : "assertion failed");
}

/* fmod / cos / acos are now provided by the full musl-libm (lib/zeosm),
 * vendored for QuickJS — accurate versions supersede the old stb-grade
 * approximations that used to live here. */

/* 128-bit unsigned division — referenced by bignum.c on x86_64.
 * Standard binary long division; only invoked occasionally during
 * handshake bignum ops. */
typedef unsigned __int128 zeos_u128_t;

zeos_u128_t __udivti3(zeos_u128_t n, zeos_u128_t d) {
    if (d == 0) return ~(zeos_u128_t)0;
    zeos_u128_t q = 0, r = 0;
    for (int i = 127; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= ((zeos_u128_t)1 << i); }
    }
    return q;
}

zeos_u128_t __umodti3(zeos_u128_t n, zeos_u128_t d) {
    if (d == 0) return n;
    zeos_u128_t r = 0;
    for (int i = 127; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) r -= d;
    }
    return r;
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
