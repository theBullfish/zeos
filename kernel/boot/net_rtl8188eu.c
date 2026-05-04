/*
 * Zeos -- Realtek RTL8188EU USB WiFi driver
 *
 * Currently: chip init + firmware load + passive scan.
 * Association / WPA2 / IPv4 over WiFi = future work.
 *
 * The bring-up sequence here follows the canonical Realtek HAL flow as
 * implemented by Linux's drivers/staging/rtl8188eu/hal/usb_halinit.c:
 *   1. Power-on reset (REG_APS_FSMCO bring-up)
 *   2. LDO / regulator enable
 *   3. MCU reset (REG_MCUFWDL bit 7 toggle)
 *   4. Stream firmware payload in 4 KiB pages via vendor-write 0x05.
 *      The 32-byte header at the front of rtl8188eufw.bin is firmware
 *      metadata for the host driver and is NOT shipped to the chip.
 *   5. Poll REG_MCUFWDL for FWDL_CHKSUM_OK (bit 0) and FWDL_RDY (bit 1).
 *   6. Read MAC from EFUSE.
 *   7. Passive scan: parking on a channel and listening for beacons.
 *
 * `wifi-scan` returns a list of visible APs once the firmware reports
 * ready. The driver is intentionally NOT wired into CHAIN_NET_TX/RX —
 * passive scan does not carry IP frames, and association/WPA2 is not
 * yet implemented, so plumbing it as the active NIC would mislead the
 * DHCP and TCP code paths.
 */

#include "net_rtl8188eu.h"
#include "usb_xhci.h"
#include "usb.h"
#include "kprint.h"
#include "timer.h"

extern void *memset(void *s, int c, unsigned long n);
extern void *memcpy(void *dst, const void *src, unsigned long n);

struct rtl8188eu_state g_rtl8188eu;

/* Embedded firmware blob (linked via objcopy --input-target=binary). */
extern const unsigned char _binary_rtl8188eufw_bin_start[];
extern const unsigned char _binary_rtl8188eufw_bin_end[];

/* RTL8188EU vendor request opcode (read/write differ in bmRequestType). */
#define RTW_USB_VENDOR_REQ              0x05

/* Register offsets — Realtek 8188E HAL.  The names match
 * include/rtl8188e_spec.h in the Linux staging tree. */
#define REG_SYS_FUNC_EN                 0x0002
#define REG_APS_FSMCO                   0x0004  /* power-on FSM */
#define REG_SYS_CLKR                    0x0008
#define REG_AFE_MISC                    0x0010
#define REG_LDOA15_CTRL                 0x0020
#define REG_LDOV12D_CTRL                0x0021
#define REG_SYS_ISO_CTRL                0x0000
#define REG_SYS_CFG                     0x00F0
#define REG_MCUFWDL                     0x0080
#define REG_FW_START_ADDRESS            0x1000

/* REG_MCUFWDL (0x0080) bit definitions */
#define MCUFWDL_EN                      (1u << 0)
#define MCUFWDL_RDY                     (1u << 1)
#define FWDL_CHKSUM_RPT                 (1u << 2)
#define MCUFWDL_RAM_DL_SEL              (1u << 7)
#define WINTINI_RDY                     (1u << 6)

/* Page select for FWDL: bits[2:0] of REG_MCUFWDL+1 select 4KB page index. */
#define MCUFWDL_PAGE_SHIFT              4   /* in REG_MCUFWDL byte 1 */

/* MAC offsets in the register file (after firmware ready). */
#define REG_MACID                       0x0610  /* 6 bytes */

/* Match table */
struct rtl_match { uint16_t vid; uint16_t pid; };
static const struct rtl_match k_match_table[] = {
    { RTL_VENDOR_ID, RTL8188EU_PID_8179 },
    { RTL_VENDOR_ID, RTL8188EU_PID_FFEF },
    { RTL_VENDOR_ID, RTL8188EU_PID_0179 },
};

/* ─── low-level register I/O over USB control ───────────────────── */

static int rtl_read_reg(struct xhci_device *dev, uint16_t addr,
                        void *out, int len)
{
    if (len != 1 && len != 2 && len != 4) return -1;
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE;
    sp.bRequest      = RTW_USB_VENDOR_REQ;
    sp.wValue        = addr;
    sp.wIndex        = 0;
    sp.wLength       = (uint16_t)len;
    return xhci_control_transfer(dev, &sp, out, len);
}

static int rtl_write_reg_n(struct xhci_device *dev, uint16_t addr,
                           const void *in, int len)
{
    if (len < 1 || len > 64) return -1;
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE;
    sp.bRequest      = RTW_USB_VENDOR_REQ;
    sp.wValue        = addr;
    sp.wIndex        = 0;
    sp.wLength       = (uint16_t)len;
    return xhci_control_transfer(dev, &sp, (void *)in, len);
}

static int rtl_write8(struct xhci_device *dev, uint16_t addr, uint8_t v)
{
    return rtl_write_reg_n(dev, addr, &v, 1);
}

static int rtl_write16(struct xhci_device *dev, uint16_t addr, uint16_t v)
{
    return rtl_write_reg_n(dev, addr, &v, 2);
}

static uint8_t rtl_read8(struct xhci_device *dev, uint16_t addr)
{
    uint8_t v = 0;
    rtl_read_reg(dev, addr, &v, 1);
    return v;
}

static uint16_t rtl_read16(struct xhci_device *dev, uint16_t addr)
{
    uint16_t v = 0;
    rtl_read_reg(dev, addr, &v, 2);
    return v;
}

/* ─── device match ──────────────────────────────────────────────── */

static struct xhci_device *find_rtl8188eu_device(void)
{
    int n = xhci_device_count();
    for (int i = 0; i < n; i++) {
        struct xhci_device *d = xhci_get_device(i);
        if (!d) continue;
        for (unsigned m = 0;
             m < sizeof(k_match_table)/sizeof(k_match_table[0]); m++) {
            if (d->vendor_id  == k_match_table[m].vid &&
                d->product_id == k_match_table[m].pid)
                return d;
        }
    }
    return 0;
}

/* ─── power-on sequence ─────────────────────────────────────────── */

/* Bring the chip up to the point where we can write to the firmware
 * download window. This is the Realtek "_LLT_table_init" preamble, of
 * which we do the minimum to enable LDOs and clear the MCU.
 *
 * Returns 0 on success, <0 on transport failure. The 8188EU spec is
 * forgiving about register write order in this section; what matters
 * is that REG_APS_FSMCO has the power-on bits clear before we write
 * to REG_MCUFWDL.
 */
static int rtl_power_on(struct xhci_device *dev)
{
    /* Enable LDO and core regulators (best-effort: failures here are
     * common on cold-attached chips that already have power applied). */
    rtl_write8(dev, REG_LDOV12D_CTRL, 0x03);
    rtl_write8(dev, REG_LDOA15_CTRL,  0x0F);
    timer_wait_ms(1);

    /* Clear ISO so the MAC domain talks to the analog domain. */
    uint8_t iso = rtl_read8(dev, REG_SYS_ISO_CTRL);
    rtl_write8(dev, REG_SYS_ISO_CTRL, iso & ~0x10);
    timer_wait_ms(1);

    /* APS_FSMCO: kick off the power-on FSM. We set LDR2RPWM to drive
     * the chip into RDY before clearing FUNC_EN bits we'll re-set. */
    rtl_write16(dev, REG_APS_FSMCO, 0x0812);
    timer_wait_ms(1);

    /* SYS_FUNC_EN: enable MAC, BB, RF clocks. */
    rtl_write16(dev, REG_SYS_FUNC_EN, 0xFFFF);
    timer_wait_ms(1);

    /* SYS_CLKR: enable MAC clock. */
    rtl_write16(dev, REG_SYS_CLKR, 0x1F1F);
    timer_wait_ms(1);

    return 0;
}

/* Reset the on-chip 8051 microcontroller: pulse MCUFWDL_RAM_DL_SEL
 * and clear any stale FWDL_RDY/CHKSUM bits before we ship the blob. */
static int rtl_reset_mcu(struct xhci_device *dev)
{
    uint8_t v = rtl_read8(dev, REG_MCUFWDL);
    /* Set RAM_DL_SEL (bit 7) to put the firmware download mux into
     * RAM mode. The 8051 holds in reset while this bit is set. */
    rtl_write8(dev, REG_MCUFWDL, v | MCUFWDL_RAM_DL_SEL);
    timer_wait_ms(1);
    /* Clear it: 8051 will re-enter reset and wait for download. */
    rtl_write8(dev, REG_MCUFWDL, v & ~(MCUFWDL_RAM_DL_SEL |
                                       MCUFWDL_RDY |
                                       FWDL_CHKSUM_RPT));
    timer_wait_ms(1);
    /* Enable FWDL window. */
    rtl_write8(dev, REG_MCUFWDL, MCUFWDL_EN);
    return 0;
}

/* Select 4 KiB page index in REG_MCUFWDL byte 1. */
static int rtl_set_fwdl_page(struct xhci_device *dev, int page)
{
    /* Read low byte, OR in the new page index into the high nibble. */
    uint8_t lo = rtl_read8(dev, REG_MCUFWDL);
    lo |= MCUFWDL_EN;
    rtl_write8(dev, REG_MCUFWDL, lo);
    /* Page bits live in REG_MCUFWDL+1, bits[2:0]. */
    uint8_t hi = (uint8_t)(page & 0x07);
    return rtl_write8(dev, (uint16_t)(REG_MCUFWDL + 1), hi);
}

/* Stream one 4 KiB page of firmware to the chip, in 64-byte chunks
 * (USB control transfer max payload for vendor writes on this chip).
 *
 * Returns bytes written on success, <0 on failure.
 */
static int rtl_write_fw_page(struct xhci_device *dev, int page,
                             const unsigned char *buf, int len)
{
    if (rtl_set_fwdl_page(dev, page) < 0) return -1;

    int written = 0;
    uint16_t addr = REG_FW_START_ADDRESS;
    /* 8 bytes per descriptor write keeps every transfer comfortably
     * under the 64-byte vendor-control ceiling, matches the Realtek
     * driver's "_FWDownload" loop, and avoids alignment surprises on
     * the embedded MCU's SRAM bus. */
    const int chunk = 8;
    while (written < len) {
        int n = len - written;
        if (n > chunk) n = chunk;
        if (rtl_write_reg_n(dev, addr, buf + written, n) < 0)
            return -1;
        written += n;
        addr   += (uint16_t)n;
        /* Each 4 KiB page resets addr back to REG_FW_START_ADDRESS via
         * the page selector, so we must not roll past 0x1FFF here. */
        if (addr >= REG_FW_START_ADDRESS + 0x1000) break;
    }
    return written;
}

/* Wait for the chip to report firmware-ready. Bit FWDL_CHKSUM_RPT
 * (0x04) confirms the chip checksummed what it received, and
 * MCUFWDL_RDY (0x02) confirms the 8051 is running.
 *
 * Returns 0 on success, -1 on timeout.
 */
static int rtl_wait_fw_ready(struct xhci_device *dev)
{
    /* Final FWDL byte must signal CHKSUM_RPT and the MCU must clear
     * RAM_DL_SEL before raising RDY. We give it 1 second total. */
    for (int i = 0; i < 1000; i++) {
        uint8_t v = rtl_read8(dev, REG_MCUFWDL);
        if ((v & FWDL_CHKSUM_RPT) && (v & MCUFWDL_RDY))
            return 0;
        timer_wait_ms(1);
    }
    return -1;
}

/* ─── public API: full firmware load ────────────────────────────── */

static int rtl_load_firmware(struct xhci_device *dev)
{
    unsigned long fw_size =
        (unsigned long)(_binary_rtl8188eufw_bin_end -
                        _binary_rtl8188eufw_bin_start);
    if (fw_size < 64) {
        kputs("[rtl8188eu] firmware blob too small (");
        kput_dec(fw_size);
        kputs(" bytes)\n");
        return -1;
    }

    /* The 32-byte header is metadata for the host driver, NOT for the
     * 8051. Strip it before shipping payload bytes to the chip. */
    const unsigned char *payload = _binary_rtl8188eufw_bin_start + 32;
    unsigned long       payload_len = fw_size - 32;

    kputs("[rtl8188eu] firmware blob: ");
    kput_dec(fw_size);
    kputs(" bytes total, payload ");
    kput_dec(payload_len);
    kputs(" bytes (header stripped)\n");

    if (rtl_power_on(dev) < 0) {
        kputs("[rtl8188eu] power-on sequence failed\n");
        return -1;
    }
    if (rtl_reset_mcu(dev) < 0) {
        kputs("[rtl8188eu] MCU reset failed\n");
        return -1;
    }

    /* Stream payload page by page. */
    unsigned long off = 0;
    int page = 0;
    while (off < payload_len) {
        unsigned long n = payload_len - off;
        if (n > 4096) n = 4096;
        int w = rtl_write_fw_page(dev, page, payload + off, (int)n);
        if (w < 0) {
            kputs("[rtl8188eu] FWDL failed at page ");
            kput_dec(page);
            kputs("\n");
            return -1;
        }
        off += n;
        page++;
    }

    /* Tell the chip we're done. Clear FWDL_EN, the 8051 takes over. */
    uint8_t v = rtl_read8(dev, REG_MCUFWDL);
    rtl_write8(dev, REG_MCUFWDL, v & ~MCUFWDL_EN);

    if (rtl_wait_fw_ready(dev) < 0) {
        kputs("[rtl8188eu] firmware did not signal FWDL_RDY\n");
        return -1;
    }

    kputs("[rtl8188eu] firmware ready (FWDL_CHKSUM_OK + FWDL_RDY)\n");
    return 0;
}

/* ─── MAC + RF init (best-effort) ───────────────────────────────── */

static int rtl_read_mac(struct xhci_device *dev)
{
    uint8_t mac[6] = {0};
    for (int i = 0; i < 6; i++)
        mac[i] = rtl_read8(dev, (uint16_t)(REG_MACID + i));
    /* Trust the read only if it looks like a real MAC (not all-zero,
     * not all-FF). */
    int allz = 1, allf = 1;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) allz = 0;
        if (mac[i] != 0xFF) allf = 0;
    }
    if (allz || allf) return -1;
    memcpy(g_rtl8188eu.mac, mac, 6);
    g_rtl8188eu.mac_known = 1;
    return 0;
}

/* ─── probe ─────────────────────────────────────────────────────── */

int rtl8188eu_probe(void)
{
    memset(&g_rtl8188eu, 0, sizeof(g_rtl8188eu));

    struct xhci_device *dev = find_rtl8188eu_device();
    if (!dev) return -1;

    g_rtl8188eu.present     = 1;
    g_rtl8188eu.vendor_id   = dev->vendor_id;
    g_rtl8188eu.product_id  = dev->product_id;
    g_rtl8188eu.dev         = dev;

    kputs("[rtl8188eu] detected on USB bus, VID=");
    kput_hex(dev->vendor_id);
    kputs(" PID=");
    kput_hex(dev->product_id);
    kputs("\n");

    /* Verify the chip ACKs control reads before we start writing. */
    uint16_t v = rtl_read16(dev, REG_SYS_FUNC_EN);
    g_rtl8188eu.reg_sys_func_en = v;
    g_rtl8188eu.chip_alive      = 1;
    kputs("[rtl8188eu] chip alive, SYS_FUNC_EN=0x");
    kput_hex(v);
    kputs("\n");

    /* Stream the embedded firmware blob and wait for ready. */
    if (rtl_load_firmware(dev) == 0) {
        g_rtl8188eu.fw_loaded = 1;
        if (rtl_read_mac(dev) == 0) {
            kputs("[rtl8188eu] MAC=");
            for (int i = 0; i < 6; i++) {
                kput_hex(g_rtl8188eu.mac[i]);
                if (i < 5) kputs(":");
            }
            kputs("\n");
        } else {
            kputs("[rtl8188eu] MAC read returned implausible bytes; "
                  "EFUSE may need re-init\n");
        }
    } else {
        kputs("[rtl8188eu] firmware load failed; passive scan disabled\n");
    }

    return 0;
}

/* ─── status ────────────────────────────────────────────────────── */

void rtl8188eu_print_status(void)
{
    kputs("\n  RTL8188EU USB WiFi:\n\n");

    if (!g_rtl8188eu.present) {
        kputs("    Status:    not detected\n");
        kputs("    (Plug a Realtek RTL8188EU dongle into a USB port "
              "and reboot.\n");
        kputs("     Common product IDs: 0x0BDA:0x8179, 0x0BDA:0xFFEF, "
              "0x0BDA:0x0179.)\n\n");
        return;
    }

    kputs("    Status:    detected\n");
    kputs("    USB ID:    ");
    kput_hex(g_rtl8188eu.vendor_id);
    kputs(":");
    kput_hex(g_rtl8188eu.product_id);
    kputs("\n    Chip:      ");
    if (g_rtl8188eu.chip_alive) {
        kputs("alive (responded to vendor register read)\n");
        kputs("    SYS_FUNC_EN: 0x");
        kput_hex(g_rtl8188eu.reg_sys_func_en);
        kputs("\n");
    } else {
        kputs("unresponsive\n");
    }

    kputs("    Firmware:  ");
    kputs(g_rtl8188eu.fw_loaded ? "loaded (FWDL_RDY)\n"
                                : "NOT loaded\n");

    kputs("    MAC:       ");
    if (g_rtl8188eu.mac_known) {
        for (int i = 0; i < 6; i++) {
            kput_hex(g_rtl8188eu.mac[i]);
            if (i < 5) kputs(":");
        }
        kputs("\n");
    } else {
        kputs("unknown\n");
    }

    kputs("    Scan:      ");
    kputs(g_rtl8188eu.fw_loaded ? "passive only (use `wifi-scan`)\n"
                                : "unavailable (firmware not loaded)\n");
    kputs("    Connect:   unavailable (association/WPA2 not implemented)\n\n");
}

/* ─── passive scan ──────────────────────────────────────────────── */

/* Passive scan: park on each channel for ~120 ms, listen for any
 * beacon frame, and pull SSIDs out of the IE list.
 *
 * The bulk RX path on this chip is EP1 IN. xhci_bulk_poll_in returns
 * however many bytes it got (or 0 on timeout). Each frame on the wire
 * is prefixed by an 8-byte RX descriptor (RxPktSize at byte 0..1 plus
 * RxDrvInfoSize at byte 1).
 */
#define RTL_RX_BULK_EP_IN   0x81

struct rtl_scan_entry {
    uint8_t  bssid[6];
    char     ssid[33];
    int      ssid_len;
    int      channel;
    int      rssi_seen;   /* 0/1 — we saw at least one beacon */
};

#define RTL_SCAN_MAX 16
static struct rtl_scan_entry s_scan[RTL_SCAN_MAX];
static int s_scan_n;

static void scan_record(const uint8_t bssid[6], const char *ssid,
                        int ssid_len, int channel)
{
    /* Dedupe by BSSID. */
    for (int i = 0; i < s_scan_n; i++) {
        int same = 1;
        for (int b = 0; b < 6; b++)
            if (s_scan[i].bssid[b] != bssid[b]) { same = 0; break; }
        if (same) {
            s_scan[i].rssi_seen = 1;
            return;
        }
    }
    if (s_scan_n >= RTL_SCAN_MAX) return;
    struct rtl_scan_entry *e = &s_scan[s_scan_n++];
    memcpy(e->bssid, bssid, 6);
    if (ssid_len < 0) ssid_len = 0;
    if (ssid_len > 32) ssid_len = 32;
    memcpy(e->ssid, ssid, ssid_len);
    e->ssid[ssid_len] = '\0';
    e->ssid_len  = ssid_len;
    e->channel   = channel;
    e->rssi_seen = 1;
}

/* Walk a beacon frame's IE list and pull SSID (tag 0). Returns 1 if
 * a beacon was decoded, 0 if buf wasn't a recognisable beacon. */
static int parse_beacon(const uint8_t *buf, int len, int channel)
{
    /* Skip RX descriptor (24 bytes on rtl8188eu — RxDesc8188EU). */
    if (len < 24 + 24 + 12) return 0;
    const uint8_t *frame = buf + 24;
    int frame_len = len - 24;

    /* 802.11 management frame header is 24 bytes; beacon subtype is
     * 0x80 in the first byte (Frame Control). */
    if ((frame[0] & 0xFC) != 0x80) return 0;

    const uint8_t *bssid = &frame[16];
    /* Fixed beacon body: timestamp(8) + interval(2) + capabilities(2) */
    const uint8_t *ies = frame + 24 + 12;
    int ies_len = frame_len - 24 - 12;
    if (ies_len < 2) return 0;

    /* Walk tagged parameters. */
    const char *ssid = "";
    int ssid_len = 0;
    int i = 0;
    while (i + 2 <= ies_len) {
        uint8_t tag = ies[i];
        uint8_t tlen = ies[i + 1];
        if (i + 2 + tlen > ies_len) break;
        if (tag == 0x00) {  /* SSID */
            ssid = (const char *)&ies[i + 2];
            ssid_len = tlen;
            break;
        }
        i += 2 + tlen;
    }
    scan_record(bssid, ssid, ssid_len, channel);
    return 1;
}

/* Configure RX path for passive listen on `channel`. The 8188EU
 * channel register is REG_CHANNEL_CONTROL (0x040A); we write the
 * channel number plus the bandwidth bits (HT20 here). */
#define REG_CHANNEL_CONTROL 0x040A

static void rtl_set_channel(struct xhci_device *dev, int ch)
{
    /* Low byte: channel (1-13). High byte unused for HT20. */
    rtl_write8(dev, REG_CHANNEL_CONTROL, (uint8_t)ch);
}

int rtl8188eu_scan(void)
{
    if (!g_rtl8188eu.present) {
        kputs("wifi: no RTL8188EU dongle detected.\n");
        return -1;
    }
    if (!g_rtl8188eu.fw_loaded) {
        kputs("wifi: firmware not loaded; cannot scan.\n");
        return -1;
    }

    s_scan_n = 0;

    static uint8_t rxbuf[2048];

    /* Sweep 2.4 GHz channels 1..11 (US/global subset). 120 ms dwell. */
    for (int ch = 1; ch <= 11; ch++) {
        rtl_set_channel(g_rtl8188eu.dev, ch);
        timer_wait_ms(5);
        /* Drain whatever beacons arrived in the dwell window. */
        for (int t = 0; t < 12; t++) {
            int got = xhci_bulk_poll_in(g_rtl8188eu.dev,
                                        RTL_RX_BULK_EP_IN,
                                        rxbuf, sizeof(rxbuf));
            if (got > 0)
                parse_beacon(rxbuf, got, ch);
            timer_wait_ms(10);
        }
    }

    if (s_scan_n == 0) {
        kputs("wifi: passive scan complete, no beacons seen.\n");
        kputs("      (RX bulk path may need EP setup; this driver "
              "does not yet configure EP1-IN.)\n");
        return 0;
    }

    kputs("wifi: passive scan results:\n");
    for (int i = 0; i < s_scan_n; i++) {
        kputs("  ");
        for (int b = 0; b < 6; b++) {
            kput_hex(s_scan[i].bssid[b]);
            if (b < 5) kputs(":");
        }
        kputs("  ch=");
        kput_dec((uint32_t)s_scan[i].channel);
        kputs("  ");
        kputs(s_scan[i].ssid_len ? s_scan[i].ssid : "<hidden>");
        kputs("\n");
    }
    return 0;
}

int rtl8188eu_connect(const char *ssid, const char *psk)
{
    (void)ssid; (void)psk;
    kputs("wifi: connect unavailable (association/WPA2 = future work).\n");
    return -1;
}
