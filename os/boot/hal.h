/*
 * Zeos — HAL (Hardware Abstraction Layer)  [O.2]
 *
 * Arch-neutral façade over the low-level machine primitives: port I/O,
 * descriptor tables, interrupt controller, timer, and CPU interrupt flag.
 * The x86-64 backend (hal_x86.c) implements this over io.h / gdt / idt /
 * pic / timer. An ARM64 backend (O.3) implements the same contract over
 * MMIO / GIC / generic timer — callers stay arch-neutral by using hal_*.
 *
 * Migration is incremental: hal_* is the target interface; drivers move
 * their raw inb/outb/idt_register/pic_* calls behind these over time.
 */
#ifndef ZEOS_HAL_H
#define ZEOS_HAL_H

#include <stdint.h>

/* ── CPU port I/O (x86) / MMIO-mapped equivalents (arm) ── */
void     hal_out8(uint16_t port, uint8_t  val);
uint8_t  hal_in8(uint16_t port);
void     hal_out16(uint16_t port, uint16_t val);
uint16_t hal_in16(uint16_t port);
void     hal_out32(uint16_t port, uint32_t val);
uint32_t hal_in32(uint16_t port);

/* Short bus settling delay after a register write. On x86 this is the classic
 * write-to-port-0x80 trick; on ARM it is a memory barrier. Drivers need "wait a
 * moment for the device to catch up" without knowing which machine they are on. */
void     hal_io_wait(void);

/* ── Descriptor tables / CPU tables ── */
void hal_cpu_init_tables(void);          /* GDT + IDT (x86); page tables/EL setup (arm) */

/* ── Interrupt controller ── */
typedef void (*hal_isr_t)(uint64_t vec, uint64_t err);
void hal_irq_register(uint8_t vector, hal_isr_t handler);
void hal_irq_remap(void);
void hal_irq_eoi(uint8_t irq);
void hal_irq_mask(uint8_t irq);
void hal_irq_unmask(uint8_t irq);

/* ── Timer ── */
void hal_timer_init(uint32_t hz);

/* ── CPU interrupt flag ── */
void hal_cli(void);
void hal_sti(void);

/* Which backend is compiled in. */
const char *hal_arch_name(void);

#endif /* ZEOS_HAL_H */
