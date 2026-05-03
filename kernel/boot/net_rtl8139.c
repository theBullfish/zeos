/*
 * Zeos — Realtek RTL8139 NIC driver
 *
 * Datasheet: RTL8139C(L) v1.4. Register layout:
 *   0x00-0x05  IDR0..5         MAC address
 *   0x10-0x1C  TSD0..TSD3      TX status (4 descriptors)
 *   0x20-0x2C  TSAD0..TSAD3    TX address (phys ptr to TX buffer)
 *   0x30       RBSTART         RX ring DMA address
 *   0x37       CR              Command (RST/RE/TE/BUFE)
 *   0x38       CAPR            RX read pointer
 *   0x3A       CBR             RX write pointer (HW)
 *   0x3C       IMR             interrupt mask
 *   0x3E       ISR             interrupt status (write 1 to clear)
 *   0x40       TCR             TX config
 *   0x44       RCR             RX config
 *   0x52       CONFIG1         power state
 *
 * RX uses a single linear ring buffer the chip DMAs into. CAPR is our
 * read cursor; CBR is the chip's write cursor. Each packet has a 4-byte
 * header: status(2 LE) + length(2 LE), then payload, padded to 4 bytes.
 *
 * TX uses 4 descriptors. Each holds one full Ethernet frame; we copy
 * the user's bytes into a static buffer, write the buffer's phys
 * address to TSAD, then write the length (with OWN=0) to TSD. Hardware
 * sets OWN=1 + TOK when done.
 */

#include "net_rtl8139.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "kprint.h"

#define RTL_VENDOR  0x10EC
#define RTL_DEVICE  0x8139

/* Registers (offsets from io_base) */
#define R_IDR0      0x00
#define R_TSD0      0x10
#define R_TSAD0     0x20
#define R_RBSTART   0x30
#define R_CR        0x37
#define R_CAPR      0x38
#define R_CBR       0x3A
#define R_IMR       0x3C
#define R_ISR       0x3E
#define R_TCR       0x40
#define R_RCR       0x44
#define R_CONFIG1   0x52

/* CR bits */
#define CR_RST      0x10
#define CR_RE       0x08
#define CR_TE       0x04
#define CR_BUFE     0x01

/* RCR bits */
#define RCR_AAP     (1 << 0)   /* accept all packets (promisc) */
#define RCR_APM     (1 << 1)   /* accept physical match */
#define RCR_AM      (1 << 2)   /* accept multicast */
#define RCR_AB      (1 << 3)   /* accept broadcast */
#define RCR_WRAP    (1 << 7)   /* allow ring wrap */

/* TSD bits */
#define TSD_OWN     (1 << 13)  /* set by HW when done */
#define TSD_TOK     (1 << 15)  /* transmit OK */

/* Buffer sizes */
#define RX_BUF_SIZE   8192
#define RX_PAD        16     /* WRAP pad — chip needs slack past end */
#define RX_BUF_TOTAL  (RX_BUF_SIZE + RX_PAD + 1500)
#define TX_BUF_SIZE   1536

static uint16_t io_base;
static int      ready;
static struct mac_addr mac;

/* Static buffers in BSS — physical == virtual under our identity map. */
static uint8_t rx_buf[RX_BUF_TOTAL] __attribute__((aligned(4)));
static uint8_t tx_buf[4][TX_BUF_SIZE] __attribute__((aligned(4)));
static uint16_t capr;        /* our read cursor into rx_buf */
static int      tx_slot;     /* next TX descriptor to use */

/* ---- Register helpers ---- */
static inline void w8 (uint16_t r, uint8_t  v) { outb(io_base + r, v); }
static inline void w16(uint16_t r, uint16_t v) { outw(io_base + r, v); }
static inline void w32(uint16_t r, uint32_t v) { outl(io_base + r, v); }
static inline uint8_t  r8 (uint16_t r) { return inb(io_base + r); }
static inline uint16_t r16(uint16_t r) { return inw(io_base + r); }
static inline uint32_t r32(uint16_t r) { return inl(io_base + r); }

int rtl8139_init(void)
{
    ready = 0;

    int count = pci_device_count();
    struct pci_device *nic = 0;
    for (int i = 0; i < count; i++) {
        struct pci_device *d = pci_get_device(i);
        if (d && d->vendor_id == RTL_VENDOR && d->device_id == RTL_DEVICE) {
            nic = d;
            break;
        }
    }
    if (!nic) {
        kputs("rtl8139: no RTL8139 found\n");
        return -1;
    }

    kputs("rtl8139: found at ");
    kput_dec(nic->bus); kputs(":");
    kput_dec(nic->dev); kputs(".");
    kput_dec(nic->func); kputs("\n");

    /* BAR0 is I/O space (bit 0 = 1). Mask to get port base. */
    if (!(nic->bar[0] & 1)) {
        kputs("rtl8139: BAR0 isn't I/O — unexpected\n");
        return -1;
    }
    io_base = (uint16_t)(nic->bar[0] & ~0x3);

    /* Bus-master enable in PCI command register (offset 0x04, bit 2)
     * so DMA from chip-to-RAM works. Also keep IO Space (bit 0) on. */
    uint32_t cmd = pci_config_read32(nic->bus, nic->dev, nic->func, 0x04);
    pci_config_write32(nic->bus, nic->dev, nic->func, 0x04, cmd | 0x05);

    /* Power on (CONFIG1 = 0x00 leaves device in D0). */
    w8(R_CONFIG1, 0x00);

    /* Software reset, wait for it to clear (typically a few µs). */
    w8(R_CR, CR_RST);
    for (int i = 0; i < 100000; i++) {
        if (!(r8(R_CR) & CR_RST)) break;
        io_wait();
    }
    if (r8(R_CR) & CR_RST) {
        kputs("rtl8139: reset timeout\n");
        return -1;
    }

    /* Read MAC address */
    for (int i = 0; i < 6; i++) mac.b[i] = r8(R_IDR0 + i);

    /* Point chip at our RX buffer (low 32 bits — chip is 32-bit DMA). */
    w32(R_RBSTART, (uint32_t)(uintptr_t)rx_buf);

    /* Enable interesting interrupts (we poll, but ISR bits are still
     * usable as status). TOK | ROK | RER | TER. */
    w16(R_IMR, 0x0005);

    /* RX config: WRAP + accept broadcast/multicast/physical match.
     * No promisc — let the chip filter to our MAC. Buffer size: 8K+16. */
    w32(R_RCR, RCR_AB | RCR_AM | RCR_APM | RCR_WRAP /* RBLEN=00 = 8K */);

    /* TX config: defaults are fine. */
    w32(R_TCR, 0x03000700);  /* IFG=11, MXDMA=111 (1024 bytes) */

    /* Enable RX + TX */
    w8(R_CR, CR_RE | CR_TE);

    capr = 0;
    tx_slot = 0;
    ready = 1;

    kputs("rtl8139: ready, MAC=");
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i) kputc(':');
        kputc(hx[(mac.b[i] >> 4) & 0xf]);
        kputc(hx[mac.b[i] & 0xf]);
    }
    kputs("\n");
    return 0;
}

int rtl8139_send(const void *frame, uint16_t len)
{
    if (!ready) return -1;
    if (len < 60) len = 60;            /* pad to min Ethernet frame */
    if (len > TX_BUF_SIZE) return -1;

    /* Wait for current descriptor's previous send to complete (OWN=1). */
    int slot = tx_slot;
    uint32_t tsd_off = R_TSD0 + slot * 4;
    /* OWN bit gets SET by HW at completion. After reset all slots are
     * ready (OWN=1 effectively means "available"). */
    for (int i = 0; i < 100000; i++) {
        uint32_t s = r32(tsd_off);
        if (s & TSD_OWN) break;
        if (i == 0 && (s & 0xFFFF) == 0) break;  /* fresh slot, never used */
        io_wait();
    }

    /* Copy into the slot's TX buffer */
    const uint8_t *src = (const uint8_t *)frame;
    uint8_t *dst = tx_buf[slot];
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];
    /* Pad if needed */
    for (uint16_t i = len; i < 60; i++) dst[i] = 0;

    /* Tell chip where it is + length. Writing TSD with OWN=0 starts it. */
    w32(R_TSAD0 + slot * 4, (uint32_t)(uintptr_t)tx_buf[slot]);
    w32(tsd_off, (uint32_t)len & 0x1FFF);

    tx_slot = (slot + 1) & 0x3;
    return 0;
}

int rtl8139_recv(void *frame, uint16_t max_len)
{
    if (!ready) return 0;
    if (r8(R_CR) & CR_BUFE) return 0;  /* RX buffer empty */

    /* Header at our cursor: 2 bytes status, 2 bytes length-incl-CRC. */
    uint16_t status = *(uint16_t *)(rx_buf + capr);
    uint16_t pkt_len = *(uint16_t *)(rx_buf + capr + 2);

    if (!(status & 0x0001)) {
        /* RX_OK bit clear — bad packet. Reset to flush. */
        w8(R_CR, CR_TE);
        w8(R_CR, CR_RE | CR_TE);
        w32(R_RCR, RCR_AB | RCR_AM | RCR_APM | RCR_WRAP);
        capr = 0;
        return 0;
    }

    if (pkt_len < 4 || pkt_len > 1518) {
        /* Garbage — reset RX */
        w8(R_CR, CR_TE);
        w8(R_CR, CR_RE | CR_TE);
        capr = 0;
        return 0;
    }

    uint16_t payload = pkt_len - 4;  /* strip CRC */
    uint16_t copy = payload > max_len ? max_len : payload;
    uint8_t *dst = (uint8_t *)frame;

    /* Read payload LINEARLY from rx_buf+capr+4. With WRAP=1 the chip
     * writes packets that straddle end-of-buffer into the slack region
     * past RX_BUF_SIZE (up to MTU-sized slack). Modulo'ing here would
     * read wrong bytes for those wrap-spanning packets and the body
     * would silently corrupt — that was the rtl8139 body=0 bug. */
    const uint8_t *src = rx_buf + capr + 4;
    for (uint16_t i = 0; i < copy; i++) dst[i] = src[i];

    /* Advance cursor (4-byte aligned), wrap modulo RX_BUF_SIZE so the
     * NEXT packet's header gets read from the right place. */
    uint32_t next = (capr + pkt_len + 4 + 3) & ~3u;
    capr = (uint16_t)(next % RX_BUF_SIZE);

    /* Tell chip our new read pointer. CAPR is "current address of
     * packet read", chip wants it at (capr - 16) mod RX_BUF_SIZE. */
    w16(R_CAPR, (uint16_t)(capr - 16));

    /* Ack RX interrupt (write 1 to clear) */
    w16(R_ISR, 0x0001);

    return (int)copy;
}

void rtl8139_get_mac(struct mac_addr *out)
{
    *out = mac;
}
