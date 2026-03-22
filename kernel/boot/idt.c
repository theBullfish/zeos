/*
 * Zeos — IDT and interrupt handling
 *
 * Sets up the Interrupt Descriptor Table for x86_64 long mode.
 * Remaps the 8259 PIC so IRQ0-15 map to vectors 0x20-0x2F
 * (avoiding collision with CPU exceptions at 0x00-0x1F).
 */

#include "idt.h"
#include "io.h"
#include "fb.h"

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
    idt[vector].selector    = 0x38;  /* UEFI code segment selector */
    idt[vector].ist         = 0;
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
    if (vector < IDT_ENTRIES && handlers[vector]) {
        handlers[vector](vector, error_code);
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

/* Generate stubs for the IRQs we care about: IRQ0 (timer) and IRQ1 (keyboard) */
/* These are vectors 0x20 and 0x21 after PIC remap */
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

/*
 * Read the current CS selector — needed for IDT gate setup.
 */
static uint16_t get_cs(void)
{
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

void idt_init(void)
{
    uint16_t cs = get_cs();

    /* Zero out */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i] = (struct idt_entry){0};
        handlers[i] = 0;
    }

    /* Update all gates to use actual CS */
    /* Install IRQ stubs (vectors 0x20-0x2F) */
    typedef void (*stub_fn)(void);
    stub_fn stubs[16] = {
        isr_stub_0x20, isr_stub_0x21, isr_stub_0x22, isr_stub_0x23,
        isr_stub_0x24, isr_stub_0x25, isr_stub_0x26, isr_stub_0x27,
        isr_stub_0x28, isr_stub_0x29, isr_stub_0x2a, isr_stub_0x2b,
        isr_stub_0x2c, isr_stub_0x2d, isr_stub_0x2e, isr_stub_0x2f,
    };

    for (int i = 0; i < 16; i++) {
        idt_set_gate(0x20 + i, (uint64_t)stubs[i], IDT_GATE_INTERRUPT);
        idt[0x20 + i].selector = cs;
    }

    /* Remap PIC before enabling interrupts */
    pic_remap();

    /* Load IDT */
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;
    __asm__ volatile("lidt %0" : : "m"(idtr));
}
