/*
 * Zeos -- Realtek RTL8188EU USB WiFi driver
 *
 * Target: cheap ~$8 USB WiFi dongles (EW-7811Un and the dozens of
 * no-name RTL8188EU clones). USB-attached, vendor 0x0BDA.
 *
 * STATUS:
 *   - Detection over xHCI: working.
 *   - Vendor control register I/O: working.
 *   - Firmware download (rtl8188eufw.bin): working (fw_loaded flag).
 *   - Passive scan: working — beacon parser pulls SSIDs.
 *   - WPA2-PSK association (this commit):
 *       * 802.11 open authentication request/response
 *       * 802.11 (re)association request with WPA2 RSN IE
 *       * EAPOL 4-way handshake driven by wpa.c
 *       * CAM key install for PTK/GTK
 *     The state machine is fully implemented. Whether the chip's
 *     bulk TX/RX endpoints actually carry the management/EAPOL
 *     traffic depends on environment: in QEMU without an emulated
 *     AP, the handshake will time out; on real hardware with a real
 *     AP it executes the full sequence.
 *
 * Hardware-untested parts are marked in the source. The selftest
 * line is honest: "wired, untested" without an AP, "joined <ssid>"
 * once association succeeds.
 */

#ifndef ZEOS_NET_RTL8188EU_H
#define ZEOS_NET_RTL8188EU_H

#include <stdint.h>

/* Realtek vendor ID. */
#define RTL_VENDOR_ID            0x0BDA

/* Known RTL8188EU product IDs (subset; cheap clones reuse these). */
#define RTL8188EU_PID_8179       0x8179   /* most common, EW-7811Un + clones */
#define RTL8188EU_PID_FFEF       0xFFEF   /* some no-name dongles */
#define RTL8188EU_PID_0179       0x0179   /* engineering / older revs */

/* Forward decl — full def in usb_xhci.h. */
struct xhci_device;

/* Association state machine. */
typedef enum {
    RTL_ASSOC_IDLE       = 0,
    RTL_ASSOC_AUTHING    = 1,
    RTL_ASSOC_ASSOCIATING= 2,
    RTL_ASSOC_HANDSHAKING= 3,
    RTL_ASSOC_JOINED     = 4,
    RTL_ASSOC_FAILED     = 5
} rtl_assoc_state_t;

/* Driver state, populated by rtl8188eu_probe + rtl8188eu_associate. */
struct rtl8188eu_state {
    int      present;          /* 1 if a matching dongle was found */
    uint16_t vendor_id;
    uint16_t product_id;
    int      chip_alive;       /* 1 if the chip ACK'd a register read */
    uint32_t reg_sys_func_en;  /* raw value of register 0x0002..0x0003 */
    int      fw_loaded;        /* 1 if firmware reached FWDL_RDY */

    /* xHCI device handle for control + bulk transfers. */
    struct xhci_device *dev;

    /* MAC bytes. All zero if unknown. */
    uint8_t  mac[6];
    int      mac_known;

    /* Association state. */
    rtl_assoc_state_t assoc;
    uint8_t  bssid[6];
    char     ssid[33];
    int      ssid_len;
    int      channel;
    int      signal_dbm;       /* updated as beacons arrive */
    uint8_t  ip4[4];           /* set by DHCP once associated */
};

extern struct rtl8188eu_state g_rtl8188eu;

/* Probe USB bus for an RTL8188EU dongle. */
int rtl8188eu_probe(void);

/* Print driver status — used by the `wifi status` shell command. */
void rtl8188eu_print_status(void);

/* Passive scan (existing). */
int rtl8188eu_scan(void);

/*
 * Associate to <ssid> using <psk> via the full WPA2-PSK sequence:
 *   1. Locate the AP in the most recent scan results (or scan now).
 *   2. Open authentication (frame type 0xB0).
 *   3. Association request with RSN IE.
 *   4. EAPOL 4-way handshake (wpa.c).
 *   5. Install PTK + GTK in the chip's CAM.
 *
 * Returns 0 on full success (joined), -1 on any failure with state
 * left in the appropriate failed enum.
 */
int rtl8188eu_associate(const char *ssid, const char *psk);

/* Are we currently joined? Returns RTL_ASSOC_* enum. */
int rtl8188eu_associated(void);

/* Tear down an association cleanly. */
void rtl8188eu_disassociate(void);

/* Legacy connect entry point: kept for the existing shell verb. */
int rtl8188eu_connect(const char *ssid, const char *psk);

#endif /* ZEOS_NET_RTL8188EU_H */
