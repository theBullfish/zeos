/*
 * Zeos — Local APIC.
 *
 * Maps the LAPIC MMIO at the address ACPI told us about (typically
 * 0xFEE00000), enables it via the Spurious Interrupt Vector Register,
 * masks every Local Vector Table entry so nothing fires until each
 * subsystem opts in, and calibrates the APIC timer bus frequency
 * against the PIT (channel 2, gate-controlled, no IRQ involved).
 *
 * EOI: lapic_eoi() writes 0 to LAPIC + 0xB0. msix.c calls this at the
 * tail of msix_dispatch so the LAPIC will accept the next interrupt
 * on the same vector.
 *
 * The APIC timer is calibrated then disarmed (LVT masked, init count
 * 0). lapic_timer_oneshot() is provided so the future scheduler
 * preemption agent can arm it without having to know the divider.
 */

#include "lapic.h"
#include "acpi.h"
#include "vmm.h"
#include "io.h"
#include "kprint.h"

#define LAPIC_ID         0x020
#define LAPIC_VERSION    0x030
#define LAPIC_TPR        0x080
#define LAPIC_EOI        0x0B0
#define LAPIC_SVR        0x0F0
#define LAPIC_LVT_TIMER  0x320
#define LAPIC_LVT_LINT0  0x350
#define LAPIC_LVT_LINT1  0x360
#define LAPIC_LVT_ERROR  0x370
#define LAPIC_LVT_PMC    0x340
#define LAPIC_LVT_THERM  0x330
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

#define LAPIC_SVR_ENABLE     (1u << 8)
#define LAPIC_LVT_MASKED     (1u << 16)
#define LAPIC_TIMER_ONESHOT  (0u << 17)
#define LAPIC_TIMER_PERIODIC (1u << 17)

/* Divider register encoding for ÷16 (3.6.9 of SDM Vol 3): 0b0011. */
#define LAPIC_TIMER_DIV_16   0x3

#define PIT_CH2_DATA  0x42
#define PIT_CMD       0x43
#define PIT_GATE_PORT 0x61
#define PIT_BASE_FREQ 1193182u

static volatile uint8_t *g_lapic = 0;
static int g_ready = 0;
static uint64_t g_lapic_base = 0;
static uint32_t g_lapic_ticks_per_us = 0;

static inline uint32_t lapic_read(uint32_t reg)
{
    return *(volatile uint32_t *)(g_lapic + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(g_lapic + reg) = val;
}

uint64_t lapic_base(void) { return g_lapic_base; }
int      lapic_ready(void) { return g_ready; }

uint32_t lapic_id(void)
{
    if (!g_ready) return 0;
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void)
{
    if (!g_ready) return;
    lapic_write(LAPIC_EOI, 0);
}

int lapic_init(void)
{
    if (g_ready) return 0;

    acpi_madt_t *m = acpi_madt();
    uint64_t base = m ? m->lapic_addr : 0xFEE00000ULL;
    if (!base) base = 0xFEE00000ULL;
    g_lapic_base = base;

    /* Map the 4KiB LAPIC page as uncached writable. */
    vmm_map_range(base & ~0xFFFULL, base & ~0xFFFULL, 1,
                  PTE_WRITABLE | PTE_NOCACHE);
    g_lapic = (volatile uint8_t *)(uintptr_t)base;

    /* Make sure the global APIC enable bit in IA32_APIC_BASE MSR is on.
     * Bit 11 = global enable. We do not relocate the base. */
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));
        lo |= (1u << 11);
        __asm__ volatile("wrmsr" : : "c"(0x1B), "a"(lo), "d"(hi));
    }

    /* Mask every LVT entry so nothing fires until a subsystem
     * explicitly arms it. EXCEPT LINT0 / LINT1: when the LAPIC is
     * enabled, LINT0 is the wire that delivers 8259 PIC interrupts
     * to the CPU and LINT1 is the legacy NMI path. Masking either
     * one strands legacy IRQ0 (PIT) which the existing timer_init
     * still relies on. We program ExtINT on LINT0 and NMI on LINT1
     * to preserve the pre-LAPIC interrupt topology. */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    /* LINT0: ExtINT delivery mode (7 << 8) = 0x700, vector 0. */
    lapic_write(LAPIC_LVT_LINT0, 0x700);
    /* LINT1: NMI delivery mode (4 << 8) = 0x400, vector 0. */
    lapic_write(LAPIC_LVT_LINT1, 0x400);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PMC,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERM, LAPIC_LVT_MASKED);

    /* Task Priority = 0 (accept all priorities). */
    lapic_write(LAPIC_TPR, 0);

    /* Spurious Interrupt Vector: vector 0xFF + APIC enabled. */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);

    g_ready = 1;
    return 0;
}

/*
 * Calibrate the APIC bus frequency using PIT channel 2 (the speaker
 * gate, but we keep the speaker silent: PIT_GATE_PORT bit 1 stays 0).
 * Channel 2 is gate-controlled which lets us wait a precise number
 * of PIT ticks without depending on IRQ delivery.
 */
void lapic_timer_calibrate(void)
{
    if (!g_ready) return;

    /* Wait period: 10 ms = 11932 PIT ticks at 1.193182 MHz. */
    const uint32_t pit_ticks = PIT_BASE_FREQ / 100;

    /* Disable the speaker (bit 1 = speaker enable), keep gate (bit 0)
     * disabled while we program. */
    uint8_t old61 = inb(PIT_GATE_PORT);
    outb(PIT_GATE_PORT, (old61 & ~0x02) & ~0x01);

    /* PIT mode 0 (interrupt on terminal count), channel 2,
     * lo/hi byte access, binary. */
    outb(PIT_CMD, 0xB0);
    outb(PIT_CH2_DATA, pit_ticks & 0xFF);
    outb(PIT_CH2_DATA, (pit_ticks >> 8) & 0xFF);

    /* Set LAPIC timer divider to 16, masked, init count = max. */
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | 0xFE);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);

    /* Open PIT gate (bit 0) — counter starts. */
    outb(PIT_GATE_PORT, (old61 & ~0x02) | 0x01);

    /* Spin until PIT channel 2 OUT pin (port 0x61 bit 5) goes high,
     * which signals the count expired. */
    while ((inb(PIT_GATE_PORT) & 0x20) == 0)
        ;

    uint32_t lapic_now = lapic_read(LAPIC_TIMER_CUR);
    /* Stop LAPIC counter. */
    lapic_write(LAPIC_TIMER_INIT, 0);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);

    /* Restore port 0x61 (close gate, speaker off). */
    outb(PIT_GATE_PORT, old61 & ~0x03);

    uint32_t elapsed = 0xFFFFFFFFu - lapic_now;
    /* elapsed LAPIC ticks happened in 10 ms = 10000 µs. */
    g_lapic_ticks_per_us = elapsed / 10000u;
    if (g_lapic_ticks_per_us == 0) g_lapic_ticks_per_us = 1;
}

uint32_t lapic_ticks_per_us(void) { return g_lapic_ticks_per_us; }

void lapic_timer_oneshot(uint32_t ticks, uint8_t vector)
{
    if (!g_ready) return;
    lapic_write(LAPIC_TIMER_DIV,  LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER,  LAPIC_TIMER_ONESHOT | vector);
    lapic_write(LAPIC_TIMER_INIT, ticks);
}
