/*
 * Zeos -- Realtek RTL8188EU USB WiFi driver (detection stage)
 *
 * See net_rtl8188eu.h for the full honest accounting of what works
 * and what doesn't. Short version:
 *   - We can find the dongle on the USB bus.
 *   - We can issue vendor control reads to its register file.
 *   - We cannot upload firmware (no blob), and the bulk endpoint
 *     path is not yet wired in usb_xhci.c, so we cannot tx/rx data.
 *   - Therefore: no scan, no connect, no DHCP. The driver refuses
 *     to fake any of those and tells the operator clearly.
 */

#include "net_rtl8188eu.h"
#include "usb_xhci.h"
#include "usb.h"
#include "kprint.h"

extern void *memset(void *s, int c, unsigned long n);
extern void *memcpy(void *dst, const void *src, unsigned long n);

struct rtl8188eu_state g_rtl8188eu;

/* RTL8188EU vendor request opcodes (from open-source rtl8188eus
 * driver / Realtek HAL headers). These are the bRequest values used
 * to read/write the chip's MMIO register file over USB control
 * transfers. They do NOT require firmware. */
#define RTW_USB_VENDOR_REQ_READ_REG   0x05
#define RTW_USB_VENDOR_REQ_WRITE_REG  0x05  /* same opcode; direction in bmRequestType */

/* A few register offsets we can poke without firmware running. The
 * SYS_FUNC_EN register at 0x0002 is power-domain enables; on a
 * cold-attached chip it returns a non-zero defaults value before
 * firmware boots. SYS_CFG at 0x00F0 returns chip ID nibbles. */
#define REG_SYS_FUNC_EN     0x0002
#define REG_SYS_CFG         0x00F0

/* Match table: (vid, pid) pairs we recognise as RTL8188EU. */
struct rtl_match { uint16_t vid; uint16_t pid; };
static const struct rtl_match k_match_table[] = {
    { RTL_VENDOR_ID, RTL8188EU_PID_8179 },
    { RTL_VENDOR_ID, RTL8188EU_PID_FFEF },
    { RTL_VENDOR_ID, RTL8188EU_PID_0179 },
};

/*
 * Read 'len' bytes (1, 2 or 4) from chip register 'addr' via a vendor
 * IN control transfer. Returns >=0 byte count on success, <0 on
 * failure.
 *
 * RTL8188EU vendor control read packet (from Realtek HAL):
 *   bmRequestType = 0xC0 (vendor | device | IN)
 *   bRequest      = 0x05
 *   wValue        = register offset (low 16 bits of address)
 *   wIndex        = 0  (page select; 0 for the main register file)
 *   wLength       = 1, 2 or 4
 *
 * This is the same scheme used by Linux's rtl8188eu / r8188eu
 * drivers and the open r8188eus tree.
 */
static int rtl_read_reg(struct xhci_device *dev, uint16_t addr, void *out, int len)
{
    if (len != 1 && len != 2 && len != 4) return -1;

    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE;
    sp.bRequest      = RTW_USB_VENDOR_REQ_READ_REG;
    sp.wValue        = addr;
    sp.wIndex        = 0;
    sp.wLength       = (uint16_t)len;

    return xhci_control_transfer(dev, &sp, out, len);
}

/* Locate an RTL8188EU on the USB bus. Returns the xhci_device* or
 * NULL. */
static struct xhci_device *find_rtl8188eu_device(void)
{
    int n = xhci_device_count();
    for (int i = 0; i < n; i++) {
        struct xhci_device *d = xhci_get_device(i);
        if (!d) continue;
        for (unsigned m = 0; m < sizeof(k_match_table)/sizeof(k_match_table[0]); m++) {
            if (d->vendor_id  == k_match_table[m].vid &&
                d->product_id == k_match_table[m].pid) {
                return d;
            }
        }
    }
    return 0;
}

int rtl8188eu_probe(void)
{
    memset(&g_rtl8188eu, 0, sizeof(g_rtl8188eu));

    struct xhci_device *dev = find_rtl8188eu_device();
    if (!dev) {
        /* No matching dongle -- not an error, just absent. */
        return -1;
    }

    g_rtl8188eu.present     = 1;
    g_rtl8188eu.vendor_id   = dev->vendor_id;
    g_rtl8188eu.product_id  = dev->product_id;

    kputs("[rtl8188eu] detected on USB bus, VID=");
    kput_hex(dev->vendor_id);
    kputs(" PID=");
    kput_hex(dev->product_id);
    kputs("\n");

    /* Try a vendor control read of SYS_FUNC_EN (0x0002, 2 bytes).
     * This proves the chip ACKs our control packets — i.e. the USB
     * link is up and the device's USB-to-MAC bridge is alive. We do
     * NOT use the value to fabricate state; we just log it. */
    uint16_t v = 0xFFFF;
    int got = rtl_read_reg(dev, REG_SYS_FUNC_EN, &v, 2);
    if (got == 2) {
        g_rtl8188eu.chip_alive       = 1;
        g_rtl8188eu.reg_sys_func_en  = v;
        kputs("[rtl8188eu] chip alive, SYS_FUNC_EN=0x");
        kput_hex(v);
        kputs("\n");
    } else {
        kputs("[rtl8188eu] chip did not respond to vendor read (got=");
        kput_dec(got < 0 ? 0 : (uint32_t)got);
        kputs("). Detection-only.\n");
    }

    /* MAC read deliberately omitted. EFUSE access on RTL8188EU needs
     * the LDO/clock/EFUSE-burst init sequence which is firmware-gated
     * on production silicon. Without rtl8188eufw.bin we don't have
     * a reliable path. Reporting the truth: */
    kputs("[rtl8188eu] MAC unknown (firmware required to unlock EFUSE; "
          "drop rtl8188eufw.bin into kernel/lib/firmware/)\n");
    kputs("[rtl8188eu] scan/connect unavailable: needs firmware blob "
          "AND xhci bulk-endpoint support (xhci_bulk_transfer is -1)\n");

    return 0;
}

void rtl8188eu_print_status(void)
{
    kputs("\n  RTL8188EU USB WiFi:\n\n");

    if (!g_rtl8188eu.present) {
        kputs("    Status:    not detected\n");
        kputs("    (Plug a Realtek RTL8188EU dongle into a USB port and reboot.\n");
        kputs("     Common product IDs: 0x0BDA:0x8179, 0x0BDA:0xFFEF, 0x0BDA:0x0179.)\n\n");
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
        kputs("unresponsive (USB enumerated but vendor read failed)\n");
    }

    kputs("    MAC:       unknown\n");
    kputs("    Firmware:  not loaded (rtl8188eufw.bin not in kernel/lib/firmware/)\n");
    kputs("    Bulk EP:   not yet supported by xhci driver\n");
    kputs("    Scan:      unavailable\n");
    kputs("    Connect:   unavailable\n\n");
    kputs("    What's missing to bring this up to a working link:\n");
    kputs("      1. Embed rtl8188eufw.bin (~32 KB) in the kernel image.\n");
    kputs("      2. Implement xhci_bulk_transfer + endpoint-context setup\n");
    kputs("         in usb_xhci.c so we can move data on EP1/EP2/EP3.\n");
    kputs("      3. Run the firmware-download sequence (vendor write\n");
    kputs("         REQ=0x05 to the FWDL register block, then poll\n");
    kputs("         REG_MCUFWDL for the FWDL_RDY bit).\n");
    kputs("      4. Read MAC from EFUSE offsets 0xD0..0xD5.\n");
    kputs("      5. Add net_rtl8188eu_send/recv to the net.c dispatch.\n");
    kputs("      6. WPA2: HMAC-SHA1 + AES + 4-way handshake (mbedtls\n");
    kputs("         provides the primitives; the state machine is new).\n\n");
}

int rtl8188eu_scan(void)
{
    if (!g_rtl8188eu.present) {
        kputs("wifi: no RTL8188EU dongle detected.\n");
        return -1;
    }
    kputs("wifi: scan unavailable -- firmware not loaded and bulk "
          "endpoints not supported. See `wifi status` for details.\n");
    return -1;
}

int rtl8188eu_connect(const char *ssid, const char *psk)
{
    (void)ssid; (void)psk;
    if (!g_rtl8188eu.present) {
        kputs("wifi: no RTL8188EU dongle detected.\n");
        return -1;
    }
    kputs("wifi: connect unavailable -- firmware not loaded and bulk "
          "endpoints not supported. See `wifi status` for details.\n");
    return -1;
}
