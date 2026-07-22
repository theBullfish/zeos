/* Zeos aarch64 — SMP bring-up (M3) via PSCI CPU_ON. */
#include <stdint.h>

volatile uint64_t g_sec_mpidr;
volatile int      g_sec_online;

/* PSCI CPU_ON (64-bit function id 0xC4000003). Conduit = HVC on qemu virt. */
int64_t psci_cpu_on(uint64_t cpu, uint64_t entry)
{
    register uint64_t x0 __asm__("x0") = 0xC4000003UL;
    register uint64_t x1 __asm__("x1") = cpu;
    register uint64_t x2 __asm__("x2") = entry;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("hvc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "memory", "cc");
    return (int64_t)x0;
}

/* Secondary core lands here (from secondary_entry in boot.S). Report and park.
 * Only touches globals (no UART) to avoid a cross-core console race. */
void kmain_secondary(void)
{
    uint64_t m;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(m));
    g_sec_mpidr = m & 0xFF;
    __asm__ volatile("dmb sy");
    g_sec_online = 1;
    __asm__ volatile("sev");            /* wake cpu0 if parked in wfe */
    for (;;) __asm__ volatile("wfe");
}
