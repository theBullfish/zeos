/*
 * Zeos -- Intel e1000/e1000e NIC driver
 *
 * Memory-mapped register interface for Intel Gigabit Ethernet.
 * Targets:
 *   - 82540EM  (0x100E) -- QEMU default e1000
 *   - 82574L   (0x10D3) -- common server NIC
 *   - I217-LM  (0x153A) -- Haswell integrated
 *   - I218-LM  (0x15A0) -- Broadwell integrated
 *   - I219-LM  (0x15A3) -- Skylake+ integrated (CN60 has this variant)
 *   - I219-V   (0x1570) -- consumer variant
 *
 * Polling-only for Alpha -- no interrupts.
 * Uses 16-entry descriptor rings for both TX and RX.
 *
 * KNOWN LIMITATION (QEMU 8.2.2):
 *   The 82540EM emulator has a non-deterministic RX delivery race under
 *   polling-only mode. DHCP DISCOVER goes out and OFFER comes back on the
 *   wire (verified via filter-dump pcap), but TPR (Total Packets Received)
 *   stays 0 in 7/8 runs even with correct ring discipline, RDH=0/RDT=N-1
 *   init, RDTR/RADV=0, RAL/RAH programmed, BAM/UPE/MPE set. RXDCTL writes
 *   are silently ignored on this model. Same flakiness on 82545EM/82544GC.
 *   virtio-net is 100% reliable; use it for QEMU testing.
 *   Real-hardware behavior (CN60 I219, server 82574L) is unaffected — this
 *   is purely a QEMU emulator quirk under polling. Wiring MSI-X (TODO when
 *   chain MSI-X migration lands) is the canonical fix.
 *   See net_chain.c for the chain integration; this driver is the
 *   hardware_dma backend only.
 */

#include "net_e1000.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "kprint.h"

/* ---- e1000 register offsets (from BAR0 MMIO base) ---- */

#define E1000_CTRL      0x0000  /* Device Control */
#define E1000_STATUS    0x0008  /* Device Status */
#define E1000_EERD      0x0014  /* EEPROM Read */
#define E1000_ICR       0x00C0  /* Interrupt Cause Read */
#define E1000_IMS       0x00D0  /* Interrupt Mask Set */
#define E1000_IMC       0x00D8  /* Interrupt Mask Clear */
#define E1000_RCTL      0x0100  /* Receive Control */
#define E1000_TCTL      0x0400  /* Transmit Control */
#define E1000_TIPG      0x0410  /* Transmit IPG */
#define E1000_RDBAL     0x2800  /* RX Descriptor Base Low */
#define E1000_RDBAH     0x2804  /* RX Descriptor Base High */
#define E1000_RDLEN     0x2808  /* RX Descriptor Length */
#define E1000_RDH       0x2810  /* RX Descriptor Head */
#define E1000_RDT       0x2818  /* RX Descriptor Tail */
#define E1000_TDBAL     0x3800  /* TX Descriptor Base Low */
#define E1000_TDBAH     0x3804  /* TX Descriptor Base High */
#define E1000_TDLEN     0x3808  /* TX Descriptor Length */
#define E1000_TDH       0x3810  /* TX Descriptor Head */
#define E1000_TDT       0x3818  /* TX Descriptor Tail */
#define E1000_RAL0      0x5400  /* Receive Address Low */
#define E1000_RAH0      0x5404  /* Receive Address High */
#define E1000_MTA       0x5200  /* Multicast Table Array (128 entries) */

/* CTRL register bits */
#define CTRL_SLU        (1 << 6)   /* Set Link Up */
#define CTRL_RST        (1 << 26)  /* Device Reset */
#define CTRL_ASDE       (1 << 5)   /* Auto-Speed Detection Enable */

/* STATUS register bits */
#define STATUS_LU       (1 << 1)   /* Link Up */

/* RCTL register bits */
#define RCTL_EN         (1 << 1)   /* Receiver Enable */
#define RCTL_SBP        (1 << 2)   /* Store Bad Packets */
#define RCTL_UPE        (1 << 3)   /* Unicast Promiscuous */
#define RCTL_MPE        (1 << 4)   /* Multicast Promiscuous */
#define RCTL_LBM_NONE   (0 << 6)   /* No Loopback */
#define RCTL_BAM        (1 << 15)  /* Broadcast Accept Mode */
#define RCTL_SECRC      (1 << 26)  /* Strip Ethernet CRC */
#define RCTL_BSIZE_2048 (0 << 16)  /* Buffer Size 2048 (default) */
#define RCTL_BSIZE_4096 ((3 << 16) | (1 << 25))  /* BSIZE=11 + BSEX */

/* TCTL register bits */
#define TCTL_EN         (1 << 1)   /* Transmitter Enable */
#define TCTL_PSP        (1 << 3)   /* Pad Short Packets */
#define TCTL_CT_SHIFT   4          /* Collision Threshold */
#define TCTL_COLD_SHIFT 12         /* Collision Distance */

/* TX descriptor command bits */
#define TCMD_EOP        (1 << 0)   /* End Of Packet */
#define TCMD_IFCS       (1 << 1)   /* Insert FCS/CRC */
#define TCMD_RS         (1 << 3)   /* Report Status */

/* Descriptor status bits */
#define DSTAT_DD        (1 << 0)   /* Descriptor Done */

/* EEPROM read bits */
#define EERD_START      (1 << 0)   /* Start Read */
#define EERD_DONE       (1 << 4)   /* Read Done */
#define EERD_ADDR_SHIFT 8          /* Address shift */
#define EERD_DATA_SHIFT 16         /* Data shift */

/* IPG recommended values for IEEE 802.3 */
#define TIPG_IPGT       10
#define TIPG_IPGR1      (10 << 10)
#define TIPG_IPGR2      (10 << 20)

/* ---- Descriptor structures ---- */

/* RX descriptor (legacy format, 16 bytes) */
struct e1000_rx_desc {
    uint64_t addr;          /* Buffer physical address */
    uint16_t length;        /* Received byte count */
    uint16_t checksum;      /* Packet checksum */
    uint8_t  status;        /* Descriptor status */
    uint8_t  errors;        /* Descriptor errors */
    uint16_t special;       /* VLAN tag */
} __attribute__((packed));

/* TX descriptor (legacy format, 16 bytes) */
struct e1000_tx_desc {
    uint64_t addr;          /* Buffer physical address */
    uint16_t length;        /* Data length */
    uint8_t  cso;           /* Checksum offset */
    uint8_t  cmd;           /* Command */
    uint8_t  status;        /* Status */
    uint8_t  css;           /* Checksum start */
    uint16_t special;       /* VLAN tag */
} __attribute__((packed));

/* ---- Ring configuration ---- */

#define E1000_NUM_RX_DESC  16
#define E1000_NUM_TX_DESC  16
#define E1000_RX_BUF_SIZE  2048

/* ---- Device state ---- */

static volatile uint8_t *mmio_base;     /* BAR0 mapped address */
static struct mac_addr   dev_mac;       /* Device MAC address */
static int               e1000_ready;   /* Driver initialized? */

/* Descriptor rings -- page-aligned for DMA */
static struct e1000_rx_desc rx_ring[E1000_NUM_RX_DESC]
    __attribute__((aligned(4096)));
static struct e1000_tx_desc tx_ring[E1000_NUM_TX_DESC]
    __attribute__((aligned(4096)));

/* Packet buffers */
static uint8_t rx_buffers[E1000_NUM_RX_DESC][E1000_RX_BUF_SIZE]
    __attribute__((aligned(16)));
static uint8_t tx_buffers[E1000_NUM_TX_DESC][NET_BUF_SIZE]
    __attribute__((aligned(16)));

/* Current ring positions */
static uint16_t rx_tail;
static uint16_t tx_tail;

/* ---- MMIO register access ---- */

static inline void e1000_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(mmio_base + reg) = val;
}

static inline uint32_t e1000_read(uint32_t reg)
{
    return *(volatile uint32_t *)(mmio_base + reg);
}

/* Memory barrier -- ensure writes are visible to hardware */
static inline void e1000_wmb(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

/* ---- EEPROM access ---- */

/*
 * Read a 16-bit word from the EEPROM via the EERD register.
 * Returns the word, or 0 on timeout.
 */
static uint16_t e1000_eeprom_read(uint8_t addr)
{
    e1000_write(E1000_EERD, ((uint32_t)addr << EERD_ADDR_SHIFT) | EERD_START);

    /* Poll for completion */
    for (int i = 0; i < 10000; i++) {
        uint32_t val = e1000_read(E1000_EERD);
        if (val & EERD_DONE)
            return (uint16_t)(val >> EERD_DATA_SHIFT);
    }

    return 0;  /* Timeout */
}

/* ---- MAC address detection ---- */

/*
 * Try to read MAC from EEPROM first (words 0-2).
 * If that fails, try RAL0/RAH0 registers directly
 * (e1000e variants like i219 often have MAC in registers).
 */
static void e1000_read_mac(void)
{
    /* Attempt EEPROM read */
    uint16_t w0 = e1000_eeprom_read(0);
    uint16_t w1 = e1000_eeprom_read(1);
    uint16_t w2 = e1000_eeprom_read(2);

    if (w0 != 0 || w1 != 0 || w2 != 0) {
        /* EEPROM had valid data */
        dev_mac.b[0] = (uint8_t)(w0 & 0xFF);
        dev_mac.b[1] = (uint8_t)(w0 >> 8);
        dev_mac.b[2] = (uint8_t)(w1 & 0xFF);
        dev_mac.b[3] = (uint8_t)(w1 >> 8);
        dev_mac.b[4] = (uint8_t)(w2 & 0xFF);
        dev_mac.b[5] = (uint8_t)(w2 >> 8);
        return;
    }

    /* Fallback: read from RAL0/RAH0 registers */
    uint32_t ral = e1000_read(E1000_RAL0);
    uint32_t rah = e1000_read(E1000_RAH0);

    if (ral != 0 || (rah & 0xFFFF) != 0) {
        dev_mac.b[0] = (uint8_t)(ral & 0xFF);
        dev_mac.b[1] = (uint8_t)((ral >> 8) & 0xFF);
        dev_mac.b[2] = (uint8_t)((ral >> 16) & 0xFF);
        dev_mac.b[3] = (uint8_t)((ral >> 24) & 0xFF);
        dev_mac.b[4] = (uint8_t)(rah & 0xFF);
        dev_mac.b[5] = (uint8_t)((rah >> 8) & 0xFF);
        return;
    }

    /* No MAC found -- leave as zeros */
    for (int i = 0; i < 6; i++)
        dev_mac.b[i] = 0;
}

/* ---- Receive ring setup ---- */

static void e1000_rx_init(void)
{
    /* Zero the descriptor ring */
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_ring[i].addr     = (uint64_t)(unsigned long)rx_buffers[i];
        rx_ring[i].length   = 0;
        rx_ring[i].checksum = 0;
        rx_ring[i].status   = 0;
        rx_ring[i].errors   = 0;
        rx_ring[i].special  = 0;
    }

    /* Set descriptor ring base address */
    uint64_t rx_phys = (uint64_t)(unsigned long)rx_ring;
    e1000_write(E1000_RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFF));
    e1000_write(E1000_RDBAH, (uint32_t)(rx_phys >> 32));

    /* Set descriptor ring length (in bytes) */
    e1000_write(E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(struct e1000_rx_desc));

    /* Head = 0, Tail = NUM-1 (all descriptors available to HW) */
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, E1000_NUM_RX_DESC - 1);
    rx_tail = E1000_NUM_RX_DESC - 1;

    /* Enable receiver:
     * - EN: receiver enable
     * - BAM: accept broadcast
     * - UPE: unicast promiscuous — accept unicast frames addressed to us.
     *   Under QEMU's e1000 model the RAL0/RAH0 exact-match unicast filter
     *   silently drops frames addressed to our own MAC (verified: broadcast
     *   DHCP frames arrive, but unicast ARP replies never enter the RX ring),
     *   which stalls every unicast flow — ARP resolution never completes so
     *   DNS/NTP/TCP can't send. We're a single-NIC endpoint, so accepting all
     *   unicast is correct behavior; the net_rx mac_filter chain node still
     *   drops frames not addressed to us before they reach the IP layer.
     * - MPE: multicast promiscuous (SLAAC / mDNS ingress).
     * - SECRC: strip CRC from incoming frames
     * - BSIZE 2048 (default, bits 16:17 = 00)
     */
    e1000_write(E1000_RCTL,
        RCTL_EN | RCTL_BAM | RCTL_UPE | RCTL_MPE | RCTL_SECRC |
        RCTL_BSIZE_2048 | RCTL_LBM_NONE);
}

/* ---- Transmit ring setup ---- */

static void e1000_tx_init(void)
{
    /* Zero the descriptor ring */
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_ring[i].addr    = 0;
        tx_ring[i].length  = 0;
        tx_ring[i].cso     = 0;
        tx_ring[i].cmd     = 0;
        tx_ring[i].status  = DSTAT_DD;  /* Mark as done (available) */
        tx_ring[i].css     = 0;
        tx_ring[i].special = 0;
    }

    /* Set descriptor ring base address */
    uint64_t tx_phys = (uint64_t)(unsigned long)tx_ring;
    e1000_write(E1000_TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFF));
    e1000_write(E1000_TDBAH, (uint32_t)(tx_phys >> 32));

    /* Set descriptor ring length */
    e1000_write(E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(struct e1000_tx_desc));

    /* Head = 0, Tail = 0 (no packets queued) */
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);
    tx_tail = 0;

    /* Set Inter-Packet Gap to recommended IEEE 802.3 values */
    e1000_write(E1000_TIPG, TIPG_IPGT | TIPG_IPGR1 | TIPG_IPGR2);

    /* Enable transmitter:
     * - EN: transmitter enable
     * - PSP: pad short packets to 64 bytes
     * - CT: collision threshold = 15
     * - COLD: collision distance = 64 (full-duplex)
     */
    e1000_write(E1000_TCTL,
        TCTL_EN | TCTL_PSP | (15 << TCTL_CT_SHIFT) | (64 << TCTL_COLD_SHIFT));
}

/* ---- Known Intel NIC device IDs ---- */

struct e1000_id {
    uint16_t device_id;
    const char *name;
};

static const struct e1000_id known_ids[] = {
    { 0x100E, "82540EM"  },   /* QEMU default e1000 */
    { 0x100F, "82545EM"  },   /* VMware */
    { 0x10D3, "82574L"   },   /* Common server NIC */
    { 0x1533, "I210"     },   /* Server/embedded */
    { 0x153A, "I217-LM"  },   /* Haswell */
    { 0x155A, "I218-V"   },   /* Broadwell consumer */
    { 0x15A0, "I218-LM"  },   /* Broadwell */
    { 0x15A1, "I218-V"   },   /* Broadwell consumer */
    { 0x15A2, "I219-LM"  },   /* Skylake */
    { 0x15A3, "I219-V"   },   /* Skylake consumer */
    { 0x1570, "I219-V"   },   /* Kaby Lake consumer */
    { 0x156F, "I219-LM"  },   /* Kaby Lake */
    { 0x15B7, "I219-LM"  },   /* Coffee Lake */
    { 0x15B8, "I219-V"   },   /* Coffee Lake consumer */
    { 0x15BC, "I219-V"   },   /* Whiskey Lake consumer */
    { 0x15BD, "I219-LM"  },   /* Whiskey Lake */
    { 0x15D7, "I219-LM"  },   /* Cannon Lake */
    { 0x15D8, "I219-V"   },   /* Cannon Lake consumer */
    { 0x15E3, "I219-LM"  },   /* Comet Lake */
    { 0x0D4E, "I219-LM"  },   /* Comet Lake-H */
    { 0x0D4F, "I219-V"   },   /* Comet Lake-H consumer */
    { 0, 0 }
};

/* ---- Public API ---- */

int e1000_init(void)
{
    e1000_ready = 0;

    /* Scan PCI for Intel NIC: vendor 0x8086, class 0x02 (network) */
    int count = pci_device_count();
    struct pci_device *nic = 0;
    const char *nic_name = "unknown";

    for (int i = 0; i < count; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d)
            continue;
        if (d->vendor_id != 0x8086)
            continue;
        if (d->class_code != 0x02)
            continue;

        /* Check against known device IDs */
        for (const struct e1000_id *id = known_ids; id->device_id; id++) {
            if (d->device_id == id->device_id) {
                nic = d;
                nic_name = id->name;
                break;
            }
        }
        if (nic)
            break;

        /* Accept any Intel class-0x02 device as a fallback */
        if (!nic) {
            nic = d;
            nic_name = "Intel NIC";
        }
    }

    if (!nic) {
        kputs("e1000: no Intel NIC found\n");
        return -1;
    }

    kputs("e1000: found ");
    kputs(nic_name);
    kputs(" [");
    kput_hex(nic->device_id);
    kputs("] at ");
    kput_dec(nic->bus);
    kputs(":");
    kput_dec(nic->dev);
    kputs(".");
    kput_dec(nic->func);
    kputs("\n");

    /* ---- Map BAR0 (memory-mapped I/O) ---- */
    uint32_t bar0 = nic->bar[0];
    if (bar0 & 1) {
        /* BAR0 is I/O space -- unexpected for e1000 MMIO */
        kputs("e1000: BAR0 is I/O space, not MMIO\n");
        return -1;
    }

    /* Mask low bits to get base address */
    uint64_t bar_addr = bar0 & 0xFFFFFFF0;

    /* Check if 64-bit BAR (type bits [2:1] == 10) */
    if ((bar0 & 0x06) == 0x04) {
        bar_addr |= ((uint64_t)nic->bar[1]) << 32;
    }

    mmio_base = (volatile uint8_t *)(unsigned long)bar_addr;

    kputs("e1000: MMIO base = 0x");
    kput_hex(bar_addr);
    kputs("\n");

    /* ---- Enable PCI bus mastering + memory space ---- */
    uint32_t cmd = pci_config_read32(nic->bus, nic->dev, nic->func, 0x04);
    cmd |= (1 << 2);  /* Bus Master Enable */
    cmd |= (1 << 1);  /* Memory Space Enable */
    pci_config_write32(nic->bus, nic->dev, nic->func, 0x04, cmd);

    /* ---- Reset the controller ---- */
    uint32_t ctrl = e1000_read(E1000_CTRL);
    e1000_write(E1000_CTRL, ctrl | CTRL_RST);

    /* Wait for reset to complete (RST bit self-clears) */
    for (int i = 0; i < 100000; i++) {
        if (!(e1000_read(E1000_CTRL) & CTRL_RST))
            break;
    }

    /* Small delay after reset */
    for (volatile int i = 0; i < 100000; i++);

    /* ---- Read MAC address ---- */
    e1000_read_mac();

    /* Program MAC into RAL0/RAH0 (receive filter) */
    uint32_t ral = (uint32_t)dev_mac.b[0]
                 | ((uint32_t)dev_mac.b[1] << 8)
                 | ((uint32_t)dev_mac.b[2] << 16)
                 | ((uint32_t)dev_mac.b[3] << 24);
    uint32_t rah = (uint32_t)dev_mac.b[4]
                 | ((uint32_t)dev_mac.b[5] << 8)
                 | (1 << 31);  /* AV (Address Valid) bit */
    e1000_write(E1000_RAL0, ral);
    e1000_write(E1000_RAH0, rah);

    /* ---- Clear multicast table ---- */
    for (int i = 0; i < 128; i++) {
        e1000_write(E1000_MTA + i * 4, 0);
    }

    /* ---- Disable all interrupts (polling only) ---- */
    e1000_write(E1000_IMC, 0xFFFFFFFF);
    e1000_read(E1000_ICR);  /* Clear any pending */

    /* ---- Set up RX ---- */
    e1000_rx_init();

    /* ---- Set up TX ---- */
    e1000_tx_init();

    /* ---- Force link up ---- */
    ctrl = e1000_read(E1000_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~CTRL_RST;
    e1000_write(E1000_CTRL, ctrl);

    /* Wait a bit for link */
    for (volatile int i = 0; i < 500000; i++);

    /* Check link status */
    uint32_t status = e1000_read(E1000_STATUS);
    if (status & STATUS_LU) {
        kputs("e1000: link UP\n");
    } else {
        kputs("e1000: link DOWN (may come up later)\n");
    }

    e1000_ready = 1;

    kputs("e1000: MAC ");
    for (int i = 0; i < 6; i++) {
        if (i > 0) kputs(":");
        uint8_t b = dev_mac.b[i];
        static const char hex[] = "0123456789abcdef";
        kputc(hex[(b >> 4) & 0xf]);
        kputc(hex[b & 0xf]);
    }
    kputs("\n");

    return 0;
}

int e1000_send(const void *frame, uint16_t len)
{
    if (!e1000_ready || len > NET_FRAME_MAX)
        return -1;

    /* Get the current TX descriptor */
    uint16_t idx = tx_tail;

    /* Wait for the descriptor to be available (DD set from previous use) */
    for (int i = 0; i < 100000; i++) {
        if (tx_ring[idx].status & DSTAT_DD)
            break;
    }
    if (!(tx_ring[idx].status & DSTAT_DD))
        return -1;  /* Timeout -- descriptor still busy */

    /* Copy frame data into the TX buffer */
    const uint8_t *src = (const uint8_t *)frame;
    uint8_t *dst = tx_buffers[idx];
    for (uint16_t i = 0; i < len; i++)
        dst[i] = src[i];

    /* Set up the descriptor */
    tx_ring[idx].addr    = (uint64_t)(unsigned long)tx_buffers[idx];
    tx_ring[idx].length  = len;
    tx_ring[idx].cmd     = TCMD_EOP | TCMD_RS | TCMD_IFCS;
    tx_ring[idx].status  = 0;
    tx_ring[idx].cso     = 0;
    tx_ring[idx].css     = 0;
    tx_ring[idx].special = 0;

    /* Advance tail -- this triggers the hardware to send */
    tx_tail = (tx_tail + 1) % E1000_NUM_TX_DESC;
    e1000_wmb();
    e1000_write(E1000_TDT, tx_tail);

    /* Poll for transmit completion (DD bit) */
    for (int i = 0; i < 100000; i++) {
        if (tx_ring[idx].status & DSTAT_DD)
            return 0;
    }

    /* Timeout -- frame may still be sent, but we can't confirm */
    return 0;
}

int e1000_recv(void *frame, uint16_t max_len)
{
    if (!e1000_ready)
        return 0;

    /* Check the next descriptor after tail */
    uint16_t idx = (rx_tail + 1) % E1000_NUM_RX_DESC;

    /* Is there a completed packet? */
    if (!(rx_ring[idx].status & DSTAT_DD))
        return 0;  /* Nothing available */

    /* Get frame length */
    uint16_t len = rx_ring[idx].length;
    if (len > max_len)
        len = max_len;

    /* Copy frame data out */
    const uint8_t *src = rx_buffers[idx];
    uint8_t *dst = (uint8_t *)frame;
    for (uint16_t i = 0; i < len; i++)
        dst[i] = src[i];

    /* Clear descriptor status and re-arm it for reuse */
    rx_ring[idx].status = 0;

    /* Advance tail -- tell hardware this descriptor is available again */
    rx_tail = idx;
    e1000_wmb();
    e1000_write(E1000_RDT, rx_tail);

    return (int)len;
}

void e1000_get_mac(struct mac_addr *mac)
{
    *mac = dev_mac;
}
