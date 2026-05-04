/*
 * Zeos -- Bluetooth L2CAP (Logical Link Control and Adaptation Protocol)
 *
 * L2CAP on top of HCI ACL. Sits between bt_hci.c (which owns HCI commands
 * + events + ACL transport) and the layered profiles (GATT/ATT, RFCOMM,
 * HID-over-BT). Owns:
 *   - ACL recombination + L2CAP framing decode (Length, CID).
 *   - Outbound: ACL fragmentation with the L2CAP header.
 *   - Signaling channel state machine for classic (CID 0x0001) and
 *     LE (CID 0x0005): Connection Req/Rsp, Configuration Req/Rsp,
 *     Disconnection Req/Rsp, Echo Req/Rsp, Information Req/Rsp.
 *   - CID multiplexing: up to 32 active dynamic channels.
 *   - MTU negotiation (default 672 classic / 23 LE; bumped via Config
 *     to 1024 / 247 after Connect Response).
 *   - Pre-routed fixed CID hooks: 0x0004 (ATT/GATT), 0x0006 (LE SMP),
 *     so layered protocols can register a callback table.
 *
 * Foundation for GATT (ATT/CID 0x0004), RFCOMM (PSM 0x0003), and
 * HID-over-BT (PSM 0x0011 control / 0x0013 interrupt).
 *
 * Pipeline (CHAIN_BT_L2CAP, parent CHAIN_BLUETOOTH):
 *   acl_rx -> l2cap_decode -> channel_dispatch -> app_signal
 */

#ifndef ZEOS_BT_L2CAP_H
#define ZEOS_BT_L2CAP_H

#include <stdint.h>

/* Fixed channel identifiers (Core Spec Vol 3 Part A §2.1). */
#define L2CAP_CID_NULL              0x0000
#define L2CAP_CID_SIGNAL_CLASSIC    0x0001
#define L2CAP_CID_CONNLESS          0x0002
#define L2CAP_CID_ATT               0x0004  /* BLE GATT */
#define L2CAP_CID_SIGNAL_LE         0x0005
#define L2CAP_CID_SMP               0x0006  /* LE Security Manager */

/* Dynamic CID range for connection-oriented channels. */
#define L2CAP_CID_DYN_BASE          0x0040
#define L2CAP_CID_DYN_MAX           0x007F

/* Default and target MTUs. */
#define L2CAP_MTU_CLASSIC_DEFAULT   672
#define L2CAP_MTU_CLASSIC_TARGET    1024
#define L2CAP_MTU_LE_DEFAULT        23
#define L2CAP_MTU_LE_TARGET         247

/* L2CAP signaling command opcodes (Vol 3 Part A §4). */
#define L2CAP_SIG_CMD_REJECT        0x01
#define L2CAP_SIG_CONN_REQ          0x02
#define L2CAP_SIG_CONN_RSP          0x03
#define L2CAP_SIG_CONFIG_REQ        0x04
#define L2CAP_SIG_CONFIG_RSP        0x05
#define L2CAP_SIG_DISCONN_REQ       0x06
#define L2CAP_SIG_DISCONN_RSP       0x07
#define L2CAP_SIG_ECHO_REQ          0x08
#define L2CAP_SIG_ECHO_RSP          0x09
#define L2CAP_SIG_INFO_REQ          0x0A
#define L2CAP_SIG_INFO_RSP          0x0B
/* LE-only signaling */
#define L2CAP_SIG_LE_CONN_REQ       0x14
#define L2CAP_SIG_LE_CONN_RSP       0x15
#define L2CAP_SIG_LE_CREDIT         0x16

/* Information Request types. */
#define L2CAP_INFO_TYPE_CL_MTU      0x0001
#define L2CAP_INFO_TYPE_FEATURES    0x0002
#define L2CAP_INFO_TYPE_FIXED_CHANS 0x0003

/* Channel states. */
typedef enum {
    L2CAP_STATE_CLOSED = 0,
    L2CAP_STATE_WAIT_CONN_RSP,      /* sent ConnectReq, awaiting Rsp */
    L2CAP_STATE_CONFIG,             /* exchanging ConfigReq/Rsp */
    L2CAP_STATE_OPEN,               /* config done, ready for data */
    L2CAP_STATE_WAIT_DISCONN_RSP,
} l2cap_state_t;

/* Channel transport flavor. */
typedef enum {
    L2CAP_FLAVOR_FIXED = 0,         /* signaling, ATT, SMP */
    L2CAP_FLAVOR_CLASSIC,           /* connection-oriented BR/EDR */
    L2CAP_FLAVOR_LE_CREDIT,         /* LE credit-based flow */
} l2cap_flavor_t;

#define L2CAP_MAX_CHANNELS          32

/* Snapshot record returned by l2cap_channels_snapshot() for the shell. */
typedef struct {
    uint16_t        cid;
    uint16_t        peer_cid;
    uint16_t        psm;
    uint16_t        conn_handle;
    uint16_t        local_mtu;
    uint16_t        peer_mtu;
    l2cap_state_t   state;
    l2cap_flavor_t  flavor;
} l2cap_chan_info_t;

/* Per-CID receive callback (used by ATT, SMP, layered profiles). */
typedef void (*l2cap_rx_cb_t)(uint16_t conn_handle,
                              uint16_t cid,
                              const uint8_t *data,
                              uint16_t len);

/* ── Lifecycle ──────────────────────────────────────────────────────*/

/* Initialize L2CAP. Wires the signaling state machine and pre-routes
 * the fixed CIDs. Idempotent. Returns 0. */
int  bt_l2cap_init(void);

/* Drain ACL data from bt_usb_acl_poll(), recombine and dispatch one
 * frame at a time. Call from the chain pipeline / scheduler tick. */
void bt_l2cap_poll(void);

/* ── Fixed-channel registration ─────────────────────────────────────*/

/* Register a callback for a fixed CID (ATT 0x0004, SMP 0x0006). The
 * callback fires for every recombined L2CAP frame on that CID. Pass
 * NULL to unregister. Returns 0 on success, -1 on bad CID. */
int  bt_l2cap_register_fixed(uint16_t cid, l2cap_rx_cb_t cb);

/* ── Dynamic-channel API ────────────────────────────────────────────*/

/* Open a classic connection-oriented L2CAP channel to `psm` on the
 * peer at `conn_handle` (HCI ACL handle). On success, *out_cid is set
 * to the locally-allocated CID. Returns 0 on dispatch (Connect Req
 * sent, awaiting Connect Rsp), -1 on bad args / channel table full /
 * not initialized. The channel reaches L2CAP_STATE_OPEN asynchronously
 * after Configuration completes; poll l2cap_channel_state() or wait
 * for the registered rx callback. */
int  l2cap_open(uint16_t conn_handle, uint16_t psm, uint16_t *out_cid);

/* Open an LE credit-based channel. */
int  l2cap_open_le(uint16_t conn_handle, uint16_t psm, uint16_t *out_cid);

/* Send a frame on a dynamic channel. Returns bytes shipped or -1. */
int  l2cap_send(uint16_t cid, const void *data, uint16_t len);

/* Pull at most `max` bytes of a queued inbound frame. Returns frame
 * length on success, 0 if queue empty, -1 on bad CID. The first frame
 * in the queue is consumed regardless of `max`; bytes beyond `max` are
 * dropped (ATT/GATT framing always respects MTU so this is a safety
 * truncation only). */
int  l2cap_recv(uint16_t cid, void *buf, uint16_t max);

/* Tear down a dynamic channel. Sends Disconnect Req. Returns 0 on
 * dispatch, -1 on bad CID / not open. */
int  l2cap_close(uint16_t cid);

/* ── Inspection ─────────────────────────────────────────────────────*/

l2cap_state_t l2cap_channel_state(uint16_t cid);

/* Snapshot the active channel table (state != CLOSED). Returns count
 * actually copied. */
int  l2cap_channels_snapshot(l2cap_chan_info_t *out, int max);

/* Total number of active channels (state != CLOSED), including fixed. */
int  l2cap_channel_count(void);

/* Was bt_l2cap_init() run and signaling registered? */
int  l2cap_signaling_ready(void);

/* Provenance counters (for chain vault_version bumps). */
uint32_t l2cap_rx_frames(void);
uint32_t l2cap_tx_frames(void);
uint32_t l2cap_signaling_count(void);

/* ── Internals exposed to chain_registry.c resolves only ─────────── */

/* Push a raw inbound ACL fragment. Bypasses bt_usb_acl_poll() — used
 * by the test path. Real flow comes from bt_l2cap_poll(). */
void bt_l2cap_ingest_acl(const uint8_t *acl, int len);

#endif /* ZEOS_BT_L2CAP_H */
