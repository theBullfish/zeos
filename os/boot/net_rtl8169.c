/*
 * Zeos — Realtek RTL8169/RTL8168 Gigabit NIC driver
 *
 * Targets the giant consumer fleet of Realtek gigabit parts
 * (RTL8169, RTL8168, RTL8111, RTL8161, RTL8136/RTL8101E).
 * All speak the same descriptor-ring MMIO programming model
 * once initialised — the differences live in PHY init quirks
 * which we don't need for basic packet I/O on already-linked
 * media (QEMU + most real hardware after BIOS POST).
 *
 * Programming model (from the Realtek public RTL8168 datasheet
 * and the canonical Linux drivers/net/ethernet/realtek/r8169_main.c):
 *
 *   BAR0: I/O space alias (we ignore)
 *   BAR2: 4 KiB MMIO window with the register file
 *
 *   Registers (offsets from BAR2):
 *     0x00 IDR0..5      MAC address (6 bytes)
 *     0x20 TNPDS_LO/HI  Transmit Normal Priority Descriptor Start (8-byte aligned)
 *     0x28 THPDS_LO/HI  Transmit High Priority Descriptor Start
 *     0x37 CR           Command (RST/RE/TE)
 *     0x38 TPPoll       TX poll (write 0x40 = NPQ to nudge HW)
 *     0x3C IMR          interrupt mask
 *     0x3E ISR          interrupt status (write 1 to clear)
 *     0x40 TCR          TX config
 *     0x44 RCR          RX config
 *     0x50 9346CR       EEPROM/config-register lock
 *     0xDA RMS          Max RX packet size (12 bits)
 *     0xE0 CplusCR      C+ command (must enable RX/TX checksum DMA)
 *     0xE4 RDSAR_LO/HI  Receive Descriptor Start Address (256-byte aligned)
 *     0xEC MTPS         Max TX packet size (in 128-byte units)
 *
 *   Descriptor (16 bytes, little-endian):
 *     +0  uint32 cmd     (OWN | EOR | FS | LS | length[15:0])
 *     +4  uint32 vlan    (we leave 0)
 *     +8  uint32 addr_lo (buffer phys, low 32)
 *     +12 uint32 addr_hi (buffer phys, high 32)
 *
 *   OWN = bit 31 — when SET means the chip owns the descriptor
 *                  (TX: chip will send it; RX: chip will fill it).
 *                  Cleared by chip on completion.
 *   EOR = bit 30 — End of Ring marker on the last descriptor
 *   FS  = bit 29 — First Segment (we set on every TX since we send single-seg)
 *   LS  = bit 28 — Last Segment
 *
 *   RX descriptor length field (low 14 bits) = our buffer size on rearm.
 *   On RX completion, low 14 bits = received length (incl. nothing extra
 *   when CRC stripping is enabled — see RCR).
 *
 * Polling-only. No interrupts wired.
 */

#include "net_rtl8169.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "kprint.h"

#define RTL_VENDOR  0x10EC

/* ---- Register offsets ---- */
#define R_IDR0       0x00
#define R_MAR0       0x08   /* Multicast filter (8 bytes) */
#define R_TNPDS      0x20   /* TX Normal-Pri descriptor start (64-bit) */
#define R_THPDS      0x28   /* TX High-Pri descriptor start (64-bit) */
#define R_CR         0x37
#define R_TPPOLL     0x38
#define R_IMR        0x3C
#define R_ISR        0x3E
#define R_TCR        0x40
#define R_RCR        0x44
#define R_9346CR     0x50
#define R_CONFIG1    0x52
#define R_PHYSTATUS  0x6C
#define R_RMS        0xDA
#define R_CPCR       0xE0
#define R_RDSAR      0xE4   /* RX descriptor start (64-bit) */
#define R_MTPS       0xEC

/* CR bits */
#define CR_RST       (1 << 4)
#define CR_RE        (1 << 3)
#define CR_TE        (1 << 2)

/* TPPoll bits */
#define TPPOLL_NPQ   (1 << 6)   /* Normal priority queue kick */

/* RCR bits */
#define RCR_AAP      (1 << 0)   /* accept all (promisc) */
#define RCR_APM      (1 << 1)   /* accept physical match */
#define RCR_AM       (1 << 2)   /* accept multicast */
#define RCR_AB       (1 << 3)   /* accept broadcast */
#define RCR_AR       (1 << 4)   /* accept runt */
#define RCR_AER      (1 << 5)   /* accept error */
#define RCR_9356SEL  (1 << 6)
/* MXDMA = bits 10:8, default unlimited (0x07) */
#define RCR_MXDMA_UNLIM (7 << 8)

/* TCR bits */
/* MXDMA in TCR is bits 10:8, IFG bits 24:25, default values are fine */
#define TCR_IFG_STD     (3 << 24)   /* Standard inter-frame gap */
#define TCR_MXDMA_UNLIM (7 << 8)

/* CPCR bits — must be set so the chip honors descriptors at all on
 * the C+ family (which is everything 8169-and-newer). */
#define CPCR_RXCHKSUM (1 << 5)
#define CPCR_RXVLAN   (1 << 6)
#define CPCR_PCIDAC   (1 << 4)   /* dual-address-cycle for >32-bit DMA */
#define CPCR_MULRW    (1 << 3)   /* enable PCI multiple read/write */

/* 9346CR bits */
#define EEM_NONE     0x00
#define EEM_CONFIG   0xC0   /* unlock config registers */

/* Descriptor cmd bits */
#define DESC_OWN     (1u << 31)
#define DESC_EOR     (1u << 30)
#define DESC_FS      (1u << 29)
#define DESC_LS      (1u << 28)

/* ---- Ring sizes ---- */
#define NUM_TX_DESC   8
#define NUM_RX_DESC   8
#define RX_BUF_SIZE   2048
#define TX_BUF_SIZE   2048

/* ---- Descriptor layout ---- */
struct rtl_desc {
    uint32_t cmd;       /* OWN/EOR/FS/LS + length */
    uint32_t vlan;
    uint32_t addr_lo;
    uint32_t addr_hi;
} __attribute__((packed));

/* ---- Device IDs that speak this programming model ---- */
struct rtl_id {
    uint16_t device_id;
    const char *name;
};

static const struct rtl_id known_ids[] = {
    { 0x8169, "RTL8169"  },   /* original gigabit, QEMU model=rtl8169 */
    { 0x8161, "RTL8161"  },
    { 0x8168, "RTL8168/8111" }, /* the volume part — most laptops */
    { 0x8136, "RTL8101E/8136" }, /* fast-ethernet sibling, same regs */
    { 0, 0 }
};

/* ---- Static state ---- */
static volatile uint8_t *mmio;
static struct mac_addr   dev_mac;
static int               ready;

/* Descriptor rings: 256-byte aligned per datasheet. We over-align
 * to 4 KiB to keep them clearly separate and DMA-safe. */
static struct rtl_desc tx_ring[NUM_TX_DESC] __attribute__((aligned(256)));
static struct rtl_desc rx_ring[NUM_RX_DESC] __attribute__((aligned(256)));

/* Per-descriptor packet buffers (identity-mapped under our paging). */
static uint8_t tx_buffers[NUM_TX_DESC][TX_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t rx_buffers[NUM_RX_DESC][RX_BUF_SIZE] __attribute__((aligned(16)));

static int tx_idx;   /* next TX slot to use */
static int rx_idx;   /* next RX slot to inspect */

/* ---- MMIO accessors ---- */
static inline void mw8 (uint32_t r, uint8_t  v) { *(volatile uint8_t  *)(mmio + r) = v; }
static inline void mw16(uint32_t r, uint16_t v) { *(volatile uint16_t *)(mmio + r) = v; }
static inline void mw32(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }
static inline uint8_t  mr8 (uint32_t r) { return *(volatile uint8_t  *)(mmio + r); }
static inline uint16_t mr16(uint32_t r) { return *(volatile uint16_t *)(mmio + r); }
static inline uint32_t mr32(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }

static inline void wmb(void) { __sync_synchronize(); }

/* ---- Ring init ---- */
static void rx_ring_init(void)
{
    for (int i = 0; i < NUM_RX_DESC; i++) {
        uint64_t buf = (uint64_t)(uintptr_t)rx_buffers[i];
        rx_ring[i].vlan    = 0;
        rx_ring[i].addr_lo = (uint32_t)(buf & 0xFFFFFFFFu);
        rx_ring[i].addr_hi = (uint32_t)(buf >> 32);
        uint32_t cmd = DESC_OWN | (RX_BUF_SIZE & 0x3FFFu);
        if (i == NUM_RX_DESC - 1)
            cmd |= DESC_EOR;
        rx_ring[i].cmd = cmd;
    }
    rx_idx = 0;
}

static void tx_ring_init(void)
{
    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_ring[i].cmd     = 0;             /* not OWNed by HW yet */
        tx_ring[i].vlan    = 0;
        tx_ring[i].addr_lo = 0;
        tx_ring[i].addr_hi = 0;
        if (i == NUM_TX_DESC - 1)
            tx_ring[i].cmd |= DESC_EOR;
    }
    tx_idx = 0;
}

/* ---- Public API ---- */
int rtl8169_init(void)
{
    ready = 0;

    /* Find a Realtek class-0x02 device matching our table. */
    int count = pci_device_count();
    struct pci_device *nic = 0;
    const char *nic_name = "unknown";

    for (int i = 0; i < count; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->vendor_id != RTL_VENDOR) continue;
        if (d->class_code != 0x02) continue;

        /* Skip the RTL8139 — it's handled by the legacy driver. */
        if (d->device_id == 0x8139) continue;

        for (const struct rtl_id *id = known_ids; id->device_id; id++) {
            if (d->device_id == id->device_id) {
                nic = d;
                nic_name = id->name;
                break;
            }
        }
        if (nic) break;
    }

    if (!nic) {
        kputs("rtl8169: no RTL8169-family NIC found\n");
        return -1;
    }

    kputs("rtl8169: found ");
    kputs(nic_name);
    kputs(" [");
    kput_hex(nic->device_id);
    kputs("] at ");
    kput_dec(nic->bus); kputs(":");
    kput_dec(nic->dev); kputs(".");
    kput_dec(nic->func); kputs("\n");

    /* Map BAR2 (MMIO). It may be a 64-bit BAR — combine with BAR3. */
    uint32_t bar2 = nic->bar[2];
    if (bar2 & 1) {
        kputs("rtl8169: BAR2 is I/O space, expected MMIO\n");
        return -1;
    }
    uint64_t mmio_phys = bar2 & 0xFFFFFFF0u;
    if ((bar2 & 0x06) == 0x04) {
        mmio_phys |= ((uint64_t)nic->bar[3]) << 32;
    }
    mmio = (volatile uint8_t *)(uintptr_t)mmio_phys;

    kputs("rtl8169: MMIO @ 0x");
    kput_hex(mmio_phys);
    kputs("\n");

    /* PCI command register: bus master + memory space enable. */
    uint32_t cmd = pci_config_read32(nic->bus, nic->dev, nic->func, 0x04);
    cmd |= (1 << 1);  /* memory space */
    cmd |= (1 << 2);  /* bus master */
    pci_config_write32(nic->bus, nic->dev, nic->func, 0x04, cmd);

    /* Software reset, wait for it to clear. */
    mw8(R_CR, CR_RST);
    int i;
    for (i = 0; i < 1000; i++) {
        if (!(mr8(R_CR) & CR_RST)) break;
        for (volatile int d = 0; d < 1000; d++);
    }
    if (mr8(R_CR) & CR_RST) {
        kputs("rtl8169: reset timeout\n");
        return -1;
    }

    /* Read MAC from IDR0..5. */
    for (int j = 0; j < 6; j++) dev_mac.b[j] = mr8(R_IDR0 + j);

    /* Make sure RX/TX are off while we (re)program rings. */
    mw8(R_CR, 0x00);

    /* Unlock config-register writes (some bits we touch live in
     * locked space until 9346CR is set to EEM_CONFIG). */
    mw8(R_9346CR, EEM_CONFIG);

    /* Enable the C+ command register — without RXCHKSUM/MULRW the
     * chip will refuse to honor the descriptor ring on most parts.
     * PCIDAC enables 64-bit DMA addressing path. */
    mw16(R_CPCR, CPCR_RXCHKSUM | CPCR_MULRW | CPCR_PCIDAC);

    /* Build descriptor rings. */
    tx_ring_init();
    rx_ring_init();

    /* Hand ring base addresses to the chip (must come before RX/TX
     * are enabled, and after CPCR — TNPDS/RDSAR are 8-byte aligned
     * for TX and 256-byte aligned for RX, both 64-bit registers). */
    uint64_t tx_phys = (uint64_t)(uintptr_t)tx_ring;
    uint64_t rx_phys = (uint64_t)(uintptr_t)rx_ring;
    mw32(R_TNPDS + 0, (uint32_t)(tx_phys & 0xFFFFFFFFu));
    mw32(R_TNPDS + 4, (uint32_t)(tx_phys >> 32));
    mw32(R_THPDS + 0, 0);     /* high-pri queue unused */
    mw32(R_THPDS + 4, 0);
    mw32(R_RDSAR + 0, (uint32_t)(rx_phys & 0xFFFFFFFFu));
    mw32(R_RDSAR + 4, (uint32_t)(rx_phys >> 32));

    /* Max RX packet size (12-bit field) — full 1518 + slack. */
    mw16(R_RMS, 1518 + 8);

    /* Max TX packet size in 128-byte units. */
    mw8(R_MTPS, 0x3F);

    /* RX config: accept broadcast/multicast/match, MXDMA unlimited.
     * No promisc, no error/runt acceptance. */
    mw32(R_RCR, RCR_AB | RCR_AM | RCR_APM | RCR_MXDMA_UNLIM);

    /* TX config: standard IFG, MXDMA unlimited. */
    mw32(R_TCR, TCR_IFG_STD | TCR_MXDMA_UNLIM);

    /* Re-lock config space. */
    mw8(R_9346CR, EEM_NONE);

    /* Disable interrupts (polling only). Ack any pending. */
    mw16(R_IMR, 0x0000);
    mw16(R_ISR, 0xFFFF);

    /* Enable RX + TX. */
    mw8(R_CR, CR_RE | CR_TE);

    wmb();

    ready = 1;

    kputs("rtl8169: ready, MAC=");
    static const char hx[] = "0123456789abcdef";
    for (int k = 0; k < 6; k++) {
        if (k) kputc(':');
        kputc(hx[(dev_mac.b[k] >> 4) & 0xf]);
        kputc(hx[dev_mac.b[k] & 0xf]);
    }
    kputs("\n");
    return 0;
}

int rtl8169_send(const void *frame, uint16_t len)
{
    if (!ready) return -1;
    if (len < 60) len = 60;            /* pad runt frames to 60 */
    if (len > TX_BUF_SIZE) return -1;

    int slot = tx_idx;
    struct rtl_desc *d = &tx_ring[slot];

    /* Wait for OWN to clear (chip done with previous use of this slot). */
    for (int i = 0; i < 100000; i++) {
        if (!(d->cmd & DESC_OWN)) break;
        for (volatile int z = 0; z < 50; z++);
    }
    if (d->cmd & DESC_OWN) return -1;   /* timeout */

    /* Copy + pad. */
    const uint8_t *src = (const uint8_t *)frame;
    uint8_t *dst = tx_buffers[slot];
    uint16_t copy = (len < TX_BUF_SIZE) ? len : TX_BUF_SIZE;
    for (uint16_t i = 0; i < copy; i++) dst[i] = src[i];
    for (uint16_t i = copy; i < 60; i++) dst[i] = 0;

    uint64_t buf = (uint64_t)(uintptr_t)dst;
    d->addr_lo = (uint32_t)(buf & 0xFFFFFFFFu);
    d->addr_hi = (uint32_t)(buf >> 32);
    d->vlan    = 0;

    /* Build cmd: OWN | FS | LS | (EOR if last in ring) | length. */
    uint32_t cmd = DESC_OWN | DESC_FS | DESC_LS | ((uint32_t)len & 0x3FFFu);
    if (slot == NUM_TX_DESC - 1) cmd |= DESC_EOR;

    wmb();
    d->cmd = cmd;
    wmb();

    /* Kick the normal-priority TX queue. */
    mw8(R_TPPOLL, TPPOLL_NPQ);

    tx_idx = (slot + 1) % NUM_TX_DESC;

    /* Best-effort completion poll — don't block forever. */
    for (int i = 0; i < 100000; i++) {
        if (!(d->cmd & DESC_OWN)) break;
        for (volatile int z = 0; z < 50; z++);
    }
    return 0;
}

int rtl8169_recv(void *frame, uint16_t max_len)
{
    if (!ready) return 0;

    struct rtl_desc *d = &rx_ring[rx_idx];

    /* OWN=0 means chip handed it back to us with a packet. */
    if (d->cmd & DESC_OWN) return 0;

    uint32_t cmd = d->cmd;
    uint16_t pkt_len = (uint16_t)(cmd & 0x3FFFu);

    /* CRC is stripped only when explicitly told to via certain RCR
     * bits; on 8169 it's left in. We strip 4 bytes of FCS here. */
    if (pkt_len >= 4) pkt_len -= 4;

    if (pkt_len > RX_BUF_SIZE) pkt_len = 0;

    uint16_t copy = (pkt_len > max_len) ? max_len : pkt_len;
    const uint8_t *src = rx_buffers[rx_idx];
    uint8_t *dst = (uint8_t *)frame;
    for (uint16_t i = 0; i < copy; i++) dst[i] = src[i];

    /* Re-arm this descriptor. */
    uint64_t buf = (uint64_t)(uintptr_t)rx_buffers[rx_idx];
    d->addr_lo = (uint32_t)(buf & 0xFFFFFFFFu);
    d->addr_hi = (uint32_t)(buf >> 32);
    d->vlan    = 0;
    uint32_t newcmd = DESC_OWN | (RX_BUF_SIZE & 0x3FFFu);
    if (rx_idx == NUM_RX_DESC - 1) newcmd |= DESC_EOR;
    wmb();
    d->cmd = newcmd;
    wmb();

    rx_idx = (rx_idx + 1) % NUM_RX_DESC;

    return (int)copy;
}

void rtl8169_get_mac(struct mac_addr *out)
{
    *out = dev_mac;
}
