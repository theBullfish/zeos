/*
 * Zeos — IDT and interrupt handling
 *
 * Sets up the Interrupt Descriptor Table for x86_64 long mode.
 * Remaps the 8259 PIC so IRQ0-15 map to vectors 0x20-0x2F
 * (avoiding collision with CPU exceptions at 0x00-0x1F).
 */

#include "idt.h"
#include "gdt.h"
#include "panic.h"
#include "io.h"
#include "fb.h"
#include "msix.h"

/* IDT entry — 16 bytes in long mode */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;           /* Interrupt Stack Table offset (0 = none) */
    uint8_t  type_attr;     /* Gate type, DPL, present */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

/* IDTR — loaded via LIDT */
struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idtr idtr;
static isr_handler_t handlers[IDT_ENTRIES];

/*
 * Set an IDT gate entry.
 */
static void idt_set_gate(uint8_t vector, uint64_t handler_addr, uint8_t type)
{
    idt[vector].offset_low  = handler_addr & 0xFFFF;
    idt[vector].selector    = GDT_KERNEL_CODE;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = type;
    idt[vector].offset_mid  = (handler_addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler_addr >> 32) & 0xFFFFFFFF;
    idt[vector].reserved    = 0;
}

void idt_set_gate_ext(uint8_t vector, uint64_t handler_addr,
                      uint16_t selector, uint8_t ist, uint8_t type)
{
    idt[vector].offset_low  = handler_addr & 0xFFFF;
    idt[vector].selector    = selector;
    idt[vector].ist         = ist;
    idt[vector].type_attr   = type;
    idt[vector].offset_mid  = (handler_addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler_addr >> 32) & 0xFFFFFFFF;
    idt[vector].reserved    = 0;
}

/*
 * Common interrupt dispatch — called from assembly stubs.
 * Each stub pushes the vector number and error code (or 0).
 */
void isr_dispatch(uint64_t vector, uint64_t error_code)
{
    if (vector >= 0x40 && vector < 0x80) {
        /* MSI-X delivery range — dispatch through the vector pool. */
        msix_dispatch((int)vector);
    } else if (vector < IDT_ENTRIES && handlers[vector]) {
        handlers[vector](vector, error_code);
    } else if (vector < 0x20) {
        /* Unhandled CPU exception — panic with register capture */
        struct cpu_state regs;
        __asm__ volatile("movq %%rbx, %0" : "=m"(regs.rbx));
        __asm__ volatile("movq %%rbp, %0" : "=m"(regs.rbp));
        __asm__ volatile("movq %%r12, %0" : "=m"(regs.r12));
        __asm__ volatile("movq %%r13, %0" : "=m"(regs.r13));
        __asm__ volatile("movq %%r14, %0" : "=m"(regs.r14));
        __asm__ volatile("movq %%r15, %0" : "=m"(regs.r15));
        regs.rax = 0; regs.rcx = 0; regs.rdx = 0;
        regs.rsi = 0; regs.rdi = 0; regs.rsp = 0;
        regs.r8 = 0; regs.r9 = 0; regs.r10 = 0; regs.r11 = 0;
        regs.rip = 0; regs.rflags = 0;
        __asm__ volatile("movq %%cr0, %%rax; movq %%rax, %0" : "=m"(regs.cr0) : : "rax");
        __asm__ volatile("movq %%cr2, %%rax; movq %%rax, %0" : "=m"(regs.cr2) : : "rax");
        __asm__ volatile("movq %%cr3, %%rax; movq %%rax, %0" : "=m"(regs.cr3) : : "rax");
        __asm__ volatile("movq %%cr4, %%rax; movq %%rax, %0" : "=m"(regs.cr4) : : "rax");
        regs.cs = GDT_KERNEL_CODE;
        regs.ss = GDT_KERNEL_DATA;
        panic_with_state("Unhandled CPU exception", vector, error_code, &regs);
    } else {
        fb_puts("\n!!! Unhandled interrupt: vector=0x");
        fb_put_hex(vector);
        fb_puts(" error=0x");
        fb_put_hex(error_code);
        fb_puts("\n");
    }

    /* Send EOI for hardware interrupts (vectors 0x20-0x2F) */
    if (vector >= 0x20 && vector < 0x30) {
        pic_eoi((uint8_t)(vector - 0x20));
    }
}

/*
 * We need actual ISR stubs in assembly, but for the minimal bootstrap
 * we'll use a single stub approach: the ISR stubs are generated as
 * naked functions that push vector/error and call isr_dispatch.
 */

/* Generic ISR stub — no error code */
#define ISR_STUB_NOERR(n) \
    static void __attribute__((naked)) isr_stub_##n(void) { \
        __asm__ volatile( \
            "pushq $0\n"       /* Fake error code */ \
            "pushq $" #n "\n"  /* Vector number */ \
            "pushq %%rax\n" \
            "pushq %%rcx\n" \
            "pushq %%rdx\n" \
            "pushq %%rdi\n" \
            "pushq %%rsi\n" \
            "pushq %%r8\n" \
            "pushq %%r9\n" \
            "pushq %%r10\n" \
            "pushq %%r11\n" \
            "movq 72(%%rsp), %%rdi\n"  /* vector */ \
            "movq 80(%%rsp), %%rsi\n"  /* error code */ \
            "call isr_dispatch\n" \
            "popq %%r11\n" \
            "popq %%r10\n" \
            "popq %%r9\n" \
            "popq %%r8\n" \
            "popq %%rsi\n" \
            "popq %%rdi\n" \
            "popq %%rdx\n" \
            "popq %%rcx\n" \
            "popq %%rax\n" \
            "addq $16, %%rsp\n" /* Pop vector + error */ \
            "iretq\n" \
            ::: "memory"); \
    }

/* ISR stub WITH error code (CPU pushes error code for vectors 8, 10-14, 17, 21, 29, 30) */
#define ISR_STUB_ERR(n) \
    static void __attribute__((naked)) isr_stub_##n(void) { \
        __asm__ volatile( \
            /* Error code already on stack from CPU */ \
            "pushq $" #n "\n"  /* Vector number */ \
            "pushq %%rax\n" \
            "pushq %%rcx\n" \
            "pushq %%rdx\n" \
            "pushq %%rdi\n" \
            "pushq %%rsi\n" \
            "pushq %%r8\n" \
            "pushq %%r9\n" \
            "pushq %%r10\n" \
            "pushq %%r11\n" \
            "movq 72(%%rsp), %%rdi\n"  /* vector */ \
            "movq 80(%%rsp), %%rsi\n"  /* error code */ \
            "call isr_dispatch\n" \
            "popq %%r11\n" \
            "popq %%r10\n" \
            "popq %%r9\n" \
            "popq %%r8\n" \
            "popq %%rsi\n" \
            "popq %%rdi\n" \
            "popq %%rdx\n" \
            "popq %%rcx\n" \
            "popq %%rax\n" \
            "addq $16, %%rsp\n" /* Pop vector + error */ \
            "iretq\n" \
            ::: "memory"); \
    }

/* CPU exception stubs — vectors 0-20 */
ISR_STUB_NOERR(0x00)   /* #DE Divide Error */
ISR_STUB_NOERR(0x01)   /* #DB Debug */
ISR_STUB_NOERR(0x02)   /* NMI */
ISR_STUB_NOERR(0x03)   /* #BP Breakpoint */
ISR_STUB_NOERR(0x04)   /* #OF Overflow */
ISR_STUB_NOERR(0x05)   /* #BR Bound Range */
ISR_STUB_NOERR(0x06)   /* #UD Invalid Opcode */
ISR_STUB_NOERR(0x07)   /* #NM Device Not Available */
ISR_STUB_ERR(0x08)      /* #DF Double Fault */
ISR_STUB_NOERR(0x09)   /* Coprocessor Segment Overrun */
ISR_STUB_ERR(0x0a)      /* #TS Invalid TSS */
ISR_STUB_ERR(0x0b)      /* #NP Segment Not Present */
ISR_STUB_ERR(0x0c)      /* #SS Stack-Segment Fault */
ISR_STUB_ERR(0x0d)      /* #GP General Protection Fault */
ISR_STUB_ERR(0x0e)      /* #PF Page Fault */
ISR_STUB_NOERR(0x0f)   /* (reserved) */
ISR_STUB_NOERR(0x10)   /* #MF x87 FP Error */
ISR_STUB_ERR(0x11)      /* #AC Alignment Check */
ISR_STUB_NOERR(0x12)   /* #MC Machine Check */
ISR_STUB_NOERR(0x13)   /* #XM SIMD FP Error */
ISR_STUB_NOERR(0x14)   /* #VE Virtualization */

/* IRQ stubs (vectors 0x20-0x2F after PIC remap) */
ISR_STUB_NOERR(0x20)
ISR_STUB_NOERR(0x21)
ISR_STUB_NOERR(0x22)
ISR_STUB_NOERR(0x23)
ISR_STUB_NOERR(0x24)
ISR_STUB_NOERR(0x25)
ISR_STUB_NOERR(0x26)
ISR_STUB_NOERR(0x27)
ISR_STUB_NOERR(0x28)
ISR_STUB_NOERR(0x29)
ISR_STUB_NOERR(0x2a)
ISR_STUB_NOERR(0x2b)
ISR_STUB_NOERR(0x2c)
ISR_STUB_NOERR(0x2d)
ISR_STUB_NOERR(0x2e)
ISR_STUB_NOERR(0x2f)

/* LAPIC timer preemption vector — 0xEF. Used by scheduler to preempt
 * runaway chain_resolve() calls via setjmp/longjmp. */
ISR_STUB_NOERR(0xef)

/* TLB shootdown IPI — 0xF0. BSP issues lapic_send_ipi(0xF0) after
 * modifying kernel page tables; APs INVLPG the pending range and ack.
 * ISR body lives in smp.c (tlb_shootdown_isr); registered via
 * idt_register at boot. */
ISR_STUB_NOERR(0xf0)

/* MSI-X stubs (vectors 0x40-0x7F, 64 vectors) */
ISR_STUB_NOERR(0x40) ISR_STUB_NOERR(0x41) ISR_STUB_NOERR(0x42) ISR_STUB_NOERR(0x43)
ISR_STUB_NOERR(0x44) ISR_STUB_NOERR(0x45) ISR_STUB_NOERR(0x46) ISR_STUB_NOERR(0x47)
ISR_STUB_NOERR(0x48) ISR_STUB_NOERR(0x49) ISR_STUB_NOERR(0x4a) ISR_STUB_NOERR(0x4b)
ISR_STUB_NOERR(0x4c) ISR_STUB_NOERR(0x4d) ISR_STUB_NOERR(0x4e) ISR_STUB_NOERR(0x4f)
ISR_STUB_NOERR(0x50) ISR_STUB_NOERR(0x51) ISR_STUB_NOERR(0x52) ISR_STUB_NOERR(0x53)
ISR_STUB_NOERR(0x54) ISR_STUB_NOERR(0x55) ISR_STUB_NOERR(0x56) ISR_STUB_NOERR(0x57)
ISR_STUB_NOERR(0x58) ISR_STUB_NOERR(0x59) ISR_STUB_NOERR(0x5a) ISR_STUB_NOERR(0x5b)
ISR_STUB_NOERR(0x5c) ISR_STUB_NOERR(0x5d) ISR_STUB_NOERR(0x5e) ISR_STUB_NOERR(0x5f)
ISR_STUB_NOERR(0x60) ISR_STUB_NOERR(0x61) ISR_STUB_NOERR(0x62) ISR_STUB_NOERR(0x63)
ISR_STUB_NOERR(0x64) ISR_STUB_NOERR(0x65) ISR_STUB_NOERR(0x66) ISR_STUB_NOERR(0x67)
ISR_STUB_NOERR(0x68) ISR_STUB_NOERR(0x69) ISR_STUB_NOERR(0x6a) ISR_STUB_NOERR(0x6b)
ISR_STUB_NOERR(0x6c) ISR_STUB_NOERR(0x6d) ISR_STUB_NOERR(0x6e) ISR_STUB_NOERR(0x6f)
ISR_STUB_NOERR(0x70) ISR_STUB_NOERR(0x71) ISR_STUB_NOERR(0x72) ISR_STUB_NOERR(0x73)
ISR_STUB_NOERR(0x74) ISR_STUB_NOERR(0x75) ISR_STUB_NOERR(0x76) ISR_STUB_NOERR(0x77)
ISR_STUB_NOERR(0x78) ISR_STUB_NOERR(0x79) ISR_STUB_NOERR(0x7a) ISR_STUB_NOERR(0x7b)
ISR_STUB_NOERR(0x7c) ISR_STUB_NOERR(0x7d) ISR_STUB_NOERR(0x7e) ISR_STUB_NOERR(0x7f)

/*
 * Remap the 8259 PIC.
 * IRQ 0-7  → vectors 0x20-0x27
 * IRQ 8-15 → vectors 0x28-0x2F
 */
void pic_remap(void)
{
    /* Save masks */
    uint8_t mask1 = inb(0x21);
    uint8_t mask2 = inb(0xA1);

    /* ICW1: start init, ICW4 needed */
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();

    /* ICW2: vector offsets */
    outb(0x21, 0x20); io_wait();  /* Master: IRQ 0-7 → 0x20-0x27 */
    outb(0xA1, 0x28); io_wait();  /* Slave:  IRQ 8-15 → 0x28-0x2F */

    /* ICW3: cascade */
    outb(0x21, 0x04); io_wait();  /* Master: slave on IRQ2 */
    outb(0xA1, 0x02); io_wait();  /* Slave: cascade identity */

    /* ICW4: 8086 mode */
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();

    /* Restore masks — mask everything initially */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    (void)mask1;
    (void)mask2;
}

void pic_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(0xA0, 0x20);  /* EOI to slave */
    outb(0x20, 0x20);      /* EOI to master */
}

void pic_unmask(uint8_t irq)
{
    uint16_t port;
    if (irq < 8) {
        port = 0x21;
    } else {
        /* Slave IRQs (8-15) cascade through the master's IRQ2 line. Unmasking
         * only the slave-side bit leaves that signal unable to ever reach the
         * CPU if IRQ2 itself is still masked on the master -- which it is by
         * default (pic_remap masks everything). Every slave IRQ is dead until
         * this is opened; do it here so no caller has to remember it. */
        outb(0x21, inb(0x21) & ~(1 << 2));
        port = 0xA1;
        irq -= 8;
    }
    outb(port, inb(port) & ~(1 << irq));
}

void pic_mask(uint8_t irq)
{
    uint16_t port;
    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }
    outb(port, inb(port) | (1 << irq));
}

void idt_register(uint8_t vector, isr_handler_t handler)
{
    handlers[vector] = handler;
}

void idt_init(void)
{
    uint16_t cs = gdt_kernel_cs();

    /* Zero out */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i] = (struct idt_entry){0};
        handlers[i] = 0;
    }

    /* ── CPU exception stubs (vectors 0x00-0x14) ────────────── */
    typedef void (*stub_fn)(void);
    stub_fn exc_stubs[21] = {
        isr_stub_0x00, isr_stub_0x01, isr_stub_0x02, isr_stub_0x03,
        isr_stub_0x04, isr_stub_0x05, isr_stub_0x06, isr_stub_0x07,
        isr_stub_0x08, isr_stub_0x09, isr_stub_0x0a, isr_stub_0x0b,
        isr_stub_0x0c, isr_stub_0x0d, isr_stub_0x0e, isr_stub_0x0f,
        isr_stub_0x10, isr_stub_0x11, isr_stub_0x12, isr_stub_0x13,
        isr_stub_0x14,
    };

    for (int i = 0; i < 21; i++) {
        idt_set_gate(i, (uint64_t)exc_stubs[i], IDT_GATE_INTERRUPT);
        idt[i].selector = cs;
    }

    /* Double fault (#DF) uses IST1 — must not trust the current stack */
    idt[8].ist = IST_DOUBLE_FAULT;

    /* NMI uses IST2 — can fire at any time */
    idt[2].ist = IST_NMI;

    /* Machine check uses IST3 — hardware failure, stack unreliable */
    idt[18].ist = IST_MCE;

    /* ── IRQ stubs (vectors 0x20-0x2F) ──────────────────────── */
    stub_fn irq_stubs[16] = {
        isr_stub_0x20, isr_stub_0x21, isr_stub_0x22, isr_stub_0x23,
        isr_stub_0x24, isr_stub_0x25, isr_stub_0x26, isr_stub_0x27,
        isr_stub_0x28, isr_stub_0x29, isr_stub_0x2a, isr_stub_0x2b,
        isr_stub_0x2c, isr_stub_0x2d, isr_stub_0x2e, isr_stub_0x2f,
    };

    for (int i = 0; i < 16; i++) {
        idt_set_gate(0x20 + i, (uint64_t)irq_stubs[i], IDT_GATE_INTERRUPT);
        idt[0x20 + i].selector = cs;
    }

    /* ── MSI-X stubs (vectors 0x40-0x7F) ────────────────────── */
    stub_fn msix_stubs[64] = {
        isr_stub_0x40, isr_stub_0x41, isr_stub_0x42, isr_stub_0x43,
        isr_stub_0x44, isr_stub_0x45, isr_stub_0x46, isr_stub_0x47,
        isr_stub_0x48, isr_stub_0x49, isr_stub_0x4a, isr_stub_0x4b,
        isr_stub_0x4c, isr_stub_0x4d, isr_stub_0x4e, isr_stub_0x4f,
        isr_stub_0x50, isr_stub_0x51, isr_stub_0x52, isr_stub_0x53,
        isr_stub_0x54, isr_stub_0x55, isr_stub_0x56, isr_stub_0x57,
        isr_stub_0x58, isr_stub_0x59, isr_stub_0x5a, isr_stub_0x5b,
        isr_stub_0x5c, isr_stub_0x5d, isr_stub_0x5e, isr_stub_0x5f,
        isr_stub_0x60, isr_stub_0x61, isr_stub_0x62, isr_stub_0x63,
        isr_stub_0x64, isr_stub_0x65, isr_stub_0x66, isr_stub_0x67,
        isr_stub_0x68, isr_stub_0x69, isr_stub_0x6a, isr_stub_0x6b,
        isr_stub_0x6c, isr_stub_0x6d, isr_stub_0x6e, isr_stub_0x6f,
        isr_stub_0x70, isr_stub_0x71, isr_stub_0x72, isr_stub_0x73,
        isr_stub_0x74, isr_stub_0x75, isr_stub_0x76, isr_stub_0x77,
        isr_stub_0x78, isr_stub_0x79, isr_stub_0x7a, isr_stub_0x7b,
        isr_stub_0x7c, isr_stub_0x7d, isr_stub_0x7e, isr_stub_0x7f,
    };
    for (int i = 0; i < 64; i++) {
        idt_set_gate(0x40 + i, (uint64_t)msix_stubs[i], IDT_GATE_INTERRUPT);
        idt[0x40 + i].selector = cs;
    }

    /* LAPIC timer preemption vector at 0xEF — wired so the scheduler
     * can arm a one-shot timer and longjmp out of a hung chain_resolve. */
    idt_set_gate(0xEF, (uint64_t)isr_stub_0xef, IDT_GATE_INTERRUPT);
    idt[0xEF].selector = cs;

    /* TLB shootdown IPI vector at 0xF0 — handler body in smp.c. The
     * generic isr_dispatch will invoke handlers[0xF0] = tlb_shootdown_isr,
     * which performs the local INVLPG/CR3 reload and writes LAPIC EOI. */
    idt_set_gate(0xF0, (uint64_t)isr_stub_0xf0, IDT_GATE_INTERRUPT);
    idt[0xF0].selector = cs;
    {
        extern void tlb_shootdown_isr(uint64_t vector, uint64_t error_code);
        idt_register(0xF0, tlb_shootdown_isr);
    }

    /* Initialize MSI-X subsystem (clear vector pool). */
    msix_init();

    /* Remap PIC before enabling interrupts */
    pic_remap();

    /* Load IDT */
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;
    __asm__ volatile("lidt %0" : : "m"(idtr));
}
