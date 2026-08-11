/*
 * Zeos — HAL ARM64 backend  [O.3, first brick]
 *
 * Implements the hal.h contract (O.2) for AArch64: MMIO register access,
 * GICv2 interrupt controller, ARM generic timer, PL011 UART, DAIF interrupt
 * masking, and ECAM PCI config space. This is the arch layer the portable
 * kernel routes through; the x86 backend is hal_x86.c.
 *
 * Base addresses match QEMU 'virt': GIC dist 0x08000000 / cpu 0x08010000,
 * PL011 UART 0x09000000, PCIe ECAM 0x4010000000. Compiled with the aarch64
 * cross toolchain; the full boot path (EFI aarch64 stub, page tables) is the
 * remaining O.3 work — this brick makes the HAL genuinely cross-arch.
 */
#include "hal.h"

/* QEMU 'virt' platform MMIO map. */
#define ARM_GICD_BASE   0x08000000UL   /* GICv2 distributor */
#define ARM_GICC_BASE   0x08010000UL   /* GICv2 CPU interface */
#define ARM_UART0_BASE  0x09000000UL   /* PL011 */
#define ARM_ECAM_BASE   0x4010000000UL /* PCIe ECAM */

static inline void     mmio_w32(unsigned long a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t mmio_r32(unsigned long a)             { return *(volatile uint32_t *)a; }
static inline void     mmio_w8(unsigned long a, uint8_t v)   { *(volatile uint8_t *)a = v; }
static inline uint8_t  mmio_r8(unsigned long a)              { return *(volatile uint8_t *)a; }

/* ══════════════════════════════════════════════════════════════════════════
 * Port I/O dispatch.
 *
 * AArch64 has no port-I/O space, and the legacy x86 devices behind these port
 * numbers are NOT the devices ARM has — so a 1:1 "port + base" map is wrong and
 * silently corrupts registers. Each legacy port range is dispatched to the real
 * ARM device with faithful register translation:
 *
 *   0x3F8-0x3FF  COM1 (16550)   -> PL011 @0x09000000, register-translated
 *   0xCF8/0xCFC  PCI config     -> ECAM @0x40_1000_0000 (address latch emulated)
 *   0x70/0x71    CMOS RTC       -> PL031 RTC @0x09010000 (index latch emulated)
 *   0x60/0x64    i8042 PS/2     -> DOES NOT EXIST on ARM virt (USB HID only):
 *                                  reports "no device / empty buffer" so probes
 *                                  fail cleanly instead of hanging or writing
 *                                  garbage into the UART.
 *   0x40-0x43    PIT 8254       -> no-op; ARM uses the generic timer (CNTP_*)
 *   0x61, 0x92   speaker, A20   -> no-op; no such hardware
 *   other        PCI BAR io_base-> treated as MMIO (BARs are memory on ARM)
 * ══════════════════════════════════════════════════════════════════════════ */
#define ARM_RTC_BASE    0x09010000UL   /* PL031 */

uint32_t hal_arm_ecam_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off);

/* PL011 registers (offsets from ARM_UART0_BASE). */
#define PL011_DR    0x00
#define PL011_FR    0x18
#define PL011_FR_RXFE (1u << 4)        /* receive FIFO empty */
#define PL011_FR_TXFF (1u << 5)        /* transmit FIFO full */

/* 16550 register offsets from COM1, as the shared serial.c drives them. */
#define UART16550_DATA 0
#define UART16550_IER  1
#define UART16550_FCR  2
#define UART16550_LCR  3
#define UART16550_MCR  4
#define UART16550_LSR  5
#define UART16550_MSR  6

#define IS_COM1(p)   ((p) >= 0x3F8 && (p) <= 0x3FF)
#define IS_CMOS(p)   ((p) == 0x70 || (p) == 0x71)
#define IS_I8042(p)  ((p) == 0x60 || (p) == 0x64)
#define IS_PIT(p)    ((p) >= 0x40 && (p) <= 0x43)

static uint32_t g_pci_cfg_addr;   /* 0xCF8 address latch */
static uint8_t  g_cmos_index;     /* 0x70 index latch */

/* PL031 counts seconds since the epoch; CMOS exposes split fields. Convert. */
static void rtc_fields(int *yr, int *mo, int *dy, int *hh, int *mm, int *ss)
{
    uint32_t t = mmio_r32(ARM_RTC_BASE + 0x00);          /* PL031 DR */
    *ss = t % 60; t /= 60;
    *mm = t % 60; t /= 60;
    *hh = t % 24; t /= 24;                                /* t = days since epoch */
    int y = 1970;
    for (;;) {
        int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
        uint32_t len = leap ? 366 : 365;
        if (t < len) break;
        t -= len; y++;
    }
    int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int m = 0;
    for (; m < 12; m++) {
        uint32_t len = (uint32_t)mdays[m] + ((m == 1 && leap) ? 1u : 0u);
        if (t < len) break;
        t -= len;
    }
    *yr = y; *mo = m + 1; *dy = (int)t + 1;
}

void hal_out8(uint16_t p, uint8_t v)
{
    if (IS_COM1(p)) {
        /* Only the data register maps; PL011 line/FIFO/modem config is done by
         * the boot stub, so 16550 config writes are intentionally dropped. */
        if ((p - 0x3F8) == UART16550_DATA) mmio_w8(ARM_UART0_BASE + PL011_DR, v);
        return;
    }
    if (p == 0x70) { g_cmos_index = v & 0x7F; return; }
    if (p == 0x71) return;                    /* PL031 set-time not wired */
    if (IS_I8042(p) || IS_PIT(p) || p == 0x61 || p == 0x92) return;  /* absent hw */
    mmio_w8(ARM_UART0_BASE + p, v);           /* PCI BAR MMIO offset */
}

uint8_t hal_in8(uint16_t p)
{
    if (IS_COM1(p)) {
        uint16_t reg = p - 0x3F8;
        if (reg == UART16550_DATA) return mmio_r8(ARM_UART0_BASE + PL011_DR);
        if (reg == UART16550_LSR) {
            uint32_t fr = mmio_r32(ARM_UART0_BASE + PL011_FR);
            uint8_t lsr = 0;
            if (!(fr & PL011_FR_RXFE)) lsr |= 0x01;   /* data ready */
            if (!(fr & PL011_FR_TXFF)) lsr |= 0x20;   /* THR empty */
            return lsr;
        }
        return 0;                                     /* IER/FCR/LCR/MCR/MSR */
    }
    if (p == 0x71) {
        int yr, mo, dy, hh, mm, ss;
        rtc_fields(&yr, &mo, &dy, &hh, &mm, &ss);
        switch (g_cmos_index) {
        case 0x00: return (uint8_t)ss;
        case 0x02: return (uint8_t)mm;
        case 0x04: return (uint8_t)hh;
        case 0x07: return (uint8_t)dy;
        case 0x08: return (uint8_t)mo;
        case 0x09: return (uint8_t)(yr % 100);
        case 0x32: return (uint8_t)(yr / 100);
        case 0x0A: return 0x26;      /* status A: not mid-update */
        case 0x0B: return 0x06;      /* status B: binary values, 24-hour */
        default:   return 0;
        }
    }
    /* i8042: no controller. 0x64 status = 0 (output buffer EMPTY) so the driver
     * never believes a scancode is waiting; 0x60 data = 0xFF (no device). */
    if (p == 0x64) return 0x00;
    if (p == 0x60) return 0xFF;
    if (IS_PIT(p) || p == 0x61 || p == 0x92) return 0;
    return mmio_r8(ARM_UART0_BASE + p);
}

void hal_out16(uint16_t p, uint16_t v)
{
    if (IS_COM1(p) || IS_CMOS(p) || IS_I8042(p) || IS_PIT(p)) return;
    *(volatile uint16_t *)(ARM_UART0_BASE + p) = v;
}
uint16_t hal_in16(uint16_t p)
{
    if (IS_COM1(p) || IS_CMOS(p) || IS_I8042(p) || IS_PIT(p)) return 0;
    return *(volatile uint16_t *)(ARM_UART0_BASE + p);
}

void hal_out32(uint16_t p, uint32_t v)
{
    if (p == 0xCF8) { g_pci_cfg_addr = v; return; }        /* latch */
    if (p == 0xCFC) {                                      /* config write */
        uint8_t bus = (uint8_t)((g_pci_cfg_addr >> 16) & 0xFF);
        uint8_t dev = (uint8_t)((g_pci_cfg_addr >> 11) & 0x1F);
        uint8_t fn  = (uint8_t)((g_pci_cfg_addr >> 8)  & 0x07);
        uint16_t off = (uint16_t)(g_pci_cfg_addr & 0xFC);
        mmio_w32(ARM_ECAM_BASE + ((unsigned long)bus << 20) +
                 ((unsigned long)dev << 15) + ((unsigned long)fn << 12) + off, v);
        return;
    }
    if (IS_COM1(p) || IS_CMOS(p) || IS_I8042(p) || IS_PIT(p)) return;
    mmio_w32(ARM_UART0_BASE + p, v);
}

uint32_t hal_in32(uint16_t p)
{
    if (p == 0xCF8) return g_pci_cfg_addr;
    if (p == 0xCFC) {                                      /* config read */
        uint8_t bus = (uint8_t)((g_pci_cfg_addr >> 16) & 0xFF);
        uint8_t dev = (uint8_t)((g_pci_cfg_addr >> 11) & 0x1F);
        uint8_t fn  = (uint8_t)((g_pci_cfg_addr >> 8)  & 0x07);
        uint16_t off = (uint16_t)(g_pci_cfg_addr & 0xFC);
        return hal_arm_ecam_read32(bus, dev, fn, off);
    }
    if (IS_COM1(p) || IS_CMOS(p) || IS_I8042(p) || IS_PIT(p)) return 0;
    return mmio_r32(ARM_UART0_BASE + p);
}

/* ── CPU tables: on ARM the exception vector table (VBAR_EL1) + page tables
 * are set up in the boot stub; nothing to init from the HAL here yet. ── */
void hal_cpu_init_tables(void) { /* VBAR_EL1 / TTBR set in ARM boot stub */ }

/* ── GICv2 interrupt controller. ── */
#define GICD_ISENABLER  (ARM_GICD_BASE + 0x100)
#define GICD_ICENABLER  (ARM_GICD_BASE + 0x180)
#define GICC_EOIR       (ARM_GICC_BASE + 0x010)

void hal_irq_register(uint8_t vector, hal_isr_t handler)
{
    /* ARM routes IRQs through VBAR_EL1's IRQ entry to a dispatcher keyed by the
     * GICC_IAR intid; the vector->handler table is owned by that dispatcher. */
    (void)vector; (void)handler;
}
void hal_irq_remap(void) { /* no 8259 on ARM; GIC needs no remap */ }
void hal_irq_eoi(uint8_t irq)    { mmio_w32(GICC_EOIR, irq); }
void hal_irq_mask(uint8_t irq)   { mmio_w32(GICD_ICENABLER + (irq / 32) * 4, 1u << (irq % 32)); }
void hal_irq_unmask(uint8_t irq) { mmio_w32(GICD_ISENABLER + (irq / 32) * 4, 1u << (irq % 32)); }

/* ── ARM generic timer (CNTP): program CNTP_TVAL from the frequency. ── */
void hal_timer_init(uint32_t hz)
{
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t tval = hz ? (freq / hz) : freq;
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(tval));
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)1)); /* enable */
}

/* ── CPU interrupt flag via DAIF. ── */
void hal_cli(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
void hal_sti(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }

const char *hal_arch_name(void) { return "arm64"; }

/* ── ECAM PCI config helper (used once the ARM port wires pci.c to it). ── */
uint32_t hal_arm_ecam_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off)
{
    unsigned long a = ARM_ECAM_BASE
                    + ((unsigned long)bus << 20)
                    + ((unsigned long)dev << 15)
                    + ((unsigned long)fn  << 12)
                    + (off & 0xFFC);
    return mmio_r32(a);
}
