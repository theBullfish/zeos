/*
 * Zeos — Global Descriptor Table (GDT) and Task State Segment (TSS)
 *
 * Replaces UEFI's GDT (which lives in freed memory) with our own.
 * The TSS provides:
 *   - IST stacks for safe exception handling (double fault, NMI, MCE)
 *   - RSP0 for ring 3 → ring 0 transitions (future user-mode)
 *   - I/O permission bitmap base (future)
 */

#ifndef ZEOS_GDT_H
#define ZEOS_GDT_H

#include <stdint.h>

/* GDT segment selectors (byte offsets into GDT) */
#define GDT_NULL        0x00
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28  /* TSS descriptor is 16 bytes (2 entries) */

/* IST indices (1-based, 0 = no IST) */
#define IST_DOUBLE_FAULT  1
#define IST_NMI           2
#define IST_MCE           3

/* IST stack size: 16KB each */
#define IST_STACK_SIZE    16384

/*
 * Initialize the GDT and TSS, load them via LGDT and LTR.
 * Must be called before idt_init() since IDT gates reference GDT selectors.
 */
void gdt_init(void);

/*
 * Get the kernel code segment selector (for IDT gates).
 */
uint16_t gdt_kernel_cs(void);

#endif /* ZEOS_GDT_H */
