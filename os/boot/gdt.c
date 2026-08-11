/*
 * Zeos — Global Descriptor Table and Task State Segment
 *
 * In long mode, the GDT is mostly vestigial (base/limit ignored for
 * code/data segments), but it is still required for:
 *   1. Defining code vs. data segments (CS must point to a code descriptor)
 *   2. The TSS descriptor (needed for IST and ring transitions)
 *   3. DPL (privilege level) enforcement
 *
 * The TSS provides Interrupt Stack Table (IST) entries — dedicated stacks
 * for critical exceptions that must not trust the current stack:
 *   IST1: Double Fault (#DF) — current stack may be corrupted
 *   IST2: NMI — can fire at any time, even during exception handling
 *   IST3: Machine Check (#MC) — hardware failure, stack unreliable
 */

#include "gdt.h"
#include "pmm.h"
#include "kprint.h"

/* ── GDT entry (8 bytes for normal, 16 bytes for TSS) ─────── */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;  /* flags (4 bits) + limit_high (4 bits) */
    uint8_t  base_high;
} __attribute__((packed));

/* TSS descriptor is 16 bytes in long mode (two GDT entries) */
struct gdt_tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/*
 * x86_64 TSS structure.
 * Only IST and RSP0 matter in long mode.
 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;          /* Kernel stack for ring 3 → ring 0 */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;          /* IST stacks (1-7) */
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    /* Offset to I/O permission bitmap */
} __attribute__((packed));

/* ── Static storage ──────────────────────────────────────────── */

/*
 * GDT: null + kernel code + kernel data + user code + user data + TSS (16 bytes)
 * That's 5 normal entries (40 bytes) + 1 TSS entry (16 bytes) = 56 bytes = 7 entries
 */
static struct gdt_entry gdt[7] __attribute__((aligned(16)));
static struct tss tss __attribute__((aligned(16)));
static struct gdtr gdtr;

/* IST stacks — allocated from PMM */
static uint64_t ist_stack_1;  /* Double fault */
static uint64_t ist_stack_2;  /* NMI */
static uint64_t ist_stack_3;  /* Machine check */

/* ── Helpers ─────────────────────────────────────────────────── */

static void set_entry(int idx, uint32_t base, uint32_t limit,
                      uint8_t access, uint8_t flags)
{
    gdt[idx].limit_low   = limit & 0xFFFF;
    gdt[idx].base_low    = base & 0xFFFF;
    gdt[idx].base_mid    = (base >> 16) & 0xFF;
    gdt[idx].access      = access;
    gdt[idx].granularity  = ((flags & 0xF) << 4) | ((limit >> 16) & 0xF);
    gdt[idx].base_high   = (base >> 24) & 0xFF;
}

static void set_tss_entry(uint64_t base, uint32_t limit)
{
    struct gdt_tss_entry *tss_ent = (struct gdt_tss_entry *)&gdt[5];
    tss_ent->limit_low   = limit & 0xFFFF;
    tss_ent->base_low    = base & 0xFFFF;
    tss_ent->base_mid    = (base >> 16) & 0xFF;
    tss_ent->access      = 0x89;  /* Present, DPL=0, TSS available (not busy) */
    tss_ent->granularity  = (limit >> 16) & 0xF;
    tss_ent->base_high   = (base >> 24) & 0xFF;
    tss_ent->base_upper  = (uint32_t)(base >> 32);
    tss_ent->reserved    = 0;
}

static uint64_t alloc_ist_stack(void)
{
    /* Allocate pages for an IST stack */
    uint64_t pages = IST_STACK_SIZE / 4096;
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys)
        return 0;

    /* Stack grows downward: return top of allocation */
    return phys + IST_STACK_SIZE;
}

/* ── Public API ──────────────────────────────────────────────── */

void gdt_init(void)
{
    /* 0x00: Null descriptor */
    set_entry(0, 0, 0, 0, 0);

    /* 0x08: Kernel Code — 64-bit, DPL=0 */
    set_entry(1, 0, 0xFFFFF,
              0x9A,   /* P=1, DPL=0, S=1, E=1, R=1 (code, readable) */
              0xA);   /* L=1 (64-bit), G=0 (limit in bytes doesn't matter in long mode) */

    /* 0x10: Kernel Data — DPL=0 */
    set_entry(2, 0, 0xFFFFF,
              0x92,   /* P=1, DPL=0, S=1, W=1 (data, writable) */
              0x0);

    /* 0x18: User Code — 64-bit, DPL=3 */
    set_entry(3, 0, 0xFFFFF,
              0xFA,   /* P=1, DPL=3, S=1, E=1, R=1 (code, readable) */
              0xA);   /* L=1 (64-bit) */

    /* 0x20: User Data — DPL=3 */
    set_entry(4, 0, 0xFFFFF,
              0xF2,   /* P=1, DPL=3, S=1, W=1 (data, writable) */
              0x0);

    /* Allocate IST stacks */
    ist_stack_1 = alloc_ist_stack();
    ist_stack_2 = alloc_ist_stack();
    ist_stack_3 = alloc_ist_stack();

    if (!ist_stack_1 || !ist_stack_2 || !ist_stack_3) {
        kputs("GDT: WARNING — could not allocate IST stacks\n");
    }

    /* Set up TSS */
    for (int i = 0; i < (int)sizeof(tss); i++)
        ((uint8_t *)&tss)[i] = 0;

    tss.ist1 = ist_stack_1;
    tss.ist2 = ist_stack_2;
    tss.ist3 = ist_stack_3;
    tss.iomap_base = sizeof(tss);  /* No I/O bitmap (offset past TSS end) */

    /* 0x28: TSS descriptor (16 bytes, spans entries 5-6) */
    set_tss_entry((uint64_t)&tss, sizeof(tss) - 1);

    /* Load GDT */
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)&gdt;
    __asm__ volatile("lgdt %0" : : "m"(gdtr));

    /* Reload segment registers with new selectors */
    __asm__ volatile(
        /* Load new CS via far return */
        "pushq $0x08\n"             /* GDT_KERNEL_CODE */
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        /* Load data segments */
        "movw $0x10, %%ax\n"        /* GDT_KERNEL_DATA */
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        : : : "rax", "memory"
    );

    /* Load TSS */
    __asm__ volatile(
        "movw $0x28, %%ax\n"        /* GDT_TSS selector */
        "ltr %%ax\n"
        : : : "rax"
    );
}

uint16_t gdt_kernel_cs(void)
{
    return GDT_KERNEL_CODE;
}
