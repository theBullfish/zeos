/*
 * Zeos -- EHCI 1.0 Host Controller Driver (polling)
 *
 * Real, no-stub minimal EHCI driver for legacy systems with no xHCI.
 *
 *   - PCI discovery (class 0x0C / sub 0x03 / prog-if 0x20)
 *   - BIOS handoff via USBLEGSUP extended capability (if present)
 *   - HCRESET via USBCMD, wait for clear
 *   - Periodic Frame List (1024 entries, 4 KiB aligned)
 *   - Async List Head + dummy Queue Head
 *   - USBINTR=0 (polling), CONFIGFLAG=1 (own ports)
 *   - Per-port reset (50 ms PR, then clear, wait for PED)
 *   - Address Device via SET_ADDRESS standard control transfer through a
 *     dummy queue head on the asynchronous list
 *   - Control transfers (SETUP / DATA / STATUS via QH + qTDs)
 *   - Bulk transfers (qTDs on per-endpoint bulk Queue Heads on async list)
 *   - Interrupt-IN endpoints via Periodic Frame List entries
 *
 * Reference: EHCI 1.0 spec (Intel "Enhanced Host Controller Interface
 * Specification for Universal Serial Bus", Rev 1.0, 2002), Sections 2-4.
 *
 * NOTE: handles only high-speed (USB 2.0) devices on the EHCI itself.
 * Low/full-speed devices on companion controllers (UHCI/OHCI) get released
 * back to those companion HCs by the BIOS routing logic when the EHCI
 * detects a non-HS device on a port; we don't drive UHCI/OHCI here.
 * QEMU's `-device usb-mouse` is high-speed when attached to usb-ehci.
 */

#include "usb_ehci.h"
#include "usb_xhci.h"
#include "usb.h"
#include "pci.h"
#include "pmm.h"
#include "heap.h"
#include "kprint.h"
#include "vmm.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);

/* ── Capability registers (relative to BAR0) ─────────────────── */
#define EHCI_CAP_CAPLENGTH      0x00
#define EHCI_CAP_HCIVERSION     0x02
#define EHCI_CAP_HCSPARAMS      0x04
#define EHCI_CAP_HCCPARAMS      0x08

/* ── Operational registers (relative to op base = BAR0 + CAPLENGTH) ── */
#define EHCI_OP_USBCMD          0x00
#define EHCI_OP_USBSTS          0x04
#define EHCI_OP_USBINTR         0x08
#define EHCI_OP_FRINDEX         0x0C
#define EHCI_OP_CTRLDSSEGMENT   0x10
#define EHCI_OP_PERIODICLISTBASE 0x14
#define EHCI_OP_ASYNCLISTADDR   0x18
#define EHCI_OP_CONFIGFLAG      0x40
#define EHCI_OP_PORTSC(n)       (0x44 + 4 * (n))

/* USBCMD bits */
#define USBCMD_RS               (1U << 0)
#define USBCMD_HCRESET          (1U << 1)
#define USBCMD_PSE              (1U << 4)   /* Periodic Schedule Enable */
#define USBCMD_ASE              (1U << 5)   /* Async Schedule Enable */
#define USBCMD_IAA              (1U << 6)   /* Interrupt on Async Advance Doorbell */

/* USBSTS bits */
#define USBSTS_USBINT           (1U << 0)
#define USBSTS_ERRINT           (1U << 1)
#define USBSTS_PCD              (1U << 2)
#define USBSTS_FLR              (1U << 3)
#define USBSTS_HSE              (1U << 4)
#define USBSTS_IAA              (1U << 5)
#define USBSTS_HCH              (1U << 12)
#define USBSTS_PSS              (1U << 14)
#define USBSTS_ASS              (1U << 15)

/* PORTSC bits */
#define PORTSC_CCS              (1U << 0)   /* Current Connect Status */
#define PORTSC_CSC              (1U << 1)   /* Connect Status Change (RWC) */
#define PORTSC_PED              (1U << 2)   /* Port Enabled */
#define PORTSC_PEDC             (1U << 3)   /* PED Change (RWC) */
#define PORTSC_OCC              (1U << 4)
#define PORTSC_OCCC             (1U << 5)
#define PORTSC_FPR              (1U << 6)
#define PORTSC_SUSPEND          (1U << 7)
#define PORTSC_PR               (1U << 8)   /* Port Reset */
#define PORTSC_LS_MASK          (3U << 10)  /* Line status */
#define PORTSC_PP               (1U << 12)  /* Port Power */
#define PORTSC_PO               (1U << 13)  /* Port Owner (1 = companion) */
#define PORTSC_RWC_MASK         (PORTSC_CSC | PORTSC_PEDC | PORTSC_OCCC)

/* HCCPARAMS bits */
#define HCCPARAMS_64BIT         (1U << 0)
#define HCCPARAMS_PFLF          (1U << 1)   /* Programmable Frame List Flag */
#define HCCPARAMS_ASPC          (1U << 2)
#define HCCPARAMS_EECP_SHIFT    8
#define HCCPARAMS_EECP_MASK     (0xFFU << 8)

/* USBLEGSUP register inside extended capability (PCI config space) */
#define EECP_USBLEGSUP_ID       0x01
#define USBLEGSUP_BIOS_OWNED    (1U << 16)
#define USBLEGSUP_OS_OWNED      (1U << 24)
#define USBLEGSUP_NEXT_SHIFT    8

/* ── Queue Head (48 bytes, 32-byte aligned) ──────────────────── */
typedef struct ehci_qh {
    uint32_t  hlink;        /* Horizontal link pointer + Typ + T */
    uint32_t  ep_chars;     /* EP characteristics (Address, EP#, MPS, ...) */
    uint32_t  ep_caps;      /* EP capabilities (S-mask, C-mask, Mult, ...) */
    uint32_t  current_qtd;  /* Read-only by SW after QH activated */
    /* Transfer overlay area (32 bytes) -- HW maintains state here */
    uint32_t  next_qtd;
    uint32_t  alt_next_qtd;
    uint32_t  qtd_token;
    uint32_t  qtd_buf[5];
} __attribute__((packed, aligned(32))) ehci_qh_t;

/* ── Queue Element Transfer Descriptor (32 bytes, 32-byte aligned) ── */
typedef struct ehci_qtd {
    uint32_t  next;         /* + T bit */
    uint32_t  alt_next;     /* + T bit */
    uint32_t  token;
    uint32_t  buf[5];       /* up to 5 4 KiB buffer pages */
    /* Pad to 32 bytes total: layout above is exactly 32 bytes (4 + 4 + 4 + 20) */
} __attribute__((packed, aligned(32))) ehci_qtd_t;

/* qTD token bits */
#define QTD_T                   (1U << 0)
#define QTD_STATUS_PINGE        (1U << 0)
#define QTD_STATUS_SPLIT_XACT   (1U << 1)
#define QTD_STATUS_MMF          (1U << 2)
#define QTD_STATUS_XACTERR      (1U << 3)
#define QTD_STATUS_BABBLE       (1U << 4)
#define QTD_STATUS_DBE          (1U << 5)
#define QTD_STATUS_HALTED       (1U << 6)
#define QTD_STATUS_ACTIVE       (1U << 7)
#define QTD_STATUS_ERROR_MASK   (QTD_STATUS_XACTERR | QTD_STATUS_BABBLE | \
                                 QTD_STATUS_DBE | QTD_STATUS_HALTED)
#define QTD_PID_OUT             0
#define QTD_PID_IN              1
#define QTD_PID_SETUP           2
#define QTD_PID_SHIFT           8
#define QTD_CERR_SHIFT          10
#define QTD_TOTAL_SHIFT         16
#define QTD_IOC                 (1U << 15)
#define QTD_DT                  (1U << 31)  /* Data Toggle */

/* Horizontal Link Type */
#define HLINK_T                 (1U << 0)
#define HLINK_TYP_QH            (1U << 1)

/* QH ep_chars */
#define QH_EP_DTC               (1U << 14)  /* Data Toggle Control = use qTD's DT */
#define QH_EP_H                 (1U << 15)  /* Head of reclamation list */
#define QH_EP_DEVADDR_SHIFT     0
#define QH_EP_EPNUM_SHIFT       8
#define QH_EP_EPS_SHIFT         12          /* 00=FS, 01=LS, 10=HS */
#define QH_EP_MPS_SHIFT         16

/* QH ep_caps */
#define QH_CAPS_MULT_SHIFT      30
#define QH_CAPS_RL_SHIFT        28          /* NAK reload */
#define QH_CAPS_SMASK_SHIFT     0
#define QH_CAPS_CMASK_SHIFT     8

/* ── Per-endpoint state for non-control transfers ────────────── */
#define EHCI_MAX_DEVICES        8
#define EHCI_MAX_BULK_EPS       4

typedef struct ehci_bulk_ep {
    int       configured;
    uint8_t   ep_addr;
    uint16_t  max_packet;
    ehci_qh_t *qh;
    uint64_t  qh_phys;
    uint8_t   data_toggle;
} ehci_bulk_ep_t;

typedef struct ehci_int_ep {
    int       configured;
    uint8_t   ep_addr;
    uint16_t  max_packet;
    uint8_t   interval;
    ehci_qh_t *qh;
    uint64_t  qh_phys;
    ehci_qtd_t *pending_qtd;
    uint64_t  pending_qtd_phys;
    void     *pending_buf;
    int       pending_len;
    uint8_t   data_toggle;
} ehci_int_ep_t;

/* Per-device EHCI state attached via xhci_device::hc_priv */
typedef struct ehci_dev_priv {
    ehci_qh_t      *control_qh;
    uint64_t        control_qh_phys;
    uint8_t         ctrl_data_toggle; /* unused: control xfers use DT field per stage */
    ehci_bulk_ep_t  bulk[EHCI_MAX_BULK_EPS];
    ehci_int_ep_t   int_in;
} ehci_dev_priv_t;

/* ── Driver state ────────────────────────────────────────────── */
typedef struct {
    int present;

    uint8_t pci_bus, pci_dev, pci_func;

    volatile uint8_t *cap_base;
    volatile uint8_t *op_base;

    uint32_t hcsparams;
    uint32_t hccparams;
    uint8_t  num_ports;
    uint8_t  caps64;

    uint32_t *frame_list;       /* 1024 entries */
    uint64_t  frame_list_phys;

    /* Reclamation list head (the H bit QH always lives on the async list). */
    ehci_qh_t *async_head;
    uint64_t   async_head_phys;

    /* Pool of static qTDs. We allocate from a single 4 KiB page.
     * 4096 / 32 = 128 qTDs. Plenty for control + small bulk traffic. */
    ehci_qtd_t *qtd_pool;
    uint64_t    qtd_pool_phys;
    int         qtd_used;

    /* QH pool (per-endpoint). 4 KiB / 64 (rounded up from 48) = 64 QHs */
    ehci_qh_t  *qh_pool;
    uint64_t    qh_pool_phys;
    int         qh_used;

    /* Devices */
    struct xhci_device devices[EHCI_MAX_DEVICES];
    ehci_dev_priv_t    dev_priv[EHCI_MAX_DEVICES];
    int dev_count;
    uint8_t next_address;
} ehci_t;

static ehci_t ehci;

/* ── Register accessors ──────────────────────────────────────── */
static inline uint32_t cap_r32(uint32_t off)
{ return *(volatile uint32_t *)(ehci.cap_base + off); }
static inline uint8_t  cap_r8(uint32_t off)
{ return *(volatile uint8_t *)(ehci.cap_base + off); }

static inline uint32_t op_r32(uint32_t off)
{ return *(volatile uint32_t *)(ehci.op_base + off); }
static inline void     op_w32(uint32_t off, uint32_t v)
{ *(volatile uint32_t *)(ehci.op_base + off) = v; }

#define EHCI_POLL_LIMIT 50000000U

static void spin(uint32_t n) { for (volatile uint32_t i = 0; i < n; i++) ; }

/* Coarse delay in milliseconds (ad-hoc; timer not necessarily up at init). */
static void delay_ms(uint32_t ms)
{
    /* ~100k spin = 1ms-ish on QEMU; tune is non-critical because the 50 ms
     * port-reset window has slack on both sides. */
    for (uint32_t i = 0; i < ms; i++) spin(100000);
}

/* ── Allocation helpers ──────────────────────────────────────── */
static void *alloc_pages(int npages)
{
    uint64_t phys;
    if (npages == 1) phys = pmm_alloc();
    else             phys = pmm_alloc_contiguous(npages);
    if (!phys) return 0;
    void *p = (void *)(uintptr_t)phys;
    memset(p, 0, (uint64_t)npages * 4096);
    return p;
}

static ehci_qtd_t *qtd_alloc(uint64_t *out_phys)
{
    if (ehci.qtd_used >= 128) return 0;
    ehci_qtd_t *q = &ehci.qtd_pool[ehci.qtd_used];
    uint64_t phys = ehci.qtd_pool_phys +
                    (uint64_t)ehci.qtd_used * sizeof(ehci_qtd_t);
    ehci.qtd_used++;
    memset(q, 0, sizeof(*q));
    if (out_phys) *out_phys = phys;
    return q;
}

static void qtd_pool_reset(void) { ehci.qtd_used = 0; }

static ehci_qh_t *qh_alloc(uint64_t *out_phys)
{
    /* Each QH 48 bytes; we round to 64-byte slots so pool index math is easy
     * and 32-byte alignment is preserved. */
    if (ehci.qh_used >= 64) return 0;
    uint8_t *base = (uint8_t *)ehci.qh_pool;
    uint64_t phys = ehci.qh_pool_phys + (uint64_t)ehci.qh_used * 64;
    ehci_qh_t *qh = (ehci_qh_t *)(base + (uint64_t)ehci.qh_used * 64);
    ehci.qh_used++;
    memset(qh, 0, sizeof(*qh));
    if (out_phys) *out_phys = phys;
    return qh;
}

/* ── BIOS handoff via USBLEGSUP extended capability ──────────── */
static void bios_handoff(uint8_t eecp)
{
    if (eecp < 0x40) return;     /* not in PCI cfg space, skip */
    uint8_t off = eecp;
    for (int i = 0; i < 16 && off; i++) {
        uint32_t cap = pci_config_read32(ehci.pci_bus, ehci.pci_dev,
                                         ehci.pci_func, off);
        uint8_t id = cap & 0xFF;
        if (id == EECP_USBLEGSUP_ID) {
            /* Set OS Owned, then poll for BIOS Owned to clear. */
            uint32_t legsup = cap;
            legsup |= USBLEGSUP_OS_OWNED;
            pci_config_write32(ehci.pci_bus, ehci.pci_dev, ehci.pci_func,
                               off, legsup);
            for (uint32_t j = 0; j < EHCI_POLL_LIMIT; j++) {
                uint32_t v = pci_config_read32(ehci.pci_bus, ehci.pci_dev,
                                               ehci.pci_func, off);
                if (!(v & USBLEGSUP_BIOS_OWNED) && (v & USBLEGSUP_OS_OWNED))
                    break;
                spin(100);
            }
            /* Disable SMI on legacy events: clear USBLEGCTLSTS at off+4 */
            pci_config_write32(ehci.pci_bus, ehci.pci_dev, ehci.pci_func,
                               off + 4, 0);
            return;
        }
        off = (cap >> USBLEGSUP_NEXT_SHIFT) & 0xFF;
    }
}

/* ── Halt + reset ────────────────────────────────────────────── */
static int ehci_halt(void)
{
    uint32_t cmd = op_r32(EHCI_OP_USBCMD);
    cmd &= ~(USBCMD_RS | USBCMD_PSE | USBCMD_ASE);
    op_w32(EHCI_OP_USBCMD, cmd);
    for (uint32_t i = 0; i < EHCI_POLL_LIMIT; i++) {
        if (op_r32(EHCI_OP_USBSTS) & USBSTS_HCH) return 0;
        spin(100);
    }
    return -1;
}

static int ehci_reset(void)
{
    op_w32(EHCI_OP_USBCMD, op_r32(EHCI_OP_USBCMD) | USBCMD_HCRESET);
    for (uint32_t i = 0; i < EHCI_POLL_LIMIT; i++) {
        if (!(op_r32(EHCI_OP_USBCMD) & USBCMD_HCRESET)) return 0;
        spin(100);
    }
    return -1;
}

/* ── PCI helpers ─────────────────────────────────────────────── */
static void pci_enable(uint8_t b, uint8_t d, uint8_t f)
{
    uint32_t cmd = pci_config_read32(b, d, f, 0x04);
    cmd |= (1 << 2) | (1 << 1);  /* Bus master + memory space */
    cmd &= ~(1 << 10);
    pci_config_write32(b, d, f, 0x04, cmd);
}

static uint64_t pci_read_bar(uint8_t b, uint8_t d, uint8_t f, int idx)
{
    uint32_t lo = pci_config_read32(b, d, f, 0x10 + idx * 4);
    if ((lo & 0x06) == 0x04) {
        uint32_t hi = pci_config_read32(b, d, f, 0x10 + (idx + 1) * 4);
        return ((uint64_t)hi << 32) | (lo & ~0xFULL);
    }
    return (uint64_t)(lo & ~0xFULL);
}

/* ── qTD builder ─────────────────────────────────────────────── */
static void qtd_fill(ehci_qtd_t *qtd, uint32_t buf_phys, int len,
                     int pid, int dt, int ioc)
{
    qtd->next = QTD_T;
    qtd->alt_next = QTD_T;
    uint32_t tok = ((uint32_t)len << QTD_TOTAL_SHIFT) |
                   ((uint32_t)pid << QTD_PID_SHIFT) |
                   (3U << QTD_CERR_SHIFT) |
                   QTD_STATUS_ACTIVE;
    if (dt)  tok |= QTD_DT;
    if (ioc) tok |= QTD_IOC;
    qtd->token = tok;

    if (len > 0 && buf_phys) {
        qtd->buf[0] = buf_phys;
        /* Subsequent buffer pointers are page-aligned successors. EHCI
         * supports up to 5 contiguous 4 KiB pages per qTD = 20 KiB max.
         * The first page's offset is from the low 12 bits of buf[0]. */
        uint32_t page = (buf_phys & ~0xFFFU) + 0x1000;
        for (int i = 1; i < 5; i++) {
            qtd->buf[i] = page;
            page += 0x1000;
        }
    }
}

static void qtd_link(ehci_qtd_t *prev, uint32_t next_phys)
{
    prev->next = next_phys & ~0x1FU;  /* bit0 = T (clear), bits[4:1] reserved */
}

/* ── Queue Head helper for control transfers (per device) ────── */
static void qh_init_control(ehci_qh_t *qh, struct xhci_device *dev,
                            int is_dummy_async_head)
{
    memset(qh, 0, sizeof(*qh));
    /* Horizontal link points back to itself initially (closed loop on
     * async list). The "H" bit identifies the reclamation head. */
    qh->hlink = (uint32_t)(ehci.async_head_phys) | HLINK_TYP_QH;

    int eps = 2; /* High Speed default */
    switch (dev ? dev->speed : USB_SPEED_HIGH) {
        case USB_SPEED_FULL: eps = 0; break;
        case USB_SPEED_LOW:  eps = 1; break;
        case USB_SPEED_HIGH: eps = 2; break;
        default:             eps = 2; break;
    }

    uint32_t mps = dev && dev->max_packet_size0 ? dev->max_packet_size0 : 64;
    uint32_t addr = dev ? dev->address : 0;

    qh->ep_chars = (addr << QH_EP_DEVADDR_SHIFT) |
                   (0U << QH_EP_EPNUM_SHIFT) |
                   ((uint32_t)eps << QH_EP_EPS_SHIFT) |
                   (mps << QH_EP_MPS_SHIFT) |
                   QH_EP_DTC;            /* DT comes from qTD */
    if (is_dummy_async_head) qh->ep_chars |= QH_EP_H;

    /* Mult = 1 (one transaction per micro-frame for control) */
    qh->ep_caps = (1U << QH_CAPS_MULT_SHIFT);

    /* Overlay: terminate so HC won't execute until we point to a qTD. */
    qh->current_qtd = 0;
    qh->next_qtd = QTD_T;
    qh->alt_next_qtd = QTD_T;
    qh->qtd_token = 0;
}

/* ── Wait for a qTD to complete (poll status). Returns 0 on success,
 *    -1 on error / timeout, or the (negative) error bits. ──────── */
static int qtd_wait(ehci_qtd_t *qtd)
{
    for (uint32_t i = 0; i < EHCI_POLL_LIMIT; i++) {
        uint32_t tok = qtd->token;
        if (!(tok & QTD_STATUS_ACTIVE)) {
            if (tok & QTD_STATUS_ERROR_MASK) {
                kputs("[ehci] qTD error tok=");
                kput_hex(tok);
                kputs("\n");
                return -1;
            }
            return 0;
        }
        spin(100);
    }
    kputs("[ehci] qTD timeout tok=");
    kput_hex(qtd->token);
    kputs("\n");
    return -1;
}

/* ── Run an arbitrary control transfer through a per-device QH ── */
static int do_control(struct xhci_device *dev,
                      uint8_t devaddr_override,
                      struct usb_setup_packet *setup,
                      void *buf, int max_len)
{
    if (!dev || !setup) return -1;
    ehci_dev_priv_t *priv = (ehci_dev_priv_t *)dev->hc_priv;
    if (!priv || !priv->control_qh) return -1;

    int dir_in = (setup->bmRequestType & 0x80) ? 1 : 0;
    int data_len = setup->wLength;
    if (data_len > max_len) data_len = max_len;

    /* Reset qTD pool for this transfer (simple bump allocator) */
    qtd_pool_reset();

    /* Allocate a small buffer for the SETUP packet (8 bytes) */
    static uint8_t setup_buf[8] __attribute__((aligned(64)));
    memcpy(setup_buf, setup, 8);

    uint64_t s_phys, d_phys = 0, st_phys;
    ehci_qtd_t *qs = qtd_alloc(&s_phys);
    if (!qs) return -1;
    qtd_fill(qs, (uint32_t)(uintptr_t)setup_buf, 8, QTD_PID_SETUP, 0, 0);

    ehci_qtd_t *qd = 0;
    if (data_len > 0) {
        qd = qtd_alloc(&d_phys);
        if (!qd) return -1;
        qtd_fill(qd, (uint32_t)(uintptr_t)buf, data_len,
                 dir_in ? QTD_PID_IN : QTD_PID_OUT, 1 /* DT=1 */, 0);
    }
    ehci_qtd_t *qst = qtd_alloc(&st_phys);
    if (!qst) return -1;
    qtd_fill(qst, 0, 0,
             dir_in ? QTD_PID_OUT : QTD_PID_IN,
             1 /* DT=1 for status stage */, 1 /* IOC */);

    /* Chain */
    if (qd) {
        qtd_link(qs, (uint32_t)d_phys);
        qtd_link(qd, (uint32_t)st_phys);
    } else {
        qtd_link(qs, (uint32_t)st_phys);
    }

    /* Refresh QH ep_chars in case device address / mps changed since init.
     * (Address Device path uses devaddr_override = 0 for the first xfer
     * and then we patch the address to the new one.) */
    ehci_qh_t *qh = priv->control_qh;
    int eps;
    switch (dev->speed) {
        case USB_SPEED_FULL: eps = 0; break;
        case USB_SPEED_LOW:  eps = 1; break;
        default:             eps = 2; break;
    }
    uint32_t mps = dev->max_packet_size0 ? dev->max_packet_size0 : 64;
    uint8_t  addr = devaddr_override ? devaddr_override : dev->address;
    qh->ep_chars = ((uint32_t)addr << QH_EP_DEVADDR_SHIFT) |
                   (0U << QH_EP_EPNUM_SHIFT) |
                   ((uint32_t)eps << QH_EP_EPS_SHIFT) |
                   ((uint32_t)mps << QH_EP_MPS_SHIFT) |
                   QH_EP_DTC;
    /* For non-HS control endpoints, set the Control Endpoint Flag. */
    if (eps != 2) qh->ep_chars |= (1U << 27);

    /* Point QH at the SETUP qTD. */
    qh->current_qtd = 0;
    qh->next_qtd = (uint32_t)s_phys;
    qh->alt_next_qtd = QTD_T;
    qh->qtd_token = 0;   /* clear status overlay so HC reloads from next_qtd */

    /* Wait for the status-stage qTD to complete (IOC marker). We poll the
     * qTD itself rather than the IAA doorbell since polling is fine. */
    int rc = qtd_wait(qst);
    if (rc != 0) return -1;

    /* Compute bytes transferred for the data stage (if any). */
    int xferred = data_len;
    if (qd) {
        int residual = (int)((qd->token >> QTD_TOTAL_SHIFT) & 0x7FFF);
        xferred = data_len - residual;
        if (xferred < 0) xferred = 0;
    } else {
        xferred = 0;
    }
    return xferred;
}

/* ── Public: control transfer ────────────────────────────────── */
int ehci_real_control_transfer(struct xhci_device *dev,
                               struct usb_setup_packet *setup,
                               void *buf, int max_len)
{
    return do_control(dev, 0, setup, buf, max_len);
}

int ehci_real_get_config_descriptor(struct xhci_device *dev,
                                    void *out, int max)
{
    if (!dev || !out || max <= 0) return -1;
    uint8_t hdr[9];
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    sp.bRequest = USB_REQ_GET_DESCRIPTOR;
    sp.wValue = (USB_DT_CONFIG << 8) | 0;
    sp.wIndex = 0;
    sp.wLength = 9;
    int g = ehci_real_control_transfer(dev, &sp, hdr, 9);
    if (g < 9) return -1;
    int total = hdr[2] | ((int)hdr[3] << 8);
    if (total > max) total = max;
    sp.wLength = (uint16_t)total;
    return ehci_real_control_transfer(dev, &sp, out, total);
}

int ehci_real_set_configuration(struct xhci_device *dev, uint8_t cfgv)
{
    if (!dev) return -1;
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    sp.bRequest = USB_REQ_SET_CONFIGURATION;
    sp.wValue = cfgv;
    sp.wIndex = 0;
    sp.wLength = 0;
    int r = ehci_real_control_transfer(dev, &sp, 0, 0);
    if (r < 0) return -1;
    dev->configuration = cfgv;
    return 0;
}

/* ── Bulk endpoint setup / transfer ──────────────────────────── */
static ehci_bulk_ep_t *bulk_lookup(ehci_dev_priv_t *priv, uint8_t ep_addr)
{
    for (int i = 0; i < EHCI_MAX_BULK_EPS; i++)
        if (priv->bulk[i].configured && priv->bulk[i].ep_addr == ep_addr)
            return &priv->bulk[i];
    return 0;
}
static ehci_bulk_ep_t *bulk_slot(ehci_dev_priv_t *priv)
{
    for (int i = 0; i < EHCI_MAX_BULK_EPS; i++)
        if (!priv->bulk[i].configured) return &priv->bulk[i];
    return 0;
}

int ehci_real_setup_bulk_endpoint(struct xhci_device *dev,
                                  uint8_t ep_addr, uint16_t max_packet)
{
    if (!dev || !max_packet) return -1;
    ehci_dev_priv_t *priv = (ehci_dev_priv_t *)dev->hc_priv;
    if (!priv) return -1;
    if (bulk_lookup(priv, ep_addr)) return 0;
    ehci_bulk_ep_t *be = bulk_slot(priv);
    if (!be) return -1;

    uint64_t qh_phys;
    ehci_qh_t *qh = qh_alloc(&qh_phys);
    if (!qh) return -1;
    int epnum = ep_addr & 0x0F;
    int eps;
    switch (dev->speed) {
        case USB_SPEED_FULL: eps = 0; break;
        case USB_SPEED_LOW:  eps = 1; break;
        default:             eps = 2; break;
    }
    qh->ep_chars = ((uint32_t)dev->address << QH_EP_DEVADDR_SHIFT) |
                   ((uint32_t)epnum << QH_EP_EPNUM_SHIFT) |
                   ((uint32_t)eps << QH_EP_EPS_SHIFT) |
                   ((uint32_t)max_packet << QH_EP_MPS_SHIFT) |
                   QH_EP_DTC;
    qh->ep_caps = (1U << QH_CAPS_MULT_SHIFT);
    qh->current_qtd = 0;
    qh->next_qtd = QTD_T;
    qh->alt_next_qtd = QTD_T;
    qh->qtd_token = 0;

    /* Splice into the async list right after the head QH. */
    qh->hlink = ehci.async_head->hlink;
    ehci.async_head->hlink = (uint32_t)qh_phys | HLINK_TYP_QH;

    be->configured = 1;
    be->ep_addr = ep_addr;
    be->max_packet = max_packet;
    be->qh = qh;
    be->qh_phys = qh_phys;
    be->data_toggle = 0;
    return 0;
}

int ehci_real_bulk_transfer(struct xhci_device *dev, int ep_addr,
                            void *buf, int len, int in)
{
    if (!dev || len < 0) return -1;
    ehci_dev_priv_t *priv = (ehci_dev_priv_t *)dev->hc_priv;
    if (!priv) return -1;
    int is_in = ((uint8_t)ep_addr & 0x80) ? 1 : 0;
    if (is_in != (in ? 1 : 0)) return -1;
    ehci_bulk_ep_t *be = bulk_lookup(priv, (uint8_t)ep_addr);
    if (!be) return -1;
    if (len > 0 && !buf) return -1;

    qtd_pool_reset();

    if (len == 0) {
        uint64_t z_phys;
        ehci_qtd_t *qz = qtd_alloc(&z_phys);
        if (!qz) return -1;
        qtd_fill(qz, 0, 0, is_in ? QTD_PID_IN : QTD_PID_OUT,
                 be->data_toggle, 1);
        be->qh->current_qtd = 0;
        be->qh->next_qtd = (uint32_t)z_phys;
        be->qh->alt_next_qtd = QTD_T;
        be->qh->qtd_token = 0;
        if (qtd_wait(qz) != 0) return -1;
        be->data_toggle ^= 1;
        return 0;
    }

    /* Each qTD can carry up to 5 pages * 4 KiB = 20480 bytes max. We chain
     * qTDs as needed for longer transfers. */
    const int CHUNK_MAX = 16384;
    uint8_t *cur = (uint8_t *)buf;
    int remaining = len;
    ehci_qtd_t *first = 0;
    uint64_t first_phys = 0;
    ehci_qtd_t *prev = 0;
    ehci_qtd_t *last = 0;
    while (remaining > 0) {
        int this_chunk = remaining > CHUNK_MAX ? CHUNK_MAX : remaining;
        uint64_t qphys;
        ehci_qtd_t *q = qtd_alloc(&qphys);
        if (!q) return -1;
        int is_last = (remaining - this_chunk == 0);
        qtd_fill(q, (uint32_t)(uintptr_t)cur, this_chunk,
                 is_in ? QTD_PID_IN : QTD_PID_OUT,
                 be->data_toggle, is_last ? 1 : 0);
        be->data_toggle ^= 1;
        if (!first) { first = q; first_phys = qphys; }
        if (prev)   qtd_link(prev, (uint32_t)qphys);
        prev = q;
        last = q;
        cur += this_chunk;
        remaining -= this_chunk;
    }

    be->qh->current_qtd = 0;
    be->qh->next_qtd = (uint32_t)first_phys;
    be->qh->alt_next_qtd = QTD_T;
    be->qh->qtd_token = 0;

    if (qtd_wait(last) != 0) return -1;

    /* Compute residual based on remaining-bytes field of last qTD */
    int residual = (int)((last->token >> QTD_TOTAL_SHIFT) & 0x7FFF);
    int xferred = len - residual;
    if (xferred < 0) xferred = 0;
    return xferred;
    (void)first;
}

int ehci_real_bulk_poll_in(struct xhci_device *dev, int ep_addr,
                           void *buf, int len)
{
    /* Synchronous emulation: same as bulk_transfer with a short timeout
     * window. Class drivers handle 0-byte returns as "no data yet". */
    if (!dev || !buf || len <= 0) return -1;
    if (!((uint8_t)ep_addr & 0x80)) return -1;
    ehci_dev_priv_t *priv = (ehci_dev_priv_t *)dev->hc_priv;
    if (!priv) return -1;
    ehci_bulk_ep_t *be = bulk_lookup(priv, (uint8_t)ep_addr);
    if (!be) return -1;

    qtd_pool_reset();
    uint64_t qphys;
    ehci_qtd_t *q = qtd_alloc(&qphys);
    if (!q) return -1;
    qtd_fill(q, (uint32_t)(uintptr_t)buf, len, QTD_PID_IN,
             be->data_toggle, 1);
    be->qh->current_qtd = 0;
    be->qh->next_qtd = (uint32_t)qphys;
    be->qh->alt_next_qtd = QTD_T;
    be->qh->qtd_token = 0;

    /* Short poll window — return 0 on no-data. */
    for (uint32_t i = 0; i < 200000; i++) {
        uint32_t tok = q->token;
        if (!(tok & QTD_STATUS_ACTIVE)) {
            if (tok & QTD_STATUS_ERROR_MASK) {
                /* NAK isn't an error in EHCI; HALTED is. Treat HALTED as -1. */
                if (tok & QTD_STATUS_HALTED) return -1;
                /* Else fall through to "no data". */
                return 0;
            }
            be->data_toggle ^= 1;
            int residual = (int)((tok >> QTD_TOTAL_SHIFT) & 0x7FFF);
            int xferred = len - residual;
            if (xferred < 0) xferred = 0;
            return xferred;
        }
    }
    /* Cancel by terminating the QH. */
    be->qh->next_qtd = QTD_T;
    return 0;
}

/* ── Interrupt-IN endpoint via Periodic Frame List ───────────── */
int ehci_real_setup_interrupt_in(struct xhci_device *dev,
                                 uint8_t ep_addr, uint16_t max_packet,
                                 uint8_t interval)
{
    if (!dev) return -1;
    if (!(ep_addr & 0x80)) return -1;
    ehci_dev_priv_t *priv = (ehci_dev_priv_t *)dev->hc_priv;
    if (!priv) return -1;
    if (priv->int_in.configured) return -1;

    uint64_t qh_phys;
    ehci_qh_t *qh = qh_alloc(&qh_phys);
    if (!qh) return -1;
    int epnum = ep_addr & 0x0F;
    int eps;
    switch (dev->speed) {
        case USB_SPEED_FULL: eps = 0; break;
        case USB_SPEED_LOW:  eps = 1; break;
        default:             eps = 2; break;
    }
    qh->ep_chars = ((uint32_t)dev->address << QH_EP_DEVADDR_SHIFT) |
                   ((uint32_t)epnum << QH_EP_EPNUM_SHIFT) |
                   ((uint32_t)eps << QH_EP_EPS_SHIFT) |
                   ((uint32_t)max_packet << QH_EP_MPS_SHIFT) |
                   QH_EP_DTC;
    /* Interrupt schedule: SMask=0x01 (micro-frame 0), Mult=1 */
    qh->ep_caps = (1U << QH_CAPS_MULT_SHIFT) |
                  (0x01U << QH_CAPS_SMASK_SHIFT);
    /* For FS/LS interrupt EPs we'd also need CMask + Hub Addr/Port; QEMU's
     * usb-mouse on EHCI is high-speed so this minimal path is enough for
     * the EHCI test target. Real-world FS/LS HID is very rare on EHCI-only
     * boards because BIOS routes those to UHCI/OHCI. */
    qh->current_qtd = 0;
    qh->next_qtd = QTD_T;
    qh->alt_next_qtd = QTD_T;

    /* Place into every slot of the periodic frame list as a tier-1
     * polling interval. Real QH terminates with T bit on hlink (no further
     * periodic schedule entries beyond this single QH). */
    qh->hlink = HLINK_T;
    for (int i = 0; i < 1024; i++) {
        ehci.frame_list[i] = (uint32_t)qh_phys | HLINK_TYP_QH;
    }

    /* Enable Periodic Schedule if not already. */
    op_w32(EHCI_OP_USBCMD, op_r32(EHCI_OP_USBCMD) | USBCMD_PSE);
    for (uint32_t i = 0; i < EHCI_POLL_LIMIT; i++) {
        if (op_r32(EHCI_OP_USBSTS) & USBSTS_PSS) break;
        spin(100);
    }

    priv->int_in.configured = 1;
    priv->int_in.ep_addr = ep_addr;
    priv->int_in.max_packet = max_packet;
    priv->int_in.interval = interval;
    priv->int_in.qh = qh;
    priv->int_in.qh_phys = qh_phys;
    priv->int_in.pending_qtd = 0;
    priv->int_in.pending_qtd_phys = 0;
    priv->int_in.pending_buf = 0;
    priv->int_in.pending_len = 0;
    priv->int_in.data_toggle = 0;
    return 0;
}

static void int_arm(ehci_int_ep_t *ie)
{
    /* Allocate a dedicated qTD page for interrupt EP if not yet. We use a
     * static slot so we don't churn the qtd_pool which is reused per
     * synchronous transfer. */
    static ehci_qtd_t int_qtd_storage __attribute__((aligned(32)));
    ehci_qtd_t *q = &int_qtd_storage;
    uint64_t qphys = (uint64_t)(uintptr_t)q;

    memset(q, 0, sizeof(*q));
    qtd_fill(q, (uint32_t)(uintptr_t)ie->pending_buf, ie->pending_len,
             QTD_PID_IN, ie->data_toggle, 1);

    ie->qh->current_qtd = 0;
    ie->qh->next_qtd = (uint32_t)qphys;
    ie->qh->alt_next_qtd = QTD_T;
    ie->qh->qtd_token = 0;
    ie->pending_qtd = q;
    ie->pending_qtd_phys = qphys;
}

int ehci_real_interrupt_poll(struct xhci_device *dev, void *buf, int len)
{
    if (!dev || !buf || len <= 0) return -1;
    ehci_dev_priv_t *priv = (ehci_dev_priv_t *)dev->hc_priv;
    if (!priv) return -1;
    ehci_int_ep_t *ie = &priv->int_in;
    if (!ie->configured) return -1;

    if (!ie->pending_qtd) {
        ie->pending_buf = buf;
        ie->pending_len = len;
        int_arm(ie);
        return 0;
    }

    uint32_t tok = ie->pending_qtd->token;
    if (tok & QTD_STATUS_ACTIVE) return 0;
    if (tok & QTD_STATUS_HALTED) {
        ie->pending_qtd = 0;
        return -1;
    }
    int residual = (int)((tok >> QTD_TOTAL_SHIFT) & 0x7FFF);
    int xferred = ie->pending_len - residual;
    if (xferred < 0) xferred = 0;
    if (xferred > len) xferred = len;
    ie->data_toggle ^= 1;
    ie->pending_qtd = 0;
    /* Re-arm immediately for next packet */
    ie->pending_buf = buf;
    ie->pending_len = len;
    int_arm(ie);
    return xferred;
}

int ehci_real_interrupt_transfer(struct xhci_device *dev, void *buf, int len)
{
    for (uint32_t i = 0; i < EHCI_POLL_LIMIT; i++) {
        int r = ehci_real_interrupt_poll(dev, buf, len);
        if (r != 0) return r;
    }
    return -1;
}

/* ── Port reset + addressing ─────────────────────────────────── */
static int port_reset(int port)
{
    uint32_t v = op_r32(EHCI_OP_PORTSC(port));
    if (!(v & PORTSC_CCS)) return -1;

    /* Clear RWC bits we don't want to lose; assert PR; clear PED. */
    v &= ~PORTSC_RWC_MASK;
    v &= ~PORTSC_PED;
    v |= PORTSC_PR;
    op_w32(EHCI_OP_PORTSC(port), v);

    delay_ms(50);

    /* Clear PR */
    v = op_r32(EHCI_OP_PORTSC(port));
    v &= ~PORTSC_RWC_MASK;
    v &= ~PORTSC_PR;
    op_w32(EHCI_OP_PORTSC(port), v);

    /* Wait up to 2 ms for PR to clear */
    for (uint32_t i = 0; i < 20000; i++) {
        if (!(op_r32(EHCI_OP_PORTSC(port)) & PORTSC_PR)) break;
        spin(100);
    }

    /* If PED set, device is high-speed and EHCI owns it. Otherwise
     * the port owner bit will get flipped to companion controller. */
    v = op_r32(EHCI_OP_PORTSC(port));
    if (!(v & PORTSC_PED)) {
        /* Hand off to companion controller (UHCI/OHCI) */
        v &= ~PORTSC_RWC_MASK;
        v |= PORTSC_PO;
        op_w32(EHCI_OP_PORTSC(port), v);
        return -1;
    }
    return 0;
}

/* Send SET_ADDRESS to the default address (0) */
static int set_address(struct xhci_device *dev, uint8_t new_addr)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    sp.bRequest = USB_REQ_SET_ADDRESS;
    sp.wValue = new_addr;
    sp.wIndex = 0;
    sp.wLength = 0;
    /* Use device address 0 for this transfer (default address state) */
    int r = do_control(dev, 0 /* override addr=0 */, &sp, 0, 0);
    if (r < 0) return -1;
    dev->address = new_addr;
    /* USB spec: 2 ms recovery after SET_ADDRESS before next request */
    delay_ms(2);
    return 0;
}

/* ── Device enumeration ──────────────────────────────────────── */
static int read_device_descriptor(struct xhci_device *dev,
                                  struct usb_device_descriptor *out, int len)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    sp.bRequest = USB_REQ_GET_DESCRIPTOR;
    sp.wValue = (USB_DT_DEVICE << 8) | 0;
    sp.wIndex = 0;
    sp.wLength = (uint16_t)len;
    return ehci_real_control_transfer(dev, &sp, out, len);
}

static int address_device(struct xhci_device *dev)
{
    /* Allocate per-device priv + per-device control QH */
    ehci_dev_priv_t *priv = &ehci.dev_priv[ehci.dev_count];
    memset(priv, 0, sizeof(*priv));
    dev->hc_priv = priv;

    uint64_t qh_phys;
    ehci_qh_t *qh = qh_alloc(&qh_phys);
    if (!qh) return -1;
    qh_init_control(qh, dev, 0);
    priv->control_qh = qh;
    priv->control_qh_phys = qh_phys;

    /* Splice into async list right after head. */
    qh->hlink = ehci.async_head->hlink;
    ehci.async_head->hlink = (uint32_t)qh_phys | HLINK_TYP_QH;

    /* Step 1: Read first 8 bytes of device descriptor at address 0 to learn
     * MaxPacketSize0. EHCI HS is always 64; FS/LS varies. */
    dev->address = 0;
    dev->max_packet_size0 = 64;
    uint8_t small[8];
    memset(small, 0, sizeof(small));
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    sp.bRequest = USB_REQ_GET_DESCRIPTOR;
    sp.wValue = (USB_DT_DEVICE << 8);
    sp.wIndex = 0;
    sp.wLength = 8;
    int g = ehci_real_control_transfer(dev, &sp, small, 8);
    if (g >= 8) {
        dev->max_packet_size0 = small[7] ? small[7] : 64;
    }

    /* Step 2: SET_ADDRESS to a fresh address. */
    uint8_t addr = ehci.next_address++;
    if (set_address(dev, addr) != 0) return -1;

    return 0;
}

static void enumerate_root_ports(void)
{
    for (int p = 0; p < ehci.num_ports; p++) {
        uint32_t v = op_r32(EHCI_OP_PORTSC(p));
        if (!(v & PORTSC_CCS)) continue;
        if (ehci.dev_count >= EHCI_MAX_DEVICES) break;

        struct xhci_device *dev = &ehci.devices[ehci.dev_count];
        memset(dev, 0, sizeof(*dev));
        dev->hc_kind = HC_KIND_EHCI;
        dev->port = p + 1;

        kputs("[ehci] port ");
        kput_dec(p);
        kputs(" CCS detected\n");

        if (port_reset(p) != 0) {
            kputs("[ehci] port ");
            kput_dec(p);
            kputs(" reset failed (or non-HS, owned by companion)\n");
            continue;
        }
        dev->speed = USB_SPEED_HIGH;

        if (address_device(dev) != 0) {
            kputs("[ehci] address device failed on port ");
            kput_dec(p);
            kputs("\n");
            continue;
        }

        struct usb_device_descriptor desc;
        memset(&desc, 0, sizeof(desc));
        int got = read_device_descriptor(dev, &desc, 18);
        if (got >= 18) {
            dev->vendor_id    = desc.idVendor;
            dev->product_id   = desc.idProduct;
            dev->bcd_usb      = desc.bcdUSB;
            dev->dev_class    = desc.bDeviceClass;
            dev->dev_subclass = desc.bDeviceSubClass;
            dev->dev_protocol = desc.bDeviceProtocol;
            dev->slot_id      = dev->address;   /* class-driver "configured" flag */
            kputs("[ehci]   VID=");
            kput_hex(desc.idVendor);
            kputs(" PID=");
            kput_hex(desc.idProduct);
            kputs(" class=");
            kput_hex(desc.bDeviceClass);
            kputs(" mps0=");
            kput_dec(dev->max_packet_size0);
            kputs("\n");
        } else {
            kputs("[ehci]   GET_DESCRIPTOR(Device) failed (");
            kput_dec(got < 0 ? 0 : (uint32_t)got);
            kputs(" bytes)\n");
        }

        ehci.dev_count++;
    }
}

/* ── Initialization ──────────────────────────────────────────── */
int ehci_init(void)
{
    memset(&ehci, 0, sizeof(ehci));
    ehci.next_address = 1;

    /* Locate first EHCI controller. */
    struct pci_device *pdev = 0;
    int n = pci_device_count();
    for (int i = 0; i < n; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x20) {
            pdev = d;
            break;
        }
    }
    if (!pdev) {
        kputs("[ehci] no EHCI controller found\n");
        return -1;
    }

    ehci.pci_bus = pdev->bus;
    ehci.pci_dev = pdev->dev;
    ehci.pci_func = pdev->func;

    kputs("[ehci] found controller at ");
    kput_hex(pdev->bus); kputs(":");
    kput_hex(pdev->dev); kputs(".");
    kput_hex(pdev->func);
    kputs(" vid=");  kput_hex(pdev->vendor_id);
    kputs(" did=");  kput_hex(pdev->device_id);
    kputs("\n");

    pci_enable(pdev->bus, pdev->dev, pdev->func);

    uint64_t bar0 = pci_read_bar(pdev->bus, pdev->dev, pdev->func, 0);
    if (!bar0) {
        kputs("[ehci] BAR0 = 0\n");
        return -1;
    }
    if (bar0 >= 0x100000000ULL) {
        uint64_t base = bar0 & ~0xFFFULL;
        vmm_map_range(base, base, 16,
                      PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE);
    }
    ehci.cap_base = (volatile uint8_t *)(uintptr_t)bar0;

    uint32_t cap0 = cap_r32(0);
    uint8_t  caplen  = (uint8_t)(cap0 & 0xFF);
    uint16_t hciver  = (uint16_t)(cap0 >> 16);
    ehci.hcsparams = cap_r32(EHCI_CAP_HCSPARAMS);
    ehci.hccparams = cap_r32(EHCI_CAP_HCCPARAMS);
    ehci.num_ports = (uint8_t)(ehci.hcsparams & 0x0F);
    ehci.caps64 = (ehci.hccparams & HCCPARAMS_64BIT) ? 1 : 0;

    uint8_t eecp = (uint8_t)((ehci.hccparams & HCCPARAMS_EECP_MASK) >>
                             HCCPARAMS_EECP_SHIFT);

    ehci.op_base = ehci.cap_base + caplen;

    kputs("[ehci] hciver=");
    kput_hex(hciver);
    kputs(" caplen=");
    kput_dec(caplen);
    kputs(" ports=");
    kput_dec(ehci.num_ports);
    kputs(" 64bit=");
    kput_dec(ehci.caps64);
    kputs(" eecp=");
    kput_hex(eecp);
    kputs("\n");

    if (ehci.num_ports == 0) {
        kputs("[ehci] degenerate parameters\n");
        return -1;
    }

    /* BIOS handoff */
    if (eecp >= 0x40) bios_handoff(eecp);

    /* Halt + reset */
    if (ehci_halt() != 0) {
        kputs("[ehci] halt failed\n");
        return -1;
    }
    if (ehci_reset() != 0) {
        kputs("[ehci] reset failed\n");
        return -1;
    }

    /* Allocate frame list (4 KiB) and pools */
    ehci.frame_list = alloc_pages(1);
    if (!ehci.frame_list) return -1;
    ehci.frame_list_phys = (uint64_t)(uintptr_t)ehci.frame_list;
    /* Initialize all entries with T (terminate). */
    for (int i = 0; i < 1024; i++) ehci.frame_list[i] = HLINK_T;

    ehci.qtd_pool = alloc_pages(1);
    if (!ehci.qtd_pool) return -1;
    ehci.qtd_pool_phys = (uint64_t)(uintptr_t)ehci.qtd_pool;

    ehci.qh_pool = alloc_pages(1);
    if (!ehci.qh_pool) return -1;
    ehci.qh_pool_phys = (uint64_t)(uintptr_t)ehci.qh_pool;

    /* Allocate dummy async-head QH (H bit set, points back to itself). */
    uint64_t head_phys;
    ehci.async_head = qh_alloc(&head_phys);
    if (!ehci.async_head) return -1;
    ehci.async_head_phys = head_phys;
    qh_init_control(ehci.async_head, 0, 1 /* H bit */);
    ehci.async_head->hlink = (uint32_t)head_phys | HLINK_TYP_QH;
    /* Mark overlay terminated so HC walks the loop without executing. */
    ehci.async_head->next_qtd = QTD_T;
    ehci.async_head->alt_next_qtd = QTD_T;
    ehci.async_head->qtd_token = QTD_STATUS_HALTED; /* HC skips this QH */

    /* Program registers: 32-bit DMA segment = 0, periodic + async lists */
    op_w32(EHCI_OP_CTRLDSSEGMENT, 0);
    op_w32(EHCI_OP_USBINTR, 0);          /* polling */
    op_w32(EHCI_OP_FRINDEX, 0);
    op_w32(EHCI_OP_PERIODICLISTBASE, (uint32_t)ehci.frame_list_phys);
    op_w32(EHCI_OP_ASYNCLISTADDR, (uint32_t)head_phys);

    /* Ack any sticky USBSTS bits (W1C). */
    op_w32(EHCI_OP_USBSTS, 0x3F);

    /* Run, with Async Schedule enabled. Periodic enables when first int EP is set up. */
    uint32_t cmd = op_r32(EHCI_OP_USBCMD);
    cmd &= ~0x00FF0000U;          /* clear FrameListSize/IntThresholdControl high bits */
    cmd |= (8U << 16);            /* Interrupt Threshold Control = 8 micro-frames */
    cmd |= USBCMD_ASE | USBCMD_RS;
    op_w32(EHCI_OP_USBCMD, cmd);

    /* Wait for HCH = 0 */
    int running = 0;
    for (uint32_t i = 0; i < EHCI_POLL_LIMIT; i++) {
        if (!(op_r32(EHCI_OP_USBSTS) & USBSTS_HCH)) { running = 1; break; }
        spin(100);
    }
    if (!running) {
        kputs("[ehci] controller did not start\n");
        return -1;
    }

    /* Take control of all ports (route to EHCI rather than companion). */
    op_w32(EHCI_OP_CONFIGFLAG, 1);

    /* Power on each port (PP only meaningful if PPC reported in HCSPARAMS). */
    uint32_t ppc = (ehci.hcsparams >> 4) & 1;
    if (ppc) {
        for (int p = 0; p < ehci.num_ports; p++) {
            uint32_t v = op_r32(EHCI_OP_PORTSC(p));
            v &= ~PORTSC_RWC_MASK;
            v |= PORTSC_PP;
            op_w32(EHCI_OP_PORTSC(p), v);
        }
        delay_ms(20);
    }

    /* Settle delay */
    delay_ms(100);

    ehci.present = 1;

    enumerate_root_ports();

    kputs("[ehci] ready, ");
    kput_dec(ehci.num_ports);
    kputs(" port(s), ");
    kput_dec(ehci.dev_count);
    kputs(" device(s) found");
    if (ehci.dev_count > 0) {
        kputs(" (first VID:PID ");
        kput_hex(ehci.devices[0].vendor_id);
        kputs(":");
        kput_hex(ehci.devices[0].product_id);
        kputs(")");
    }
    kputs("\n");

    return 0;
}

int ehci_device_count(void) { return ehci.dev_count; }

struct xhci_device *ehci_get_device(int index)
{
    if (index < 0 || index >= ehci.dev_count) return 0;
    return &ehci.devices[index];
}

struct xhci_device *ehci_find_device(uint16_t vid, uint16_t pid)
{
    for (int i = 0; i < ehci.dev_count; i++) {
        struct xhci_device *d = &ehci.devices[i];
        if ((vid == 0 || d->vendor_id == vid) &&
            (pid == 0 || d->product_id == pid))
            return d;
    }
    return 0;
}
