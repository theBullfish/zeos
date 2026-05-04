/*
 * Zeos — Local APIC driver.
 *
 * Brings up the BSP's LAPIC, exposes the EOI write that MSI-X needs,
 * and calibrates the LAPIC bus frequency against the existing PIT/TSC
 * timer subsystem so the eventual scheduler-preemption agent has a
 * pre-measured ticks/µs constant to use.
 *
 * The APIC timer is intentionally left disarmed here (LVT masked).
 * Arming it for periodic preemption is a separate change.
 */

#ifndef ZEOS_LAPIC_H
#define ZEOS_LAPIC_H

#include <stdint.h>

int      lapic_init(void);
void     lapic_eoi(void);
uint32_t lapic_id(void);
int      lapic_ready(void);
uint64_t lapic_base(void);

void     lapic_timer_calibrate(void);
uint32_t lapic_ticks_per_us(void);

/* Arm a one-shot timer interrupt at `vector` after `ticks` LAPIC bus ticks.
 * `ticks` is at the divider configured in lapic_init (16). */
void     lapic_timer_oneshot(uint32_t ticks, uint8_t vector);

#endif /* ZEOS_LAPIC_H */
