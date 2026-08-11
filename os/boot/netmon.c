/*
 * Zeos — network activity monitor. See netmon.h for why this exists.
 *
 * Implementation note on why this is a HARDWIRE and not a hook: net.c assigns
 * net_drv_send / net_drv_recv exactly once. We capture those two pointers and
 * replace them with shims that count and forward. Every existing caller — the
 * TCP stack, DHCP, DNS, the browser, anything added later — is counted with no
 * cooperation from that code and no way to opt out short of reassigning the
 * pointers, which would be a visible, deliberate act in review.
 */
#include "netmon.h"
#include "net.h"
#include "timer.h"

static int (*real_send)(const void *data, uint16_t len);
static int (*real_recv)(void *buf, uint16_t max_len);
static int installed;

static uint64_t tot_tx, tot_rx, pk_tx, pk_rx;
static uint32_t hist_tx[NETMON_SLOTS], hist_rx[NETMON_SLOTS];
static uint32_t cur_tx, cur_rx;      /* accumulating into the newest slot */
static uint32_t idle_samples = 9999; /* large = quiet since boot */

uint64_t netmon_total_tx(void) { return tot_tx; }
uint64_t netmon_total_rx(void) { return tot_rx; }
uint64_t netmon_pkts_tx(void)  { return pk_tx; }
uint64_t netmon_pkts_rx(void)  { return pk_rx; }
const uint32_t *netmon_hist_tx(void) { return hist_tx; }
const uint32_t *netmon_hist_rx(void) { return hist_rx; }
int netmon_active(void) { return idle_samples < 3; }

uint32_t netmon_hist_peak(void)
{
    uint32_t p = 0;
    for (int i = 0; i < NETMON_SLOTS; i++) {
        if (hist_tx[i] > p) p = hist_tx[i];
        if (hist_rx[i] > p) p = hist_rx[i];
    }
    return p;
}

static int mon_send(const void *data, uint16_t len)
{
    int rc = real_send ? real_send(data, len) : -1;
    /* Count what was actually accepted by the hardware. A failed send still
     * attempted to put bytes on the wire, so count the attempt either way —
     * under-reporting is the one failure mode this must never have. */
    tot_tx += len; pk_tx++; cur_tx += len; idle_samples = 0;
    return rc;
}

static int mon_recv(void *buf, uint16_t max_len)
{
    int rc = real_recv ? real_recv(buf, max_len) : -1;
    if (rc > 0) { tot_rx += (uint64_t)rc; pk_rx++; cur_rx += (uint32_t)rc; idle_samples = 0; }
    return rc;
}

void netmon_install(void)
{
    if (installed) return;
    real_send = net_drv_send;
    real_recv = net_drv_recv;
    net_drv_send = mon_send;
    net_drv_recv = mon_recv;
    installed = 1;
}

/* ~100 ms per sample, so NETMON_SLOTS spans a few seconds of history. Driven off
 * the TSC rather than the tick count: the scheduler's rate is an implementation
 * detail, and a window measured in milliseconds is what a person can actually
 * catch. At tick rate the whole window was a fraction of a second and traffic
 * vanished before anyone could see it. */
#define NETMON_SAMPLE_MS 250ULL

void netmon_sample(void)
{
    static uint64_t next_at;
    uint64_t now = timer_read_tsc();
    uint64_t hz  = timer_tsc_freq();
    if (hz) {
        if (now < next_at) return;                  /* accumulate, don't shift */
        next_at = now + (hz / 1000ULL) * NETMON_SAMPLE_MS;
    }

    /* Shift the window left and drop in the accumulated sample. */
    for (int i = 0; i < NETMON_SLOTS - 1; i++) {
        hist_tx[i] = hist_tx[i + 1];
        hist_rx[i] = hist_rx[i + 1];
    }
    hist_tx[NETMON_SLOTS - 1] = cur_tx;
    hist_rx[NETMON_SLOTS - 1] = cur_rx;
    if (cur_tx == 0 && cur_rx == 0) {
        if (idle_samples < 0xFFFFFFFFu) idle_samples++;
    } else {
        idle_samples = 0;
    }
    cur_tx = 0; cur_rx = 0;
}

/* ── The always-on-top indicator ────────────────────────────────────────────
 *
 * Deliberately small and unobtrusive, deliberately impossible to hide: drawn by
 * compositor_present() after every window, the panel and the cursor, with no
 * visibility flag and no setting to disable it.
 *
 * Reading it: a flat line means silence. Any movement means bytes crossed the
 * NIC. Bars above the midline are transmit (what LEAVES this machine — the half
 * that matters for privacy); below is receive. The glow tracks recent activity so
 * motion is caught peripherally, without staring at it.
 */
#include "fb.h"
#include "wm.h"

#define MON_W      96
#define MON_H      22
#define MON_MARGIN 8

/* Neon orange -> yellow. TX runs hotter (orange) than RX (yellow) so the
 * direction is readable at a glance, at this size, without a legend. */
#define NEON_TX      0xFFFF7A18u
#define NEON_TX_DIM  0x60FF7A18u
#define NEON_RX      0xFFFFD230u
#define NEON_RX_DIM  0x50FFD230u
#define MON_BG       0x66101014u
#define MON_BASELINE 0x66FFA030u

/* The ONE case where the indicator stands down: a program the user deliberately
 * put fullscreen. Choosing fullscreen means "show me only this", and burning a
 * trace into a video or a game the user asked to fill the screen would be wrong.
 *
 * This is a user act, not something software does behind their back — which is the
 * whole distinction the indicator exists to police. Leave fullscreen and it is
 * back instantly; the counters never stopped (netmon_total_tx/rx keep running), so
 * nothing that happened while it was hidden is lost — `netstat`-style totals still
 * account for every byte. */
static int fullscreen_covering(void)
{
    extern int wm_get_focused(void);
    extern chain_surface_t *wm_get_surface(int id);
    int id = wm_get_focused();
    if (id < 0) return 0;
    chain_surface_t *s = wm_get_surface(id);
    if (!s) return 0;
    /* Judge by COVERAGE, not by a flag: anything that actually spans the display
     * counts, however it got there. */
    int sw = (int)fb_width(), sh = (int)fb_height();
    if (sw <= 0 || sh <= 0) return 0;
    return (s->x <= 0 && s->y <= 0 && s->w >= sw && s->h >= sh);
}

void netmon_draw_overlay(void)
{
    int sw = (int)fb_width(), sh = (int)fb_height();
    if (sw <= 0 || sh <= 0) return;
    if (fullscreen_covering()) return;    /* user asked for one thing only */

    /* Top-right corner: out of the way of content, never under the dock. */
    int x0 = sw - MON_W - MON_MARGIN;
    int y0 = MON_MARGIN;
    int mid = y0 + MON_H / 2;

    /* Faint smoked panel so the trace stays legible over any wallpaper, window
     * or video underneath. Kept translucent: present, not shouting. */
    fb_rect_blend(x0 - 3, y0 - 3, MON_W + 6, MON_H + 6, MON_BG);

    int active = netmon_active();
    if (active) {
        /* Glow: two wider, fainter passes behind the trace. */
        fb_rect_blend(x0 - 2, mid - 4, MON_W + 4, 9, NEON_TX_DIM & 0x2AFFFFFFu);
        fb_rect_blend(x0 - 1, mid - 2, MON_W + 2, 5, NEON_RX_DIM & 0x2AFFFFFFu);
    }

    /* Baseline — always visible, so "quiet" is a positive statement rather than
     * an absence of drawing (a blank area could mean broken; a line cannot). */
    fb_rect_blend(x0, mid, MON_W, 1, MON_BASELINE);

    const uint32_t *tx = netmon_hist_tx();
    const uint32_t *rx = netmon_hist_rx();
    uint32_t peak = netmon_hist_peak();
    if (peak < 64) peak = 64;                 /* floor so a single packet shows */

    int half = (MON_H / 2) - 1;
    int step = MON_W / NETMON_SLOTS;
    if (step < 1) step = 1;

    for (int i = 0; i < NETMON_SLOTS; i++) {
        int bx = x0 + i * step;
        if (bx + step > x0 + MON_W) break;

        /* Log-ish compression: one packet must be visible next to a burst, so
         * scale by a shifted ratio rather than linear-to-peak. */
        uint32_t t = tx[i], r = rx[i];
        int th = t ? (int)((t * (uint32_t)half) / peak) : 0;
        int rh = r ? (int)((r * (uint32_t)half) / peak) : 0;
        if (t && th < 2) th = 2;              /* never render traffic as nothing */
        if (r && rh < 2) rh = 2;
        if (th > half) th = half;
        if (rh > half) rh = half;

        /* Newest samples at the right, brightest; older fade back. */
        int fresh = (i >= NETMON_SLOTS - 6);
        if (th) fb_rect_blend(bx, mid - th, step, th, fresh ? NEON_TX : NEON_TX_DIM);
        if (rh) fb_rect_blend(bx, mid + 1, step, rh, fresh ? NEON_RX : NEON_RX_DIM);
    }
}
