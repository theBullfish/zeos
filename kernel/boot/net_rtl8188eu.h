/*
 * Zeos -- Realtek RTL8188EU USB WiFi driver (detection-only stage)
 *
 * Target: cheap ~$8 USB WiFi dongles (EW-7811Un and the dozens of
 * no-name RTL8188EU clones). USB-attached, vendor 0x0BDA.
 *
 * STATUS — honest:
 *   - Detection over xHCI: WORKS (matches VID/PID against enumerated
 *     USB devices, identifies the chipset).
 *   - Vendor control transfer (register read on offsets that don't
 *     require firmware): WORKS at the transport level via
 *     xhci_control_transfer. We probe register 0x0000 (SYS_ISO_CTRL)
 *     to confirm the chip is alive.
 *   - MAC read from EFUSE: NOT IMPLEMENTED. EFUSE access on RTL8188EU
 *     requires a multi-step power-on sequence (LDO/clock/EFUSE-burst
 *     init) which is firmware-gated on most production silicon
 *     revisions. Without rtl8188eufw.bin in kernel/lib/firmware/ we
 *     do not pretend to read a MAC. The driver logs "MAC unknown
 *     (firmware required)" rather than fabricating one.
 *   - Firmware load: NOT IMPLEMENTED. rtl8188eufw.bin (~32 KB) is not
 *     embedded; the upload sequence (FWDL command, beacon-time CKSUM,
 *     polling FWHALT) is documented but useless without the blob.
 *   - Scan / connect: NOT IMPLEMENTED. These require:
 *       1. Loaded firmware (above).
 *       2. Bulk OUT/IN endpoints on the dongle. xhci_bulk_transfer
 *          currently returns -1 in usb_xhci.c — the bulk endpoint
 *          context machinery is not wired in this branch.
 *       3. WPA2 4-way handshake on top, when applicable.
 *     The shell `wifi scan` and `wifi connect` commands therefore
 *     report exactly this state and refuse to fake a result.
 *
 * The driver is also not plugged into net.c's NIC dispatch — there is
 * no working data path yet, so installing it as the active NIC would
 * mislead dhcp_discover() into hanging. Once bulk + firmware are in
 * place, net_rtl8188eu_send/recv get added to the dispatch chain.
 *
 * Build target is: detect the dongle, confirm chip-alive via control
 * read, refuse to invent data we don't have.
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

/* Driver state, populated by rtl8188eu_probe. */
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
};

extern struct rtl8188eu_state g_rtl8188eu;

/*
 * Probe USB bus for an RTL8188EU dongle. Must be called after
 * xhci_init. Returns:
 *   0   — dongle present, chip responsive (limited functionality)
 *  -1   — no dongle / not RTL8188EU / chip didn't respond
 *
 * Always honest: never returns 0 with fabricated data.
 */
int rtl8188eu_probe(void);

/*
 * Print driver status — used by the `wifi status` shell command.
 * Tells the operator exactly what works and what doesn't.
 */
void rtl8188eu_print_status(void);

/*
 * Stubs that intentionally refuse to lie.
 * scan/connect both return -1 and log why.
 */
int rtl8188eu_scan(void);
int rtl8188eu_connect(const char *ssid, const char *psk);

#endif /* ZEOS_NET_RTL8188EU_H */
