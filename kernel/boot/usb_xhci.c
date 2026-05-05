/*
 * Zeos -- xHCI 1.2 Host Controller Driver (polling)
 *
 * Minimal but real. No stubs:
 *   - PCI discovery (class 0x0C / sub 0x03 / prog-if 0x30)
 *   - Capability + Operational + Runtime + Doorbell window mapping
 *   - HCRST controller reset and Run/Stop start
 *   - Device Context Base Address Array
 *   - Scratchpad Buffer Array (if HCSPARAMS2.MaxScratchpadBufs > 0)
 *   - Command ring (single segment, 256 TRBs, link TRB at end)
 *   - Event ring + 1-entry ERST
 *   - Root-hub port scan + reset
 *   - Enable Slot + Address Device (Block Set Address = 0)
 *   - Re-fetch full 18B device descriptor via Control IN
 *
 * Reference: xHCI 1.2 spec, especially sections 4.2, 4.3, 4.6, 4.8, 4.9, 6.
 */

#include "usb_xhci.h"
#include "usb.h"
#include "pci.h"
#include "pmm.h"
#include "heap.h"
#include "kprint.h"
#include "vmm.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);

/* ── Capability register offsets ─────────────────────────────── */
#define XHCI_CAP_CAPLENGTH      0x00    /* u8  */
#define XHCI_CAP_HCIVERSION     0x02    /* u16 */
#define XHCI_CAP_HCSPARAMS1     0x04
#define XHCI_CAP_HCSPARAMS2     0x08
#define XHCI_CAP_HCSPARAMS3     0x0C
#define XHCI_CAP_HCCPARAMS1     0x10
#define XHCI_CAP_DBOFF          0x14
#define XHCI_CAP_RTSOFF         0x18
#define XHCI_CAP_HCCPARAMS2     0x1C

/* ── Operational register offsets (relative to op base) ──────── */
#define XHCI_OP_USBCMD          0x00
#define XHCI_OP_USBSTS          0x04
#define XHCI_OP_PAGESIZE        0x08
#define XHCI_OP_DNCTRL          0x14
#define XHCI_OP_CRCR            0x18    /* 64-bit */
#define XHCI_OP_DCBAAP          0x30    /* 64-bit */
#define XHCI_OP_CONFIG          0x38
#define XHCI_OP_PORTSC_BASE     0x400   /* +0x10 * (port - 1) */

#define XHCI_PORTSC(p)          (XHCI_OP_PORTSC_BASE + 0x10 * ((p) - 1))

/* USBCMD bits */
#define USBCMD_RS               (1U << 0)
#define USBCMD_HCRST            (1U << 1)
#define USBCMD_INTE             (1U << 2)
#define USBCMD_HSEE             (1U << 3)

/* USBSTS bits */
#define USBSTS_HCH              (1U << 0)   /* HC Halted */
#define USBSTS_HSE              (1U << 2)   /* Host System Error */
#define USBSTS_EINT             (1U << 3)   /* Event Interrupt */
#define USBSTS_PCD              (1U << 4)   /* Port Change Detect */
#define USBSTS_CNR              (1U << 11)  /* Controller Not Ready */
#define USBSTS_HCE              (1U << 12)

/* PORTSC bits */
#define PORTSC_CCS              (1U << 0)   /* Current Connect Status */
#define PORTSC_PED              (1U << 1)   /* Port Enabled/Disabled */
#define PORTSC_OCA              (1U << 3)
#define PORTSC_PR               (1U << 4)   /* Port Reset */
#define PORTSC_PP               (1U << 9)   /* Port Power */
#define PORTSC_PSPEED_SHIFT     10
#define PORTSC_PSPEED_MASK      (0xFU << 10)
#define PORTSC_CSC              (1U << 17)  /* Connect Status Change */
#define PORTSC_PEC              (1U << 18)
#define PORTSC_WRC              (1U << 19)
#define PORTSC_OCC              (1U << 20)
#define PORTSC_PRC              (1U << 21)  /* Port Reset Change */
#define PORTSC_PLC              (1U << 22)
#define PORTSC_CEC              (1U << 23)
/* Mask of RW1C bits we want to preserve / clear carefully when modifying */
#define PORTSC_RW1C_MASK        (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | \
                                 PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | \
                                 PORTSC_CEC)
/* PED is RW1C (write 1 to clear/disable) -- treat as preserve-by-zero */
#define PORTSC_PRESERVE_MASK    0x0E00C3E0U  /* PortSpeed | PIC | PLS | PP | bits we keep */

/* CRCR bits */
#define CRCR_RCS                (1ULL << 0)
#define CRCR_CS                 (1ULL << 1)
#define CRCR_CA                 (1ULL << 2)
#define CRCR_CRR                (1ULL << 3)

/* ── TRB types ───────────────────────────────────────────────── */
#define TRB_NORMAL              1
#define TRB_SETUP_STAGE         2
#define TRB_DATA_STAGE          3
#define TRB_STATUS_STAGE        4
#define TRB_LINK                6
#define TRB_EVENT_DATA          7
#define TRB_NOOP                8
#define TRB_ENABLE_SLOT         9
#define TRB_DISABLE_SLOT        10
#define TRB_ADDRESS_DEVICE      11
#define TRB_CONFIGURE_ENDPOINT  12
#define TRB_EVALUATE_CONTEXT    13
#define TRB_NOOP_CMD            23
#define TRB_TRANSFER_EVENT      32
#define TRB_COMMAND_COMPLETION  33
#define TRB_PORT_STATUS_CHANGE  34

#define TRB_TYPE(t)             (((uint32_t)(t)) << 10)
#define TRB_TYPE_OF(c)          ((((c) >> 10) & 0x3F))

/* TRB control flags */
#define TRB_C                   (1U << 0)   /* Cycle */
#define TRB_ENT                 (1U << 1)
#define TRB_NS                  (1U << 3)
#define TRB_CH                  (1U << 4)   /* Chain */
#define TRB_IOC                 (1U << 5)   /* Interrupt On Completion */
#define TRB_IDT                 (1U << 6)   /* Immediate Data */
#define TRB_TC                  (1U << 1)   /* (Link TRB) Toggle Cycle */
#define TRB_BSR                 (1U << 9)   /* Address Device: Block Set Address */

/* Transfer Type (for Setup/Status Stage TRB control word) */
#define TRT_NO_DATA             0
#define TRT_OUT_DATA            2
#define TRT_IN_DATA             3
#define TRT_TRT_SHIFT           16

/* Direction bit on Data Stage TRB */
#define TRB_DIR_IN              (1U << 16)

/* ── TRB structure ───────────────────────────────────────────── */
typedef struct {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

#define TRBS_PER_RING           256
#define EVENT_RING_TRBS         256

/* ── Event Ring Segment Table Entry ──────────────────────────── */
typedef struct {
    uint64_t base;
    uint32_t size;      /* Ring Segment Size, bits [15:0] */
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

/* ── Slot Context (32-byte version; 64-byte if HCCPARAMS1.CSZ=1) ── */
typedef struct {
    uint32_t dword0;    /* Route String, Speed, MTT, Hub, Context Entries */
    uint32_t dword1;    /* Max Exit Latency, Root Hub Port Number, Number of Ports */
    uint32_t dword2;    /* Parent Hub Slot ID etc, Interrupter Target */
    uint32_t dword3;    /* USB Device Address, Slot State */
    uint32_t reserved[4];
} __attribute__((packed)) xhci_slot_ctx_t;

/* ── Endpoint Context (32-byte version) ──────────────────────── */
typedef struct {
    uint32_t dword0;    /* EP State, Mult, MaxPStreams, LSA, Interval */
    uint32_t dword1;    /* CErr, EP Type, HID, Max Burst, Max Packet Size */
    uint32_t dword2;    /* TR Dequeue Pointer Lo + DCS */
    uint32_t dword3;    /* TR Dequeue Pointer Hi */
    uint32_t dword4;    /* Avg TRB Length, Max ESIT Payload Lo */
    uint32_t reserved[3];
} __attribute__((packed)) xhci_ep_ctx_t;

/* ── Input Control Context ───────────────────────────────────── */
typedef struct {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved[5];
    uint32_t configuration;
} __attribute__((packed)) xhci_input_ctrl_ctx_t;

/* ── Driver state ────────────────────────────────────────────── */
typedef struct {
    int present;

    /* PCI */
    uint8_t pci_bus, pci_dev, pci_func;

    /* Register windows (identity-mapped) */
    volatile uint8_t  *cap_base;
    volatile uint8_t  *op_base;
    volatile uint8_t  *rt_base;
    volatile uint8_t  *db_base;

    /* Capability snapshot */
    uint32_t hcsparams1;
    uint32_t hcsparams2;
    uint32_t hccparams1;
    uint8_t  max_slots;
    uint8_t  max_intrs;
    uint8_t  max_ports;
    uint8_t  ctx_size_64;   /* 1 if 64-byte contexts, else 0 (32-byte) */

    /* DCBAA */
    uint64_t *dcbaa;
    uint64_t  dcbaa_phys;

    /* Scratchpad */
    uint64_t *scratchpad_array;
    uint64_t  scratchpad_array_phys;
    int       num_scratchpads;

    /* Command Ring */
    xhci_trb_t *cmd_ring;
    uint64_t    cmd_ring_phys;
    uint32_t    cmd_enqueue;
    uint32_t    cmd_cycle;

    /* Event Ring */
    xhci_trb_t        *event_ring;
    uint64_t           event_ring_phys;
    xhci_erst_entry_t *erst;
    uint64_t           erst_phys;
    uint32_t           event_dequeue;
    uint32_t           event_cycle;

    /* Devices */
    struct xhci_device devices[XHCI_MAX_DEVICES];
    int dev_count;
} xhci_t;

static xhci_t xhci;

/* ── Register helpers ────────────────────────────────────────── */
static inline uint8_t cap_r8(uint32_t off)   { return *(volatile uint8_t  *)(xhci.cap_base + off); }
static inline uint16_t cap_r16(uint32_t off) { return *(volatile uint16_t *)(xhci.cap_base + off); }
static inline uint32_t cap_r32(uint32_t off) { return *(volatile uint32_t *)(xhci.cap_base + off); }

static inline uint32_t op_r32(uint32_t off)            { return *(volatile uint32_t *)(xhci.op_base + off); }
static inline void     op_w32(uint32_t off, uint32_t v){ *(volatile uint32_t *)(xhci.op_base + off) = v; }
static inline uint64_t op_r64(uint32_t off)
{
    uint32_t lo = *(volatile uint32_t *)(xhci.op_base + off);
    uint32_t hi = *(volatile uint32_t *)(xhci.op_base + off + 4);
    return ((uint64_t)hi << 32) | lo;
}
static inline void op_w64(uint32_t off, uint64_t v)
{
    *(volatile uint32_t *)(xhci.op_base + off) = (uint32_t)(v & 0xFFFFFFFFU);
    *(volatile uint32_t *)(xhci.op_base + off + 4) = (uint32_t)(v >> 32);
}

static inline uint32_t rt_r32(uint32_t off)            { return *(volatile uint32_t *)(xhci.rt_base + off); }
static inline void     rt_w32(uint32_t off, uint32_t v){ *(volatile uint32_t *)(xhci.rt_base + off) = v; }
static inline void     rt_w64(uint32_t off, uint64_t v)
{
    *(volatile uint32_t *)(xhci.rt_base + off) = (uint32_t)(v & 0xFFFFFFFFU);
    *(volatile uint32_t *)(xhci.rt_base + off + 4) = (uint32_t)(v >> 32);
}

static inline void db_ring(uint32_t slot, uint32_t target)
{
    *(volatile uint32_t *)(xhci.db_base + slot * 4) = target;
}

/* Interrupter 0 register block is at RTSOFF + 0x20 */
#define IR0_IMAN        0x20
#define IR0_IMOD        0x24
#define IR0_ERSTSZ      0x28
#define IR0_ERSTBA      0x30
#define IR0_ERDP        0x38

/* ── Spin / delay helpers ────────────────────────────────────── */
#define XHCI_POLL_LIMIT     50000000U

static void spin(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++) ;
}

/* ── PCI helpers ─────────────────────────────────────────────── */
static void pci_enable(uint8_t b, uint8_t d, uint8_t f)
{
    uint32_t cmd = pci_config_read32(b, d, f, 0x04);
    cmd |= (1 << 2) | (1 << 1);  /* Bus master + memory space */
    cmd &= ~(1 << 10);            /* Clear interrupt-disable to allow PCI access (we mask at IR level) */
    pci_config_write32(b, d, f, 0x04, cmd);
}

static uint64_t pci_read_bar64(uint8_t b, uint8_t d, uint8_t f, int idx)
{
    uint32_t lo = pci_config_read32(b, d, f, 0x10 + idx * 4);
    uint64_t addr;
    if ((lo & 0x06) == 0x04) {
        uint32_t hi = pci_config_read32(b, d, f, 0x10 + (idx + 1) * 4);
        addr = ((uint64_t)hi << 32) | (lo & ~0xFULL);
    } else {
        addr = lo & ~0xFULL;
    }
    return addr;
}

/* ── Allocate page-aligned cleared memory ───────────────────── */
static void *alloc_pages(int npages)
{
    uint64_t phys;
    if (npages == 1) {
        phys = pmm_alloc();
    } else {
        phys = pmm_alloc_contiguous(npages);
    }
    if (!phys) return 0;
    void *p = (void *)(uintptr_t)phys;
    memset(p, 0, (uint64_t)npages * 4096);
    return p;
}

/* ── Context helpers: handle 32 vs 64 byte context size ─────── */
static uint32_t ctx_size(void) { return xhci.ctx_size_64 ? 64 : 32; }

static xhci_input_ctrl_ctx_t *input_ctrl_of(void *input_ctx)
{
    return (xhci_input_ctrl_ctx_t *)input_ctx;
}
static xhci_slot_ctx_t *input_slot_of(void *input_ctx)
{
    return (xhci_slot_ctx_t *)((uint8_t *)input_ctx + ctx_size());
}
static xhci_ep_ctx_t *input_ep_of(void *input_ctx, int ep_index)
{
    /* Layout: [0] InputCtrl, [1] Slot, [2] DCI1=EP0, [3] DCI2, ...
     * ep_index 0 = EP0 (DCI 1), 1 = DCI 2 (EP1 OUT), 2 = DCI 3 (EP1 IN), ... */
    return (xhci_ep_ctx_t *)((uint8_t *)input_ctx + ctx_size() * (2 + ep_index));
}

/* ── Command ring ────────────────────────────────────────────── */
static void cmd_ring_init(void)
{
    xhci.cmd_ring = alloc_pages(1);
    xhci.cmd_ring_phys = (uint64_t)(uintptr_t)xhci.cmd_ring;
    xhci.cmd_enqueue = 0;
    xhci.cmd_cycle = 1;

    /* Last TRB is a Link back to start, with Toggle Cycle */
    xhci_trb_t *link = &xhci.cmd_ring[TRBS_PER_RING - 1];
    link->param_lo = (uint32_t)(xhci.cmd_ring_phys & 0xFFFFFFFFU);
    link->param_hi = (uint32_t)(xhci.cmd_ring_phys >> 32);
    link->status = 0;
    link->control = TRB_TYPE(TRB_LINK) | TRB_TC;  /* TC bit = toggle */
}

/* Generic ring for transfer rings (EP0). 1 page, 256 TRBs, link with TC. */
static void *xfer_ring_init(uint64_t *out_phys)
{
    xhci_trb_t *ring = alloc_pages(1);
    if (!ring) return 0;
    uint64_t phys = (uint64_t)(uintptr_t)ring;
    xhci_trb_t *link = &ring[TRBS_PER_RING - 1];
    link->param_lo = (uint32_t)(phys & 0xFFFFFFFFU);
    link->param_hi = (uint32_t)(phys >> 32);
    link->status = 0;
    link->control = TRB_TYPE(TRB_LINK) | TRB_TC;
    *out_phys = phys;
    return ring;
}

/* ── Event ring ──────────────────────────────────────────────── */
static int event_ring_init(void)
{
    xhci.event_ring = alloc_pages(1);
    if (!xhci.event_ring) return -1;
    xhci.event_ring_phys = (uint64_t)(uintptr_t)xhci.event_ring;
    xhci.event_dequeue = 0;
    xhci.event_cycle = 1;

    xhci.erst = alloc_pages(1);
    if (!xhci.erst) return -1;
    xhci.erst_phys = (uint64_t)(uintptr_t)xhci.erst;
    xhci.erst[0].base = xhci.event_ring_phys;
    xhci.erst[0].size = EVENT_RING_TRBS;
    xhci.erst[0].reserved = 0;

    /* Configure interrupter 0:
     *   ERSTSZ = 1 segment
     *   ERDP   = ring base (with EHB cleared)
     *   ERSTBA = ERST phys (must come after ERDP per spec; some HCs require last)
     */
    rt_w32(IR0_ERSTSZ, 1);
    rt_w64(IR0_ERDP, xhci.event_ring_phys);
    rt_w64(IR0_ERSTBA, xhci.erst_phys);
    /* Mask interrupter 0 (polling only); IMAN.IE = 0 */
    rt_w32(IR0_IMAN, rt_r32(IR0_IMAN) & ~0x2U);
    return 0;
}

/* Pop next event TRB (returns NULL if none ready). */
static xhci_trb_t *event_pop(void)
{
    xhci_trb_t *trb = &xhci.event_ring[xhci.event_dequeue];
    if ((trb->control & TRB_C) != xhci.event_cycle)
        return 0;
    /* Advance */
    xhci.event_dequeue++;
    if (xhci.event_dequeue >= EVENT_RING_TRBS) {
        xhci.event_dequeue = 0;
        xhci.event_cycle ^= 1;
    }
    /* Update ERDP -- write the dequeue ptr | EHB (bit 3) to clear it. */
    uint64_t erdp = xhci.event_ring_phys +
                    (uint64_t)xhci.event_dequeue * sizeof(xhci_trb_t);
    rt_w64(IR0_ERDP, erdp | (1ULL << 3));
    return trb;
}

/* Wait for an event of given type. Optionally match a specific command TRB
 * physical address (for command completions). Fills *out_event with a copy.
 * Returns 0 on success, -1 on timeout. */
static int wait_event(uint8_t want_type, uint64_t match_trb_phys,
                      xhci_trb_t *out_event)
{
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        xhci_trb_t *e = event_pop();
        if (!e) continue;
        uint8_t t = TRB_TYPE_OF(e->control);
        if (t != want_type) {
            /* Skip events we're not waiting for (e.g. Port Status Change
             * firing during enumeration). */
            continue;
        }
        if (match_trb_phys) {
            uint64_t evt_ptr = ((uint64_t)e->param_hi << 32) | e->param_lo;
            if (evt_ptr != match_trb_phys)
                continue;
        }
        if (out_event) *out_event = *e;
        return 0;
    }
    return -1;
}

/* Issue a command TRB. Returns the physical address it landed at, 0 on full. */
static uint64_t cmd_issue(uint32_t p_lo, uint32_t p_hi,
                          uint32_t status, uint32_t control)
{
    xhci_trb_t *slot = &xhci.cmd_ring[xhci.cmd_enqueue];
    /* Don't write into the link TRB */
    if (xhci.cmd_enqueue == TRBS_PER_RING - 1) {
        /* Toggle the link cycle so HW follows it, then jump back to 0 */
        xhci_trb_t *link = slot;
        if (xhci.cmd_cycle)
            link->control = (link->control & ~TRB_C) | TRB_C;
        else
            link->control &= ~TRB_C;
        /* Actually flip cycle properly */
        link->control = (TRB_TYPE(TRB_LINK) | TRB_TC) | (xhci.cmd_cycle ? TRB_C : 0);
        xhci.cmd_enqueue = 0;
        xhci.cmd_cycle ^= 1;
        slot = &xhci.cmd_ring[0];
    }
    slot->param_lo = p_lo;
    slot->param_hi = p_hi;
    slot->status = status;
    __asm__ volatile("" ::: "memory");
    slot->control = (control & ~TRB_C) | (xhci.cmd_cycle ? TRB_C : 0);
    __asm__ volatile("" ::: "memory");
    uint64_t phys = xhci.cmd_ring_phys +
                    (uint64_t)xhci.cmd_enqueue * sizeof(xhci_trb_t);
    xhci.cmd_enqueue++;
    if (xhci.cmd_enqueue >= TRBS_PER_RING - 1) {
        /* Wrap via link TRB next time */
    }
    /* Ring host controller doorbell 0, target 0 -> command ring */
    db_ring(0, 0);
    return phys;
}

/* Wait for Command Completion event matching given command phys address.
 * Returns completion code (1 = SUCCESS) and slot id via *out_slot. */
static int cmd_wait(uint64_t cmd_phys, int *out_slot)
{
    xhci_trb_t evt;
    if (wait_event(TRB_COMMAND_COMPLETION, cmd_phys, &evt) != 0) {
        kputs("[xhci] command timeout\n");
        return -1;
    }
    /* Completion code is bits [31:24] of status word */
    int code = (evt.status >> 24) & 0xFF;
    if (out_slot) *out_slot = (evt.control >> 24) & 0xFF;
    if (code != 1) {
        kputs("[xhci] cmd evt status=");
        kput_hex(evt.status);
        kputs(" ctrl=");
        kput_hex(evt.control);
        kputs("\n");
    }
    return code;
}

/* ── Scratchpad allocation ───────────────────────────────────── */
static int scratchpad_alloc(void)
{
    /* HCSPARAMS2: MaxScratchpadBufs = bits[31:27] (hi5) | bits[25:21] (lo5) */
    uint32_t s2 = xhci.hcsparams2;
    uint32_t hi = (s2 >> 21) & 0x1F;
    uint32_t lo = (s2 >> 27) & 0x1F;
    uint32_t total = (hi << 5) | lo;
    xhci.num_scratchpads = (int)total;
    if (total == 0) return 0;

    /* Array of pointers, one per scratchpad page, page-aligned. */
    xhci.scratchpad_array = alloc_pages(1);
    if (!xhci.scratchpad_array) return -1;
    xhci.scratchpad_array_phys = (uint64_t)(uintptr_t)xhci.scratchpad_array;

    for (uint32_t i = 0; i < total; i++) {
        uint64_t p = pmm_alloc();
        if (!p) return -1;
        memset((void *)(uintptr_t)p, 0, 4096);
        xhci.scratchpad_array[i] = p;
    }
    /* DCBAA[0] points to the scratchpad pointer array */
    xhci.dcbaa[0] = xhci.scratchpad_array_phys;
    return 0;
}

/* ── Controller halt + reset ─────────────────────────────────── */
static int xhci_halt(void)
{
    uint32_t cmd = op_r32(XHCI_OP_USBCMD);
    cmd &= ~USBCMD_RS;
    op_w32(XHCI_OP_USBCMD, cmd);

    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        if (op_r32(XHCI_OP_USBSTS) & USBSTS_HCH) return 0;
        spin(100);
    }
    return -1;
}

static int xhci_reset(void)
{
    /* Wait for CNR clear before issuing reset */
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        if (!(op_r32(XHCI_OP_USBSTS) & USBSTS_CNR)) break;
        spin(100);
    }
    op_w32(XHCI_OP_USBCMD, op_r32(XHCI_OP_USBCMD) | USBCMD_HCRST);
    /* Reset done when HCRST=0 and CNR=0 */
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        uint32_t cmd = op_r32(XHCI_OP_USBCMD);
        uint32_t sts = op_r32(XHCI_OP_USBSTS);
        if (!(cmd & USBCMD_HCRST) && !(sts & USBSTS_CNR))
            return 0;
        spin(100);
    }
    return -1;
}

/* ── Port reset ──────────────────────────────────────────────── */
static int port_reset(int port)
{
    uint32_t psc = op_r32(XHCI_PORTSC(port));
    if (!(psc & PORTSC_CCS)) return -1;

    /* USB3 ports auto-train; USB2 ports require us to assert PR.
     * Easiest: set PR for all (USB3 ports treat as warm reset). */
    /* Preserve PP and other R/W bits, clear RW1C bits we don't want to clear. */
    uint32_t v = (psc & ~PORTSC_RW1C_MASK) | PORTSC_PR;
    /* Clear PED (which is RW1C) by leaving its bit 0; clearing PR-related
     * RW1C bits explicitly: PRC (write 1) so the change-bit clears now. */
    v |= PORTSC_PRC;  /* clear stale PRC */
    op_w32(XHCI_PORTSC(port), v);

    /* Wait for PRC to be set again -> reset complete */
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        psc = op_r32(XHCI_PORTSC(port));
        if (psc & PORTSC_PRC) {
            /* Clear PRC */
            uint32_t w = (psc & ~PORTSC_RW1C_MASK) | PORTSC_PRC;
            op_w32(XHCI_PORTSC(port), w);
            /* Confirm enabled */
            if (op_r32(XHCI_PORTSC(port)) & PORTSC_PED)
                return 0;
            /* Some USB2 ports need a second poll to flag PED */
            for (uint32_t j = 0; j < 1000000; j++) {
                if (op_r32(XHCI_PORTSC(port)) & PORTSC_PED) return 0;
                spin(10);
            }
            return -1;
        }
        spin(100);
    }
    return -1;
}

static int port_speed(int port)
{
    uint32_t psc = op_r32(XHCI_PORTSC(port));
    return (int)((psc & PORTSC_PSPEED_MASK) >> PORTSC_PSPEED_SHIFT);
}

/* Translate xHCI port speed -> default Max Packet Size 0 */
static uint16_t default_mps0(int speed)
{
    switch (speed) {
        case USB_SPEED_LOW:        return 8;
        case USB_SPEED_FULL:       return 64;   /* will refine after first 8B descriptor read */
        case USB_SPEED_HIGH:       return 64;
        case USB_SPEED_SUPER:      return 512;  /* SS uses 2^9 */
        case USB_SPEED_SUPER_PLUS: return 512;
        default:                   return 8;
    }
}

/* ── Transfer ring: enqueue helpers ──────────────────────────── */
static uint64_t xfer_enqueue(struct xhci_device *dev,
                             uint32_t p_lo, uint32_t p_hi,
                             uint32_t status, uint32_t control)
{
    xhci_trb_t *ring = (xhci_trb_t *)dev->ep0_ring;

    if (dev->ep0_enqueue == TRBS_PER_RING - 1) {
        xhci_trb_t *link = &ring[TRBS_PER_RING - 1];
        link->control = (TRB_TYPE(TRB_LINK) | TRB_TC) | (dev->ep0_cycle ? TRB_C : 0);
        dev->ep0_enqueue = 0;
        dev->ep0_cycle ^= 1;
    }
    xhci_trb_t *slot = &ring[dev->ep0_enqueue];
    slot->param_lo = p_lo;
    slot->param_hi = p_hi;
    slot->status = status;
    slot->control = (control & ~TRB_C) | (dev->ep0_cycle ? TRB_C : 0);
    uint64_t phys = (uint64_t)(uintptr_t)ring +
                    (uint64_t)dev->ep0_enqueue * sizeof(xhci_trb_t);
    dev->ep0_enqueue++;
    return phys;
}

/* ── Control transfer on EP0 ─────────────────────────────────── */
int xhci_real_control_transfer(struct xhci_device *dev,
                          struct usb_setup_packet *setup,
                          void *buf, int max_len)
{
    if (!dev || !setup) return -1;

    int dir_in = (setup->bmRequestType & 0x80) ? 1 : 0;
    int data_len = setup->wLength;
    if (data_len > max_len) data_len = max_len;

    /* Setup Stage TRB (immediate data) */
    uint32_t s_lo = ((uint32_t)setup->bmRequestType) |
                    ((uint32_t)setup->bRequest << 8) |
                    ((uint32_t)setup->wValue << 16);
    uint32_t s_hi = ((uint32_t)setup->wIndex) |
                    ((uint32_t)setup->wLength << 16);
    uint32_t s_status = 8;  /* Transfer Length = 8 */
    uint32_t s_ctrl = TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT;
    uint32_t trt;
    if (data_len == 0)       trt = TRT_NO_DATA;
    else if (dir_in)         trt = TRT_IN_DATA;
    else                     trt = TRT_OUT_DATA;
    s_ctrl |= (trt << TRT_TRT_SHIFT);

    xfer_enqueue(dev, s_lo, s_hi, s_status, s_ctrl);

    /* Data Stage TRB (if any) */
    uint64_t buf_phys = buf ? (uint64_t)(uintptr_t)buf : 0;
    if (data_len > 0) {
        uint32_t d_status = (uint32_t)data_len;  /* TD Size left at 0 */
        uint32_t d_ctrl = TRB_TYPE(TRB_DATA_STAGE);
        if (dir_in) d_ctrl |= TRB_DIR_IN;
        xfer_enqueue(dev,
                     (uint32_t)(buf_phys & 0xFFFFFFFFU),
                     (uint32_t)(buf_phys >> 32),
                     d_status, d_ctrl);
    }

    /* Status Stage TRB. Direction is opposite of data stage. IOC set so
     * we get a Transfer Event. */
    uint32_t st_ctrl = TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC;
    /* Direction bit on status stage: 1=IN. If no data or OUT data => IN status */
    if (data_len == 0 || !dir_in) st_ctrl |= TRB_DIR_IN;
    uint64_t status_phys = xfer_enqueue(dev, 0, 0, 0, st_ctrl);

    /* Ring doorbell for this slot, target = 1 (EP0 / DCI 1) */
    db_ring(dev->slot_id, 1);

    /* Wait for Transfer Event matching status TRB */
    xhci_trb_t evt;
    if (wait_event(TRB_TRANSFER_EVENT, status_phys, &evt) != 0)
        return -1;
    int code = (evt.status >> 24) & 0xFF;
    if (code != 1 /* SUCCESS */ && code != 13 /* SHORT_PACKET */) {
        return -1;
    }
    int residual = (int)(evt.status & 0xFFFFFF);
    int xferred = data_len - residual;
    if (xferred < 0) xferred = 0;
    return xferred;
}

/* ── Configuration descriptor + SET_CONFIGURATION ────────────── */
int xhci_real_get_config_descriptor(struct xhci_device *dev, void *out, int max)
{
    if (!dev || !out || max <= 0) return -1;
    uint8_t hdr[9];
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    sp.bRequest = USB_REQ_GET_DESCRIPTOR;
    sp.wValue = (USB_DT_CONFIG << 8) | 0;
    sp.wIndex = 0;
    sp.wLength = 9;
    int g = xhci_real_control_transfer(dev, &sp, hdr, 9);
    if (g < 9) return -1;
    int total = hdr[2] | ((int)hdr[3] << 8);
    if (total > max) total = max;
    sp.wLength = (uint16_t)total;
    int got = xhci_real_control_transfer(dev, &sp, out, total);
    return got;
}

int xhci_real_set_configuration(struct xhci_device *dev, uint8_t config_value)
{
    if (!dev) return -1;
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    sp.bRequest = USB_REQ_SET_CONFIGURATION;
    sp.wValue = config_value;
    sp.wIndex = 0;
    sp.wLength = 0;
    int r = xhci_real_control_transfer(dev, &sp, 0, 0);
    if (r < 0) return -1;
    dev->configuration = config_value;
    return 0;
}

/* ── Bulk endpoint support ───────────────────────────────────── */
static struct xhci_bulk_ep *bulk_lookup(struct xhci_device *dev, uint8_t ep_addr)
{
    for (int i = 0; i < XHCI_MAX_BULK_EPS; i++) {
        if (dev->bulk[i].configured && dev->bulk[i].ep_addr == ep_addr)
            return &dev->bulk[i];
    }
    return 0;
}

static struct xhci_bulk_ep *bulk_alloc(struct xhci_device *dev)
{
    for (int i = 0; i < XHCI_MAX_BULK_EPS; i++) {
        if (!dev->bulk[i].configured) return &dev->bulk[i];
    }
    return 0;
}

static uint64_t bulk_ring_enqueue(struct xhci_bulk_ep *be,
                                  uint32_t p_lo, uint32_t p_hi,
                                  uint32_t status, uint32_t control)
{
    xhci_trb_t *ring = (xhci_trb_t *)be->ring;
    if (be->enqueue == TRBS_PER_RING - 1) {
        xhci_trb_t *link = &ring[TRBS_PER_RING - 1];
        link->control = (TRB_TYPE(TRB_LINK) | TRB_TC) | (be->cycle ? TRB_C : 0);
        be->enqueue = 0;
        be->cycle ^= 1;
    }
    xhci_trb_t *slot = &ring[be->enqueue];
    slot->param_lo = p_lo;
    slot->param_hi = p_hi;
    slot->status = status;
    __asm__ volatile("" ::: "memory");
    slot->control = (control & ~TRB_C) | (be->cycle ? TRB_C : 0);
    __asm__ volatile("" ::: "memory");
    uint64_t phys = be->ring_phys + (uint64_t)be->enqueue * sizeof(xhci_trb_t);
    be->enqueue++;
    return phys;
}

int xhci_real_setup_bulk_endpoint(struct xhci_device *dev,
                             uint8_t ep_addr, uint16_t max_packet)
{
    if (!dev || dev->slot_id == 0 || max_packet == 0) return -1;
    if (bulk_lookup(dev, ep_addr)) return 0;

    int is_in = (ep_addr & 0x80) ? 1 : 0;
    int epnum = ep_addr & 0x0F;
    if (epnum == 0 || epnum > 15) return -1;
    int dci = epnum * 2 + (is_in ? 1 : 0);
    if (dci >= 32) return -1;

    struct xhci_bulk_ep *be = bulk_alloc(dev);
    if (!be) return -1;

    uint64_t ring_phys;
    void *ring = xfer_ring_init(&ring_phys);
    if (!ring) return -1;
    be->ring = ring;
    be->ring_phys = ring_phys;
    be->enqueue = 0;
    be->cycle = 1;
    be->ep_addr = ep_addr;
    be->dci = (uint8_t)dci;
    be->max_packet = max_packet;
    be->pending_trb_phys = 0;
    be->pending_buf = 0;
    be->pending_len = 0;

    int max_dci = 1;
    if (dev->int_in.configured && dev->int_in.dci > max_dci) max_dci = dev->int_in.dci;
    for (int i = 0; i < XHCI_MAX_BULK_EPS; i++) {
        if (dev->bulk[i].configured && dev->bulk[i].dci > max_dci)
            max_dci = dev->bulk[i].dci;
    }
    if (dci > max_dci) max_dci = dci;

    xhci_input_ctrl_ctx_t *icc = input_ctrl_of(dev->input_ctx);
    icc->drop_flags = 0;
    icc->add_flags = (1U << 0) | (1U << dci);

    xhci_slot_ctx_t *slot = input_slot_of(dev->input_ctx);
    slot->dword0 = ((uint32_t)dev->speed << 20) | ((uint32_t)max_dci << 27);
    slot->dword1 = ((uint32_t)dev->port << 16);
    slot->dword2 = 0;
    slot->dword3 = 0;

    int ep_index = dci - 1;
    xhci_ep_ctx_t *epc = input_ep_of(dev->input_ctx, ep_index);
    memset(epc, 0, sizeof(*epc));
    uint32_t ep_type = is_in ? 6 : 2;  /* Bulk IN=6, Bulk OUT=2 */
    epc->dword1 = (3U << 1) | (ep_type << 3) | ((uint32_t)max_packet << 16);
    epc->dword2 = (uint32_t)(ring_phys & 0xFFFFFFF0U) | 1U;
    epc->dword3 = (uint32_t)(ring_phys >> 32);
    epc->dword4 = max_packet;

    uint64_t in_phys = (uint64_t)(uintptr_t)dev->input_ctx;
    uint32_t cctrl = TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
                     ((uint32_t)dev->slot_id << 24);
    uint64_t cmd_phys = cmd_issue((uint32_t)(in_phys & 0xFFFFFFFFU),
                                  (uint32_t)(in_phys >> 32),
                                  0, cctrl);
    int code = cmd_wait(cmd_phys, 0);
    if (code != 1) {
        kputs("[xhci] Configure Endpoint (bulk) failed code=");
        kput_hex(code);
        kputs(" ep=");
        kput_hex(ep_addr);
        kputs("\n");
        return -1;
    }
    be->configured = 1;
    return 0;
}

int xhci_real_bulk_transfer(struct xhci_device *dev, int ep_addr,
                       void *buf, int len, int in)
{
    if (!dev || dev->slot_id == 0 || len < 0) return -1;
    int is_in = ((uint8_t)ep_addr & 0x80) ? 1 : 0;
    if (is_in != (in ? 1 : 0)) return -1;

    struct xhci_bulk_ep *be = bulk_lookup(dev, (uint8_t)ep_addr);
    if (!be) return -1;
    if (len > 0 && !buf) return -1;

    if (be->pending_trb_phys) {
        xhci_trb_t evt;
        if (wait_event(TRB_TRANSFER_EVENT, be->pending_trb_phys, &evt) == 0)
            be->pending_trb_phys = 0;
    }

    if (len == 0) {
        uint64_t trb_phys = bulk_ring_enqueue(be, 0, 0, 0,
                                              TRB_TYPE(TRB_NORMAL) | TRB_IOC);
        db_ring(dev->slot_id, be->dci);
        xhci_trb_t evt;
        if (wait_event(TRB_TRANSFER_EVENT, trb_phys, &evt) != 0) return -1;
        int code = (evt.status >> 24) & 0xFF;
        return (code == 1 || code == 13) ? 0 : -1;
    }

    uint64_t buf_phys = (uint64_t)(uintptr_t)buf;
    int remaining = len;
    uint64_t last_trb_phys = 0;
    const int CHUNK_MAX = 65536;
    while (remaining > 0) {
        int this_chunk = remaining > CHUNK_MAX ? CHUNK_MAX : remaining;
        int is_last = (remaining - this_chunk == 0);
        uint32_t status = (uint32_t)this_chunk;
        uint32_t ctrl = TRB_TYPE(TRB_NORMAL);
        if (is_last) ctrl |= TRB_IOC;
        else         ctrl |= TRB_CH;
        uint64_t trb_phys = bulk_ring_enqueue(be,
                                              (uint32_t)(buf_phys & 0xFFFFFFFFU),
                                              (uint32_t)(buf_phys >> 32),
                                              status, ctrl);
        if (is_last) last_trb_phys = trb_phys;
        buf_phys += this_chunk;
        remaining -= this_chunk;
    }

    db_ring(dev->slot_id, be->dci);

    xhci_trb_t evt;
    if (wait_event(TRB_TRANSFER_EVENT, last_trb_phys, &evt) != 0) {
        kputs("[xhci] bulk transfer timeout ep=");
        kput_hex(ep_addr);
        kputs("\n");
        return -1;
    }
    int code = (evt.status >> 24) & 0xFF;
    if (code != 1 && code != 13) return -1;
    int residual = (int)(evt.status & 0xFFFFFF);
    int xferred = len - residual;
    if (xferred < 0) xferred = 0;
    return xferred;
}

int xhci_real_bulk_poll_in(struct xhci_device *dev, int ep_addr, void *buf, int len)
{
    if (!dev || !buf || len <= 0) return -1;
    if (!((uint8_t)ep_addr & 0x80)) return -1;
    struct xhci_bulk_ep *be = bulk_lookup(dev, (uint8_t)ep_addr);
    if (!be) return -1;

    if (!be->pending_trb_phys) {
        uint64_t bp = (uint64_t)(uintptr_t)buf;
        uint32_t status = (uint32_t)len;
        uint32_t ctrl = TRB_TYPE(TRB_NORMAL) | TRB_IOC;
        be->pending_trb_phys = bulk_ring_enqueue(be,
                                                 (uint32_t)(bp & 0xFFFFFFFFU),
                                                 (uint32_t)(bp >> 32),
                                                 status, ctrl);
        be->pending_buf = buf;
        be->pending_len = len;
        db_ring(dev->slot_id, be->dci);
        return 0;
    }

    for (int i = 0; i < 64; i++) {
        xhci_trb_t *e = event_pop();
        if (!e) return 0;
        if (TRB_TYPE_OF(e->control) != TRB_TRANSFER_EVENT) continue;
        uint64_t evt_ptr = ((uint64_t)e->param_hi << 32) | e->param_lo;
        if (evt_ptr != be->pending_trb_phys) continue;
        int code = (e->status >> 24) & 0xFF;
        int residual = (int)(e->status & 0xFFFFFF);
        int armed = be->pending_len ? be->pending_len : len;
        be->pending_trb_phys = 0;
        be->pending_buf = 0;
        be->pending_len = 0;
        if (code != 1 && code != 13) return -1;
        int x = armed - residual;
        if (x < 0) x = 0;
        if (x > len) x = len;
        return x;
    }
    return 0;
}

/* Interrupt-IN endpoint stubs — HID driver provides real impls. */
/* ── Interrupt-IN endpoint support ───────────────────────────── */

static uint64_t int_ring_enqueue(struct xhci_device *dev,
                                 uint32_t p_lo, uint32_t p_hi,
                                 uint32_t status, uint32_t control)
{
    struct xhci_int_ep *ie = &dev->int_in;
    xhci_trb_t *ring = (xhci_trb_t *)ie->ring;
    if (ie->enqueue == TRBS_PER_RING - 1) {
        xhci_trb_t *link = &ring[TRBS_PER_RING - 1];
        link->control = (TRB_TYPE(TRB_LINK) | TRB_TC) | (ie->cycle ? TRB_C : 0);
        ie->enqueue = 0;
        ie->cycle ^= 1;
    }
    xhci_trb_t *slot = &ring[ie->enqueue];
    slot->param_lo = p_lo;
    slot->param_hi = p_hi;
    slot->status = status;
    __asm__ volatile("" ::: "memory");
    slot->control = (control & ~TRB_C) | (ie->cycle ? TRB_C : 0);
    __asm__ volatile("" ::: "memory");
    uint64_t phys = ie->ring_phys + (uint64_t)ie->enqueue * sizeof(xhci_trb_t);
    ie->enqueue++;
    return phys;
}

static void int_ep_arm(struct xhci_device *dev)
{
    struct xhci_int_ep *ie = &dev->int_in;
    uint64_t buf_phys = (uint64_t)(uintptr_t)ie->pending_buf;
    uint32_t status = (uint32_t)ie->pending_len;
    uint32_t ctrl = TRB_TYPE(TRB_NORMAL) | TRB_IOC;
    uint64_t trb_phys = int_ring_enqueue(dev,
                                         (uint32_t)(buf_phys & 0xFFFFFFFFU),
                                         (uint32_t)(buf_phys >> 32),
                                         status, ctrl);
    ie->pending_trb_phys = trb_phys;
    db_ring(dev->slot_id, ie->dci);
}

int xhci_real_setup_interrupt_in(struct xhci_device *dev,
                            uint8_t ep_addr, uint16_t max_packet,
                            uint8_t interval)
{
    if (!dev || dev->slot_id == 0) {
        kputs("[xhci] int_in: bad dev/slot\n");
        return -1;
    }
    if (!(ep_addr & 0x80)) {
        kputs("[xhci] int_in: ep not IN\n");
        return -1;
    }
    int epnum = ep_addr & 0x0F;
    int dci = epnum * 2 + 1;
    if (dci >= 32) return -1;

    struct xhci_int_ep *ie = &dev->int_in;
    if (ie->configured) {
        kputs("[xhci] int_in: already configured\n");
        return -1;
    }

    uint64_t ring_phys;
    void *ring = xfer_ring_init(&ring_phys);
    if (!ring) {
        kputs("[xhci] int_in: ring alloc failed\n");
        return -1;
    }
    ie->ring = ring;
    ie->ring_phys = ring_phys;
    ie->enqueue = 0;
    ie->cycle = 1;
    ie->ep_addr = ep_addr;
    ie->dci = (uint8_t)dci;
    ie->max_packet = max_packet;
    ie->interval = interval;
    ie->pending_trb_phys = 0;
    ie->pending_buf = 0;
    ie->pending_len = 0;

    /* Compute highest DCI in use across this device's EPs. */
    int max_dci = dci;
    for (int i = 0; i < XHCI_MAX_BULK_EPS; i++) {
        if (dev->bulk[i].configured && dev->bulk[i].dci > max_dci)
            max_dci = dev->bulk[i].dci;
    }

    xhci_input_ctrl_ctx_t *icc = input_ctrl_of(dev->input_ctx);
    icc->drop_flags = 0;
    icc->add_flags = (1U << 0) | (1U << dci);

    xhci_slot_ctx_t *slot = input_slot_of(dev->input_ctx);
    slot->dword0 = ((uint32_t)dev->speed << 20) | ((uint32_t)max_dci << 27);
    slot->dword1 = ((uint32_t)dev->port << 16);
    slot->dword2 = 0;
    slot->dword3 = 0;

    int ep_index = dci - 1;
    xhci_ep_ctx_t *ep = input_ep_of(dev->input_ctx, ep_index);
    memset(ep, 0, sizeof(*ep));
    /* Interrupt IN: EP Type = 7 */
    ep->dword0 = ((uint32_t)interval << 16);
    ep->dword1 = (3U << 1) | (7U << 3) | ((uint32_t)max_packet << 16);
    ep->dword2 = (uint32_t)(ring_phys & 0xFFFFFFF0U) | 1U;
    ep->dword3 = (uint32_t)(ring_phys >> 32);
    ep->dword4 = max_packet;

    uint64_t in_phys = (uint64_t)(uintptr_t)dev->input_ctx;
    uint32_t cctrl = TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
                     ((uint32_t)dev->slot_id << 24);
    uint64_t cmd_phys = cmd_issue((uint32_t)(in_phys & 0xFFFFFFFFU),
                                  (uint32_t)(in_phys >> 32),
                                  0, cctrl);
    int code = cmd_wait(cmd_phys, 0);
    if (code != 1) {
        kputs("[xhci] Configure Endpoint (int) failed code=");
        kput_hex(code);
        kputs(" ep=");
        kput_hex(ep_addr);
        kputs("\n");
        return -1;
    }
    ie->configured = 1;
    return 0;
}

int xhci_real_interrupt_poll(struct xhci_device *dev, void *buf, int len)
{
    if (!dev || !buf || len <= 0) return -1;
    struct xhci_int_ep *ie = &dev->int_in;
    if (!ie->configured) return -1;

    if (ie->pending_trb_phys == 0) {
        ie->pending_buf = buf;
        ie->pending_len = len;
        int_ep_arm(dev);
        return 0;
    }

    for (int i = 0; i < 16; i++) {
        xhci_trb_t *e = event_pop();
        if (!e) return 0;
        if (TRB_TYPE_OF(e->control) != TRB_TRANSFER_EVENT) continue;
        uint64_t evt_ptr = ((uint64_t)e->param_hi << 32) | e->param_lo;
        if (evt_ptr != ie->pending_trb_phys) continue;
        int code = (e->status >> 24) & 0xFF;
        if (code != 1 && code != 13) {
            ie->pending_trb_phys = 0;
            return -1;
        }
        int residual = (int)(e->status & 0xFFFFFF);
        int xferred = ie->pending_len - residual;
        if (xferred < 0) xferred = 0;
        if (xferred > len) xferred = len;
        if (ie->pending_buf != buf && xferred > 0)
            memcpy(buf, ie->pending_buf, xferred);
        ie->pending_buf = buf;
        ie->pending_len = len;
        ie->pending_trb_phys = 0;
        int_ep_arm(dev);
        return xferred;
    }
    return 0;
}

int xhci_real_interrupt_transfer(struct xhci_device *dev, void *buf, int len)
{
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        int r = xhci_real_interrupt_poll(dev, buf, len);
        if (r != 0) return r;
    }
    return -1;
}

/* ── Isochronous-IN endpoint support ─────────────────────────────
 * xHCI 1.2 §4.10.3.1: Isoch transfers use TRB type 5 (Isoch).
 * EP Type for Isoch IN is 5; Mult field is in dword0[9:8] (HS/SS hi-bw),
 * SIA (Start Iso ASAP) bit is dword3[31].
 *
 * Driver pattern:
 *   - Allocate a ring of N TRBs and N receive buffers.
 *   - Arm every TRB with TRB_ISOCH | IOC | SIA, ring doorbell.
 *   - On each completion event, invoke the callback with the residual
 *     length and re-arm the same TRB slot.
 * The xfer ring already has a Link TRB at slot 255 with TC bit; iso
 * shares that ring discipline. We only use the first XHCI_ISO_RING_TRBS
 * slots so the link TRB is never touched.
 */

#define TRB_ISOCH               5
#define TRB_SIA                 (1U << 31)
#define TRB_TYPE_STOP_ENDPOINT  15

static uint64_t iso_ring_enqueue(struct xhci_device *dev, int slot_idx,
                                 uint32_t p_lo, uint32_t p_hi,
                                 uint32_t status, uint32_t control)
{
    struct xhci_iso_ep *ie = &dev->iso_in;
    xhci_trb_t *ring = (xhci_trb_t *)ie->ring;
    xhci_trb_t *slot = &ring[slot_idx];
    slot->param_lo = p_lo;
    slot->param_hi = p_hi;
    slot->status = status;
    __asm__ volatile("" ::: "memory");
    /* All slots use the initial cycle (1) for the first lap. We never
     * re-write a slot during a single lap because re-arm uses the same
     * slot index after a completion; the cycle bit is then flipped on
     * each subsequent lap. */
    slot->control = (control & ~TRB_C) | (ie->cycle ? TRB_C : 0);
    __asm__ volatile("" ::: "memory");
    uint64_t phys = ie->ring_phys + (uint64_t)slot_idx * sizeof(xhci_trb_t);
    ie->trb_phys[slot_idx] = phys;
    ie->trb_armed[slot_idx] = 1;
    return phys;
}

static int iso_arm_one(struct xhci_device *dev, int slot_idx)
{
    struct xhci_iso_ep *ie = &dev->iso_in;
    void *buf = ie->trb_buf[slot_idx];
    if (!buf) return -1;
    uint64_t bp = (uint64_t)(uintptr_t)buf;
    uint32_t armed = (uint32_t)ie->max_packet * ie->pkts_per_frame;
    /* TBC=0 (1 burst); TLBPC=0; SIA=1 (Start Iso ASAP). */
    uint32_t ctrl = TRB_TYPE(TRB_ISOCH) | TRB_IOC | TRB_SIA;
    iso_ring_enqueue(dev, slot_idx,
                     (uint32_t)(bp & 0xFFFFFFFFU),
                     (uint32_t)(bp >> 32),
                     armed, ctrl);
    return 0;
}

int xhci_real_iso_setup(struct xhci_device *dev, uint8_t ep_addr,
                        uint16_t mps, uint8_t pkts_per_frame,
                        uint8_t interval, xhci_iso_cb_t cb, void *user)
{
    if (!dev || dev->slot_id == 0) return -1;
    if (!(ep_addr & 0x80)) return -1;
    if (!cb || mps == 0) return -1;
    if (pkts_per_frame == 0) pkts_per_frame = 1;
    if (pkts_per_frame > XHCI_ISO_MAX_PKTS_PER_FRAME)
        pkts_per_frame = XHCI_ISO_MAX_PKTS_PER_FRAME;

    int epnum = ep_addr & 0x0F;
    int dci = epnum * 2 + 1;
    if (dci >= 32) return -1;

    struct xhci_iso_ep *ie = &dev->iso_in;
    if (ie->configured) return -1;

    uint64_t ring_phys;
    void *ring = xfer_ring_init(&ring_phys);
    if (!ring) return -1;
    ie->ring = ring;
    ie->ring_phys = ring_phys;
    ie->enqueue = 0;
    ie->cycle = 1;
    ie->ep_addr = ep_addr;
    ie->dci = (uint8_t)dci;
    ie->max_packet = mps;
    ie->pkts_per_frame = pkts_per_frame;
    ie->interval = interval;
    ie->cb = cb;
    ie->cb_user = user;

    /* Buffer pool: per-slot capacity = mps * pkts_per_frame, capped. */
    uint32_t per = (uint32_t)mps * pkts_per_frame;
    if (per > XHCI_ISO_MAX_PKT) per = XHCI_ISO_MAX_PKT;
    uint32_t total = per * XHCI_ISO_RING_TRBS;
    uint32_t pages = (total + 4095) / 4096;
    if (pages == 0) pages = 1;
    ie->bufpool = alloc_pages(pages);
    if (!ie->bufpool) return -1;
    memset(ie->bufpool, 0, pages * 4096);
    for (int i = 0; i < XHCI_ISO_RING_TRBS; i++) {
        ie->trb_buf[i] = (uint8_t *)ie->bufpool + (uint64_t)i * per;
        ie->trb_armed[i] = 0;
        ie->trb_phys[i] = 0;
    }

    /* Configure Endpoint. */
    int max_dci = dci;
    if (dev->int_in.configured && dev->int_in.dci > max_dci) max_dci = dev->int_in.dci;
    for (int i = 0; i < XHCI_MAX_BULK_EPS; i++) {
        if (dev->bulk[i].configured && dev->bulk[i].dci > max_dci)
            max_dci = dev->bulk[i].dci;
    }

    xhci_input_ctrl_ctx_t *icc = input_ctrl_of(dev->input_ctx);
    icc->drop_flags = 0;
    icc->add_flags = (1U << 0) | (1U << dci);

    xhci_slot_ctx_t *slot = input_slot_of(dev->input_ctx);
    slot->dword0 = ((uint32_t)dev->speed << 20) | ((uint32_t)max_dci << 27);
    slot->dword1 = ((uint32_t)dev->port << 16);
    slot->dword2 = 0;
    slot->dword3 = 0;

    int ep_index = dci - 1;
    xhci_ep_ctx_t *ep = input_ep_of(dev->input_ctx, ep_index);
    memset(ep, 0, sizeof(*ep));
    /* Isoch IN: EP Type = 5. Mult = pkts_per_frame - 1 (HS/SS hi-bw). */
    uint32_t mult = (pkts_per_frame > 0) ? (uint32_t)(pkts_per_frame - 1) : 0;
    ep->dword0 = ((uint32_t)interval << 16) | (mult << 8);
    /* CErr = 0 for iso (spec). */
    ep->dword1 = (5U << 3) | ((uint32_t)mps << 16);
    ep->dword2 = (uint32_t)(ring_phys & 0xFFFFFFF0U) | 1U;
    ep->dword3 = (uint32_t)(ring_phys >> 32);
    uint32_t esit = (uint32_t)mps * pkts_per_frame;
    ep->dword4 = ((esit & 0xFFFF) << 16) | (uint32_t)mps;

    uint64_t in_phys = (uint64_t)(uintptr_t)dev->input_ctx;
    uint32_t cctrl = TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
                     ((uint32_t)dev->slot_id << 24);
    uint64_t cmd_phys = cmd_issue((uint32_t)(in_phys & 0xFFFFFFFFU),
                                  (uint32_t)(in_phys >> 32),
                                  0, cctrl);
    int code = cmd_wait(cmd_phys, 0);
    if (code != 1) {
        kputs("[xhci] iso ConfigureEndpoint failed code=");
        kput_hex(code);
        kputs("\n");
        return -1;
    }
    ie->configured = 1;
    return 0;
}

int xhci_real_iso_start(struct xhci_device *dev)
{
    if (!dev) return -1;
    struct xhci_iso_ep *ie = &dev->iso_in;
    if (!ie->configured || ie->running) return -1;
    for (int i = 0; i < XHCI_ISO_RING_TRBS; i++) {
        if (iso_arm_one(dev, i) != 0) return -1;
    }
    ie->running = 1;
    db_ring(dev->slot_id, ie->dci);
    return 0;
}

int xhci_real_iso_stop(struct xhci_device *dev)
{
    if (!dev) return -1;
    struct xhci_iso_ep *ie = &dev->iso_in;
    if (!ie->configured || !ie->running) return 0;
    uint32_t cctrl = TRB_TYPE(TRB_TYPE_STOP_ENDPOINT) |
                     ((uint32_t)ie->dci << 16) |
                     ((uint32_t)dev->slot_id << 24);
    uint64_t cmd_phys = cmd_issue(0, 0, 0, cctrl);
    (void)cmd_wait(cmd_phys, 0);
    ie->running = 0;
    return 0;
}

int xhci_real_iso_pump(struct xhci_device *dev)
{
    if (!dev) return -1;
    struct xhci_iso_ep *ie = &dev->iso_in;
    if (!ie->configured || !ie->running) return 0;

    int delivered = 0;
    for (int n = 0; n < XHCI_ISO_RING_TRBS * 2; n++) {
        xhci_trb_t *e = event_pop();
        if (!e) break;
        if (TRB_TYPE_OF(e->control) != TRB_TRANSFER_EVENT) continue;
        uint64_t evt_ptr = ((uint64_t)e->param_hi << 32) | e->param_lo;
        int hit = -1;
        for (int i = 0; i < XHCI_ISO_RING_TRBS; i++) {
            if (ie->trb_phys[i] == evt_ptr) { hit = i; break; }
        }
        if (hit < 0) continue;
        int code = (e->status >> 24) & 0xFF;
        int residual = (int)(e->status & 0xFFFFFF);
        int armed = (int)ie->max_packet * ie->pkts_per_frame;
        int got = armed - residual;
        if (got < 0) got = 0;
        if (got > armed) got = armed;
        if ((code == 1 || code == 13) && got > 0 && ie->cb) {
            ie->cb(ie->cb_user, ie->trb_buf[hit], got);
            delivered++;
        }
        ie->trb_armed[hit] = 0;
        /* Flip cycle when we wrap past the highest slot we own. */
        if (hit == XHCI_ISO_RING_TRBS - 1) ie->cycle ^= 1;
        iso_arm_one(dev, hit);
        db_ring(dev->slot_id, ie->dci);
    }
    return delivered;
}

/* ── Address Device flow ─────────────────────────────────────── */
static int address_device(struct xhci_device *dev)
{
    /* Allocate Input Context (1 page) */
    dev->input_ctx = alloc_pages(1);
    dev->device_ctx = alloc_pages(1);
    if (!dev->input_ctx || !dev->device_ctx) return -1;

    uint64_t ep0_phys;
    dev->ep0_ring = xfer_ring_init(&ep0_phys);
    if (!dev->ep0_ring) return -1;
    dev->ep0_enqueue = 0;
    dev->ep0_cycle = 1;

    /* Input Control Context: A0 (slot) + A1 (EP0) */
    xhci_input_ctrl_ctx_t *icc = input_ctrl_of(dev->input_ctx);
    icc->add_flags = 0x3;
    icc->drop_flags = 0;

    /* Slot Context.
     *   dword0[19:0]  Route String (0 = direct root-hub port)
     *   dword0[23:20] Speed
     *   dword0[25]    MTT  (hubs only)
     *   dword0[26]    Hub bit
     *   dword0[31:27] Context Entries
     *   dword1[23:16] Root Hub Port Number
     *   dword1[31:24] Number of Ports (hubs only)
     *   dword2[7:0]   Parent Hub Slot ID  (LS/FS device behind HS hub)
     *   dword2[15:8]  Parent Port Number
     *   dword2[17:16] TT Think Time
     */
    xhci_slot_ctx_t *slot = input_slot_of(dev->input_ctx);
    slot->dword0 = (dev->route_string & 0xFFFFFU) |
                   ((uint32_t)dev->speed << 20) | (1U << 27);
    slot->dword1 = ((uint32_t)dev->port << 16);
    if (dev->parent_hub_slot &&
        (dev->speed == USB_SPEED_FULL || dev->speed == USB_SPEED_LOW)) {
        slot->dword2 = ((uint32_t)dev->parent_hub_slot & 0xFF) |
                       (((uint32_t)dev->parent_port & 0xFF) << 8);
    } else {
        slot->dword2 = 0;
    }
    slot->dword3 = 0;

    /* EP0 Context (DCI 1) */
    xhci_ep_ctx_t *ep0 = input_ep_of(dev->input_ctx, 0); /* index 0 -> DCI 1 */
    /* dword1: EP Type=4 (Control), CErr=3, MaxPacketSize in [31:16] */
    uint16_t mps0 = default_mps0(dev->speed);
    dev->max_packet_size0 = (uint8_t)(mps0 > 255 ? 64 : mps0);  /* will refine */
    ep0->dword1 = (3U << 1) | (4U << 3) | ((uint32_t)mps0 << 16);
    /* dword2/3: Dequeue Pointer (low|DCS)/(high) */
    ep0->dword2 = (uint32_t)(ep0_phys & 0xFFFFFFF0U) | 1U;  /* DCS=1 */
    ep0->dword3 = (uint32_t)(ep0_phys >> 32);
    /* dword4: Avg TRB Length (recommended 8 for control) */
    ep0->dword4 = 8;

    /* Issue Enable Slot first */
    uint64_t cmd_phys = cmd_issue(0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    int slot_id = 0;
    int code = cmd_wait(cmd_phys, &slot_id);
    if (code != 1 || slot_id == 0) {
        kputs("[xhci] Enable Slot failed code=");
        kput_hex(code);
        kputs("\n");
        return -1;
    }
    dev->slot_id = slot_id;

    /* Install Device Context pointer in DCBAA */
    xhci.dcbaa[slot_id] = (uint64_t)(uintptr_t)dev->device_ctx;

    /* Issue Address Device. Param = Input Context phys.
     * BSR=1 first (some controllers require this; QEMU works either way) -- use BSR=0
     * so we get a real USB address assigned in one shot. */
    uint64_t in_phys = (uint64_t)(uintptr_t)dev->input_ctx;
    uint32_t ctrl = TRB_TYPE(TRB_ADDRESS_DEVICE) |
                    ((uint32_t)slot_id << 24);
    cmd_phys = cmd_issue((uint32_t)(in_phys & 0xFFFFFFFFU),
                         (uint32_t)(in_phys >> 32),
                         0, ctrl);
    code = cmd_wait(cmd_phys, 0);
    if (code != 1) {
        kputs("[xhci] Address Device failed code=");
        kput_hex(code);
        kputs(" trying BSR=1 then BSR=0\n");
        /* Some HCs require the two-step: first BSR=1 (no SET_ADDRESS),
         * then BSR=0 to actually issue the USB SET_ADDRESS.
         * Reset Endpoint state on slot first by re-issuing with BSR=1. */
        ctrl = TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_BSR |
               ((uint32_t)slot_id << 24);
        cmd_phys = cmd_issue((uint32_t)(in_phys & 0xFFFFFFFFU),
                             (uint32_t)(in_phys >> 32), 0, ctrl);
        code = cmd_wait(cmd_phys, 0);
        if (code != 1) {
            kputs("[xhci] Address Device(BSR=1) failed code=");
            kput_hex(code);
            kputs("\n");
            return -1;
        }
        ctrl = TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24);
        cmd_phys = cmd_issue((uint32_t)(in_phys & 0xFFFFFFFFU),
                             (uint32_t)(in_phys >> 32), 0, ctrl);
        code = cmd_wait(cmd_phys, 0);
        if (code != 1) {
            kputs("[xhci] Address Device(BSR=0) failed code=");
            kput_hex(code);
            kputs("\n");
            return -1;
        }
    }

    /* Read assigned address from Output Slot Context */
    xhci_slot_ctx_t *out_slot = (xhci_slot_ctx_t *)dev->device_ctx;
    dev->address = (uint8_t)(out_slot->dword3 & 0xFF);
    return 0;
}

/* ── Read device descriptor on EP0 ───────────────────────────── */
static int read_device_descriptor(struct xhci_device *dev,
                                  struct usb_device_descriptor *out, int len)
{
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    sp.bRequest = USB_REQ_GET_DESCRIPTOR;
    sp.wValue = (USB_DT_DEVICE << 8) | 0;
    sp.wIndex = 0;
    sp.wLength = (uint16_t)len;
    int got = xhci_real_control_transfer(dev, &sp, out, len);
    return got;
}

/* ── Post-Address-Device descriptor probe ───────────────────── */
/* Read first 8 bytes of device descriptor (real MPS0) then full 18-byte
 * descriptor; cache identifying fields. Used by both root-hub and hub
 * enumeration paths. */
static int probe_device_descriptors(struct xhci_device *dev)
{
    uint8_t small[8];
    memset(small, 0, sizeof(small));
    struct usb_setup_packet sp;
    sp.bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    sp.bRequest = USB_REQ_GET_DESCRIPTOR;
    sp.wValue = (USB_DT_DEVICE << 8);
    sp.wIndex = 0;
    sp.wLength = 8;
    int g = xhci_real_control_transfer(dev, &sp, small, 8);
    if (g >= 8) dev->max_packet_size0 = small[7];

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
        kputs("[xhci]   VID=");
        kput_hex(desc.idVendor);
        kputs(" PID=");
        kput_hex(desc.idProduct);
        kputs(" class=");
        kput_hex(desc.bDeviceClass);
        kputs(" mps0=");
        kput_dec(dev->max_packet_size0);
        kputs("\n");
        return 0;
    }
    kputs("[xhci]   GET_DESCRIPTOR(Device) failed (");
    kput_dec(got < 0 ? 0 : (uint32_t)got);
    kputs(" bytes)\n");
    return -1;
}

/* ── Root-hub port enumeration ───────────────────────────────── */
static void enumerate_root_ports(void)
{
    for (int p = 1; p <= xhci.max_ports; p++) {
        uint32_t psc = op_r32(XHCI_PORTSC(p));
        if (!(psc & PORTSC_CCS)) continue;
        if (xhci.dev_count >= XHCI_MAX_DEVICES) break;

        struct xhci_device *dev = &xhci.devices[xhci.dev_count];
        memset(dev, 0, sizeof(*dev));
        dev->hc_kind = HC_KIND_XHCI;
        dev->port = p;

        if (port_reset(p) != 0) {
            kputs("[xhci] port ");
            kput_dec(p);
            kputs(" reset failed\n");
            continue;
        }
        dev->speed = port_speed(p);

        kputs("[xhci] port ");
        kput_dec(p);
        kputs(" device speed=");
        kput_dec(dev->speed);
        kputs("\n");

        if (address_device(dev) != 0) continue;
        probe_device_descriptors(dev);
        xhci.dev_count++;
    }
}

/* ── Hub-aware Address Device (public) ───────────────────────── */
/* Compute the route string for a device hanging off `parent` at the
 * parent's downstream port `parent_port`. xHCI 1.2 §4.5.2 / Table 6-4 —
 * each tier adds a nibble at the next-higher 4-bit position. */
static uint32_t compute_route(uint32_t parent_route, int parent_port)
{
    if (parent_port < 1) parent_port = 1;
    if (parent_port > 15) parent_port = 15;
    for (int i = 0; i < 5; i++) {
        uint32_t shift = (uint32_t)i * 4;
        if (((parent_route >> shift) & 0xF) == 0) {
            return (parent_route & ~(0xFU << shift)) |
                   ((uint32_t)(parent_port & 0xF) << shift);
        }
    }
    /* Out of nibbles (>5 tiers) -- return as-is; downstream not reachable. */
    return parent_route;
}

struct xhci_device *xhci_real_address_hub_device(struct xhci_device *parent,
                                                 int parent_port, int speed)
{
    if (!parent || parent->slot_id == 0 || parent_port < 1) return 0;
    if (xhci.dev_count >= XHCI_MAX_DEVICES) return 0;

    struct xhci_device *dev = &xhci.devices[xhci.dev_count];
    memset(dev, 0, sizeof(*dev));

    dev->hc_kind = HC_KIND_XHCI;
    /* Root Hub Port Number stays the parent's: it identifies the root
     * port. The route-string identifies the path from there. */
    dev->port = parent->port;
    dev->speed = speed;
    dev->route_string = compute_route(parent->route_string, parent_port);
    dev->parent_port = (uint8_t)parent_port;

    /* For LS/FS devices behind a HS hub (TT split transactions), point
     * at the closest HS hub. */
    if (speed == USB_SPEED_FULL || speed == USB_SPEED_LOW) {
        if (parent->speed == USB_SPEED_HIGH ||
            parent->speed == USB_SPEED_SUPER ||
            parent->speed == USB_SPEED_SUPER_PLUS) {
            dev->parent_hub_slot = (uint8_t)parent->slot_id;
        } else {
            dev->parent_hub_slot = parent->parent_hub_slot;
        }
    } else {
        dev->parent_hub_slot = 0;
    }

    if (address_device(dev) != 0) {
        if (dev->slot_id) xhci.dcbaa[dev->slot_id] = 0;
        memset(dev, 0, sizeof(*dev));
        return 0;
    }
    probe_device_descriptors(dev);
    xhci.dev_count++;
    return dev;
}

/* Mark an already-addressed device as a hub. Required by the spec
 * (and enforced by QEMU) before the controller will route Address
 * Device commands through it. Tries Evaluate Context first, falls
 * back to Configure Endpoint. */
int xhci_real_mark_hub(struct xhci_device *hub, int num_ports, int mtt,
                       int think_time)
{
    if (!hub || hub->slot_id == 0 || !hub->input_ctx) return -1;
    if (num_ports < 1 || num_ports > 255) return -1;

    xhci_input_ctrl_ctx_t *icc = input_ctrl_of(hub->input_ctx);
    icc->drop_flags = 0;
    icc->add_flags = 0x1;  /* Slot Context only */

    xhci_slot_ctx_t *slot = input_slot_of(hub->input_ctx);
    uint32_t d0 = (hub->route_string & 0xFFFFFU) |
                  ((uint32_t)hub->speed << 20) |
                  (1U << 27) |     /* Context Entries = 1 */
                  (1U << 26);      /* Hub */
    if (mtt) d0 |= (1U << 25);
    slot->dword0 = d0;
    slot->dword1 = ((uint32_t)hub->port << 16) |
                   ((uint32_t)num_ports << 24);
    if (hub->parent_hub_slot &&
        (hub->speed == USB_SPEED_FULL || hub->speed == USB_SPEED_LOW)) {
        slot->dword2 = ((uint32_t)hub->parent_hub_slot & 0xFF) |
                       (((uint32_t)hub->parent_port & 0xFF) << 8) |
                       (((uint32_t)think_time & 0x3U) << 16);
    } else {
        slot->dword2 = ((uint32_t)think_time & 0x3U) << 16;
    }
    slot->dword3 = 0;

    uint64_t in_phys = (uint64_t)(uintptr_t)hub->input_ctx;
    uint32_t ctrl = TRB_TYPE(TRB_EVALUATE_CONTEXT) |
                    ((uint32_t)hub->slot_id << 24);
    uint64_t cmd_phys = cmd_issue((uint32_t)(in_phys & 0xFFFFFFFFU),
                                  (uint32_t)(in_phys >> 32),
                                  0, ctrl);
    int code = cmd_wait(cmd_phys, 0);
    if (code != 1) {
        ctrl = TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
               ((uint32_t)hub->slot_id << 24);
        cmd_phys = cmd_issue((uint32_t)(in_phys & 0xFFFFFFFFU),
                             (uint32_t)(in_phys >> 32),
                             0, ctrl);
        code = cmd_wait(cmd_phys, 0);
        if (code != 1) {
            kputs("[xhci] mark_hub failed code=");
            kput_hex(code);
            kputs("\n");
            return -1;
        }
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */
int xhci_real_init(void)
{
    memset(&xhci, 0, sizeof(xhci));

    /* Find first xHCI controller in PCI enumeration */
    struct pci_device *pdev = 0;
    int n = pci_device_count();
    for (int i = 0; i < n; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30) {
            pdev = d;
            break;
        }
    }
    if (!pdev) {
        kputs("[xhci] no xHCI controller found\n");
        return -1;
    }

    xhci.pci_bus = pdev->bus;
    xhci.pci_dev = pdev->dev;
    xhci.pci_func = pdev->func;

    kputs("[xhci] found controller at ");
    kput_hex(pdev->bus); kputs(":");
    kput_hex(pdev->dev); kputs(".");
    kput_hex(pdev->func);
    kputs(" vid=");  kput_hex(pdev->vendor_id);
    kputs(" did=");  kput_hex(pdev->device_id);
    kputs("\n");

    pci_enable(pdev->bus, pdev->dev, pdev->func);

    uint64_t bar0 = pci_read_bar64(pdev->bus, pdev->dev, pdev->func, 0);
    if (!bar0) {
        kputs("[xhci] BAR0 = 0\n");
        return -1;
    }
    /* If the BAR is above the 4 GB identity map, map it as MMIO. xHCI
     * MMIO is small (Cap+Op+Runtime+Doorbell+Extended Caps fit in well
     * under 256 KiB on every controller in the wild) -- map 64 pages
     * (256 KiB) starting at the BAR base. */
    if (bar0 >= 0x100000000ULL) {
        uint64_t base = bar0 & ~0xFFFULL;
        vmm_map_range(base, base, 64,
                      PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE);
    }
    xhci.cap_base = (volatile uint8_t *)(uintptr_t)bar0;

    /* Read CAPLENGTH+HCIVERSION as a single 32-bit access; some HCs reject
     * sub-dword reads on the capability registers. */
    uint32_t cap0 = cap_r32(0);
    uint8_t  caplen = (uint8_t)(cap0 & 0xFF);
    uint16_t hciver = (uint16_t)(cap0 >> 16);
    xhci.hcsparams1 = cap_r32(XHCI_CAP_HCSPARAMS1);
    xhci.hcsparams2 = cap_r32(XHCI_CAP_HCSPARAMS2);
    xhci.hccparams1 = cap_r32(XHCI_CAP_HCCPARAMS1);
    uint32_t dboff  = cap_r32(XHCI_CAP_DBOFF) & ~0x3U;
    uint32_t rtsoff = cap_r32(XHCI_CAP_RTSOFF) & ~0x1FU;

    xhci.max_slots = (uint8_t)(xhci.hcsparams1 & 0xFF);
    xhci.max_intrs = (uint8_t)((xhci.hcsparams1 >> 8) & 0x7FF);
    xhci.max_ports = (uint8_t)((xhci.hcsparams1 >> 24) & 0xFF);
    xhci.ctx_size_64 = (xhci.hccparams1 & (1U << 2)) ? 1 : 0;

    xhci.op_base = xhci.cap_base + caplen;
    xhci.rt_base = xhci.cap_base + rtsoff;
    xhci.db_base = xhci.cap_base + dboff;

    kputs("[xhci] hciver=");
    kput_hex(hciver);
    kputs(" caplen=");
    kput_dec(caplen);
    kputs(" slots=");
    kput_dec(xhci.max_slots);
    kputs(" ports=");
    kput_dec(xhci.max_ports);
    kputs(" ctx=");
    kput_dec(xhci.ctx_size_64 ? 64 : 32);
    kputs("\n");

    if (xhci.max_ports == 0 || xhci.max_slots == 0) {
        kputs("[xhci] degenerate parameters\n");
        return -1;
    }

    /* Halt + Reset */
    if (xhci_halt() != 0) {
        kputs("[xhci] halt failed\n");
        return -1;
    }
    if (xhci_reset() != 0) {
        kputs("[xhci] reset failed\n");
        return -1;
    }

    /* Program max device slots */
    op_w32(XHCI_OP_CONFIG, (op_r32(XHCI_OP_CONFIG) & ~0xFFU) | xhci.max_slots);

    /* DCBAA: (max_slots+1) entries of 8 bytes, 64-byte aligned -- 1 page is plenty */
    xhci.dcbaa = alloc_pages(1);
    if (!xhci.dcbaa) {
        kputs("[xhci] DCBAA alloc failed\n");
        return -1;
    }
    xhci.dcbaa_phys = (uint64_t)(uintptr_t)xhci.dcbaa;

    /* Scratchpads (must be installed before DCBAAP write per some HCs;
     * works either way as long as before Run). */
    if (scratchpad_alloc() != 0) {
        kputs("[xhci] scratchpad alloc failed\n");
        return -1;
    }

    op_w64(XHCI_OP_DCBAAP, xhci.dcbaa_phys);

    /* Command Ring */
    cmd_ring_init();
    /* CRCR with RCS = our initial cycle (1) */
    op_w64(XHCI_OP_CRCR, xhci.cmd_ring_phys | (xhci.cmd_cycle ? CRCR_RCS : 0));

    /* Event Ring (must be ready before Run because controller may post
     * Port Status Change events as soon as we power up). */
    if (event_ring_init() != 0) {
        kputs("[xhci] event ring init failed\n");
        return -1;
    }

    /* Run! */
    op_w32(XHCI_OP_USBCMD, op_r32(XHCI_OP_USBCMD) | USBCMD_RS);

    /* Wait for HCH = 0 */
    int running = 0;
    for (uint32_t i = 0; i < XHCI_POLL_LIMIT; i++) {
        if (!(op_r32(XHCI_OP_USBSTS) & USBSTS_HCH)) { running = 1; break; }
        spin(100);
    }
    if (!running) {
        kputs("[xhci] controller did not start\n");
        return -1;
    }

    /* Allow ports to settle (USB2 ports need ~100ms after PP for CCS to assert).
     * spin with iteration count proportional to ms; ad-hoc since timer not init yet. */
    for (int i = 0; i < 200; i++) spin(100000);

    /* Drain any pre-existing Port Status Change events */
    while (event_pop()) ;

    xhci.present = 1;

    /* Enumerate root-hub ports */
    enumerate_root_ports();

    kputs("[xhci] ready, ");
    kput_dec(xhci.max_ports);
    kputs(" port(s), ");
    kput_dec(xhci.dev_count);
    kputs(" device(s) found");
    if (xhci.dev_count > 0) {
        kputs(" (first VID:PID ");
        kput_hex(xhci.devices[0].vendor_id);
        kputs(":");
        kput_hex(xhci.devices[0].product_id);
        kputs(")");
    }
    kputs("\n");

    return 0;
}

struct xhci_device *xhci_real_find_device(uint16_t vid, uint16_t pid)
{
    for (int i = 0; i < xhci.dev_count; i++) {
        struct xhci_device *d = &xhci.devices[i];
        if (d->slot_id == 0) continue;
        if ((vid == 0 || d->vendor_id == vid) &&
            (pid == 0 || d->product_id == pid))
            return d;
    }
    return 0;
}

int xhci_real_device_count(void) { return xhci.dev_count; }

struct xhci_device *xhci_real_get_device(int index)
{
    if (index < 0 || index >= xhci.dev_count) return 0;
    return &xhci.devices[index];
}

/* ── Hotplug pump support ──────────────────────────────────────
 *
 * Read-only PORTSC accessors so the hotplug pump (boot/hotplug.c)
 * can poll CSC + CCS without touching any xHCI state. The boot-time
 * enumeration path in enumerate_root_ports() remains the only thing
 * that mutates port reset / addressing.
 */

int xhci_hotplug_max_ports(void)
{
    if (!xhci.op_base) return 0;
    return (int)xhci.max_ports;
}

/* port is 1-based. Returns 0 on success, fills *connected/*speed/*csc. */
int xhci_hotplug_port_status(int port, int *connected, int *speed, int *csc)
{
    if (!xhci.op_base) return -1;
    if (port < 1 || port > xhci.max_ports) return -1;
    uint32_t psc = op_r32(XHCI_PORTSC(port));
    if (connected) *connected = (psc & PORTSC_CCS) ? 1 : 0;
    if (speed)     *speed     = (int)((psc & PORTSC_PSPEED_MASK) >> PORTSC_PSPEED_SHIFT);
    if (csc)       *csc       = (psc & PORTSC_CSC) ? 1 : 0;
    return 0;
}

/* Acknowledge (write-1-clear) the CSC bit on the given port. The hotplug
 * pump calls this after recording a transition so the next poll sees the
 * raw connect state without the latched change bit.
 *
 * PED is RW1C in xHCI 1.2 (write 1 to clear == disable). The naive
 * "preserve psc, OR in CSC" pattern accidentally writes 1 back to PED
 * and disables the port. Mask PED out alongside the change bits before
 * setting CSC to clear, so the operation is purely about the CSC latch. */
void xhci_hotplug_ack_csc(int port)
{
    if (!xhci.op_base) return;
    if (port < 1 || port > xhci.max_ports) return;
    uint32_t psc = op_r32(XHCI_PORTSC(port));
    uint32_t v = (psc & ~(PORTSC_RW1C_MASK | PORTSC_PED)) | PORTSC_CSC;
    op_w32(XHCI_PORTSC(port), v);
}

/* Ensure PORTSC.PP (Port Power) is set on every root-hub port so that
 * subsequent device attaches register CCS / CSC. xhci_real_init's
 * enumerate_root_ports only walks ports that already showed CCS at boot
 * — newly powered ports never report attach until PP=1. The hotplug
 * pump calls this once during seed so post-boot device_add is visible.
 *
 * No-op on controllers where PP is already 1 (the common case on most
 * real silicon — qemu-xhci leaves PP=0 by default until software
 * asserts it). */
void xhci_hotplug_power_on_all_ports(void)
{
    if (!xhci.op_base) return;
    for (int p = 1; p <= xhci.max_ports; p++) {
        uint32_t psc = op_r32(XHCI_PORTSC(p));
        if (psc & PORTSC_PP) continue;
        /* Set PP without disturbing RW1C bits or accidentally writing
         * 1 to PED (which would disable the port). */
        uint32_t v = (psc & ~(PORTSC_RW1C_MASK | PORTSC_PED)) | PORTSC_PP;
        op_w32(XHCI_PORTSC(p), v);
    }
}
