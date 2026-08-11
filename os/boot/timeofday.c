/*
 * Zeos -- Time of Day (Wall Clock)
 *
 * Wall-clock time. CMOS RTC read at boot, TSC-derived monotonic for
 * fractional seconds. UTC only initially; tz_offset_minutes hook
 * exposed for future timezone support.
 *
 * CMOS RTC layout (MC146818-compatible, the de facto standard):
 *   port 0x70: index register  (write reg number, NMI-disable bit 7)
 *   port 0x71: data register   (read/write the indexed reg)
 *
 *   reg 0x00  seconds       (BCD or binary, see Status B bit 2)
 *   reg 0x02  minutes
 *   reg 0x04  hours          (BCD or binary; if 12h, bit 7 = PM)
 *   reg 0x06  day of week    (we ignore -- recomputed from date)
 *   reg 0x07  day of month
 *   reg 0x08  month
 *   reg 0x09  year           (00..99; century from reg 0x32 if present)
 *   reg 0x0A  Status A       (bit 7 = update-in-progress)
 *   reg 0x0B  Status B       (bit 1 = 24h fmt, bit 2 = binary mode)
 *   reg 0x32  century        (BCD on most modern platforms; advisory)
 *
 * We poll Status A to avoid reading mid-update, then read all fields
 * twice and only accept when two consecutive reads agree -- standard
 * MC146818 idiom (the chip can update between byte reads otherwise).
 *
 * If every field reads back 0xFF we assume there's no RTC (some
 * hypervisors / minimal VMs) and fall back to the persisted tod.
 */

#include "timeofday.h"
#include "hal.h"
#include "timer.h"
#include "kprint.h"
#include "vault.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define CMOS_REG_SEC      0x00
#define CMOS_REG_MIN      0x02
#define CMOS_REG_HOUR     0x04
#define CMOS_REG_DAY      0x07
#define CMOS_REG_MON      0x08
#define CMOS_REG_YEAR     0x09
#define CMOS_REG_STATUS_A 0x0A
#define CMOS_REG_STATUS_B 0x0B
#define CMOS_REG_CENTURY  0x32

#define TOD_VAULT_PATH    "/time/last-known.bin"
#define TOD_VAULT_MAGIC   0x544F4453u  /* 'TODS' */

int tod_tz_offset_minutes = 0;

/* ── State ──────────────────────────────────────────────────────── */

static uint64_t      s_boot_epoch;     /* Unix seconds at boot */
static uint64_t      s_boot_tsc;       /* TSC value at the moment boot_epoch was set */
static int           s_ready;
static tod_source_t  s_source = TOD_SRC_NONE;

typedef struct {
    uint32_t magic;
    uint32_t reserved;
    uint64_t saved_epoch;
} tod_persist_record_t;

/* ── CMOS helpers ──────────────────────────────────────────────── */

static uint8_t cmos_read(uint8_t reg)
{
    /* Bit 7 of the index port is NMI-disable on legacy chipsets;
     * preserve it as 0x80 here to avoid re-enabling NMIs unexpectedly
     * mid-read. (Modern systems mostly ignore this, but the cost is a
     * single OR.) */
    hal_out8(CMOS_INDEX, (uint8_t)(0x80 | (reg & 0x7F)));
    return hal_in8(CMOS_DATA);
}

static void cmos_write(uint8_t reg, uint8_t val)
{
    hal_out8(CMOS_INDEX, (uint8_t)(0x80 | (reg & 0x7F)));
    hal_out8(CMOS_DATA, val);
}

static int cmos_update_in_progress(void)
{
    return (cmos_read(CMOS_REG_STATUS_A) & 0x80) ? 1 : 0;
}

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)(((v >> 4) * 10u) + (v & 0x0Fu));
}

static uint8_t bin_to_bcd(uint8_t v)
{
    return (uint8_t)((((v / 10u) & 0x0Fu) << 4) | (v % 10u));
}

/* ── Date math (no libc) ───────────────────────────────────────── */

static int is_leap(int y)
{
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

static const uint16_t days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

/* Convert UTC Y/M/D H:M:S to Unix epoch seconds (signed-safe for
 * 1970..2099 trivially; we operate on uint64 throughout). */
static uint64_t ymdhms_to_epoch(int year, int mon, int day,
                                int hour, int min, int sec)
{
    uint64_t days = 0;
    int y;

    /* Days from 1970-01-01 to year-01-01. */
    for (y = 1970; y < year; y++) {
        days += is_leap(y) ? 366 : 365;
    }

    /* Days from year-01-01 to year-mon-01. */
    if (mon < 1) mon = 1;
    if (mon > 12) mon = 12;
    days += days_before_month[mon - 1];
    if (mon > 2 && is_leap(year))
        days += 1;

    /* Days within the month. */
    days += (uint64_t)(day - 1);

    return days * 86400ULL +
           (uint64_t)hour * 3600ULL +
           (uint64_t)min  * 60ULL +
           (uint64_t)sec;
}

/* Inverse: epoch -> Y/M/D H:M:S (UTC). */
static void epoch_to_ymdhms(uint64_t epoch,
                            int *year, int *mon, int *day,
                            int *hour, int *min, int *sec)
{
    uint64_t s = epoch;
    *sec  = (int)(s % 60); s /= 60;
    *min  = (int)(s % 60); s /= 60;
    *hour = (int)(s % 24); s /= 24;

    /* s = days since 1970-01-01 */
    int y = 1970;
    while (1) {
        int d = is_leap(y) ? 366 : 365;
        if (s < (uint64_t)d) break;
        s -= (uint64_t)d;
        y++;
    }
    *year = y;

    int m = 1;
    while (m <= 12) {
        int dim;
        switch (m) {
        case 1: case 3: case 5: case 7: case 8: case 10: case 12: dim = 31; break;
        case 4: case 6: case 9: case 11: dim = 30; break;
        default: dim = is_leap(y) ? 29 : 28; break;
        }
        if (s < (uint64_t)dim) break;
        s -= (uint64_t)dim;
        m++;
    }
    *mon = m;
    *day = (int)s + 1;
}

/* ── CMOS read / write ─────────────────────────────────────────── */

/* Read CMOS until two consecutive reads match. Returns 0 on success.
 * Returns -1 if CMOS appears absent (all 0xFF) or update never settles. */
static int cmos_read_datetime(int *year, int *mon, int *day,
                              int *hour, int *min, int *sec)
{
    uint8_t s, mn, h, d, mo, y, c, statB;
    uint8_t s2, mn2, h2, d2, mo2, y2, c2;
    int spin = 0;

    /* Wait for any in-progress update to finish. */
    while (cmos_update_in_progress()) {
        if (++spin > 1000000) return -1;
    }

    s     = cmos_read(CMOS_REG_SEC);
    mn    = cmos_read(CMOS_REG_MIN);
    h     = cmos_read(CMOS_REG_HOUR);
    d     = cmos_read(CMOS_REG_DAY);
    mo    = cmos_read(CMOS_REG_MON);
    y     = cmos_read(CMOS_REG_YEAR);
    c     = cmos_read(CMOS_REG_CENTURY);
    statB = cmos_read(CMOS_REG_STATUS_B);

    /* Detect "no RTC": every register reads as 0xFF. */
    if (s == 0xFF && mn == 0xFF && h == 0xFF &&
        d == 0xFF && mo == 0xFF && y == 0xFF) {
        return -1;
    }

    /* Re-read to catch updates between byte reads. */
    int retries = 8;
    do {
        while (cmos_update_in_progress()) {
            if (++spin > 1000000) return -1;
        }
        s2  = cmos_read(CMOS_REG_SEC);
        mn2 = cmos_read(CMOS_REG_MIN);
        h2  = cmos_read(CMOS_REG_HOUR);
        d2  = cmos_read(CMOS_REG_DAY);
        mo2 = cmos_read(CMOS_REG_MON);
        y2  = cmos_read(CMOS_REG_YEAR);
        c2  = cmos_read(CMOS_REG_CENTURY);

        if (s == s2 && mn == mn2 && h == h2 &&
            d == d2 && mo == mo2 && y == y2 && c == c2)
            break;

        s = s2; mn = mn2; h = h2;
        d = d2; mo = mo2; y = y2; c = c2;
    } while (--retries > 0);

    /* If Status B bit 2 = 0 -> values are BCD. */
    int binary_mode = (statB & 0x04) ? 1 : 0;
    /* If Status B bit 1 = 0 -> 12-hour mode (PM bit on hours top bit). */
    int hour24      = (statB & 0x02) ? 1 : 0;

    int hour_pm = 0;
    if (!hour24 && (h & 0x80)) {
        hour_pm = 1;
        h &= 0x7F;
    }

    if (!binary_mode) {
        s  = bcd_to_bin(s);
        mn = bcd_to_bin(mn);
        h  = bcd_to_bin(h);
        d  = bcd_to_bin(d);
        mo = bcd_to_bin(mo);
        y  = bcd_to_bin(y);
        c  = bcd_to_bin(c);
    }

    if (!hour24) {
        if (h == 12) h = 0;
        if (hour_pm) h = (uint8_t)(h + 12);
    }

    /* Century reg: most modern firmware writes BCD century (e.g. 0x20).
     * But it's not universal; if it looks bogus (< 19 or > 21) fall back
     * to "20" since Zeos targets 21st-century hardware. */
    int century = (c >= 19 && c <= 21) ? c : 20;

    *year = century * 100 + y;
    *mon  = mo;
    *day  = d;
    *hour = h;
    *min  = mn;
    *sec  = s;

    /* Final sanity. If anything's out-of-range, declare failure. */
    if (*mon < 1 || *mon > 12) return -1;
    if (*day < 1 || *day > 31) return -1;
    if (*hour > 23)            return -1;
    if (*min  > 59)            return -1;
    if (*sec  > 59)            return -1;
    if (*year < 1970 || *year > 2199) return -1;

    return 0;
}

/* Write the given UTC date/time to CMOS. Honors Status B BCD/24h flags
 * so we don't accidentally flip the chip into a different mode. */
static int cmos_write_datetime(int year, int mon, int day,
                               int hour, int min, int sec)
{
    uint8_t statB = cmos_read(CMOS_REG_STATUS_B);
    if (statB == 0xFF) return -1;
    int binary_mode = (statB & 0x04) ? 1 : 0;
    int hour24      = (statB & 0x02) ? 1 : 0;

    /* Block updates while we write. Set bit 7 of Status B = SET flag. */
    cmos_write(CMOS_REG_STATUS_B, (uint8_t)(statB | 0x80));

    int hour_field = hour;
    int hour_pm    = 0;
    if (!hour24) {
        if (hour == 0) { hour_field = 12; hour_pm = 0; }
        else if (hour == 12) { hour_field = 12; hour_pm = 1; }
        else if (hour > 12) { hour_field = hour - 12; hour_pm = 1; }
        else { hour_field = hour; hour_pm = 0; }
    }

    int century = year / 100;
    int yy      = year % 100;

    uint8_t v_sec  = (uint8_t)sec;
    uint8_t v_min  = (uint8_t)min;
    uint8_t v_hour = (uint8_t)hour_field;
    uint8_t v_day  = (uint8_t)day;
    uint8_t v_mon  = (uint8_t)mon;
    uint8_t v_yr   = (uint8_t)yy;
    uint8_t v_cen  = (uint8_t)century;

    if (!binary_mode) {
        v_sec  = bin_to_bcd(v_sec);
        v_min  = bin_to_bcd(v_min);
        v_hour = bin_to_bcd(v_hour);
        v_day  = bin_to_bcd(v_day);
        v_mon  = bin_to_bcd(v_mon);
        v_yr   = bin_to_bcd(v_yr);
        v_cen  = bin_to_bcd(v_cen);
    }
    if (!hour24 && hour_pm) v_hour |= 0x80;

    cmos_write(CMOS_REG_SEC,     v_sec);
    cmos_write(CMOS_REG_MIN,     v_min);
    cmos_write(CMOS_REG_HOUR,    v_hour);
    cmos_write(CMOS_REG_DAY,     v_day);
    cmos_write(CMOS_REG_MON,     v_mon);
    cmos_write(CMOS_REG_YEAR,    v_yr);
    cmos_write(CMOS_REG_CENTURY, v_cen);

    /* Restore Status B (clear SET flag). */
    cmos_write(CMOS_REG_STATUS_B, statB);
    return 0;
}

/* ── Public API ────────────────────────────────────────────────── */

int tod_ready(void)
{
    return s_ready;
}

tod_source_t tod_source(void)
{
    return s_source;
}

uint64_t tod_now_unix(void)
{
    if (!s_ready) return 0;
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) return s_boot_epoch;
    uint64_t now_tsc = timer_read_tsc();
    uint64_t delta   = (now_tsc >= s_boot_tsc) ? (now_tsc - s_boot_tsc) : 0;
    return s_boot_epoch + (delta / freq);
}

uint32_t tod_now_millis(void)
{
    if (!s_ready) return 0;
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) return 0;
    uint64_t now_tsc = timer_read_tsc();
    uint64_t delta   = (now_tsc >= s_boot_tsc) ? (now_tsc - s_boot_tsc) : 0;
    /* Fractional second only -- modulo full second after dividing
     * the TSC delta by (freq/1000) to get milliseconds. */
    uint64_t total_ms = (delta / (freq / 1000ULL));
    return (uint32_t)(total_ms % 1000ULL);
}

/* ── Persistence ───────────────────────────────────────────────── */

void tod_persist_save(void)
{
    if (!s_ready) return;
    tod_persist_record_t rec = {
        .magic       = TOD_VAULT_MAGIC,
        .reserved    = 0,
        .saved_epoch = tod_now_unix(),
    };
    /* Best-effort. /time/ may not exist yet -- create it. */
    (void)vault_mkdir("/time", 1);
    (void)vault_write(TOD_VAULT_PATH, &rec, (uint32_t)sizeof(rec));
}

void tod_persist_load(void)
{
    tod_persist_record_t rec;
    if (vault_exists(TOD_VAULT_PATH) <= 0) return;
    int rc = vault_read(TOD_VAULT_PATH, &rec, (uint32_t)sizeof(rec));
    if (rc < (int)sizeof(rec)) return;
    if (rec.magic != TOD_VAULT_MAGIC) return;
    if (rec.saved_epoch < 1700000000ULL) return; /* < 2023, junk */

    /* Only adopt if CMOS didn't already give us a real time. */
    if (s_source == TOD_SRC_CMOS_RTC) return;

    s_boot_epoch = rec.saved_epoch;
    s_boot_tsc   = timer_read_tsc();
    s_ready      = 1;
    s_source     = TOD_SRC_PERSISTED;
}

void tod_init(void)
{
    if (s_ready && s_source == TOD_SRC_CMOS_RTC) return;

    int year, mon, day, hour, min, sec;
    if (cmos_read_datetime(&year, &mon, &day, &hour, &min, &sec) == 0) {
        s_boot_epoch = ymdhms_to_epoch(year, mon, day, hour, min, sec);
        s_boot_tsc   = timer_read_tsc();
        s_ready      = 1;
        s_source     = TOD_SRC_CMOS_RTC;
        return;
    }

    /* CMOS unreadable. Leave state pristine -- a later
     * tod_persist_load() (called from persistence_init) may upgrade us
     * to TOD_SRC_PERSISTED. */
    s_ready  = 0;
    s_source = TOD_SRC_NONE;
}

int tod_set(uint64_t epoch_secs)
{
    int year, mon, day, hour, min, sec;
    epoch_to_ymdhms(epoch_secs, &year, &mon, &day, &hour, &min, &sec);
    int rc = cmos_write_datetime(year, mon, day, hour, min, sec);
    if (rc != 0) {
        /* No CMOS -- still update our in-RAM clock so processes see
         * the new time, but report -1 so callers know it isn't durable
         * across reboot unless they also call tod_persist_save(). */
        s_boot_epoch = epoch_secs;
        s_boot_tsc   = timer_read_tsc();
        s_ready      = 1;
        if (s_source == TOD_SRC_NONE) s_source = TOD_SRC_PERSISTED;
        return -1;
    }
    s_boot_epoch = epoch_secs;
    s_boot_tsc   = timer_read_tsc();
    s_ready      = 1;
    s_source     = TOD_SRC_CMOS_RTC;
    return 0;
}

/* ── Formatting ────────────────────────────────────────────────── */

static int put2(char *buf, int max, int pos, int val)
{
    if (pos + 2 > max) return pos;
    buf[pos++] = (char)('0' + ((val / 10) % 10));
    buf[pos++] = (char)('0' + (val % 10));
    return pos;
}

static int put4(char *buf, int max, int pos, int val)
{
    if (pos + 4 > max) return pos;
    buf[pos++] = (char)('0' + ((val / 1000) % 10));
    buf[pos++] = (char)('0' + ((val / 100)  % 10));
    buf[pos++] = (char)('0' + ((val / 10)   % 10));
    buf[pos++] = (char)('0' + (val % 10));
    return pos;
}

int tod_format(uint64_t epoch_secs, char *buf, int max)
{
    if (!buf || max < 24) return 0;

    /* Apply tz offset (minutes east of UTC) for display only. */
    int64_t adj = (int64_t)epoch_secs + ((int64_t)tod_tz_offset_minutes * 60);
    if (adj < 0) adj = 0;

    int year, mon, day, hour, min, sec;
    epoch_to_ymdhms((uint64_t)adj, &year, &mon, &day, &hour, &min, &sec);

    int p = 0;
    p = put4(buf, max, p, year); buf[p++] = '-';
    p = put2(buf, max, p, mon);  buf[p++] = '-';
    p = put2(buf, max, p, day);  buf[p++] = ' ';
    p = put2(buf, max, p, hour); buf[p++] = ':';
    p = put2(buf, max, p, min);  buf[p++] = ':';
    p = put2(buf, max, p, sec);
    if (p + 4 < max) {
        buf[p++] = ' ';
        buf[p++] = 'U';
        buf[p++] = 'T';
        buf[p++] = 'C';
    }
    if (p < max) buf[p] = '\0';
    return p;
}

int tod_format_log(char *buf, int max)
{
    if (!buf || max < 16) return 0;
    if (!s_ready) {
        if (max >= 3) { buf[0] = '['; buf[1] = '?'; buf[2] = ']'; }
        if (max >= 4) buf[3] = '\0';
        return 3;
    }
    uint64_t epoch = tod_now_unix();
    uint32_t ms    = tod_now_millis();
    int year, mon, day, hour, min, sec;
    int64_t adj = (int64_t)epoch + ((int64_t)tod_tz_offset_minutes * 60);
    if (adj < 0) adj = 0;
    epoch_to_ymdhms((uint64_t)adj, &year, &mon, &day, &hour, &min, &sec);

    int p = 0;
    buf[p++] = '[';
    p = put2(buf, max, p, hour); buf[p++] = ':';
    p = put2(buf, max, p, min);  buf[p++] = ':';
    p = put2(buf, max, p, sec);  buf[p++] = '.';
    /* milliseconds 3-digit */
    buf[p++] = (char)('0' + ((ms / 100) % 10));
    buf[p++] = (char)('0' + ((ms / 10)  % 10));
    buf[p++] = (char)('0' + (ms % 10));
    buf[p++] = ']';
    if (p < max) buf[p] = '\0';
    return p;
}

/* Parse "YYYY-MM-DD HH:MM:SS" UTC. */
static int parse_digits(const char *s, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return 0;
}

int tod_parse(const char *s, uint64_t *out)
{
    if (!s || !out) return -1;
    /* Expected length: 19 chars. */
    int year, mon, day, hour, min, sec;
    if (parse_digits(s + 0,  4, &year) < 0) return -1;
    if (s[4]  != '-') return -1;
    if (parse_digits(s + 5,  2, &mon)  < 0) return -1;
    if (s[7]  != '-') return -1;
    if (parse_digits(s + 8,  2, &day)  < 0) return -1;
    if (s[10] != ' ' && s[10] != 'T') return -1;
    if (parse_digits(s + 11, 2, &hour) < 0) return -1;
    if (s[13] != ':') return -1;
    if (parse_digits(s + 14, 2, &min)  < 0) return -1;
    if (s[16] != ':') return -1;
    if (parse_digits(s + 17, 2, &sec)  < 0) return -1;
    if (mon < 1 || mon > 12)   return -1;
    if (day < 1 || day > 31)   return -1;
    if (hour > 23 || min > 59 || sec > 59) return -1;
    if (year < 1970 || year > 2199) return -1;
    *out = ymdhms_to_epoch(year, mon, day, hour, min, sec);
    return 0;
}

/* ── Selftest line ─────────────────────────────────────────────── */

void tod_print_selftest_line(void)
{
    kputs("Wall clock ............ ");
    if (!s_ready) {
        kputs("(CMOS not present, TSC-only mode)\n");
        return;
    }
    char buf[40];
    int n = tod_format(tod_now_unix(), buf, sizeof(buf));
    if (n <= 0) {
        kputs("(format error)\n");
        return;
    }
    kputs(buf);
    if (s_source == TOD_SRC_CMOS_RTC)        kputs(" (CMOS read OK)\n");
    else if (s_source == TOD_SRC_PERSISTED)  kputs(" (persisted fallback)\n");
    else                                     kputs(" (unknown source)\n");
}

/* ── Shell commands ────────────────────────────────────────────── */

/* Skip leading spaces. Returns first non-space char in s. */
static const char *skip_ws(const char *s)
{
    while (s && *s == ' ') s++;
    return s;
}

void tod_cmd_date(const char *args)
{
    args = skip_ws(args);
    if (args && *args) {
        /* Set form: date "YYYY-MM-DD HH:MM:SS"  (quotes optional) */
        const char *p = args;
        if (*p == '"' || *p == '\'') p++;
        uint64_t epoch;
        if (tod_parse(p, &epoch) != 0) {
            kputs("date: bad format. expected: date \"YYYY-MM-DD HH:MM:SS\" (UTC)\n");
            return;
        }
        int rc = tod_set(epoch);
        if (rc == 0) kputs("date: set OK (CMOS).\n");
        else         kputs("date: set in RAM (no CMOS, won't survive reboot without persist).\n");
        char buf[40];
        tod_format(tod_now_unix(), buf, sizeof(buf));
        kputs("  now = "); kputs(buf); kputc('\n');
        return;
    }

    if (!s_ready) {
        kputs("date: wall clock not initialized (TSC-only mode)\n");
        return;
    }
    char buf[40];
    tod_format(tod_now_unix(), buf, sizeof(buf));
    kputs(buf); kputc('\n');
}

void tod_cmd_uptime(const char *args)
{
    (void)args;
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) {
        kputs("uptime: TSC not calibrated\n");
        return;
    }
    uint64_t now    = timer_read_tsc();
    uint64_t base   = s_ready ? s_boot_tsc : 0;
    /* If tod_init never ran, fall back to "since whatever TSC was at
     * call site of timer_init"; we don't have that captured separately,
     * so just use 0 -> seconds since power-on (close enough). */
    uint64_t delta  = (now >= base) ? (now - base) : now;
    uint64_t secs   = delta / freq;

    uint64_t days = secs / 86400; secs %= 86400;
    uint64_t hrs  = secs / 3600;  secs %= 3600;
    uint64_t mins = secs / 60;    secs %= 60;

    if (days) { kput_dec(days); kputs("d "); }
    if (days || hrs) { kput_dec(hrs); kputs("h "); }
    kput_dec(mins); kputs("m ");
    kput_dec(secs); kputs("s\n");
}

void tod_cmd_epoch(const char *args)
{
    (void)args;
    if (!s_ready) {
        kputs("0\n");
        return;
    }
    kput_dec(tod_now_unix());
    kputc('\n');
}
