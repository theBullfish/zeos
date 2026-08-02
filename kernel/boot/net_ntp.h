#ifndef ZEOS_NET_NTP_H
#define ZEOS_NET_NTP_H

/* SNTP client. Calibrate the system clock to a central public NTP source.
 * Returns 0 on success (tod_set applied), -1 on failure. Blocks up to ~2s. */
int ntp_sync(void);

/* 1 if the last ntp_sync() succeeded, else 0. */
int ntp_last_ok(void);

/* settings knob "time.sync_now": getter/setter (writing '1' triggers a sync). */
int time_sync_get(char *out, int n);
int time_sync_set(const char *v);

#endif /* ZEOS_NET_NTP_H */
