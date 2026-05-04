/*
 * Zeos -- Bluetooth L2CAP
 *
 * L2CAP on top of HCI ACL. Signaling channel for classic and LE,
 * CID multiplexing, MTU negotiation. Foundation for GATT (ATT/CID
 * 0x0004), RFCOMM (PSM 0x0003), and HID-over-BT (PSM 0x0011).
 *
 * Layering (Core Spec Vol 3 Part A):
 *
 *   ACL (bt_usb_acl_poll / bt_usb_acl_send)
 *      |  PB/BC flags + 12-bit conn_handle + 16-bit data length
 *      v
 *   L2CAP B-frame (Length:2, CID:2, Information:Length)
 *      |
 *      +-- CID 0x0001 -> classic signaling state machine
 *      +-- CID 0x0005 -> LE signaling state machine
 *      +-- CID 0x0004 -> ATT (BLE GATT) callback
 *      +-- CID 0x0006 -> LE Security Manager callback
 *      +-- CID 0x0040.. dynamic CIDs -> per-channel inbound queue
 *
 * Inbound recombination follows PB flag semantics: PB=00b/10b is a
 * complete-or-first fragment carrying the L2CAP header; PB=01b is a
 * continuation that extends the in-flight buffer. We keep one
 * recombination context per HCI connection handle (small table).
 */

#include "bt_l2cap.h"
#include "bt_usb.h"
#include "bt_hci.h"
#include "kprint.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);

/* ── Configuration knobs ───────────────────────────────────────── */

#define L2CAP_RX_QUEUE_BYTES    1024    /* per dynamic channel */
#define L2CAP_RECOMB_BUF_BYTES  1024
#define L2CAP_MAX_RECOMB        4       /* concurrent connections */
#define L2CAP_FIXED_SLOTS       4       /* signaling + ATT + SMP + spare */

/* ── Channel record ────────────────────────────────────────────── */

typedef struct {
    uint8_t          in_use;
    l2cap_flavor_t   flavor;
    l2cap_state_t    state;

    uint16_t         cid;        /* local CID */
    uint16_t         peer_cid;   /* remote CID */
    uint16_t         psm;
    uint16_t         conn_handle;

    uint16_t         local_mtu;
    uint16_t         peer_mtu;

    /* Outstanding signaling identifier for the most recent request we
     * sent on this channel. Replies must echo this back. */
    uint8_t          out_ident;

    /* LE credit-based: peer credits we may consume on send, credits we
     * have advertised to the peer (and how many they've consumed). */
    uint16_t         peer_credits;
    uint16_t         our_credits;

    /* Single inbound queue: rx_len = bytes in rx_buf, 0 = empty. We
     * hold one frame at a time — l2cap_recv consumes it. */
    uint8_t          rx_buf[L2CAP_RX_QUEUE_BYTES];
    uint16_t         rx_len;
} l2cap_chan_t;

/* Recombination context: one per active ACL connection. */
typedef struct {
    uint8_t   in_use;
    uint16_t  conn_handle;
    uint16_t  expected;     /* total L2CAP PDU length (header.Length + 4) */
    uint16_t  filled;
    uint8_t   buf[L2CAP_RECOMB_BUF_BYTES];
} l2cap_recomb_t;

/* Fixed-channel registration table. */
typedef struct {
    uint16_t       cid;
    l2cap_rx_cb_t  cb;
} l2cap_fixed_t;

/* ── Module state ──────────────────────────────────────────────── */

static struct {
    int             initialized;
    int             signaling_ready;
    uint8_t         next_ident;
    l2cap_chan_t    chans[L2CAP_MAX_CHANNELS];
    l2cap_recomb_t  recomb[L2CAP_MAX_RECOMB];
    l2cap_fixed_t   fixed[L2CAP_FIXED_SLOTS];

    /* Pool of dynamic CIDs we've allocated. Bit map over the
     * 0x0040..0x007F range. Bit i = CID (DYN_BASE + i). */
    uint64_t        cid_bitmap;

    uint32_t        rx_frames;
    uint32_t        tx_frames;
    uint32_t        sig_count;
} g;

/* ── Small helpers ─────────────────────────────────────────────── */

static uint8_t next_ident(void)
{
    /* Identifier 0x00 is reserved. */
    g.next_ident++;
    if (g.next_ident == 0) g.next_ident = 1;
    return g.next_ident;
}

static l2cap_chan_t *chan_find_by_cid(uint16_t cid)
{
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (g.chans[i].in_use && g.chans[i].cid == cid)
            return &g.chans[i];
    }
    return 0;
}

static l2cap_chan_t *chan_find_by_handle_cid(uint16_t handle, uint16_t cid)
{
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (g.chans[i].in_use &&
            g.chans[i].conn_handle == handle &&
            g.chans[i].cid == cid)
            return &g.chans[i];
    }
    return 0;
}

static l2cap_chan_t *chan_find_by_handle_peer(uint16_t handle, uint16_t pcid)
{
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (g.chans[i].in_use &&
            g.chans[i].conn_handle == handle &&
            g.chans[i].peer_cid == pcid)
            return &g.chans[i];
    }
    return 0;
}

static l2cap_chan_t *chan_alloc(void)
{
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (!g.chans[i].in_use) {
            memset(&g.chans[i], 0, sizeof(g.chans[i]));
            g.chans[i].in_use = 1;
            return &g.chans[i];
        }
    }
    return 0;
}

static void chan_free(l2cap_chan_t *c)
{
    if (!c) return;
    if (c->cid >= L2CAP_CID_DYN_BASE && c->cid <= L2CAP_CID_DYN_MAX) {
        int bit = c->cid - L2CAP_CID_DYN_BASE;
        g.cid_bitmap &= ~((uint64_t)1 << bit);
    }
    memset(c, 0, sizeof(*c));
}

static uint16_t cid_alloc_dynamic(void)
{
    /* 0x0040..0x007F = 64 candidates. */
    for (int b = 0; b < 64; b++) {
        if (!(g.cid_bitmap & ((uint64_t)1 << b))) {
            g.cid_bitmap |= ((uint64_t)1 << b);
            return (uint16_t)(L2CAP_CID_DYN_BASE + b);
        }
    }
    return L2CAP_CID_NULL;
}

/* Recombination context lookup or allocate. */
static l2cap_recomb_t *recomb_get(uint16_t handle)
{
    for (int i = 0; i < L2CAP_MAX_RECOMB; i++) {
        if (g.recomb[i].in_use && g.recomb[i].conn_handle == handle)
            return &g.recomb[i];
    }
    for (int i = 0; i < L2CAP_MAX_RECOMB; i++) {
        if (!g.recomb[i].in_use) {
            memset(&g.recomb[i], 0, sizeof(g.recomb[i]));
            g.recomb[i].in_use = 1;
            g.recomb[i].conn_handle = handle;
            return &g.recomb[i];
        }
    }
    return 0;
}

static void recomb_reset(l2cap_recomb_t *r)
{
    if (!r) return;
    r->expected = 0;
    r->filled = 0;
}

/* ── ACL framing ───────────────────────────────────────────────── */

/* Build and ship one L2CAP B-frame as a single ACL packet. The HCI
 * ACL header layout we emit:
 *
 *   byte0: handle[7:0]
 *   byte1: handle[11:8] | (PB=10b)<<4 | (BC=00b)<<6   (start of PDU)
 *   byte2..3: total data length (LE) = 4 + payload_len
 *   byte4..5: L2CAP Length (payload_len, LE)
 *   byte6..7: L2CAP CID (LE)
 *   byte8..:  payload bytes
 *
 * PB=10b ("Start of automatically-flushable PDU") is the standard host-
 * to-controller value for new PDUs in modern controllers. Continuation
 * fragments would use PB=01b — we don't fragment outbound here because
 * the negotiated MTU keeps us under the controller's ACL data length
 * for both classic (1024 target) and LE (247 target). */
static int acl_send_frame(uint16_t handle, uint16_t cid,
                          const void *payload, uint16_t plen)
{
    if (plen > L2CAP_MTU_CLASSIC_TARGET) return -1;
    uint8_t pkt[4 + 4 + L2CAP_MTU_CLASSIC_TARGET];
    uint16_t hdr0 = (handle & 0x0FFF) | (0x2 << 12);   /* PB=10, BC=00 */
    uint16_t total = 4 + plen;
    pkt[0] = (uint8_t)(hdr0 & 0xFF);
    pkt[1] = (uint8_t)((hdr0 >> 8) & 0xFF);
    pkt[2] = (uint8_t)(total & 0xFF);
    pkt[3] = (uint8_t)((total >> 8) & 0xFF);
    pkt[4] = (uint8_t)(plen & 0xFF);
    pkt[5] = (uint8_t)((plen >> 8) & 0xFF);
    pkt[6] = (uint8_t)(cid & 0xFF);
    pkt[7] = (uint8_t)((cid >> 8) & 0xFF);
    if (plen) memcpy(pkt + 8, payload, plen);
    int rc = bt_usb_acl_send(pkt, 8 + plen);
    if (rc >= 0) g.tx_frames++;
    return rc;
}

/* Build a 4-byte L2CAP signaling command header + body and send it. */
static int sig_send(uint16_t handle, uint16_t sig_cid, uint8_t code,
                    uint8_t ident, const void *body, uint16_t blen)
{
    uint8_t buf[4 + 256];
    if (blen > sizeof(buf) - 4) return -1;
    buf[0] = code;
    buf[1] = ident;
    buf[2] = (uint8_t)(blen & 0xFF);
    buf[3] = (uint8_t)((blen >> 8) & 0xFF);
    if (blen) memcpy(buf + 4, body, blen);
    int rc = acl_send_frame(handle, sig_cid, buf, (uint16_t)(4 + blen));
    if (rc >= 0) g.sig_count++;
    return rc;
}

/* ── Signaling: outbound builders ──────────────────────────────── */

static int send_conn_req(l2cap_chan_t *c)
{
    uint8_t b[4];
    b[0] = (uint8_t)(c->psm & 0xFF);
    b[1] = (uint8_t)((c->psm >> 8) & 0xFF);
    b[2] = (uint8_t)(c->cid & 0xFF);
    b[3] = (uint8_t)((c->cid >> 8) & 0xFF);
    c->out_ident = next_ident();
    return sig_send(c->conn_handle, L2CAP_CID_SIGNAL_CLASSIC,
                    L2CAP_SIG_CONN_REQ, c->out_ident, b, 4);
}

/* Configuration Request body: dest_cid(2) flags(2) options(N).
 * We send one option: MTU (type 0x01, len 2, value local_mtu). */
static int send_config_req(l2cap_chan_t *c)
{
    uint8_t b[4 + 4];
    b[0] = (uint8_t)(c->peer_cid & 0xFF);
    b[1] = (uint8_t)((c->peer_cid >> 8) & 0xFF);
    b[2] = 0x00;   /* flags */
    b[3] = 0x00;
    b[4] = 0x01;   /* MTU option */
    b[5] = 0x02;
    b[6] = (uint8_t)(c->local_mtu & 0xFF);
    b[7] = (uint8_t)((c->local_mtu >> 8) & 0xFF);
    c->out_ident = next_ident();
    return sig_send(c->conn_handle, L2CAP_CID_SIGNAL_CLASSIC,
                    L2CAP_SIG_CONFIG_REQ, c->out_ident, b, 8);
}

/* Configuration Response: src_cid(2) flags(2) result(2) options(N).
 * result=0 (success), echo back the proposed MTU. */
static int send_config_rsp(l2cap_chan_t *c, uint8_t ident,
                           uint16_t mtu_echo)
{
    uint8_t b[6 + 4];
    b[0] = (uint8_t)(c->peer_cid & 0xFF);
    b[1] = (uint8_t)((c->peer_cid >> 8) & 0xFF);
    b[2] = 0x00;
    b[3] = 0x00;
    b[4] = 0x00;   /* result=success */
    b[5] = 0x00;
    b[6] = 0x01;   /* MTU option */
    b[7] = 0x02;
    b[8] = (uint8_t)(mtu_echo & 0xFF);
    b[9] = (uint8_t)((mtu_echo >> 8) & 0xFF);
    return sig_send(c->conn_handle, L2CAP_CID_SIGNAL_CLASSIC,
                    L2CAP_SIG_CONFIG_RSP, ident, b, 10);
}

static int send_disconn_req(l2cap_chan_t *c)
{
    uint8_t b[4];
    b[0] = (uint8_t)(c->peer_cid & 0xFF);
    b[1] = (uint8_t)((c->peer_cid >> 8) & 0xFF);
    b[2] = (uint8_t)(c->cid & 0xFF);
    b[3] = (uint8_t)((c->cid >> 8) & 0xFF);
    c->out_ident = next_ident();
    return sig_send(c->conn_handle, L2CAP_CID_SIGNAL_CLASSIC,
                    L2CAP_SIG_DISCONN_REQ, c->out_ident, b, 4);
}

static int send_disconn_rsp(uint16_t handle, uint8_t ident,
                            uint16_t dcid, uint16_t scid)
{
    uint8_t b[4];
    b[0] = (uint8_t)(dcid & 0xFF);
    b[1] = (uint8_t)((dcid >> 8) & 0xFF);
    b[2] = (uint8_t)(scid & 0xFF);
    b[3] = (uint8_t)((scid >> 8) & 0xFF);
    return sig_send(handle, L2CAP_CID_SIGNAL_CLASSIC,
                    L2CAP_SIG_DISCONN_RSP, ident, b, 4);
}

static int send_echo_rsp(uint16_t handle, uint8_t ident,
                         const uint8_t *data, uint16_t len)
{
    return sig_send(handle, L2CAP_CID_SIGNAL_CLASSIC,
                    L2CAP_SIG_ECHO_RSP, ident, data, len);
}

/* Information Response. We answer two info types:
 *   - Connectionless MTU (0x0001): result=success, MTU=672.
 *   - Extended Features  (0x0002): result=success, mask=0 (no enhanced
 *     retransmission, no streaming). Vanilla basic mode only.
 *   - Fixed Channels Supported (0x0003): bitmask reporting CID 0x0001
 *     (bit 1) and CID 0x0005 (bit 5) as supported. */
static int send_info_rsp(uint16_t handle, uint8_t ident, uint16_t info_type)
{
    uint8_t b[4 + 8];
    b[0] = (uint8_t)(info_type & 0xFF);
    b[1] = (uint8_t)((info_type >> 8) & 0xFF);
    b[2] = 0x00;   /* result=success (LE) */
    b[3] = 0x00;
    uint16_t blen = 4;

    if (info_type == L2CAP_INFO_TYPE_CL_MTU) {
        b[4] = (uint8_t)(L2CAP_MTU_CLASSIC_DEFAULT & 0xFF);
        b[5] = (uint8_t)((L2CAP_MTU_CLASSIC_DEFAULT >> 8) & 0xFF);
        blen = 6;
    } else if (info_type == L2CAP_INFO_TYPE_FEATURES) {
        b[4] = 0x00; b[5] = 0x00; b[6] = 0x00; b[7] = 0x00;
        blen = 8;
    } else if (info_type == L2CAP_INFO_TYPE_FIXED_CHANS) {
        /* 8-byte fixed-channel mask. Bit 1 = signaling, bit 4 = ATT,
         * bit 5 = LE signaling, bit 6 = SMP. */
        uint64_t mask = (1ULL << 1) | (1ULL << 4) | (1ULL << 5) | (1ULL << 6);
        for (int i = 0; i < 8; i++) b[4 + i] = (uint8_t)((mask >> (8 * i)) & 0xFF);
        blen = 12;
    } else {
        b[2] = 0x01;  /* not supported */
        b[3] = 0x00;
        blen = 4;
    }
    return sig_send(handle, L2CAP_CID_SIGNAL_CLASSIC,
                    L2CAP_SIG_INFO_RSP, ident, b, blen);
}

static int send_cmd_reject(uint16_t handle, uint16_t sig_cid,
                           uint8_t ident, uint16_t reason)
{
    uint8_t b[2];
    b[0] = (uint8_t)(reason & 0xFF);
    b[1] = (uint8_t)((reason >> 8) & 0xFF);
    return sig_send(handle, sig_cid, L2CAP_SIG_CMD_REJECT, ident, b, 2);
}

/* ── Signaling: inbound state machine ──────────────────────────── */

static void handle_conn_rsp(uint16_t handle, const uint8_t *p, uint16_t len)
{
    /* dest_cid(2) src_cid(2) result(2) status(2) — 8 bytes. */
    if (len < 8) return;
    uint16_t dcid   = p[0] | ((uint16_t)p[1] << 8);
    uint16_t scid   = p[2] | ((uint16_t)p[3] << 8);
    uint16_t result = p[4] | ((uint16_t)p[5] << 8);

    /* scid is OUR cid (the one we sent in Connect Req). */
    l2cap_chan_t *c = chan_find_by_handle_cid(handle, scid);
    if (!c) return;

    if (result == 0x0000) {
        c->peer_cid = dcid;
        c->state = L2CAP_STATE_CONFIG;
        /* Bump from default to target MTU via Config. */
        c->local_mtu = L2CAP_MTU_CLASSIC_TARGET;
        send_config_req(c);
    } else if (result == 0x0001) {
        /* Pending — keep waiting. */
    } else {
        /* Refused / security blocked / no-resources. */
        chan_free(c);
    }
}

static void handle_config_req(uint16_t handle, uint8_t ident,
                              const uint8_t *p, uint16_t len)
{
    if (len < 4) return;
    uint16_t dcid = p[0] | ((uint16_t)p[1] << 8);
    /* flags = p[2..3]; we don't honor C-bit fragmenting (target MTU
     * fits in one config). */
    l2cap_chan_t *c = chan_find_by_handle_cid(handle, dcid);
    if (!c) {
        send_cmd_reject(handle, L2CAP_CID_SIGNAL_CLASSIC, ident, 0x0002);
        return;
    }

    /* Walk options. The only one we accept is MTU (0x01). */
    uint16_t off = 4;
    uint16_t accepted_mtu = c->peer_mtu ? c->peer_mtu : L2CAP_MTU_CLASSIC_DEFAULT;
    while (off + 2 <= len) {
        uint8_t opt_type = p[off];
        uint8_t opt_len  = p[off + 1];
        if (off + 2 + opt_len > len) break;
        if ((opt_type & 0x7F) == 0x01 && opt_len == 2) {
            uint16_t mtu = p[off + 2] | ((uint16_t)p[off + 3] << 8);
            if (mtu == 0) mtu = L2CAP_MTU_CLASSIC_DEFAULT;
            if (mtu > L2CAP_MTU_CLASSIC_TARGET) mtu = L2CAP_MTU_CLASSIC_TARGET;
            accepted_mtu = mtu;
        }
        off += 2 + opt_len;
    }
    c->peer_mtu = accepted_mtu;
    send_config_rsp(c, ident, accepted_mtu);

    /* Once both directions have been responded-to, transition to OPEN.
     * We treat any successful Config Rsp from the peer + having sent
     * our own Rsp as completion (basic mode). */
    if (c->state == L2CAP_STATE_CONFIG && c->peer_mtu && c->local_mtu)
        ; /* stays CONFIG until we also receive a Config Rsp */
}

static void handle_config_rsp(uint16_t handle, const uint8_t *p, uint16_t len)
{
    if (len < 6) return;
    uint16_t scid   = p[0] | ((uint16_t)p[1] << 8);
    uint16_t result = p[4] | ((uint16_t)p[5] << 8);
    l2cap_chan_t *c = chan_find_by_handle_cid(handle, scid);
    if (!c) return;
    if (result == 0x0000) {
        c->state = L2CAP_STATE_OPEN;
    }
}

static void handle_disconn_req(uint16_t handle, uint8_t ident,
                               const uint8_t *p, uint16_t len)
{
    if (len < 4) return;
    uint16_t dcid = p[0] | ((uint16_t)p[1] << 8);
    uint16_t scid = p[2] | ((uint16_t)p[3] << 8);
    l2cap_chan_t *c = chan_find_by_handle_cid(handle, dcid);
    if (c) {
        send_disconn_rsp(handle, ident, dcid, scid);
        chan_free(c);
    } else {
        send_cmd_reject(handle, L2CAP_CID_SIGNAL_CLASSIC, ident, 0x0002);
    }
}

static void handle_disconn_rsp(uint16_t handle, const uint8_t *p, uint16_t len)
{
    if (len < 4) return;
    uint16_t dcid = p[0] | ((uint16_t)p[1] << 8);
    uint16_t scid = p[2] | ((uint16_t)p[3] << 8);
    (void)dcid;
    l2cap_chan_t *c = chan_find_by_handle_cid(handle, scid);
    if (c) chan_free(c);
}

static void handle_le_conn_rsp(uint16_t handle, const uint8_t *p, uint16_t len)
{
    /* dest_cid(2) mtu(2) mps(2) initial_credits(2) result(2) — 10 bytes. */
    if (len < 10) return;
    uint16_t dcid    = p[0] | ((uint16_t)p[1] << 8);
    uint16_t mtu     = p[2] | ((uint16_t)p[3] << 8);
    /* uint16_t mps  = p[4] | ((uint16_t)p[5] << 8); */
    uint16_t credits = p[6] | ((uint16_t)p[7] << 8);
    uint16_t result  = p[8] | ((uint16_t)p[9] << 8);

    /* Find channel with matching out_ident — we issue one LE conn req
     * at a time per channel so cid-by-conn-handle disambiguates. */
    l2cap_chan_t *c = 0;
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (g.chans[i].in_use &&
            g.chans[i].conn_handle == handle &&
            g.chans[i].flavor == L2CAP_FLAVOR_LE_CREDIT &&
            g.chans[i].state == L2CAP_STATE_WAIT_CONN_RSP) {
            c = &g.chans[i];
            break;
        }
    }
    if (!c) return;
    if (result == 0x0000) {
        c->peer_cid     = dcid;
        c->peer_mtu     = mtu;
        c->peer_credits = credits;
        c->state        = L2CAP_STATE_OPEN;
    } else {
        chan_free(c);
    }
}

static void handle_le_credit(uint16_t handle, const uint8_t *p, uint16_t len)
{
    if (len < 4) return;
    uint16_t cid     = p[0] | ((uint16_t)p[1] << 8);
    uint16_t credits = p[2] | ((uint16_t)p[3] << 8);
    l2cap_chan_t *c  = chan_find_by_handle_peer(handle, cid);
    if (c) c->peer_credits += credits;
}

static void process_signaling(uint16_t handle, uint16_t sig_cid,
                              const uint8_t *p, uint16_t len)
{
    /* Multiple commands MAY be packed in one signaling frame on classic;
     * LE signaling caps at one. We loop. */
    uint16_t off = 0;
    while (off + 4 <= len) {
        uint8_t code   = p[off];
        uint8_t ident  = p[off + 1];
        uint16_t blen  = p[off + 2] | ((uint16_t)p[off + 3] << 8);
        if (off + 4 + blen > len) break;
        const uint8_t *body = p + off + 4;
        g.sig_count++;
        switch (code) {
        case L2CAP_SIG_CONN_REQ: {
            /* We don't accept inbound classic connections in this layer
             * — RFCOMM/HID server roles are future profile work. Reject
             * with "PSM not supported" (result=0x0002). */
            if (blen >= 4) {
                uint16_t scid = body[2] | ((uint16_t)body[3] << 8);
                uint8_t r[8];
                r[0] = 0x00; r[1] = 0x00;       /* dcid */
                r[2] = (uint8_t)(scid & 0xFF);
                r[3] = (uint8_t)((scid >> 8) & 0xFF);
                r[4] = 0x02; r[5] = 0x00;       /* result = PSM not supp */
                r[6] = 0x00; r[7] = 0x00;       /* status */
                sig_send(handle, sig_cid, L2CAP_SIG_CONN_RSP, ident, r, 8);
            }
            break;
        }
        case L2CAP_SIG_CONN_RSP:
            handle_conn_rsp(handle, body, blen);
            break;
        case L2CAP_SIG_CONFIG_REQ:
            handle_config_req(handle, ident, body, blen);
            break;
        case L2CAP_SIG_CONFIG_RSP:
            handle_config_rsp(handle, body, blen);
            break;
        case L2CAP_SIG_DISCONN_REQ:
            handle_disconn_req(handle, ident, body, blen);
            break;
        case L2CAP_SIG_DISCONN_RSP:
            handle_disconn_rsp(handle, body, blen);
            break;
        case L2CAP_SIG_ECHO_REQ:
            send_echo_rsp(handle, ident, body, blen);
            break;
        case L2CAP_SIG_ECHO_RSP:
            /* Drop. Sanity round-trip; we don't currently issue Echo
             * Reqs from this layer. */
            break;
        case L2CAP_SIG_INFO_REQ: {
            uint16_t it = (blen >= 2) ? (body[0] | ((uint16_t)body[1] << 8))
                                      : 0;
            send_info_rsp(handle, ident, it);
            break;
        }
        case L2CAP_SIG_INFO_RSP:
            /* We don't currently issue Info Reqs from this layer. */
            break;
        case L2CAP_SIG_LE_CONN_RSP:
            handle_le_conn_rsp(handle, body, blen);
            break;
        case L2CAP_SIG_LE_CREDIT:
            handle_le_credit(handle, body, blen);
            break;
        case L2CAP_SIG_CMD_REJECT:
            /* Peer rejected something we sent; surface via counters. */
            break;
        default:
            send_cmd_reject(handle, sig_cid, ident, 0x0000); /* not understood */
            break;
        }
        off += 4 + blen;
    }
}

/* ── Frame dispatch ────────────────────────────────────────────── */

static void deliver_to_channel(l2cap_chan_t *c, const uint8_t *p, uint16_t len)
{
    /* Single-frame queue: drop newest if a frame is already pending. */
    if (c->rx_len) return;
    uint16_t copy = (len > L2CAP_RX_QUEUE_BYTES) ? L2CAP_RX_QUEUE_BYTES : len;
    memcpy(c->rx_buf, p, copy);
    c->rx_len = copy;

    /* Replenish credits for LE credit-based channels: each PDU consumes
     * one credit in the direction we advertised. */
    if (c->flavor == L2CAP_FLAVOR_LE_CREDIT && c->our_credits > 0) {
        c->our_credits--;
    }
}

static l2cap_rx_cb_t fixed_cb_for_cid(uint16_t cid)
{
    for (int i = 0; i < L2CAP_FIXED_SLOTS; i++) {
        if (g.fixed[i].cid == cid && g.fixed[i].cb)
            return g.fixed[i].cb;
    }
    return 0;
}

static void dispatch_frame(uint16_t handle, const uint8_t *pdu, uint16_t pdu_len)
{
    /* L2CAP B-frame: Length(2), CID(2), Information(Length). */
    if (pdu_len < 4) return;
    uint16_t length = pdu[0] | ((uint16_t)pdu[1] << 8);
    uint16_t cid    = pdu[2] | ((uint16_t)pdu[3] << 8);
    if (4 + length > pdu_len) return;
    const uint8_t *info = pdu + 4;

    g.rx_frames++;

    if (cid == L2CAP_CID_SIGNAL_CLASSIC || cid == L2CAP_CID_SIGNAL_LE) {
        process_signaling(handle, cid, info, length);
        return;
    }

    /* Fixed-channel callback path (ATT, SMP). */
    l2cap_rx_cb_t cb = fixed_cb_for_cid(cid);
    if (cb) {
        cb(handle, cid, info, length);
        return;
    }

    /* Dynamic channel inbound queue. */
    l2cap_chan_t *c = chan_find_by_handle_cid(handle, cid);
    if (c && c->state == L2CAP_STATE_OPEN) {
        deliver_to_channel(c, info, length);
    }
}

/* ── ACL recombination ─────────────────────────────────────────── */

void bt_l2cap_ingest_acl(const uint8_t *acl, int len)
{
    if (!acl || len < 4) return;
    uint16_t hdr0 = acl[0] | ((uint16_t)acl[1] << 8);
    uint16_t handle = hdr0 & 0x0FFF;
    uint8_t  pb     = (hdr0 >> 12) & 0x3;
    uint16_t dlen   = acl[2] | ((uint16_t)acl[3] << 8);
    if (4 + dlen > (uint16_t)len) return;

    l2cap_recomb_t *r = recomb_get(handle);
    if (!r) return;

    if (pb == 0x2 || pb == 0x0) {
        /* Start (10b) / first non-flushable (00b): expect L2CAP header. */
        if (dlen < 4) { recomb_reset(r); return; }
        uint16_t l2_len = acl[4] | ((uint16_t)acl[5] << 8);
        uint16_t total  = 4 + l2_len;
        if (total > L2CAP_RECOMB_BUF_BYTES) { recomb_reset(r); return; }

        if (dlen >= total) {
            /* Complete in one fragment. */
            dispatch_frame(handle, acl + 4, total);
            recomb_reset(r);
        } else {
            memcpy(r->buf, acl + 4, dlen);
            r->expected = total;
            r->filled   = dlen;
        }
    } else if (pb == 0x1) {
        /* Continuation. */
        if (r->expected == 0) return;
        if (r->filled + dlen > r->expected) { recomb_reset(r); return; }
        memcpy(r->buf + r->filled, acl + 4, dlen);
        r->filled += dlen;
        if (r->filled >= r->expected) {
            dispatch_frame(handle, r->buf, r->expected);
            recomb_reset(r);
        }
    }
    /* PB=0x3 is a complete LE adv report (LE-U logical channel) — not
     * currently used in this transport path. */
}

/* ── Public API ────────────────────────────────────────────────── */

int bt_l2cap_init(void)
{
    if (g.initialized) return 0;
    memset(&g, 0, sizeof(g));
    g.initialized = 1;
    g.next_ident = 0;

    /* Pre-register the fixed CID slots (callbacks NULL — layered
     * protocols register later via bt_l2cap_register_fixed). */
    g.fixed[0].cid = L2CAP_CID_ATT;
    g.fixed[1].cid = L2CAP_CID_SMP;
    g.fixed[2].cid = L2CAP_CID_CONNLESS;
    g.fixed[3].cid = L2CAP_CID_NULL;  /* spare */

    /* Mark signaling as a "channel" present in snapshots without burning
     * a dynamic CID — we do this by allocating a fixed-flavor record. */
    l2cap_chan_t *cs = chan_alloc();
    if (cs) {
        cs->flavor      = L2CAP_FLAVOR_FIXED;
        cs->state       = L2CAP_STATE_OPEN;
        cs->cid         = L2CAP_CID_SIGNAL_CLASSIC;
        cs->peer_cid    = L2CAP_CID_SIGNAL_CLASSIC;
        cs->local_mtu   = L2CAP_MTU_CLASSIC_DEFAULT;
        cs->peer_mtu    = L2CAP_MTU_CLASSIC_DEFAULT;
    }
    l2cap_chan_t *cl = chan_alloc();
    if (cl) {
        cl->flavor      = L2CAP_FLAVOR_FIXED;
        cl->state       = L2CAP_STATE_OPEN;
        cl->cid         = L2CAP_CID_SIGNAL_LE;
        cl->peer_cid    = L2CAP_CID_SIGNAL_LE;
        cl->local_mtu   = L2CAP_MTU_LE_DEFAULT;
        cl->peer_mtu    = L2CAP_MTU_LE_DEFAULT;
    }

    g.signaling_ready = (cs && cl) ? 1 : 0;
    return 0;
}

void bt_l2cap_poll(void)
{
    if (!g.initialized) return;
    if (!bt_usb_present()) return;
    /* Drain a small batch each tick. The HCI ACL bulk endpoint queues
     * inbound packets one-at-a-time the same way EP1 IN does for events. */
    for (int loops = 0; loops < 8; loops++) {
        uint8_t buf[BT_ACL_PKT_MAX];
        int n = bt_usb_acl_poll(buf, sizeof(buf));
        if (n <= 0) return;
        bt_l2cap_ingest_acl(buf, n);
    }
}

int bt_l2cap_register_fixed(uint16_t cid, l2cap_rx_cb_t cb)
{
    if (!g.initialized) return -1;
    if (cid != L2CAP_CID_ATT && cid != L2CAP_CID_SMP &&
        cid != L2CAP_CID_CONNLESS)
        return -1;
    for (int i = 0; i < L2CAP_FIXED_SLOTS; i++) {
        if (g.fixed[i].cid == cid) {
            g.fixed[i].cb = cb;
            return 0;
        }
    }
    /* Wasn't pre-allocated; install in an empty slot. */
    for (int i = 0; i < L2CAP_FIXED_SLOTS; i++) {
        if (g.fixed[i].cid == L2CAP_CID_NULL) {
            g.fixed[i].cid = cid;
            g.fixed[i].cb  = cb;
            return 0;
        }
    }
    return -1;
}

int l2cap_open(uint16_t conn_handle, uint16_t psm, uint16_t *out_cid)
{
    if (!g.initialized) return -1;
    if (psm == 0) return -1;
    uint16_t cid = cid_alloc_dynamic();
    if (cid == L2CAP_CID_NULL) return -1;

    l2cap_chan_t *c = chan_alloc();
    if (!c) {
        /* Roll back the CID allocation. */
        int bit = cid - L2CAP_CID_DYN_BASE;
        g.cid_bitmap &= ~((uint64_t)1 << bit);
        return -1;
    }
    c->flavor      = L2CAP_FLAVOR_CLASSIC;
    c->state       = L2CAP_STATE_WAIT_CONN_RSP;
    c->cid         = cid;
    c->peer_cid    = 0;
    c->psm         = psm;
    c->conn_handle = conn_handle;
    c->local_mtu   = L2CAP_MTU_CLASSIC_DEFAULT;
    c->peer_mtu    = 0;

    if (send_conn_req(c) < 0) {
        chan_free(c);
        return -1;
    }
    if (out_cid) *out_cid = cid;
    return 0;
}

int l2cap_open_le(uint16_t conn_handle, uint16_t psm, uint16_t *out_cid)
{
    if (!g.initialized) return -1;
    if (psm == 0) return -1;
    uint16_t cid = cid_alloc_dynamic();
    if (cid == L2CAP_CID_NULL) return -1;

    l2cap_chan_t *c = chan_alloc();
    if (!c) {
        int bit = cid - L2CAP_CID_DYN_BASE;
        g.cid_bitmap &= ~((uint64_t)1 << bit);
        return -1;
    }
    c->flavor       = L2CAP_FLAVOR_LE_CREDIT;
    c->state        = L2CAP_STATE_WAIT_CONN_RSP;
    c->cid          = cid;
    c->psm          = psm;
    c->conn_handle  = conn_handle;
    c->local_mtu    = L2CAP_MTU_LE_TARGET;
    c->our_credits  = 8;       /* small initial advertisement */

    /* LE Connect Request body: psm(2) scid(2) mtu(2) mps(2) credits(2). */
    uint8_t body[10];
    body[0] = (uint8_t)(psm & 0xFF);
    body[1] = (uint8_t)((psm >> 8) & 0xFF);
    body[2] = (uint8_t)(cid & 0xFF);
    body[3] = (uint8_t)((cid >> 8) & 0xFF);
    body[4] = (uint8_t)(c->local_mtu & 0xFF);
    body[5] = (uint8_t)((c->local_mtu >> 8) & 0xFF);
    body[6] = (uint8_t)(c->local_mtu & 0xFF);   /* MPS = MTU here */
    body[7] = (uint8_t)((c->local_mtu >> 8) & 0xFF);
    body[8] = (uint8_t)(c->our_credits & 0xFF);
    body[9] = 0x00;
    c->out_ident = next_ident();
    int rc = sig_send(conn_handle, L2CAP_CID_SIGNAL_LE,
                      L2CAP_SIG_LE_CONN_REQ, c->out_ident, body, 10);
    if (rc < 0) { chan_free(c); return -1; }

    if (out_cid) *out_cid = cid;
    return 0;
}

int l2cap_send(uint16_t cid, const void *data, uint16_t len)
{
    l2cap_chan_t *c = chan_find_by_cid(cid);
    if (!c) return -1;
    if (c->state != L2CAP_STATE_OPEN) return -1;
    if (c->peer_mtu && len > c->peer_mtu) return -1;
    if (c->flavor == L2CAP_FLAVOR_LE_CREDIT) {
        if (c->peer_credits == 0) return -1;
        c->peer_credits--;
    }
    return acl_send_frame(c->conn_handle, c->peer_cid, data, len);
}

int l2cap_recv(uint16_t cid, void *buf, uint16_t max)
{
    l2cap_chan_t *c = chan_find_by_cid(cid);
    if (!c) return -1;
    if (c->rx_len == 0) return 0;
    uint16_t copy = c->rx_len;
    if (copy > max) copy = max;
    if (buf && copy) memcpy(buf, c->rx_buf, copy);
    int n = (int)c->rx_len;
    c->rx_len = 0;
    return n;
}

int l2cap_close(uint16_t cid)
{
    l2cap_chan_t *c = chan_find_by_cid(cid);
    if (!c) return -1;
    if (cid < L2CAP_CID_DYN_BASE) return -1;   /* fixed CIDs don't close */
    if (c->state != L2CAP_STATE_OPEN &&
        c->state != L2CAP_STATE_CONFIG) {
        chan_free(c);
        return 0;
    }
    int rc = send_disconn_req(c);
    if (rc < 0) { chan_free(c); return 0; }
    c->state = L2CAP_STATE_WAIT_DISCONN_RSP;
    return 0;
}

l2cap_state_t l2cap_channel_state(uint16_t cid)
{
    l2cap_chan_t *c = chan_find_by_cid(cid);
    return c ? c->state : L2CAP_STATE_CLOSED;
}

int l2cap_channels_snapshot(l2cap_chan_info_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < L2CAP_MAX_CHANNELS && n < max; i++) {
        if (!g.chans[i].in_use) continue;
        if (g.chans[i].state == L2CAP_STATE_CLOSED) continue;
        out[n].cid          = g.chans[i].cid;
        out[n].peer_cid     = g.chans[i].peer_cid;
        out[n].psm          = g.chans[i].psm;
        out[n].conn_handle  = g.chans[i].conn_handle;
        out[n].local_mtu    = g.chans[i].local_mtu;
        out[n].peer_mtu     = g.chans[i].peer_mtu;
        out[n].state        = g.chans[i].state;
        out[n].flavor       = g.chans[i].flavor;
        n++;
    }
    return n;
}

int l2cap_channel_count(void)
{
    int n = 0;
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (g.chans[i].in_use && g.chans[i].state != L2CAP_STATE_CLOSED) n++;
    }
    return n;
}

int l2cap_signaling_ready(void) { return g.signaling_ready; }

uint32_t l2cap_rx_frames(void)        { return g.rx_frames; }
uint32_t l2cap_tx_frames(void)        { return g.tx_frames; }
uint32_t l2cap_signaling_count(void)  { return g.sig_count; }
