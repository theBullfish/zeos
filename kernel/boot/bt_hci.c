/*
 * Zeos -- Bluetooth HCI command + event layer
 *
 * Bluetooth HCI over USB. Commands + events + LE discovery + initial
 * connection. L2CAP, RFCOMM, GATT, A2DP, HFP, HID profiles are future
 * work — this lands the foundation only.
 * Foundation for OS_LITTLE_THINGS #16 BT pair UI.
 */

#include "bt_hci.h"
#include "bt_usb.h"
#include "kprint.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);

/* ── State ──────────────────────────────────────────────────────── */

#define BT_DEV_TABLE_MAX    16

typedef struct {
    int             ready;          /* boot sequence completed */
    uint8_t         bdaddr[6];
    uint8_t         hci_ver;
    uint16_t        manuf;
    uint8_t         lmp_ver;
    uint16_t        lmp_subver;
    uint16_t        last_cmd_opcode;
    int             last_status;    /* 0 = success */

    int             scanning_classic;
    int             scanning_le;

    bt_device_t     devs[BT_DEV_TABLE_MAX];
    int             dev_count;

    bt_link_state_t link_state;
    uint8_t         link_peer[6];
    uint16_t        link_handle;
} bt_hci_state_t;

static bt_hci_state_t g;

/* ── Event provenance counters (chain vault_version source) ──────── */

static uint32_t s_evt_count;
static uint32_t s_cmd_count;

/* ── Helpers ────────────────────────────────────────────────────── */

static int bdaddr_eq(const uint8_t a[6], const uint8_t b[6])
{
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int find_or_alloc_dev(const uint8_t bdaddr[6])
{
    for (int i = 0; i < g.dev_count; i++)
        if (bdaddr_eq(g.devs[i].bdaddr, bdaddr)) return i;
    if (g.dev_count >= BT_DEV_TABLE_MAX) return -1;
    int idx = g.dev_count++;
    memset(&g.devs[idx], 0, sizeof(g.devs[idx]));
    g.devs[idx].rssi = 127;
    for (int i = 0; i < 6; i++) g.devs[idx].bdaddr[i] = bdaddr[i];
    return idx;
}

/* ── Command shaper ─────────────────────────────────────────────── */
/* HCI command packet layout: [opcode_lo, opcode_hi, plen, params...] */
static int hci_send(uint16_t opcode, const void *params, int plen)
{
    uint8_t pkt[BT_HCI_PKT_MAX];
    if (plen < 0 || plen > 252) return -1;
    pkt[0] = (uint8_t)(opcode & 0xFF);
    pkt[1] = (uint8_t)((opcode >> 8) & 0xFF);
    pkt[2] = (uint8_t)plen;
    if (plen) memcpy(pkt + 3, params, plen);
    g.last_cmd_opcode = opcode;
    s_cmd_count++;
    return bt_usb_send_command(pkt, 3 + plen);
}

/* Wait synchronously for a Command Complete matching `opcode`, copying
 * its parameter bytes (after status) into `out`. Bounded retry; returns
 * status byte (0 = success), -1 on timeout, -2 on transport error. */
static int hci_wait_complete(uint16_t opcode, void *out, int max_out)
{
    uint8_t evt[BT_HCI_PKT_MAX];
    /* ~250ms budget at no-sleep polling. The Reset path on a real chip
     * can take 100–150ms; vendor firmware load is much longer (handled
     * by the firmware loader, not us). */
    for (int spin = 0; spin < 200000; spin++) {
        int n = bt_usb_poll_event(evt, sizeof(evt));
        if (n < 0) return -2;
        if (n == 0) continue;
        if (n < 2) continue;
        uint8_t ev_code = evt[0];
        uint8_t plen    = evt[1];
        if (n < 2 + plen) continue;
        s_evt_count++;
        if (ev_code == HCI_EVT_COMMAND_COMPLETE && plen >= 4) {
            uint16_t op = evt[3] | ((uint16_t)evt[4] << 8);
            uint8_t  st = evt[5];
            if (op == opcode) {
                int ret_len = plen - 4;
                if (out && max_out > 0 && ret_len > 1) {
                    int copy = ret_len - 1;
                    if (copy > max_out) copy = max_out;
                    memcpy(out, evt + 6, copy);
                }
                g.last_status = st;
                return st;
            }
        } else if (ev_code == HCI_EVT_COMMAND_STATUS && plen >= 4) {
            uint16_t op = evt[4] | ((uint16_t)evt[5] << 8);
            if (op == opcode) {
                uint8_t st = evt[2];
                /* CommandStatus comes ahead of CommandComplete for
                 * long-running ops (Inquiry, LE Create Connection).
                 * Treat it as success-so-far. */
                g.last_status = st;
                return st;
            }
        }
        /* else: queue async event — drop on the floor here, it'll be
         * re-decoded by bt_hci_poll() during normal operation. */
    }
    return -1;
}

/* ── Boot sequence ──────────────────────────────────────────────── */

int bt_hci_init(void)
{
    memset(&g, 0, sizeof(g));
    if (!bt_usb_present()) return -1;

    /* Reset */
    if (hci_send(HCI_CMD_RESET, 0, 0) != 0) return 1;
    int st = hci_wait_complete(HCI_CMD_RESET, 0, 0);
    if (st != 0) {
        kputs("[bt-hci] Reset failed (status=");
        kput_hex((uint64_t)(uint32_t)st);
        kputs(")\n");
        return 1;
    }

    /* Read Local Version. Reply (after status): hci_ver(1) hci_rev(2)
     * lmp_ver(1) manuf(2) lmp_subver(2) = 8 bytes. */
    uint8_t rlv[16];
    if (hci_send(HCI_CMD_READ_LOCAL_VERSION, 0, 0) != 0) return 1;
    st = hci_wait_complete(HCI_CMD_READ_LOCAL_VERSION, rlv, sizeof(rlv));
    if (st == 0) {
        g.hci_ver    = rlv[0];
        g.lmp_ver    = rlv[3];
        g.manuf      = rlv[4] | ((uint16_t)rlv[5] << 8);
        g.lmp_subver = rlv[6] | ((uint16_t)rlv[7] << 8);
    }

    /* Read BD_ADDR. Reply: bdaddr(6). */
    uint8_t bd[8];
    if (hci_send(HCI_CMD_READ_BD_ADDR, 0, 0) != 0) return 1;
    st = hci_wait_complete(HCI_CMD_READ_BD_ADDR, bd, sizeof(bd));
    if (st == 0) {
        for (int i = 0; i < 6; i++) g.bdaddr[i] = bd[i];
    }

    /* Set Event Filter: clear filter (params: filter_type=0). Best-effort. */
    {
        uint8_t p = 0x00;
        (void)hci_send(HCI_CMD_SET_EVENT_FILTER, &p, 1);
        (void)hci_wait_complete(HCI_CMD_SET_EVENT_FILTER, 0, 0);
    }

    g.ready = 1;
    kputs("[bt-hci] controller ready BD_ADDR=");
    /* Print as ab:cd:ef:01:02:03 (canonical big-endian display order). */
    for (int i = 5; i >= 0; i--) {
        if (g.bdaddr[i] < 0x10) kputc('0');
        kput_hex(g.bdaddr[i]);
        if (i) kputc(':');
    }
    kputs(" hci_ver=");
    kput_dec(g.hci_ver);
    kputs(" manuf=");
    kput_hex(g.manuf);
    kputs("\n");
    return 0;
}

int bt_hci_ready(void) { return g.ready; }
const uint8_t *bt_hci_bdaddr(void) { return g.bdaddr; }
void bt_hci_local_version(uint8_t *hv, uint16_t *m, uint8_t *lv)
{
    if (hv) *hv = g.hci_ver;
    if (m)  *m  = g.manuf;
    if (lv) *lv = g.lmp_ver;
}
int bt_hci_scanning(void) { return g.scanning_classic || g.scanning_le; }
bt_link_state_t bt_link_state(void) { return g.link_state; }

/* ── Event decode (async path) ──────────────────────────────────── */

static void decode_inquiry_result(const uint8_t *p, int plen)
{
    /* num_responses(1) [bdaddr(6) page_scan_rep(1) reserved(2) cod(3)
     *                   clock_offset(2)] x N — no RSSI in classic
     * inquiry result. (The "with RSSI" variant is event 0x22.) */
    if (plen < 1) return;
    int num = p[0];
    int off = 1;
    int per = 14;
    for (int i = 0; i < num && off + per <= plen; i++) {
        const uint8_t *e = p + off;
        int idx = find_or_alloc_dev(e);
        if (idx >= 0) {
            g.devs[idx].addr_type = 0;  /* BR/EDR */
        }
        off += per;
    }
}

static void decode_le_adv_report(const uint8_t *p, int plen)
{
    /* num_reports(1) [event_type(1) addr_type(1) addr(6) data_len(1)
     *                 data(N) rssi(1)] x N */
    if (plen < 1) return;
    int num = p[0];
    int off = 1;
    for (int i = 0; i < num; i++) {
        if (off + 10 > plen) break;
        uint8_t addr_type = p[off + 1];
        const uint8_t *bd = p + off + 2;
        int data_len      = p[off + 9];
        if (off + 10 + data_len + 1 > plen) break;
        const uint8_t *data = p + off + 10;
        int8_t rssi = (int8_t)p[off + 10 + data_len];

        int idx = find_or_alloc_dev(bd);
        if (idx >= 0) {
            g.devs[idx].rssi = rssi;
            g.devs[idx].addr_type = (addr_type == 0) ? 1 : 2;

            /* Walk the LE advertising AD structures looking for a
             * Local Name (0x08 short, 0x09 complete). */
            int d_off = 0;
            while (d_off + 1 < data_len) {
                uint8_t ad_len = data[d_off];
                if (ad_len == 0) break;
                if (d_off + 1 + ad_len > data_len) break;
                uint8_t ad_type = data[d_off + 1];
                if ((ad_type == 0x08 || ad_type == 0x09) && ad_len > 1) {
                    int name_len = ad_len - 1;
                    if (name_len > (int)sizeof(g.devs[idx].name) - 1)
                        name_len = sizeof(g.devs[idx].name) - 1;
                    memcpy(g.devs[idx].name, data + d_off + 2, name_len);
                    g.devs[idx].name[name_len] = 0;
                    g.devs[idx].name_len = (uint8_t)name_len;
                }
                d_off += 1 + ad_len;
            }
        }
        off += 10 + data_len + 1;
    }
}

static void decode_conn_complete(const uint8_t *p, int plen, int is_le)
{
    /* Classic: status(1) handle(2) bdaddr(6) link_type(1) encrypt(1).
     * LE meta: subevt(1) status(1) handle(2) role(1) peer_addr_type(1)
     *          peer_addr(6) ...                                       */
    if (is_le) {
        if (plen < 12) return;
        uint8_t status = p[1];
        uint16_t handle = p[2] | ((uint16_t)p[3] << 8);
        const uint8_t *peer = p + 6;
        if (status == 0) {
            g.link_state = BT_LINK_CONNECTED;
            g.link_handle = handle;
            for (int i = 0; i < 6; i++) g.link_peer[i] = peer[i];
        } else {
            g.link_state = BT_LINK_NONE;
        }
    } else {
        if (plen < 11) return;
        uint8_t status = p[0];
        uint16_t handle = p[1] | ((uint16_t)p[2] << 8);
        const uint8_t *peer = p + 3;
        if (status == 0) {
            g.link_state = BT_LINK_CONNECTED;
            g.link_handle = handle;
            for (int i = 0; i < 6; i++) g.link_peer[i] = peer[i];
        } else {
            g.link_state = BT_LINK_NONE;
        }
    }
}

static void decode_disc_complete(const uint8_t *p, int plen)
{
    /* status(1) handle(2) reason(1) */
    if (plen < 4) return;
    if (p[0] == 0) g.link_state = BT_LINK_NONE;
}

void bt_hci_poll(void)
{
    if (!bt_usb_present()) return;
    /* Drain a few events per tick. A well-behaved chip will queue ~1
     * event per HCI command + 1 per inquiry response, which fits well
     * inside an 8-tick scheduler interval. */
    for (int loops = 0; loops < 8; loops++) {
        uint8_t evt[BT_HCI_PKT_MAX];
        int n = bt_usb_poll_event(evt, sizeof(evt));
        if (n <= 0) return;
        if (n < 2) return;
        uint8_t ev_code = evt[0];
        uint8_t plen    = evt[1];
        if (n < 2 + plen) return;
        s_evt_count++;
        const uint8_t *p = evt + 2;

        switch (ev_code) {
        case HCI_EVT_COMMAND_COMPLETE:
            /* Already decoded synchronously by hci_wait_complete during
             * boot; async ones only land here after init. Track the
             * status of long-running scan-enable acks. */
            if (plen >= 4) {
                uint16_t op = p[1] | ((uint16_t)p[2] << 8);
                uint8_t  st = p[3];
                if (op == HCI_CMD_LE_SET_SCAN_ENABLE)
                    g.scanning_le = (st == 0) ? g.scanning_le : 0;
                if (op == HCI_CMD_INQUIRY)
                    g.scanning_classic = (st == 0) ? g.scanning_classic : 0;
            }
            break;
        case HCI_EVT_COMMAND_STATUS:
            /* Indicates an opcode is being processed; nothing to record
             * beyond updating the last-seen status. */
            break;
        case HCI_EVT_INQUIRY_COMPLETE:
            g.scanning_classic = 0;
            break;
        case HCI_EVT_INQUIRY_RESULT:
            decode_inquiry_result(p, plen);
            break;
        case HCI_EVT_CONNECTION_COMPLETE:
            decode_conn_complete(p, plen, 0);
            break;
        case HCI_EVT_DISCONNECTION_COMPLETE:
            decode_disc_complete(p, plen);
            break;
        case HCI_EVT_LE_META:
            if (plen >= 1) {
                uint8_t sub = p[0];
                if (sub == HCI_LE_SUBEVT_ADV_REPORT)
                    decode_le_adv_report(p + 1, plen - 1);
                else if (sub == HCI_LE_SUBEVT_CONN_COMPLETE)
                    decode_conn_complete(p, plen, 1);
            }
            break;
        default:
            break;
        }
    }
}

/* ── Discovery ──────────────────────────────────────────────────── */

int bt_inquiry_start(uint8_t duration_secs)
{
    if (!bt_usb_present()) return -1;
    if (duration_secs < 1)  duration_secs = 1;
    if (duration_secs > 61) duration_secs = 61;
    /* HCI Inquiry: LAP(3, 0x9E8B33 GIAC) + length(1) + num_responses(1).
     * length is in 1.28s units; convert duration_secs → length. */
    uint8_t p[5];
    p[0] = 0x33;
    p[1] = 0x8B;
    p[2] = 0x9E;
    p[3] = (uint8_t)((uint32_t)duration_secs * 100u / 128u);
    if (p[3] == 0) p[3] = 1;
    if (p[3] > 0x30) p[3] = 0x30;
    p[4] = 0x00;  /* unlimited responses */
    if (hci_send(HCI_CMD_INQUIRY, p, 5) != 0) return -1;
    g.scanning_classic = 1;
    /* Inquiry returns Command Status (not Complete); just dispatch and
     * let bt_hci_poll() ingest the async results + Inquiry Complete. */
    return 0;
}

int bt_le_scan_start(void)
{
    if (!bt_usb_present()) return -1;
    /* LE Set Scan Parameters: scan_type(1)=0 passive, interval(2)=0x0030
     * (~30ms), window(2)=0x0030, own_addr_type(1)=0 public,
     * filter_policy(1)=0 accept-all. */
    uint8_t sp[7] = { 0x00, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00 };
    if (hci_send(HCI_CMD_LE_SET_SCAN_PARAMS, sp, sizeof(sp)) != 0) return -1;
    (void)hci_wait_complete(HCI_CMD_LE_SET_SCAN_PARAMS, 0, 0);

    /* LE Set Scan Enable: enable(1)=1, filter_duplicates(1)=1. */
    uint8_t en[2] = { 0x01, 0x01 };
    if (hci_send(HCI_CMD_LE_SET_SCAN_ENABLE, en, sizeof(en)) != 0) return -1;
    int st = hci_wait_complete(HCI_CMD_LE_SET_SCAN_ENABLE, 0, 0);
    if (st == 0) g.scanning_le = 1;
    return (st == 0) ? 0 : -1;
}

int bt_le_scan_stop(void)
{
    if (!bt_usb_present()) return -1;
    uint8_t en[2] = { 0x00, 0x01 };
    if (hci_send(HCI_CMD_LE_SET_SCAN_ENABLE, en, sizeof(en)) != 0) return -1;
    (void)hci_wait_complete(HCI_CMD_LE_SET_SCAN_ENABLE, 0, 0);
    g.scanning_le = 0;
    return 0;
}

int bt_devices_found(bt_device_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = g.dev_count;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = g.devs[i];
    return n;
}

void bt_devices_clear(void)
{
    g.dev_count = 0;
    memset(g.devs, 0, sizeof(g.devs));
}

/* ── Connection ─────────────────────────────────────────────────── */

int bt_le_connect(const uint8_t bdaddr[6], uint8_t addr_type)
{
    if (!bt_usb_present()) return -1;
    if (!bdaddr) return -1;
    /* LE Create Connection — 25 byte param block. */
    uint8_t p[25];
    /* scan_interval(2), scan_window(2) */
    p[0] = 0x60; p[1] = 0x00;
    p[2] = 0x30; p[3] = 0x00;
    /* initiator_filter_policy(1) = 0 use peer addr */
    p[4] = 0x00;
    /* peer_addr_type(1): 0 public, 1 random — derive from addr_type
     * (LE Public=1, LE Random=2 in our internal enum). */
    p[5] = (addr_type == 2) ? 0x01 : 0x00;
    /* peer_address(6) */
    for (int i = 0; i < 6; i++) p[6 + i] = bdaddr[i];
    /* own_addr_type(1) = 0 public */
    p[12] = 0x00;
    /* conn_interval_min(2)=0x0018, max(2)=0x0028, latency(2)=0,
     * supervision_timeout(2)=0x002A, ce_min(2)=0, ce_max(2)=0 */
    p[13] = 0x18; p[14] = 0x00;
    p[15] = 0x28; p[16] = 0x00;
    p[17] = 0x00; p[18] = 0x00;
    p[19] = 0x2A; p[20] = 0x00;
    p[21] = 0x00; p[22] = 0x00;
    p[23] = 0x00; p[24] = 0x00;

    if (hci_send(HCI_CMD_LE_CREATE_CONNECTION, p, sizeof(p)) != 0) return -1;
    g.link_state = BT_LINK_CONNECTING;
    for (int i = 0; i < 6; i++) g.link_peer[i] = bdaddr[i];
    /* Reply is Command Status; the actual connect outcome arrives
     * asynchronously as LE Meta / Connection Complete. */
    return 0;
}

int bt_disconnect(const uint8_t bdaddr[6])
{
    if (!bt_usb_present()) return -1;
    if (!bdaddr) return -1;
    /* If we have a live link to this peer, use its handle; otherwise
     * accept the request and rely on the controller's sanity-check. */
    uint16_t handle = g.link_handle;
    int matches = 1;
    for (int i = 0; i < 6; i++) if (bdaddr[i] != g.link_peer[i]) { matches = 0; break; }
    if (!matches) {
        /* No record — the most-recent link is what we dispatch against;
         * caller is asking us to drop *some* link. Reject explicitly so
         * we don't disconnect the wrong handle. */
        return -1;
    }
    if (g.link_state != BT_LINK_CONNECTED) return -1;

    /* Disconnect: handle(2) reason(1). 0x13 = remote-user-terminated. */
    uint8_t p[3];
    p[0] = (uint8_t)(handle & 0xFF);
    p[1] = (uint8_t)((handle >> 8) & 0xFF);
    p[2] = 0x13;
    if (hci_send(HCI_CMD_DISCONNECT, p, sizeof(p)) != 0) return -1;
    g.link_state = BT_LINK_DISCONNECTING;
    return 0;
}

/* ── Provenance counters consumed by CHAIN_BLUETOOTH ─────────────── */

uint32_t bt_hci_event_count(void) { return s_evt_count; }
uint32_t bt_hci_command_count(void) { return s_cmd_count; }
