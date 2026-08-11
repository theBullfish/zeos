/*
 * Zeos — Interrupt Descriptor Table (IDT)
 *
 * Bare-metal interrupt handling for x86_64 long mode.
 * Sets up the IDT, maps ISRs, and provides interrupt registration.
 */

#ifndef ZEOS_IDT_H
#define ZEOS_IDT_H

#include <stdint.h>

/* IDT gate types */
#define IDT_GATE_INTERRUPT  0x8E  /* P=1, DPL=0, 64-bit interrupt gate */
#define IDT_GATE_TRAP       0x8F  /* P=1, DPL=0, 64-bit trap gate */

/* Total IDT entries */
#define IDT_ENTRIES 256

/* Interrupt handler function type */
typedef void (*isr_handler_t)(uint64_t vector, uint64_t error_code);

/* Initialize the IDT and load it via LIDT */
void idt_init(void);

/* Load the shared IDT on the current (AP) CPU. Must be called on each AP before
 * it enables interrupts, or its first exception triple-faults. */
void idt_load_ap(void);

/* Register a handler for a specific interrupt vector */
void idt_register(uint8_t vector, isr_handler_t handler);

/* Send End-of-Interrupt to the legacy 8259 PIC */
void pic_eoi(uint8_t irq);

/* Remap the 8259 PIC to vectors 0x20-0x2F */
void pic_remap(void);

/* Enable/disable specific IRQ on the PIC */
void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);

/*
 * Set an IDT gate externally (needed by gdt.c to update CS selector
 * and set IST fields after GDT is loaded).
 */
void idt_set_gate_ext(uint8_t vector, uint64_t handler_addr,
                      uint16_t selector, uint8_t ist, uint8_t type);

#endif /* ZEOS_IDT_H */
