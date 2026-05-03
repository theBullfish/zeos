/*
 * Zeos -- USB Mass Storage Class (Bulk-Only Transport) driver
 *
 * Implements MSC BBB on top of the xHCI bulk endpoint API. Speaks SCSI
 * transparent (subclass 0x06, protocol 0x50). Single-LUN, single-device.
 *
 * Spec refs:
 *  - "USB Mass Storage Class — Bulk-Only Transport", rev 1.0, 1999-09-31
 *  - SBC-3 (SCSI Block Commands)
 *  - SPC-4 (SCSI Primary Commands)
 */

#include "usb_msc.h"
#include "usb_xhci.h"
#include "usb.h"
#include "kprint.h"

extern void *memset(void *s, int c, unsigned long n);
extern void *memcpy(void *dst, const void *src, unsigned long n);

/* ── BBB CBW / CSW ──────────────────────────────────────────────── */
#define CBW_SIGNATURE       0x43425355U  /* "USBC" little-endian */
#define CSW_SIGNATURE       0x53425355U  /* "USBS" little-endian */

#define CBW_FLAG_DATA_IN    0x80
#define CBW_FLAG_DATA_OUT   0x00

struct cbw {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;     /* 1..16 */
    uint8_t  CBWCB[16];
} __attribute__((packed));

struct csw {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;        /* 0=passed, 1=failed, 2=phase err */
} __attribute__((packed));

/* ── MSC class-specific control requests ────────────────────────── */
#define MSC_REQ_RESET           0xFF  /* Bulk-Only Mass Storage Reset */
#define MSC_REQ_GET_MAX_LUN     0xFE

/* ── SCSI op codes ──────────────────────────────────────────────── */
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2A

/* ── Driver state ───────────────────────────────────────────────── */
typedef struct {
    int                  ready;
    struct xhci_device  *dev;
    uint8_t              ifnum;       /* bInterfaceNumber */
    uint8_t              ep_in;       /* bulk-IN address (0x80 | n) */
    uint8_t              ep_out;      /* bulk-OUT address */
    uint16_t             mps_in;
    uint16_t             mps_out;
    uint8_t              max_lun;
    uint64_t             sectors;
    uint32_t             block_size;
    uint32_t             tag;
} msc_t;

static msc_t msc;

/* ── Helpers ────────────────────────────────────────────────────── */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* ── Configuration descriptor walker ────────────────────────────── */
/*
 * Find an MSC BBB interface (class 0x08, sub 0x06, proto 0x50) in the
 * configuration descriptor blob. Records interface number, bulk IN +
 * bulk OUT endpoint addresses, and their wMaxPacketSize.
 *
 * Returns 0 on success, -1 on not-found.
 */
static int parse_msc_interface(const uint8_t *cfg, int len,
                               uint8_t *out_if, uint8_t *out_alt,
                               uint8_t *out_ep_in, uint8_t *out_ep_out,
                               uint16_t *out_mps_in, uint16_t *out_mps_out)
{
    int i = 0;
    int in_msc_if = 0;
    uint8_t cur_if = 0xFF, cur_alt = 0;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t mps_in = 0, mps_out = 0;
    while (i + 2 <= len) {
        uint8_t blen = cfg[i];
        uint8_t btype = cfg[i + 1];
        if (blen < 2 || i + blen > len) break;

        if (btype == USB_DT_INTERFACE && blen >= 9) {
            uint8_t ifnum   = cfg[i + 2];
            uint8_t altset  = cfg[i + 3];
            uint8_t cls     = cfg[i + 5];
            uint8_t sub     = cfg[i + 6];
            uint8_t proto   = cfg[i + 7];
            if (cls == 0x08 && sub == 0x06 && proto == 0x50) {
                in_msc_if = 1;
                cur_if = ifnum;
                cur_alt = altset;
                ep_in = ep_out = 0;
                mps_in = mps_out = 0;
            } else {
                /* If we already collected both endpoints from a prior MSC
                 * IF, accept it. */
                if (in_msc_if && ep_in && ep_out) {
                    *out_if = cur_if; *out_alt = cur_alt;
                    *out_ep_in = ep_in; *out_ep_out = ep_out;
                    *out_mps_in = mps_in; *out_mps_out = mps_out;
                    return 0;
                }
                in_msc_if = 0;
            }
        } else if (btype == USB_DT_ENDPOINT && blen >= 7 && in_msc_if) {
            uint8_t addr  = cfg[i + 2];
            uint8_t attr  = cfg[i + 3];
            uint16_t mps  = (uint16_t)cfg[i + 4] | ((uint16_t)cfg[i + 5] << 8);
            mps &= 0x07FF;  /* low 11 bits */
            if ((attr & 0x03) == 0x02) {  /* Bulk */
                if (addr & 0x80) { ep_in = addr; mps_in = mps; }
                else             { ep_out = addr; mps_out = mps; }
            }
        }
        i += blen;
    }
    if (in_msc_if && ep_in && ep_out) {
        *out_if = cur_if; *out_alt = cur_alt;
        *out_ep_in = ep_in; *out_ep_out = ep_out;
        *out_mps_in = mps_in; *out_mps_out = mps_out;
        return 0;
    }
    return -1;
}

/* ── BBB transfer engine ────────────────────────────────────────── */
/*
 * Perform a full BBB transaction:
 *   1. Send 31-byte CBW on bulk-OUT
 *   2. (If data) bulk-IN/OUT for `data_len` bytes
 *   3. Receive 13-byte CSW on bulk-IN
 *
 * Returns 0 on CSW status 0 (passed), -1 otherwise. Also returns -1 if
 * any phase fails. data_len is the transfer length the device should
 * produce/consume.
 */
static int bbb_xfer(const uint8_t *cb, int cb_len,
                    uint8_t lun, int data_len, int data_in,
                    void *data, int *out_residue)
{
    if (!msc.ready && msc.dev == 0) return -1;
    if (cb_len < 1 || cb_len > 16) return -1;

    struct cbw cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = CBW_SIGNATURE;
    cbw.dCBWTag = ++msc.tag;
    cbw.dCBWDataTransferLength = (uint32_t)data_len;
    cbw.bmCBWFlags = data_in ? CBW_FLAG_DATA_IN : CBW_FLAG_DATA_OUT;
    cbw.bCBWLUN = lun;
    cbw.bCBWCBLength = (uint8_t)cb_len;
    for (int i = 0; i < cb_len; i++) cbw.CBWCB[i] = cb[i];

    int sent = xhci_bulk_transfer(msc.dev, msc.ep_out, &cbw, 31, 0);
    if (sent != 31) {
        kputs("[usb-msc] CBW send failed sent=");
        kput_dec(sent < 0 ? 0 : (uint32_t)sent);
        kputs("\n");
        return -1;
    }

    int data_xferred = 0;
    if (data_len > 0 && data) {
        int got = xhci_bulk_transfer(msc.dev,
                                     data_in ? msc.ep_in : msc.ep_out,
                                     data, data_len, data_in ? 1 : 0);
        if (got < 0) {
            kputs("[usb-msc] data phase failed dir=");
            kputs(data_in ? "IN\n" : "OUT\n");
            return -1;
        }
        data_xferred = got;
    }

    struct csw csw;
    memset(&csw, 0, sizeof(csw));
    int csw_got = xhci_bulk_transfer(msc.dev, msc.ep_in, &csw, 13, 1);
    if (csw_got != 13) {
        kputs("[usb-msc] CSW recv failed got=");
        kput_dec(csw_got < 0 ? 0 : (uint32_t)csw_got);
        kputs("\n");
        return -1;
    }
    if (csw.dCSWSignature != CSW_SIGNATURE) {
        kputs("[usb-msc] bad CSW signature\n");
        return -1;
    }
    if (csw.dCSWTag != cbw.dCBWTag) {
        kputs("[usb-msc] CSW tag mismatch\n");
        return -1;
    }
    if (out_residue) *out_residue = (int)csw.dCSWDataResidue;
    (void)data_xferred;
    if (csw.bCSWStatus != 0) {
        return -1;
    }
    return 0;
}

/* ── Class-specific Reset / GET_MAX_LUN ─────────────────────────── */
static int msc_get_max_lun(uint8_t *out_max)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    sp.bRequest = MSC_REQ_GET_MAX_LUN;
    sp.wValue = 0;
    sp.wIndex = msc.ifnum;
    sp.wLength = 1;
    uint8_t v = 0;
    int r = xhci_control_transfer(msc.dev, &sp, &v, 1);
    if (r < 0) {
        /* Many sticks STALL this — spec says treat as max_lun=0. */
        *out_max = 0;
        return 0;
    }
    *out_max = v;
    return 0;
}

/* ── SCSI primitives ────────────────────────────────────────────── */
static int scsi_test_unit_ready(void)
{
    uint8_t cb[6];
    memset(cb, 0, sizeof(cb));
    cb[0] = SCSI_TEST_UNIT_READY;
    return bbb_xfer(cb, 6, 0, 0, 0, 0, 0);
}

static int scsi_request_sense(uint8_t *sense, int len)
{
    if (len > 18) len = 18;
    uint8_t cb[6];
    memset(cb, 0, sizeof(cb));
    cb[0] = SCSI_REQUEST_SENSE;
    cb[4] = (uint8_t)len;
    return bbb_xfer(cb, 6, 0, len, 1, sense, 0);
}

static int scsi_inquiry(uint8_t *out, int len)
{
    if (len > 36) len = 36;
    uint8_t cb[6];
    memset(cb, 0, sizeof(cb));
    cb[0] = SCSI_INQUIRY;
    cb[4] = (uint8_t)len;
    return bbb_xfer(cb, 6, 0, len, 1, out, 0);
}

static int scsi_read_capacity10(uint32_t *out_last_lba, uint32_t *out_block_size)
{
    uint8_t cb[10];
    memset(cb, 0, sizeof(cb));
    cb[0] = SCSI_READ_CAPACITY_10;
    uint8_t buf[8];
    memset(buf, 0, sizeof(buf));
    int r = bbb_xfer(cb, 10, 0, 8, 1, buf, 0);
    if (r != 0) return -1;
    *out_last_lba   = be32(buf);
    *out_block_size = be32(buf + 4);
    return 0;
}

static int scsi_read10(uint32_t lba, uint16_t count, void *buf)
{
    uint8_t cb[10];
    memset(cb, 0, sizeof(cb));
    cb[0] = SCSI_READ_10;
    put_be32(cb + 2, lba);
    put_be16(cb + 7, count);
    int data_len = (int)count * (int)msc.block_size;
    return bbb_xfer(cb, 10, 0, data_len, 1, buf, 0);
}

static int scsi_write10(uint32_t lba, uint16_t count, const void *buf)
{
    uint8_t cb[10];
    memset(cb, 0, sizeof(cb));
    cb[0] = SCSI_WRITE_10;
    put_be32(cb + 2, lba);
    put_be16(cb + 7, count);
    int data_len = (int)count * (int)msc.block_size;
    return bbb_xfer(cb, 10, 0, data_len, 0, (void *)buf, 0);
}

/* ── Probe ──────────────────────────────────────────────────────── */
int usb_msc_init(void)
{
    msc.ready = 0;
    msc.dev = 0;

    int ndev = xhci_device_count();
    for (int i = 0; i < ndev; i++) {
        struct xhci_device *d = xhci_get_device(i);
        if (!d || d->slot_id == 0) continue;

        /* Read the configuration descriptor blob. */
        uint8_t cfg[512];
        int got = xhci_get_config_descriptor(d, cfg, sizeof(cfg));
        if (got < 9) continue;

        uint8_t ifnum, alt, ep_in, ep_out;
        uint16_t mps_in, mps_out;
        if (parse_msc_interface(cfg, got, &ifnum, &alt, &ep_in, &ep_out,
                                &mps_in, &mps_out) != 0)
            continue;

        kputs("[usb-msc] candidate VID=");
        kput_hex(d->vendor_id);
        kputs(" PID=");
        kput_hex(d->product_id);
        kputs(" if=");
        kput_dec(ifnum);
        kputs(" ep_in=");
        kput_hex(ep_in);
        kputs(" ep_out=");
        kput_hex(ep_out);
        kputs(" mps=");
        kput_dec(mps_in);
        kputs("\n");

        /* SET_CONFIGURATION (use bConfigurationValue from cfg[5]). */
        uint8_t cfg_value = cfg[5];
        if (xhci_set_configuration(d, cfg_value) != 0) {
            kputs("[usb-msc] SET_CONFIGURATION failed\n");
            continue;
        }
        d->configuration = cfg_value;

        /* Configure bulk endpoints. */
        if (xhci_setup_bulk_endpoint(d, ep_out, mps_out) != 0) {
            kputs("[usb-msc] bulk-OUT setup failed\n");
            continue;
        }
        if (xhci_setup_bulk_endpoint(d, ep_in, mps_in) != 0) {
            kputs("[usb-msc] bulk-IN setup failed\n");
            continue;
        }

        msc.dev = d;
        msc.ifnum = ifnum;
        msc.ep_in = ep_in;
        msc.ep_out = ep_out;
        msc.mps_in = mps_in;
        msc.mps_out = mps_out;
        msc.tag = 0;

        /* GET_MAX_LUN (best-effort) */
        msc_get_max_lun(&msc.max_lun);

        /* SCSI bring-up: TUR until ready (clear UNIT_ATTENTION), INQUIRY,
         * READ CAPACITY. Some devices fail TUR until REQUEST SENSE clears. */
        uint8_t inq[36];
        memset(inq, 0, sizeof(inq));
        if (scsi_inquiry(inq, 36) != 0) {
            uint8_t sns[18];
            scsi_request_sense(sns, 18);
            if (scsi_inquiry(inq, 36) != 0) {
                kputs("[usb-msc] INQUIRY failed\n");
                msc.dev = 0;
                continue;
            }
        }

        /* Try TEST UNIT READY a few times; clear UNIT ATTENTION via REQUEST SENSE. */
        int ready = 0;
        for (int t = 0; t < 5; t++) {
            if (scsi_test_unit_ready() == 0) { ready = 1; break; }
            uint8_t sns[18];
            scsi_request_sense(sns, 18);
        }
        if (!ready) {
            kputs("[usb-msc] not ready\n");
            msc.dev = 0;
            continue;
        }

        uint32_t last_lba = 0, blk = 0;
        if (scsi_read_capacity10(&last_lba, &blk) != 0) {
            kputs("[usb-msc] READ CAPACITY failed\n");
            msc.dev = 0;
            continue;
        }
        if (blk == 0 || (blk != 512 && blk != 1024 && blk != 2048 && blk != 4096)) {
            kputs("[usb-msc] odd block size=");
            kput_dec(blk);
            kputs("\n");
            msc.dev = 0;
            continue;
        }
        msc.block_size = blk;
        msc.sectors = (uint64_t)last_lba + 1;
        msc.ready = 1;

        kputs("usb-msc: detected, ");
        kput_dec(msc.sectors);
        kputs(" sectors\n");
        return 0;
    }

    return -1;
}

int usb_msc_ready(void) { return msc.ready; }

uint64_t usb_msc_sector_count(void) { return msc.ready ? msc.sectors : 0; }

uint32_t usb_msc_block_size(void) { return msc.ready ? msc.block_size : 0; }

int usb_msc_read(uint64_t lba, uint32_t count, void *buf)
{
    if (!msc.ready || !buf) return -1;
    if (lba + count > msc.sectors) return -1;
    /* Cap each READ(10) at 32 sectors to keep transfers bounded. */
    while (count > 0) {
        uint16_t chunk = count > 32 ? 32 : (uint16_t)count;
        if (scsi_read10((uint32_t)lba, chunk, buf) != 0) return -1;
        lba += chunk;
        count -= chunk;
        buf = (uint8_t *)buf + (uint32_t)chunk * msc.block_size;
    }
    return 0;
}

int usb_msc_write(uint64_t lba, uint32_t count, const void *buf)
{
    if (!msc.ready || !buf) return -1;
    if (lba + count > msc.sectors) return -1;
    while (count > 0) {
        uint16_t chunk = count > 32 ? 32 : (uint16_t)count;
        if (scsi_write10((uint32_t)lba, chunk, buf) != 0) return -1;
        lba += chunk;
        count -= chunk;
        buf = (const uint8_t *)buf + (uint32_t)chunk * msc.block_size;
    }
    return 0;
}

int usb_msc_flush(void) { return msc.ready ? 0 : -1; }
