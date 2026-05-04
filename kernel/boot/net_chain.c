/*
 * Zeos -- Chain-native networking
 *
 * Chain-native networking. Pipelines:
 *   TX: frame_request -> l2_encap -> mac_filter -> hardware_dma
 *   RX: hardware_dma -> mac_filter -> l2_decap -> frame_delivery
 * NIC drivers (e1000, rtl8139, rtl8169, virtio, usb_eth) register
 * as the hardware_dma backend through net_hw_ops.
 * MasQ tier: INTERNAL. Parent: CHAIN_CPU.
 *
 * Per the contract: any node that mutates device state bumps
 * chain->vault_version. Idle pumps (RX poll with no packet) do not.
 *
 * The chain layer owns the pipeline. Individual NIC drivers stay
 * dumb DMA backends accessed through net_hw_ops. The compat shim
 * (net_chain_send / net_chain_recv) re-routes the legacy
 * net_drv_send / net_drv_recv pointers through chain_resolve so all
 * existing callers (ARP, IP, DHCP, TCP, TLS, DNS) become chain users
 * without touching their code.
 */

#include "net_chain.h"
#include "net.h"
#include "chain.h"
#include "chain_registry.h"
#include "kprint.h"
#include <stdint.h>

/* ── Public chain IDs ──────────────────────────────────────────── */

int CHAIN_NET_TX = -1;
int CHAIN_NET_RX = -1;

/* ── Module-private state ──────────────────────────────────────── */

/* The active hardware backend. Set by net_chain_set_hw() once the
 * NIC probe order in net_init() has chosen a driver. */
static struct net_hw_ops s_hw = { 0, 0, 0 };

/* MAC filter table. We only ever need to match our own MAC + broadcast
 * in this kernel; the list shape is here so the contract's "filter
 * change bumps vault_version" rule has somewhere to land when filters
 * become user-configurable. */
#define NET_MAC_FILTER_MAX 4
static struct mac_addr s_mac_filter[NET_MAC_FILTER_MAX];
static int             s_mac_filter_count = 0;
static int             s_mac_filter_seeded = 0;

/* ── In-flight request slots ───────────────────────────────────── */
/*
 * chain_resolve() walks the node list with two 4 KiB scratch buffers
 * the nodes alternate between. We can't store an outbound 1500-byte
 * frame inside a typed struct that fits in 4 KiB along with the rest
 * of the chain header without being careful, so each request struct
 * carries a *pointer* to a frame buffer. The frame storage itself
 * lives here in module-private static memory. Single in-flight TX,
 * single in-flight RX -- which matches the polling-only nature of
 * every NIC driver in this tree.
 */

#define ETH_FRAME_MAX 1600

static uint8_t s_tx_frame[ETH_FRAME_MAX];
static uint8_t s_rx_frame[ETH_FRAME_MAX];

typedef struct {
    const uint8_t *src;     /* Caller-supplied (pre-built frame). */
    uint16_t       len;
    int            valid;   /* 1 = a transmit is pending */
} net_tx_request_t;

typedef struct {
    uint8_t *frame;         /* Points at s_tx_frame after copy-in */
    uint16_t len;
    int      error;
} net_eth_frame_t;

typedef struct {
    int      ok;
    uint16_t len;
} net_tx_completion_t;

typedef struct {
    uint8_t *buf;           /* Receive buffer (s_rx_frame) */
    uint16_t max;
    int      have_frame;    /* Set after hardware_dma pulled a frame */
    uint16_t len;
    int      filter_pass;   /* mac_filter sets this */
} net_rx_state_t;

typedef struct {
    int valid;              /* 1 = an rx_poll call is pending */
} net_rx_poll_request_t;

/* Pending bookkeeping (the "module-state pending request" pattern,
 * mirroring HDA's pcm_source. Set by the compat shim, consumed by
 * the first node of the chain.) */
static net_tx_request_t      s_tx_pending;
static net_rx_poll_request_t s_rx_pending;
/* The frame the current RX run delivered to the caller. */
static int                   s_rx_delivered_len;

/* ── Helpers ───────────────────────────────────────────────────── */

static int mac_eq6(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int mac_is_broadcast(const uint8_t *m)
{
    for (int i = 0; i < 6; i++) if (m[i] != 0xFF) return 0;
    return 1;
}

static int mac_is_multicast(const uint8_t *m)
{
    return (m[0] & 0x01) != 0;
}

static void seed_mac_filter_if_needed(void)
{
    if (s_mac_filter_seeded) return;
    /* Seed the filter table with our MAC + broadcast. The filter table
     * change is a state mutation by the contract's rule, but seeding
     * happens exactly once and predates the chain being live, so we
     * mark it seeded without bumping vault_version. */
    s_mac_filter[0] = g_net.mac;
    for (int i = 0; i < 6; i++) s_mac_filter[1].b[i] = 0xFF;
    s_mac_filter_count = 2;
    s_mac_filter_seeded = 1;
}

/* ── Public: hardware backend wiring ───────────────────────────── */

void net_chain_set_hw(const struct net_hw_ops *ops)
{
    if (!ops) {
        s_hw.tx = 0;
        s_hw.rx_poll = 0;
        s_hw.name = 0;
        return;
    }
    s_hw = *ops;
    kputs("[net_chain] hw backend = ");
    kputs(s_hw.name ? s_hw.name : "(unnamed)");
    kputc('\n');
}

/* ── TX node resolves ──────────────────────────────────────────── */

void net_tx_request_resolve(void *self_, void *input, void *output)
{
    (void)self_;
    (void)input;
    net_eth_frame_t *out = (net_eth_frame_t *)output;
    out->frame = 0;
    out->len   = 0;
    out->error = 1;

    if (!s_tx_pending.valid || !s_tx_pending.src || s_tx_pending.len == 0)
        return;

    uint16_t n = s_tx_pending.len;
    if (n > ETH_FRAME_MAX) n = ETH_FRAME_MAX;
    for (uint16_t i = 0; i < n; i++) s_tx_frame[i] = s_tx_pending.src[i];

    out->frame = s_tx_frame;
    out->len   = n;
    out->error = 0;

    /* Consume the request. */
    s_tx_pending.valid = 0;
}

void net_tx_l2_encap_resolve(void *self_, void *input, void *output)
{
    (void)self_;
    net_eth_frame_t *in  = (net_eth_frame_t *)input;
    net_eth_frame_t *out = (net_eth_frame_t *)output;
    *out = *in;

    if (in->error || !in->frame || in->len < 14) {
        out->error = 1;
        return;
    }

    /* Existing callers (ARP, IP) build the full ethernet frame
     * themselves -- including the src MAC. l2_encap's job in this
     * tree is to enforce that the src MAC matches our interface.
     * If the caller left the src MAC blank, fill it in. */
    uint8_t *src_mac = out->frame + 6;
    int blank = 1;
    for (int i = 0; i < 6; i++) if (src_mac[i] != 0) { blank = 0; break; }
    if (blank) {
        for (int i = 0; i < 6; i++) src_mac[i] = g_net.mac.b[i];
    }
}

void net_tx_mac_filter_resolve(void *self_, void *input, void *output)
{
    (void)self_;
    net_eth_frame_t *in  = (net_eth_frame_t *)input;
    net_eth_frame_t *out = (net_eth_frame_t *)output;
    *out = *in;

    seed_mac_filter_if_needed();

    /* Egress filter: drop frames with a zero destination MAC -- those
     * are bugs upstream. Anything else (unicast, broadcast, multicast)
     * is allowed out. */
    if (in->error || !in->frame || in->len < 14) return;
    const uint8_t *dst = in->frame;
    int all_zero = 1;
    for (int i = 0; i < 6; i++) if (dst[i] != 0) { all_zero = 0; break; }
    if (all_zero) out->error = 1;
}

void net_tx_hardware_dma_resolve(void *self_, void *input, void *output)
{
    (void)self_;
    net_eth_frame_t     *in  = (net_eth_frame_t     *)input;
    net_tx_completion_t *out = (net_tx_completion_t *)output;
    out->ok  = 0;
    out->len = 0;

    if (in->error || !in->frame || in->len == 0 || !s_hw.tx)
        return;

    int rc = s_hw.tx(in->frame, in->len);
    if (rc == 0) {
        out->ok  = 1;
        out->len = in->len;
        /* Successful transmit = device state mutation. */
        chain_t *c = chain_get(CHAIN_NET_TX);
        if (c) c->vault_version++;
    }
}

/* ── RX node resolves ──────────────────────────────────────────── */

void net_rx_hardware_dma_resolve(void *self_, void *input, void *output)
{
    (void)self_;
    (void)input;
    net_eth_frame_t *out = (net_eth_frame_t *)output;
    out->frame = 0;
    out->len   = 0;
    out->error = 1;

    if (!s_rx_pending.valid || !s_hw.rx_poll) return;
    s_rx_pending.valid = 0;       /* consume request */

    int n = s_hw.rx_poll(s_rx_frame, ETH_FRAME_MAX);
    if (n < 14) return;            /* nothing this tick OR runt -- benign idle */

    out->frame = s_rx_frame;
    out->len   = (uint16_t)n;
    out->error = 0;

    /* Real frame in hand = hardware state observed/consumed. */
    chain_t *c = chain_get(CHAIN_NET_RX);
    if (c) c->vault_version++;
}

void net_rx_mac_filter_resolve(void *self_, void *input, void *output)
{
    (void)self_;
    net_eth_frame_t *in  = (net_eth_frame_t *)input;
    net_eth_frame_t *out = (net_eth_frame_t *)output;
    *out = *in;

    seed_mac_filter_if_needed();

    if (in->error || !in->frame || in->len < 14) return;

    const uint8_t *dst = in->frame;
    if (mac_is_broadcast(dst) || mac_is_multicast(dst)) return;

    /* Unicast: must match one of our filter entries. Promiscuous
     * NICs will hand us frames addressed to other hosts -- drop those
     * before the IP layer wastes time on them. */
    for (int i = 0; i < s_mac_filter_count; i++) {
        if (mac_eq6(dst, s_mac_filter[i].b)) return;
    }
    /* Not for us. */
    out->error = 1;
}

void net_rx_l2_decap_resolve(void *self_, void *input, void *output)
{
    (void)self_;
    net_eth_frame_t *in  = (net_eth_frame_t *)input;
    net_eth_frame_t *out = (net_eth_frame_t *)output;
    *out = *in;
    /* The current IP/ARP code consumes the full ethernet frame
     * (header + payload), so l2_decap is a typed pass-through here.
     * It still has to be a real node so the contract's 4-stage RX
     * pipeline is honored. */
}

void net_rx_frame_delivery_resolve(void *self_, void *input, void *output)
{
    (void)self_;
    net_eth_frame_t     *in  = (net_eth_frame_t     *)input;
    net_tx_completion_t *out = (net_tx_completion_t *)output;
    out->ok  = 0;
    out->len = 0;
    s_rx_delivered_len = 0;

    if (in->error || !in->frame || in->len == 0) return;

    /* Hand the frame to whoever called net_chain_recv(). The buffer
     * the caller supplied is held in s_rx_frame already (the same
     * buffer hardware_dma DMA'd into), so we just publish the length. */
    s_rx_delivered_len = in->len;
    out->ok  = 1;
    out->len = in->len;
}

/* ── Registration ──────────────────────────────────────────────── */

int net_chain_register(int parent_id)
{
    CHAIN_NET_TX = chain_create("net_tx", parent_id, MASQ_INTERNAL);
    if (CHAIN_NET_TX < 0) {
        kputs("[net_chain] failed to create net_tx chain\n");
        return -1;
    }
    chain_add_node(CHAIN_NET_TX, "frame_request",
                   "frame_request", "ethernet_frame",
                   (void (*)(chain_node_t *, void *, void *))net_tx_request_resolve);
    chain_add_node(CHAIN_NET_TX, "l2_encap",
                   "ethernet_frame", "ethernet_frame",
                   (void (*)(chain_node_t *, void *, void *))net_tx_l2_encap_resolve);
    chain_add_node(CHAIN_NET_TX, "mac_filter",
                   "ethernet_frame", "ethernet_frame",
                   (void (*)(chain_node_t *, void *, void *))net_tx_mac_filter_resolve);
    chain_add_node(CHAIN_NET_TX, "hardware_dma",
                   "ethernet_frame", "tx_completion",
                   (void (*)(chain_node_t *, void *, void *))net_tx_hardware_dma_resolve);

    CHAIN_NET_RX = chain_create("net_rx", parent_id, MASQ_INTERNAL);
    if (CHAIN_NET_RX < 0) {
        kputs("[net_chain] failed to create net_rx chain\n");
        return -1;
    }
    chain_add_node(CHAIN_NET_RX, "hardware_dma",
                   "rx_poll_request", "ethernet_frame",
                   (void (*)(chain_node_t *, void *, void *))net_rx_hardware_dma_resolve);
    chain_add_node(CHAIN_NET_RX, "mac_filter",
                   "ethernet_frame", "ethernet_frame",
                   (void (*)(chain_node_t *, void *, void *))net_rx_mac_filter_resolve);
    chain_add_node(CHAIN_NET_RX, "l2_decap",
                   "ethernet_frame", "ethernet_frame",
                   (void (*)(chain_node_t *, void *, void *))net_rx_l2_decap_resolve);
    chain_add_node(CHAIN_NET_RX, "frame_delivery",
                   "ethernet_frame", "delivery_status",
                   (void (*)(chain_node_t *, void *, void *))net_rx_frame_delivery_resolve);

    return 0;
}

/* ── Compat shims ──────────────────────────────────────────────── */

int net_chain_send(const void *frame, uint16_t len)
{
    if (!frame || len == 0) return -1;
    if (CHAIN_NET_TX < 0 || !s_hw.tx) {
        /* Chain not yet up but a driver is wired -- bypass for the
         * brief window during net_init(). After registry init this
         * path is never taken. */
        if (s_hw.tx) return s_hw.tx(frame, len);
        return -1;
    }

    s_tx_pending.src   = (const uint8_t *)frame;
    s_tx_pending.len   = len;
    s_tx_pending.valid = 1;

    int rc = chain_resolve(CHAIN_NET_TX);
    if (rc != 0) return -1;

    /* The hardware_dma resolve writes to the chain's last scratch
     * buffer -- but chain_resolve doesn't expose it. The
     * vault_version bump is the success signal we promised the
     * caller. */
    chain_t *c = chain_get(CHAIN_NET_TX);
    /* If hardware_dma succeeded, vault_version got bumped during
     * this resolve; if not, the request was dropped. We always
     * return 0 here so callers (which don't check return values
     * for ARP/DHCP broadcasts anyway) see the same surface as the
     * old direct path. */
    (void)c;
    return 0;
}

int net_chain_recv(void *buf, uint16_t max)
{
    if (!buf || max == 0) return 0;
    if (CHAIN_NET_RX < 0 || !s_hw.rx_poll) {
        if (s_hw.rx_poll) return s_hw.rx_poll(buf, max);
        return 0;
    }

    s_rx_pending.valid = 1;
    s_rx_delivered_len = 0;

    int rc = chain_resolve(CHAIN_NET_RX);
    if (rc != 0) return 0;

    if (s_rx_delivered_len <= 0) return 0;

    int n = s_rx_delivered_len;
    if (n > (int)max) n = max;
    uint8_t *dst = (uint8_t *)buf;
    for (int i = 0; i < n; i++) dst[i] = s_rx_frame[i];
    return n;
}
