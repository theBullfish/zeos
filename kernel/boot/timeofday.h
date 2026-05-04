/*
 * Zeos -- Time of Day (Wall Clock)
 *
 * Wall-clock time. CMOS RTC read at boot, TSC-derived monotonic for
 * fractional seconds. UTC only initially; tz_offset_minutes hook
 * exposed for future timezone support.
 *
 * Pipeline:
 *   tod_init()  -- runs after timer_init(): reads CMOS RTC (ports
 *                  0x70/0x71), converts BCD if Status Reg B bit 2 = 0,
 *                  builds boot_epoch (Unix seconds), captures boot_TSC.
 *   tod_now_unix() -- boot_epoch + (TSC_now - boot_TSC) / tsc_freq.
 *   tod_format()   -- "YYYY-MM-DD HH:MM:SS UTC", standalone, no libc.
 *
 * If CMOS read fails (no RTC, e.g. some hypervisors), tod_init falls
 * back to the last persisted wall clock written to VAULT, plus an
 * optional elapsed estimate. If neither is available, the module
 * stays in TSC-only mode and tod_now_unix() returns 0.
 */

#ifndef ZEOS_TIMEOFDAY_H
#define ZEOS_TIMEOFDAY_H

#include <stdint.h>

/* Source the boot epoch came from. Useful for selftest reporting. */
typedef enum {
    TOD_SRC_NONE      = 0,  /* no wall clock available */
    TOD_SRC_CMOS_RTC  = 1,  /* read CMOS RTC at boot */
    TOD_SRC_PERSISTED = 2,  /* fell back to last-known persisted tod */
} tod_source_t;

/* Read the CMOS RTC, build boot_epoch, capture boot_TSC.
 * Idempotent on repeat calls (first one wins). */
void tod_init(void);

/* Current Unix epoch seconds. 0 if tod_init hasn't been called or
 * CMOS read failed and there's no persisted fallback. */
uint64_t tod_now_unix(void);

/* Sub-second helper: returns (0..999) milliseconds within the current
 * wall-clock second. Computed from TSC delta since boot, modulo 1000. */
uint32_t tod_now_millis(void);

/* Format an arbitrary Unix epoch into "YYYY-MM-DD HH:MM:SS UTC". UTC
 * (or shifted by tz_offset_minutes if you set it). Returns number of
 * bytes written (excluding NUL), or 0 if buf too small / not ready. */
int tod_format(uint64_t epoch_secs, char *buf, int max);

/* Convenience: now in log-friendly form "[HH:MM:SS.mmm]". Writes a
 * NUL-terminated string. Returns bytes written or 0 if not ready. */
int tod_format_log(char *buf, int max);

/* Set wall clock from a Unix epoch. Writes back to CMOS RTC (BCD if
 * needed) and rebases boot_epoch / boot_TSC so subsequent
 * tod_now_unix() returns the right value immediately.
 * Returns 0 on success, -1 if CMOS not available. */
int tod_set(uint64_t epoch_secs);

/* Parse "YYYY-MM-DD HH:MM:SS" (UTC) into Unix epoch seconds.
 * Returns 0 on success, -1 on parse error. Writes *out on success. */
int tod_parse(const char *s, uint64_t *out);

/* True if tod_init successfully sourced wall clock from somewhere. */
int tod_ready(void);

/* Where did the boot epoch come from? */
tod_source_t tod_source(void);

/* Timezone offset hook -- minutes east of UTC. Default 0. Future
 * timezone DB / DST is a separate task per docs/OS_LITTLE_THINGS.md. */
extern int tod_tz_offset_minutes;

/* Persistence hooks (called from persistence.c flush path). Stores
 * the current wall clock to VAULT so a CMOS-less reboot can recover
 * a low-fidelity clock instead of starting at 1970-01-01. */
void tod_persist_save(void);
void tod_persist_load(void);

/* Selftest line printer. Called by main.c after tod_init.
 * Prints one of:
 *   "Wall clock ............ YYYY-MM-DD HH:MM:SS UTC (CMOS read OK)\n"
 *   "Wall clock ............ YYYY-MM-DD HH:MM:SS UTC (persisted fallback)\n"
 *   "Wall clock ............ (CMOS not present, TSC-only mode)\n"
 */
void tod_print_selftest_line(void);

/* ── Shell command implementations ──────────────────────────────
 * Wired internally; need a one-line entry each in shell.c's command
 * table (kept out of this commit because shell.c is in flight for
 * the suspend/resume agent). Signatures match the existing
 * cmd_*(const char *args) pattern. */
void tod_cmd_date(const char *args);
void tod_cmd_uptime(const char *args);
void tod_cmd_epoch(const char *args);

#endif /* ZEOS_TIMEOFDAY_H */
