/*
 * Zeos — Kernel Panic
 *
 * When something goes fatally wrong, panic() captures register state,
 * prints a diagnostic, and halts all CPUs. Never returns.
 */

#ifndef ZEOS_PANIC_H
#define ZEOS_PANIC_H

#include <stdint.h>

/* Saved CPU state from an exception or manual capture */
struct cpu_state {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cr0, cr2, cr3, cr4;
    uint64_t cs, ss;
};

/*
 * Halt the system with a diagnostic message.
 * Never returns.
 */
__attribute__((noreturn))
void panic(const char *msg);

/*
 * Halt with a message and saved CPU state (from exception handler).
 * Never returns.
 */
__attribute__((noreturn))
void panic_with_state(const char *msg, uint64_t vector, uint64_t error_code,
                      struct cpu_state *regs);

#endif /* ZEOS_PANIC_H */
