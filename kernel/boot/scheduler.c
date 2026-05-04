/*
 * Zeos scheduler -- chain resolution replaces the poll loop.
 * Per docs/PARADIGM_CONVERSION.md section 7: every node fires when
 * its inputs are ready; the runtime resolves the whole graph each
 * tick. Time-slicing is dead.
 *
 * B3 belief feeds back into scheduling: chains with high failure
 * rate (b3_beta > b3_alpha) skip every Nth tick until they recover.
 * MasQ records every tick with errors and slow-resolve incidents.
 *
 * Idle: HLT until next interrupt. Wake: any IRQ-driven chain
 * (kbd, mouse, NIC RX, NVMe completion, GPU vsync) bumps a pending
 * counter that the next tick consumes.
 */

#include "scheduler.h"
#include "chain.h"
#include "chain_registry.h"
#include "mde.h"
#include "keyboard.h"
#include "mouse.h"
#include "shell.h"
#include "serial.h"
#include "timer.h"
#include "kprint.h"

#define SCHED_LOG_RING 256

typedef struct {
    uint64_t tick;
    uint64_t tsc_start;
    uint64_t tsc_end;
    uint16_t chains_resolved;
    uint16_t live_chains;
    uint16_t errors;
    uint16_t slow_resolves;
} sched_log_entry_t;

static sched_log_entry_t s_log[SCHED_LOG_RING];
static uint32_t s_log_head;
static uint64_t s_log_count;        /* total entries written */

static uint64_t s_tick;             /* monotonically increasing */
static uint32_t s_quantum_us = 1000;
static uint32_t s_chains_resolved_last;
static uint32_t s_errors_last;
static uint32_t s_slow_resolves_total;
static uint32_t s_live_chains_last;

/* Backoff state per chain (skip every Nth tick). */
static uint8_t  s_skip_phase[MAX_CHAINS];

/* TPS sampling. */
static uint64_t s_sample_tsc;       /* tsc at start of current sample */
static uint64_t s_sample_tick;      /* tick count at start of sample  */
static uint32_t s_tps_last;         /* last completed sample's tps    */

void scheduler_quantum_us(uint32_t us)
{
    if (us == 0) us = 1;
    s_quantum_us = us;
}

uint32_t scheduler_quantum_us_get(void) { return s_quantum_us; }
uint64_t scheduler_tick_count(void)     { return s_tick; }
uint32_t scheduler_chains_resolved_last(void) { return s_chains_resolved_last; }
uint32_t scheduler_errors_last(void)    { return s_errors_last; }
uint32_t scheduler_slow_resolves_total(void) { return s_slow_resolves_total; }

void scheduler_init(void)
{
    s_log_head = 0;
    s_log_count = 0;
    s_tick = 0;
    s_quantum_us = 1000;
    s_chains_resolved_last = 0;
    s_errors_last = 0;
    s_slow_resolves_total = 0;
    s_live_chains_last = 0;
    for (int i = 0; i < MAX_CHAINS; i++) s_skip_phase[i] = 0;
    s_sample_tsc = timer_read_tsc();
    s_sample_tick = 0;
    s_tps_last = 0;
    kputs("[scheduler] chain resolution as scheduling primitive\n");
    kputs("[scheduler] quantum=1000us, B3 backoff enabled\n");
}

int scheduler_should_skip(int chain_id)
{
    chain_t *c = chain_get(chain_id);
    if (!c) return 0;
    /* B3 backoff: if failure beta exceeds alpha and we have enough
     * observations, skip every other tick to give the chain time to
     * settle. The skip phase advances each tick. */
    if (c->b3_observations < 4) return 0;
    if (c->b3_beta <= c->b3_alpha) return 0;
    /* Severity: ratio of failures. */
    float ratio = c->b3_beta / (c->b3_alpha + c->b3_beta);
    if (ratio < 0.5f) return 0;
    /* Skip period: 2 (50% failure) up to 8 (very bad). */
    int period = 2;
    if (ratio > 0.7f) period = 4;
    if (ratio > 0.9f) period = 8;
    s_skip_phase[chain_id]++;
    if ((s_skip_phase[chain_id] % period) != 0)
        return 1;
    return 0;
}

static void log_tick(uint64_t tsc_start, uint64_t tsc_end,
                     uint32_t resolved, uint32_t live,
                     uint32_t errors, uint32_t slow)
{
    sched_log_entry_t *e = &s_log[s_log_head];
    e->tick = s_tick;
    e->tsc_start = tsc_start;
    e->tsc_end = tsc_end;
    e->chains_resolved = (uint16_t)resolved;
    e->live_chains = (uint16_t)live;
    e->errors = (uint16_t)errors;
    e->slow_resolves = (uint16_t)slow;
    s_log_head = (s_log_head + 1) % SCHED_LOG_RING;
    s_log_count++;
}

/* Walk the chain registry: count LIVE chains, count chains whose last
 * resolve exceeded the quantum (slow). Done after mde_resolve_all so
 * timings are fresh. */
static void post_tick_scan(uint32_t *live_out, uint32_t *slow_out)
{
    uint32_t live = 0, slow = 0;
    float quantum_ms = (float)s_quantum_us / 1000.0f;
    int total = chain_count();
    /* Iterate by id since chains are sparse. */
    for (int id = 0; id < MAX_CHAINS; id++) {
        chain_t *c = chain_get(id);
        if (!c) continue;
        (void)total;
        if (c->status == CHAIN_LIVE) live++;
        if (c->last_resolve_ms > quantum_ms) slow++;
    }
    *live_out = live;
    *slow_out = slow;
}

void scheduler_idle(void)
{
    /* HLT until next interrupt. The PIT (1kHz) plus any device IRQ
     * (kbd, mouse, NIC, NVMe, etc.) wakes us promptly. */
    __asm__ volatile("hlt");
}

uint32_t scheduler_tps(void)
{
    return s_tps_last;
}

static void update_tps_sample(uint64_t now_tsc)
{
    uint64_t freq = timer_tsc_freq();
    if (freq == 0) return;
    /* 100 ms window */
    uint64_t window = freq / 10ULL;
    if (now_tsc - s_sample_tsc >= window) {
        uint64_t dt = now_tsc - s_sample_tsc;
        uint64_t dticks = s_tick - s_sample_tick;
        if (dt > 0) {
            /* tps = dticks * freq / dt */
            uint64_t tps = (dticks * freq) / dt;
            s_tps_last = (uint32_t)tps;
        }
        s_sample_tsc = now_tsc;
        s_sample_tick = s_tick;
    }
}

void scheduler_run(void)
{
    /* Banner reaffirms the paradigm switch. */
    kputs("[scheduler] entering chain-resolution main loop\n");

    for (;;) {
        s_tick++;
        uint64_t tsc_start = timer_read_tsc();

        /* 1. Resolve the entire chain graph in dependency order. */
        int errors = chain_registry_tick();

        uint64_t tsc_end = timer_read_tsc();

        /* 2. Post-tick stats. */
        uint32_t live = 0, slow = 0;
        post_tick_scan(&live, &slow);
        s_live_chains_last = live;
        s_chains_resolved_last = (uint32_t)live;  /* resolve_all walks all live */
        s_errors_last = (uint32_t)errors;
        if (slow > 0) {
            s_slow_resolves_total += slow;
            /* First slow event of a tick gets a one-line note so it's
             * visible during boot bringup; suppress otherwise. */
            if ((s_tick & 0xFFF) == 0) {
                kputs("[scheduler] slow resolve(s) this tick: ");
                kput_dec((uint64_t)slow);
                kputc('\n');
            }
        }
        log_tick(tsc_start, tsc_end, s_chains_resolved_last,
                 live, (uint32_t)errors, slow);
        update_tps_sample(tsc_end);

        /* 3. Drain the keyboard ASCII buffer into the shell pump.
         * CHAIN_KEYBOARD has already converted scancodes -> kb_buf
         * during chain_registry_tick(). The shell is just another
         * consumer of input_event signals. */
        char c;
        int dispatched = 0;
        while (keyboard_try_getc(&c)) {
            (void)shell_pump_char(c);
            dispatched++;
            if (dispatched >= 64) break;  /* fairness */
        }
        /* Serial RX: anyone driving us via the UART (debug console,
         * QEMU -serial stdio) feeds the same shell pump. CR/LF are
         * normalized so terminals work without raw mode. */
        while (serial_try_getc(&c)) {
            if (c == '\r') c = '\n';
            (void)shell_pump_char(c);
            dispatched++;
            if (dispatched >= 128) break;
        }

        /* 4. If nothing happened this tick, idle until an IRQ wakes us. */
        if (dispatched == 0 && live > 0 && errors == 0 && slow == 0) {
            /* Quick check: is anything pending we should service first? */
            if (keyboard_chain_pending() == 0 &&
                mouse_chain_pending()    == 0) {
                scheduler_idle();
            }
        }
    }
}

void scheduler_log_dump(int last_n)
{
    if (last_n <= 0 || last_n > SCHED_LOG_RING) last_n = 16;
    uint64_t total = s_log_count;
    if ((uint64_t)last_n > total) last_n = (int)total;

    kputs("\n  Scheduler MasQ log (last ");
    kput_dec((uint64_t)last_n);
    kputs(" of ");
    kput_dec(total);
    kputs(" ticks)\n");
    kputs("  ────────────────────────────────────────────\n");

    /* Walk back from head. */
    uint32_t idx = s_log_head;
    /* Move to oldest of the requested range. */
    for (int i = 0; i < last_n; i++) {
        idx = (idx == 0) ? (SCHED_LOG_RING - 1) : (idx - 1);
    }

    uint64_t freq = timer_tsc_freq();
    for (int i = 0; i < last_n; i++) {
        sched_log_entry_t *e = &s_log[idx];
        uint64_t dt = e->tsc_end - e->tsc_start;
        uint64_t us = (freq > 0) ? (dt * 1000000ULL / freq) : 0;
        kputs("  tick=");
        kput_dec(e->tick);
        kputs(" live=");
        kput_dec((uint64_t)e->live_chains);
        kputs(" resolved=");
        kput_dec((uint64_t)e->chains_resolved);
        kputs(" err=");
        kput_dec((uint64_t)e->errors);
        kputs(" slow=");
        kput_dec((uint64_t)e->slow_resolves);
        kputs(" dur=");
        kput_dec(us);
        kputs("us\n");
        idx = (idx + 1) % SCHED_LOG_RING;
    }
    kputs("\n");
}
