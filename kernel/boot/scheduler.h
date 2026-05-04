/*
 * Zeos -- Scheduler
 *
 * Chain resolution IS the scheduler. Replaces the old polling main
 * loop in shell.c. See docs/PARADIGM_CONVERSION.md section 7 and
 * docs/CHAIN_CONTRACT.md.
 */

#ifndef ZEOS_SCHEDULER_H
#define ZEOS_SCHEDULER_H

#include <stdint.h>

/* One-shot init: zero the tick log + ring buffer, set default quantum. */
void scheduler_init(void);

/* Main loop. Calls chain_registry_tick() each iteration, dispatches
 * any pending shell input, idles via HLT when nothing to do. Never
 * returns. */
void scheduler_run(void);

/* Issue HLT until the next interrupt. Wakes on any IRQ. */
void scheduler_idle(void);

/* Set max wall-time per tick in microseconds. Default: 1000 us (1ms).
 * If a chain's resolve exceeds this it gets logged as a slow_resolve. */
void scheduler_quantum_us(uint32_t us);

/* B3 backoff helper: returns 1 if this chain's failure rate is high
 * enough that we should skip this tick. */
int  scheduler_should_skip(int chain_id);

/* Stats accessors -- exposed for selftest + cmd_scheduler_log. */
uint64_t scheduler_tick_count(void);
uint32_t scheduler_chains_resolved_last(void);
uint32_t scheduler_errors_last(void);
uint32_t scheduler_slow_resolves_total(void);
uint32_t scheduler_quantum_us_get(void);

/* Sample window: returns ticks-per-second observed over a rolling
 * 100ms-or-similar window. Used by the selftest. */
uint32_t scheduler_tps(void);

/* Print the last N MasQ tick records. Newest last. */
void scheduler_log_dump(int last_n);

#endif /* ZEOS_SCHEDULER_H */
