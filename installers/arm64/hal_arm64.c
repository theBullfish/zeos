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

/* ── Port I/O: no port space on ARM; map the legacy PCI config ports
 * everything is treated as an offset from the UART base.
 *
 * ⚠ STUB — NOT CORRECT YET. ARM has no port-I/O space, so these are placeholders
 * that let the shared drivers COMPILE for aarch64; they do not yet dispatch by
 * port to the right device. In particular PCI config (0xCF8/0xCFC) is NOT
 * remapped onto ECAM here — use hal_arm_ecam_read32() below. Making this real
 * means dispatching on the port number (or, better, giving the shared drivers a
 * device handle instead of a bare port). Do not treat ARM I/O as working. ── */
void hal_out8(uint16_t p, uint8_t v)   { mmio_w8(ARM_UART0_BASE + p, v); }
uint8_t hal_in8(uint16_t p)            { return mmio_r8(ARM_UART0_BASE + p); }
void hal_out16(uint16_t p, uint16_t v) { *(volatile uint16_t *)(ARM_UART0_BASE + p) = v; }
uint16_t hal_in16(uint16_t p)          { return *(volatile uint16_t *)(ARM_UART0_BASE + p); }
void hal_out32(uint16_t p, uint32_t v) { mmio_w32(ARM_UART0_BASE + p, v); }
uint32_t hal_in32(uint16_t p)          { return mmio_r32(ARM_UART0_BASE + p); }

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
