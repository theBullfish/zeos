/*
 * Zeos — HAL x86-64 backend  [O.2]
 *
 * Implements hal.h over the existing x86 primitives (io.h, gdt, idt, pic,
 * timer). This is the arch layer everything portable routes through; the
 * ARM64 backend (O.3) provides the same symbols over GIC/MMIO/generic-timer.
 */
#include "hal.h"
#include "io.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"

/* Port I/O — forward to the inline x86 ops. */
void     hal_out8(uint16_t p, uint8_t v)  { outb(p, v); }
uint8_t  hal_in8(uint16_t p)              { return inb(p); }
void     hal_out16(uint16_t p, uint16_t v){ outw(p, v); }
uint16_t hal_in16(uint16_t p)             { return inw(p); }
void     hal_out32(uint16_t p, uint32_t v){ outl(p, v); }
uint32_t hal_in32(uint16_t p)             { return inl(p); }
void     hal_io_wait(void)                 { io_wait(); }

/* CPU tables. */
void hal_cpu_init_tables(void) { gdt_init(); idt_init(); }

/* Interrupt controller (8259 PIC + IDT vector table). */
void hal_irq_register(uint8_t vec, hal_isr_t h) { idt_register(vec, (isr_handler_t)h); }
void hal_irq_remap(void)         { pic_remap(); }
void hal_irq_eoi(uint8_t irq)    { pic_eoi(irq); }
void hal_irq_mask(uint8_t irq)   { pic_mask(irq); }
void hal_irq_unmask(uint8_t irq) { pic_unmask(irq); }

/* Timer. */
void hal_timer_init(uint32_t hz) { timer_init(hz); }

/* CPU interrupt flag. */
void hal_cli(void) { __asm__ volatile("cli"); }
void hal_sti(void) { __asm__ volatile("sti"); }

const char *hal_arch_name(void) { return "x86-64"; }

#ifdef ZEOS_DIAG_O2
#include "kprint.h"
/* O.2 selftest: the HAL forwards to the real x86 primitives. hal_in8 on the
 * PS/2 status port (0x64, read-only, safe) equals inb(0x64); hal_arch_name is
 * the x86 backend. (Table/IRQ ops are exercised live at boot; not re-invoked
 * here to avoid re-initializing running hardware.) */
void hal_o2_selftest(void)
{
    uint8_t via_hal = hal_in8(0x64);
    uint8_t via_raw = inb(0x64);
    int io_forwards = (via_hal == via_raw);
    const char *arch = hal_arch_name();
    int arch_ok = arch && arch[0]=='x' && arch[1]=='8' && arch[2]=='6';
    /* mask/unmask a spare IRQ line round-trips without fault (IRQ7, parallel) */
    hal_irq_mask(7); hal_irq_unmask(7);
    int pass = io_forwards && arch_ok;
    kputs("[O2] io_forwards(hal_in8==inb 0x64)="); kput_dec((uint64_t)io_forwards);
    kputs(" arch="); kputs(arch);
    kputs(pass ? " -> PASS\n" : " -> FAIL\n");
}
#endif
