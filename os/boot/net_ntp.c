/*
 * Zeos — SNTP client (RFC 4330). Calibrate the system clock to a central,
 * public time source. One-shot: bring the link up, resolve pool.ntp.org,
 * UDP-query :123, parse the server's transmit timestamp, hand it to
 * tod_set(). Exposed as a settings knob ("time.sync_now") so the user can
 * calibrate on demand, plus a shell command.
 */
#include "net.h"
#include "net_udp.h"
#include "net_dns.h"
#include "net_dhcp.h"
#include "timeofday.h"
#include "kprint.h"
#include "net_ntp.h"

#define NTP_PORT        123
#define NTP_SRC_PORT    45123
#define NTP_EPOCH_DELTA 2208988800ULL   /* seconds from 1900-01-01 to 1970-01-01 */

static volatile int      s_have;
static volatile uint64_t s_epoch;
static int               s_last_ok;

/* UDP receive callback: pull the transmit timestamp (bytes 40..43, seconds
 * since 1900, big-endian) and convert to Unix epoch. */
static void ntp_recv(struct ipv4_addr src, uint16_t src_port,
                     const void *data, uint16_t len)
{
    (void)src; (void)src_port;
    if (len < 48) return;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t secs = ((uint32_t)p[40] << 24) | ((uint32_t)p[41] << 16) |
                    ((uint32_t)p[42] << 8)  |  (uint32_t)p[43];
    if (secs <= NTP_EPOCH_DELTA) return;   /* bogus / unsynced server */
    s_epoch = (uint64_t)secs - NTP_EPOCH_DELTA;
    s_have  = 1;
}

int ntp_sync(void)
{
    extern void net_poll_wait(uint32_t);

    /* Need a live link + DHCP lease. DHCP is async (pumped by net_poll), so if
     * we have no lease yet, kick a discover and pump RX until it binds (~3s). */
    const struct dhcp_lease *lease = dhcp_get_lease();
    if (!lease) {
        if (dhcp_discover() < 0) { kputs("[ntp] no network device\n"); s_last_ok = 0; return -1; }
        for (int i = 0; i < 300 && !dhcp_get_lease(); i++)
            net_poll_wait(10);
        lease = dhcp_get_lease();
        if (!lease) { kputs("[ntp] no DHCP lease (timeout)\n"); s_last_ok = 0; return -1; }
    }

    /* Resolve pool.ntp.org; fall back to time.cloudflare.com anycast. */
    struct ipv4_addr ip;
    if (dns_resolve("pool.ntp.org", &ip) < 0) {
        ip.b[0] = 162; ip.b[1] = 159; ip.b[2] = 200; ip.b[3] = 123;
        kputs("[ntp] DNS failed -> fallback 162.159.200.123\n");
    }

    udp_bind(NTP_SRC_PORT, ntp_recv);

    uint8_t pkt[48];
    for (int i = 0; i < 48; i++) pkt[i] = 0;
    pkt[0] = 0x1B;   /* LI=0, VN=3, Mode=3 (client) */

    s_have = 0;
    udp_send(ip, NTP_SRC_PORT, NTP_PORT, pkt, sizeof(pkt));

    /* Pump the RX path for up to ~2s waiting for the reply. */
    for (int i = 0; i < 200 && !s_have; i++)
        net_poll_wait(10);

    if (!s_have) { kputs("[ntp] no reply (timeout)\n"); s_last_ok = 0; return -1; }

    tod_set(s_epoch);
    s_last_ok = 1;
    {
        char buf[40];
        if (tod_format(s_epoch, buf, sizeof(buf)) > 0) {
            kputs("[ntp] clock calibrated -> "); kputs(buf); kputs("\n");
        } else {
            kputs("[ntp] clock calibrated from central NTP\n");
        }
    }
    return 0;
}

int ntp_last_ok(void) { return s_last_ok; }

/* ── Settings knob: "time.sync_now" ──
 * getter reports the last sync result; writing a truthy value triggers a sync. */
static int u_to_str(uint64_t v, char *out, int n) {
    if (n < 2) return -1;
    char t[24]; int i = 0;
    if (v == 0) t[i++] = '0';
    while (v && i < 23) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    int o = 0; while (i > 0 && o < n - 1) out[o++] = t[--i]; out[o] = 0; return 0;
}

int time_sync_get(char *out, int n) { return u_to_str((uint64_t)(s_last_ok ? 1 : 0), out, n); }

int time_sync_set(const char *v)
{
    if (v && (v[0] == '1' || v[0] == 't' || v[0] == 'T' ||
              v[0] == 'y' || v[0] == 'Y' || v[0] == 'o' || v[0] == 'O'))
        return ntp_sync();
    return 0;
}
