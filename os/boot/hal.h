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

/* Spin-wait hint: tells the core we are busy-waiting so it can back off
 * (x86 PAUSE, aarch64 YIELD). Correctness never depends on it.
 *
 * This exists because spinlock.h — a header included all over the agnostic
 * layer — emitted a literal x86 `pause` in its spin loop, so every module that
 * merely took a lock failed to assemble on aarch64. One instruction in one
 * shared header was blocking a large share of the OS from building on ARM. */
void hal_cpu_relax(void);

/* Save-and-disable / restore the interrupt-enable state. hal_cli()/hal_sti()
 * are not enough for a lock taken from both task and ISR context: unlocking
 * must restore whatever the flag WAS, not unconditionally enable.
 *
 * Same story as hal_cpu_relax — spinlock.h implemented this with a literal
 * `pushfq; popq; cli`, so any agnostic module using an IRQ-safe lock could not
 * assemble on aarch64. The opaque return is the arch's own flags word (x86
 * RFLAGS, aarch64 DAIF); callers must only pass it back untouched. */
uint64_t hal_irq_save(void);
void hal_irq_restore(uint64_t flags);

/* Stop this core until an interrupt arrives (x86 HLT, aarch64 WFI). Used by
 * panic's final resting loop and by idle waits — a bare `for(;;){}` there would
 * spin a core at full power forever. */
void hal_cpu_halt(void);

/* Hardware RNG. Writes 64 random bits and returns 0; returns -1 if this core
 * has no hardware entropy source, in which case the caller MUST fall back to
 * its own mixing rather than treat the buffer as random.
 *
 * Detection belongs here, not in the caller: mbedtls_platform.c was executing
 * a raw CPUID to probe for RDRAND, which is both an x86 instruction and an x86
 * feature-detection ABI sitting in the chip-agnostic layer. Each arch answers
 * this question its own way (x86 CPUID.01:ECX[30] then RDRAND; aarch64 would
 * use RNDR from ARMv8.5-RNG). */
int hal_hw_random(uint64_t *out);

/* Which backend is compiled in. */
const char *hal_arch_name(void);

#endif /* ZEOS_HAL_H */
