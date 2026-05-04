/*
 * Zeos — IOAPIC (skeleton).
 *
 * Maps the first IOAPIC reported by ACPI MADT and lets callers
 * program redirection entries. We honor Interrupt Source Override
 * entries when wiring legacy ISA IRQs.
 *
 * MSI-X bypasses the IOAPIC entirely so this is currently used by
 * nothing in Zeos; it exists so the next agent can wire legacy IRQs
 * (PS/2, ATA, etc.) through the APIC path once the PIC is masked.
 */

#ifndef ZEOS_IOAPIC_H
#define ZEOS_IOAPIC_H

#include <stdint.h>

int  ioapic_init(void);
int  ioapic_count(void);
int  ioapic_max_redirections(int idx);
void ioapic_set_irq(uint8_t legacy_irq, uint8_t vector, uint8_t lapic_id);
void ioapic_mask(uint8_t legacy_irq);

#endif /* ZEOS_IOAPIC_H */
