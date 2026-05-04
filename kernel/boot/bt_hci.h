/*
 * Zeos -- Bluetooth HCI command + event layer
 *
 * Sits on top of bt_usb.c. Owns:
 *   - HCI opcode framing (OGF/OCF -> 16-bit opcode)
 *   - The minimum command set needed for discovery + connect:
 *       Reset, Read Local Version, Read BD_ADDR,
 *       Inquiry (classic), LE Set Scan Params/Enable (LE),
 *       LE Create Connection, Disconnect.
 *   - Event decode for Command Complete / Status, Inquiry Result,
 *     LE Advertising Report, Connection Complete, Disconnection Complete.
 *   - A per-tick poll() that drains events into the discovered-device
 *     table and updates link state.
 *
 * NOT in this file:
 *   - L2CAP, RFCOMM, GATT, A2DP, HFP, HID profiles. Those are future
 *     work — this file lands the foundation only.
 *
 * Foundation for OS_LITTLE_THINGS #16 BT pair UI.
 */

#ifndef ZEOS_BT_HCI_H
#define ZEOS_BT_HCI_H

#include <stdint.h>

/* OGF (Opcode Group Field) values we use. */
#define HCI_OGF_LINK_CTRL       0x01
#define HCI_OGF_HOST_CTRL       0x03
#define HCI_OGF_INFO            0x04
#define HCI_OGF_LE              0x08

/* Opcode = (OGF << 10) | OCF. */
#define HCI_OPCODE(ogf, ocf)    ((uint16_t)(((ogf) << 10) | (ocf)))

#define HCI_CMD_INQUIRY                 HCI_OPCODE(0x01, 0x0001)
#define HCI_CMD_DISCONNECT              HCI_OPCODE(0x01, 0x0006)
#define HCI_CMD_RESET                   HCI_OPCODE(0x03, 0x0003)
#define HCI_CMD_SET_EVENT_FILTER        HCI_OPCODE(0x03, 0x0005)
#define HCI_CMD_READ_LOCAL_VERSION      HCI_OPCODE(0x04, 0x0001)
#define HCI_CMD_READ_BD_ADDR            HCI_OPCODE(0x04, 0x0009)
#define HCI_CMD_LE_SET_SCAN_PARAMS      HCI_OPCODE(0x08, 0x000B)
#define HCI_CMD_LE_SET_SCAN_ENABLE      HCI_OPCODE(0x08, 0x000C)
#define HCI_CMD_LE_CREATE_CONNECTION    HCI_OPCODE(0x08, 0x000D)

/* HCI event codes. */
#define HCI_EVT_INQUIRY_COMPLETE        0x01
#define HCI_EVT_INQUIRY_RESULT          0x02
#define HCI_EVT_CONNECTION_COMPLETE     0x03
#define HCI_EVT_DISCONNECTION_COMPLETE  0x05
#define HCI_EVT_COMMAND_COMPLETE        0x0E
#define HCI_EVT_COMMAND_STATUS          0x0F
#define HCI_EVT_LE_META                 0x3E
/* LE Meta sub-events */
#define HCI_LE_SUBEVT_CONN_COMPLETE     0x01
#define HCI_LE_SUBEVT_ADV_REPORT        0x02

/* Discovered-device record. RSSI is signed dBm; name is best-effort
 * (only populated if the controller volunteered it in the inquiry result
 * or LE adv data — empty string otherwise). */
typedef struct {
    uint8_t  bdaddr[6];     /* little-endian, as on the wire */
    int8_t   rssi;          /* dBm; 127 if unknown */
    uint8_t  addr_type;     /* 0=BR/EDR, 1=LE Public, 2=LE Random */
    char     name[32];
    uint8_t  name_len;      /* 0 if no name */
} bt_device_t;

/* Connection-state enum returned by bt_link_state(). */
typedef enum {
    BT_LINK_NONE = 0,
    BT_LINK_CONNECTING,
    BT_LINK_CONNECTED,
    BT_LINK_DISCONNECTING,
} bt_link_state_t;

/* ── Lifecycle ──────────────────────────────────────────────────────
 *
 * bt_hci_init() must run AFTER bt_usb_init() bound a controller. It
 * issues HCI Reset → Read Local Version → Read BD_ADDR and parks the
 * stack ready to scan. Returns 0 if the chip answered the boot cmds,
 * -1 if no controller, +1 if controller present but boot sequence
 * failed (e.g. real silicon needing vendor firmware). */
int  bt_hci_init(void);

/* Drain any pending HCI events. Call from chain_resolve(). */
void bt_hci_poll(void);

/* ── Inspection ─────────────────────────────────────────────────────*/

/* Was the boot sequence successful enough to claim "controller present"?
 * Returns 0 / 1. */
int  bt_hci_ready(void);

/* Local controller BD_ADDR. Always returns 6 bytes; zeros if unknown. */
const uint8_t *bt_hci_bdaddr(void);

/* HCI version + manufacturer + LMP version (Read Local Version reply).
 * All three are zero until the boot sequence completes. */
void bt_hci_local_version(uint8_t *hci_ver, uint16_t *manuf, uint8_t *lmp_ver);

/* Are we currently scanning (classic inquiry OR LE scan)? */
int  bt_hci_scanning(void);

/* ── Discovery API ──────────────────────────────────────────────────*/

/* Start a classic Inquiry for `duration_secs` seconds (clamped to 1..61).
 * Discovered devices accumulate into the internal table; pull with
 * bt_devices_found(). Returns 0 on command dispatch, -1 if no
 * controller. */
int  bt_inquiry_start(uint8_t duration_secs);

/* Enable LE scanning. Uses standard passive 30ms-window/30ms-interval.
 * Returns 0 / -1. bt_le_scan_stop() halts scanning explicitly. */
int  bt_le_scan_start(void);
int  bt_le_scan_stop(void);

/* Snapshot the discovered-device table. Returns count actually copied. */
int  bt_devices_found(bt_device_t *out, int max);

/* Drop the discovered-device table (e.g. before a new scan). */
void bt_devices_clear(void);

/* ── Connection lifecycle ───────────────────────────────────────────*/

/* Initiate an LE Create Connection to a previously-discovered LE peer.
 * `bdaddr` is little-endian, `addr_type` matches bt_device_t::addr_type
 * (1 or 2). Returns 0 on dispatch, -1 otherwise. */
int  bt_le_connect(const uint8_t bdaddr[6], uint8_t addr_type);

/* HCI Disconnect (reason 0x13 = remote-user-terminated). */
int  bt_disconnect(const uint8_t bdaddr[6]);

/* Current link state for the most-recent connection attempt. */
bt_link_state_t bt_link_state(void);

#endif /* ZEOS_BT_HCI_H */
