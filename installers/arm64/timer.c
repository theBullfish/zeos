/* Zeos aarch64 — ARM generic timer (M2). EL1 physical timer, PPI INTID 30. */
#include <stdint.h>

static uint64_t interval;
static volatile uint64_t ticks;

void timer_init(uint64_t hz)
{
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    interval = freq / hz;
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(interval));
    __asm__ volatile("msr cntp_ctl_el0, %0"  :: "r"((uint64_t)1)); /* ENABLE, IMASK=0 */
    __asm__ volatile("isb");
}

void timer_irq(void)
{
    ticks++;
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(interval));    /* re-arm */
}

uint64_t timer_ticks(void) { return ticks; }

/* Helpers the portable kernel expects. */
uint64_t timer_read_tsc(void)
{ uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }

void timer_wait_ms(uint64_t ms)
{
    uint64_t f, start, now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
    uint64_t target = start + (f * ms) / 1000;
    do { __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now)); } while (now < target);
}
