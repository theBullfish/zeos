/* Zeos aarch64 — bring-up orchestrator. Climbs the ladder, prints each rung. */
#include <stdint.h>
#include <stddef.h>
extern void kputs(const char *);
extern void kputc(char);
extern void kput_hex(uint64_t);
extern void kput_dec(uint64_t);
extern void mmu_init(void);
extern void heap_init(uint64_t);
extern void *kmalloc(uint64_t);
extern void kfree(void *);
extern uint64_t heap_used_bytes(void);
extern void gic_init(void);
extern void gic_enable_ppi(unsigned);
extern void timer_init(uint64_t);
extern uint64_t timer_ticks(void);
extern int64_t psci_cpu_on(uint64_t, uint64_t);
extern void secondary_entry(void);
extern volatile uint64_t g_sec_mpidr;
extern volatile int g_sec_online;
/* freestanding runtime */
extern size_t strlen(const char *);
extern char *strcpy(char *, const char *);
extern int strcmp(const char *, const char *);
extern int zp_run(const char *);

static inline void irq_enable(void) { __asm__ volatile("msr daifclr, #2"); }

void kmain_aarch64(void)
{
    kputs("\n================ ZEOS / aarch64 ================\n");
    kputs("[M0] boot ok: EL1, PL011 UART, VBAR_EL1 installed.\n");

    kputs("[M1] enabling MMU (identity map, 39-bit VA, 4KB)...\n");
    mmu_init();
    heap_init(0);
    kputs("[M1] MMU: ON  -- still executing => translation is good.\n");

    kputs("[M2] GICv3 + generic timer @100Hz, unmasking IRQs...\n");
    gic_init();
    gic_enable_ppi(30);
    timer_init(100);
    irq_enable();
    while (timer_ticks() < 5) __asm__ volatile("wfi");
    kputs("[M2] timer IRQ fired, ticks="); kput_dec(timer_ticks());
    kputs("  -- the kernel has a HEARTBEAT.\n");

    kputs("[M3] PSCI CPU_ON -> secondary core 1...\n");
    int64_t r = psci_cpu_on(1, (uint64_t)secondary_entry);
    kputs("[M3] PSCI returned="); kput_hex((uint64_t)r); kputs("\n");
    for (volatile long i = 0; i < 200000000 && !g_sec_online; i++) { }
    if (g_sec_online) {
        kputs("[M3] secondary ONLINE, mpidr="); kput_dec(g_sec_mpidr);
        kputs("  -- SMP alive.\n");
    } else {
        kputs("[M3] secondary did NOT report in.\n");
    }

    /* M4 -- the kernel C runtime that every portable Zeos module builds on. */
    kputs("[M4] runtime layer: freestanding strings + heap...\n");
    char buf[24];
    strcpy(buf, "Zeos");
    strcpy(buf + 4, "/aarch64");
    void *p1 = kmalloc(1000);
    void *p2 = kmalloc(2000);
    kputs("[M4] strlen(\"Zeos\")="); kput_dec(strlen("Zeos"));
    kputs(" strcpy=\""); kputs(buf); kputs("\"");
    kputs(" match="); kput_dec(strcmp(buf, "Zeos/aarch64") == 0);
    kputs("\n[M4] kmalloc p1="); kput_hex((uint64_t)p1);
    kputs(" p2="); kput_hex((uint64_t)p2);
    kputs(" heap_used="); kput_dec(heap_used_bytes()); kputs(" bytes\n");
    kputs("[M4] runtime LIVE -- portable Zeos code can now build on this base.\n");

    /* M4.1 -- Zeos's own language engine, on bare-metal ARM. */
    kputs("[M4] launching the Z+ engine (zplus.c, 4535 LOC) on aarch64...\n");
    int fired = zp_run("heartbeat : tick(rate: 1) -> print\n");
    kputs("[M4] zp_run() returned="); kput_dec((uint64_t)(int64_t)fired);
    kputs("  -- Z+ interpreter + signal-chain runtime executed on bare-metal aarch64.\n");
    kputs("================================================\n");
}
