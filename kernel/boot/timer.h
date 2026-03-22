/*
 * Zeos — Timer (PIT + TSC)
 *
 * Uses the PIT (8254) for initial calibration, then TSC for
 * high-resolution timing. The PIT provides IRQ0 ticks for
 * scheduling until we move to APIC timer.
 */

#ifndef ZEOS_TIMER_H
#define ZEOS_TIMER_H

#include <stdint.h>

/* Initialize the PIT and TSC calibration */
void timer_init(uint32_t hz);

/* Get the current tick count (from PIT IRQ0) */
uint64_t timer_ticks(void);

/* Get TSC frequency in Hz (calibrated against PIT) */
uint64_t timer_tsc_freq(void);

/* Read TSC */
uint64_t timer_read_tsc(void);

/* Busy-wait for approximately N milliseconds */
void timer_wait_ms(uint32_t ms);

#endif /* ZEOS_TIMER_H */
