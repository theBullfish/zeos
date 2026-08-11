/*
 * Zeos — Timer
 *
 * PIT channel 0 for periodic IRQ0.
 * TSC calibrated against PIT for high-res timing.
 */

#include "timer.h"
#include "idt.h"
#include "io.h"
#include "fb.h"

#define PIT_CH0_DATA  0x40
#define PIT_CMD       0x43
#define PIT_BASE_FREQ 1193182  /* PIT oscillator: 1.193182 MHz */

static volatile uint64_t tick_count;
static uint64_t tsc_freq;

/*
 * PIT IRQ0 handler — increments tick counter.
 */
static void timer_isr(uint64_t vector, uint64_t error_code)
{
    (void)vector;
    (void)error_code;
    tick_count++;
}

uint64_t timer_read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Calibrate TSC against PIT.
 * Run PIT for ~50ms (enough for reasonable accuracy) and count TSC ticks.
 */
static void calibrate_tsc(uint32_t hz)
{
    /* Set PIT to one-shot mode, channel 0 */
    /* We'll use the current periodic mode and count ticks */
    uint64_t start_tick = tick_count;
    uint64_t start_tsc = timer_read_tsc();

    /* Wait for 50 ticks at the configured rate */
    uint64_t wait_ticks = 50;
    while (tick_count < start_tick + wait_ticks)
        __asm__ volatile("hlt");

    uint64_t end_tsc = timer_read_tsc();
    uint64_t elapsed_tsc = end_tsc - start_tsc;

    /* TSC freq = elapsed_tsc / (wait_ticks / hz)
     * But we don't know hz here at calibration time, we know it's what
     * we set in timer_init. We'll calculate based on PIT_BASE_FREQ. */

    /* Each tick = 1/hz seconds. wait_ticks ticks = wait_ticks/hz seconds.
     * tsc_freq = elapsed_tsc * hz / wait_ticks */

    /* tsc_freq = elapsed_tsc * hz / wait_ticks — honest, works at ANY PIT rate.
     * (Was hardcoded elapsed_tsc*20, i.e. assumed exactly 1000 Hz.) */
    tsc_freq = elapsed_tsc * hz / wait_ticks;
}

void timer_init(uint32_t hz)
{
    tick_count = 0;

    /* Register IRQ0 handler */
    idt_register(0x20, timer_isr);

    /* Program PIT channel 0: rate generator, lo/hi byte */
    uint16_t divisor = PIT_BASE_FREQ / hz;

    outb(PIT_CMD, 0x36);      /* Channel 0, lo/hi, rate generator */
    outb(PIT_CH0_DATA, divisor & 0xFF);
    outb(PIT_CH0_DATA, (divisor >> 8) & 0xFF);

    /* Unmask IRQ0 */
    pic_unmask(0);

    /* Wait a moment for PIT to start firing */
    while (tick_count < 2)
        __asm__ volatile("hlt");

    /* Calibrate TSC */
    calibrate_tsc(hz);
}

uint64_t timer_ticks(void)
{
    return tick_count;
}

uint64_t timer_tsc_freq(void)
{
    return tsc_freq;
}

void timer_wait_ms(uint32_t ms)
{
    uint64_t target_tsc = timer_read_tsc() + (tsc_freq / 1000) * ms;
    while (timer_read_tsc() < target_tsc)
        __asm__ volatile("pause");
}
