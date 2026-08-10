/* Zeos aarch64 — MMU bring-up (M1).
 * Identity map, 39-bit VA, 4KB granule, 1GB L1 blocks.
 * idx0 = device (peripherals: GIC 0x08.., UART 0x09..); idx1..7 = normal RAM. */
#include <stdint.h>

static uint64_t l1_table[512] __attribute__((aligned(4096)));

#define DESC_BLOCK  (1UL << 0)      /* valid block at L1: bits[1:0]=0b01 */
#define DESC_AF     (1UL << 10)
#define DESC_SH_IS  (3UL << 8)      /* inner shareable */
#define ATTR(idx)   ((uint64_t)(idx) << 2)

void mmu_init(void)
{
    /* MAIR: attr0 = Normal Write-Back (0xFF), attr1 = Device-nGnRnE (0x00) */
    uint64_t mair = (0xFFUL << 0) | (0x00UL << 8);
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair));

    for (int i = 0; i < 512; i++)
        l1_table[i] = 0;

    /* 0..1GB -> device */
    l1_table[0] = 0UL | DESC_BLOCK | DESC_AF | ATTR(1);
    /* 1GB..8GB -> normal RAM (qemu virt RAM base = 0x40000000) */
    for (int i = 1; i < 8; i++) {
        uint64_t pa = (uint64_t)i * 0x40000000UL;
        l1_table[i] = pa | DESC_BLOCK | DESC_AF | DESC_SH_IS | ATTR(0);
    }

    uint64_t tcr = (25UL << 0)   /* T0SZ = 25 -> 39-bit VA */
                 | (1UL  << 8)   /* IRGN0 = WBWA */
                 | (1UL  << 10)  /* ORGN0 = WBWA */
                 | (3UL  << 12)  /* SH0   = inner */
                 | (0UL  << 14)  /* TG0   = 4KB */
                 | (1UL  << 23)  /* EPD1  = disable TTBR1 walks */
                 | (2UL  << 32); /* IPS   = 40-bit PA */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));

    uint64_t ttbr0 = (uint64_t)&l1_table[0];
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr0));
    __asm__ volatile("isb");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0)    /* M: MMU enable */
           | (1UL << 2)    /* C: data cache */
           | (1UL << 12);  /* I: instruction cache */
    __asm__ volatile("dsb ish; isb");
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb");
}
