/*
 * Zeos — Kernel Panic
 *
 * Captures CPU state, prints diagnostics to both framebuffer and serial,
 * and halts. The system is dead after this — the only recovery is reboot.
 */

#include "panic.h"
#include "fb.h"
#include "kprint.h"

static void print_reg(const char *name, uint64_t val)
{
    kputs("  ");
    kputs(name);
    kputs("=0x");
    kput_hex(val);
}

static void dump_state(struct cpu_state *regs)
{
    kputs("\n--- Register Dump ---\n");
    print_reg("RAX", regs->rax);  print_reg(" RBX", regs->rbx);
    print_reg(" RCX", regs->rcx); print_reg(" RDX", regs->rdx);
    kputs("\n");
    print_reg("RSI", regs->rsi);  print_reg(" RDI", regs->rdi);
    print_reg(" RBP", regs->rbp); print_reg(" RSP", regs->rsp);
    kputs("\n");
    print_reg("R8 ", regs->r8);   print_reg(" R9 ", regs->r9);
    print_reg(" R10", regs->r10); print_reg(" R11", regs->r11);
    kputs("\n");
    print_reg("R12", regs->r12);  print_reg(" R13", regs->r13);
    print_reg(" R14", regs->r14); print_reg(" R15", regs->r15);
    kputs("\n");
    print_reg("RIP", regs->rip);  print_reg(" FLG", regs->rflags);
    kputs("\n");
    print_reg("CR0", regs->cr0);  print_reg(" CR2", regs->cr2);
    print_reg(" CR3", regs->cr3); print_reg(" CR4", regs->cr4);
    kputs("\n");
    print_reg("CS ", regs->cs);   print_reg(" SS ", regs->ss);
    kputs("\n");
}

static const char *exception_names[] = {
    "#DE Divide Error",                 /* 0 */
    "#DB Debug",                        /* 1 */
    "NMI",                              /* 2 */
    "#BP Breakpoint",                   /* 3 */
    "#OF Overflow",                     /* 4 */
    "#BR Bound Range",                  /* 5 */
    "#UD Invalid Opcode",               /* 6 */
    "#NM Device Not Available",         /* 7 */
    "#DF Double Fault",                 /* 8 */
    "Coprocessor Segment Overrun",      /* 9 */
    "#TS Invalid TSS",                  /* 10 */
    "#NP Segment Not Present",          /* 11 */
    "#SS Stack-Segment Fault",          /* 12 */
    "#GP General Protection Fault",     /* 13 */
    "#PF Page Fault",                   /* 14 */
    "(reserved)",                       /* 15 */
    "#MF x87 FP Error",                /* 16 */
    "#AC Alignment Check",              /* 17 */
    "#MC Machine Check",                /* 18 */
    "#XM SIMD FP Error",               /* 19 */
    "#VE Virtualization",               /* 20 */
    "#CP Control Protection",           /* 21 */
};

__attribute__((noreturn))
void panic(const char *msg)
{
    __asm__ volatile("cli");

    kputs("\n\n");
    kputs("!!! KERNEL PANIC !!!\n");
    kputs(msg);
    kputs("\n\n");
    kputs("System halted. Reboot to continue.\n");

    for (;;)
        __asm__ volatile("hlt");
}

__attribute__((noreturn))
void panic_with_state(const char *msg, uint64_t vector, uint64_t error_code,
                      struct cpu_state *regs)
{
    __asm__ volatile("cli");

    kputs("\n\n");
    kputs("!!! KERNEL PANIC !!!\n");
    kputs(msg);
    kputs("\n");

    /* Exception name */
    kputs("Exception: vector ");
    kput_dec(vector);
    kputs(" — ");
    if (vector < sizeof(exception_names) / sizeof(exception_names[0]))
        kputs(exception_names[vector]);
    else
        kputs("(unknown)");
    kputs("\n");

    kputs("Error code: 0x");
    kput_hex(error_code);
    kputs("\n");

    /* Page fault specifics */
    if (vector == 14) {
        kputs("Fault address (CR2): 0x");
        kput_hex(regs->cr2);
        kputs("\n");
        kputs("Cause: ");
        if (!(error_code & 1)) kputs("page not present");
        else                   kputs("protection violation");
        if (error_code & 2)    kputs(", write");
        else                   kputs(", read");
        if (error_code & 4)    kputs(", user-mode");
        else                   kputs(", kernel-mode");
        if (error_code & 8)    kputs(", reserved-bit");
        if (error_code & 16)   kputs(", instruction-fetch");
        kputs("\n");
    }

    /* GPF specifics */
    if (vector == 13 && error_code != 0) {
        kputs("Selector index: 0x");
        kput_hex(error_code >> 3);
        kputs(" (");
        if (error_code & 1) kputs("external");
        else                kputs("internal");
        kputs(", ");
        switch ((error_code >> 1) & 3) {
            case 0: kputs("GDT"); break;
            case 1: kputs("IDT"); break;
            case 2: kputs("LDT"); break;
            case 3: kputs("IDT"); break;
        }
        kputs(")\n");
    }

    if (regs)
        dump_state(regs);

    kputs("\nSystem halted. Reboot to continue.\n");

    for (;;)
        __asm__ volatile("hlt");
}
