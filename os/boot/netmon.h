/*
 * Zeos — network activity monitor. A GUARDRAIL, not a feature.
 *
 * Every byte in or out of this machine is counted here, because the counters are
 * installed by wrapping the driver dispatch pointers themselves (net_drv_send /
 * net_drv_recv) at the single place they are assigned. Nothing in the OS can move
 * a packet without passing through the shim — including us.
 *
 * The dock draws this continuously. If the wave moves when nothing should be
 * talking, that is visible to the person using the machine, immediately, without
 * tools and without trust. It is designed to catch OUR OWN misbehaviour as
 * readily as anyone else's.
 *
 * Deliberately NOT configurable and NOT switchable off.
 */
#ifndef ZEOS_NETMON_H
#define ZEOS_NETMON_H

#include <stdint.h>

#define NETMON_SLOTS 48        /* history samples the dock waveform shows */

/* Install the counting shims over the driver pointers. Called from net_init
 * AFTER the driver is selected. Idempotent. */
void netmon_install(void);

/* Advance the history one sample (driven by the scheduler tick). */
void netmon_sample(void);

/* Bytes seen since boot — totals never reset, so they can be sanity-checked. */
uint64_t netmon_total_tx(void);
uint64_t netmon_total_rx(void);

/* Packet counts since boot. */
uint64_t netmon_pkts_tx(void);
uint64_t netmon_pkts_rx(void);

/* History for the waveform. slot 0 = oldest, NETMON_SLOTS-1 = newest.
 * Values are bytes in that sample window. */
const uint32_t *netmon_hist_tx(void);
const uint32_t *netmon_hist_rx(void);

/* Peak byte count across the current history window (for autoscaling). */
uint32_t netmon_hist_peak(void);

/* 1 while there has been traffic very recently (drives the glow). */
int netmon_active(void);

#endif
