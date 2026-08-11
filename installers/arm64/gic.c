/* Zeos aarch64 — GICv3 (M2). QEMU virt: GICD @ 0x0800_0000, GICR @ 0x080A_0000.
 * Single core, Group1 non-secure. System-register CPU interface (ICC_*). */
#include <stdint.h>

#include "platform.h"
#define GICD       (g_plat.gicd)
#define GICR       (g_plat.gicc ? g_plat.gicc : g_plat.gicd + 0xA0000UL)
#define GICR_SGI   (GICR + 0x10000)          /* SGI/PPI frame */
#define REG(a)     (*(volatile uint32_t *)(a))

/* ICC_* system registers via raw S-encodings (portable, no -march needed). */
#define ICC_SRE     "S3_0_C12_C12_5"
#define ICC_PMR     "S3_0_C4_C6_0"
#define ICC_IGRPEN1 "S3_0_C12_C12_7"
#define ICC_IAR1    "S3_0_C12_C12_0"
#define ICC_EOIR1   "S3_0_C12_C12_1"

extern void timer_irq(void);

void gic_init(void)
{
    /* Distributor: affinity routing + Group1 enable. */
    REG(GICD + 0x000) = (1u << 4) | (1u << 1);   /* GICD_CTLR: ARE_NS | EnableGrp1 */

    /* Redistributor: wake it up. */
    uint32_t w = REG(GICR + 0x014);              /* GICR_WAKER */
    w &= ~(1u << 1);                             /* clear ProcessorSleep */
    REG(GICR + 0x014) = w;
    while (REG(GICR + 0x014) & (1u << 2)) { }    /* wait ChildrenAsleep == 0 */

    /* CPU interface. */
    uint64_t sre;
    __asm__ volatile("mrs %0, " ICC_SRE : "=r"(sre));
    sre |= 1;                                    /* SRE: system-register access */
    __asm__ volatile("msr " ICC_SRE ", %0" :: "r"(sre));
    __asm__ volatile("isb");
    __asm__ volatile("msr " ICC_PMR ", %0"     :: "r"((uint64_t)0xFF)); /* allow all */
    __asm__ volatile("msr " ICC_IGRPEN1 ", %0" :: "r"((uint64_t)1));    /* enable Grp1 */
    __asm__ volatile("isb");
}

void gic_enable_ppi(unsigned intid)
{
    REG(GICR_SGI + 0x080) |= (1u << intid);                   /* IGROUPR0: group1 */
    ((volatile uint8_t *)(GICR_SGI + 0x400))[intid] = 0x00;   /* IPRIORITYR: top */
    REG(GICR_SGI + 0x100) = (1u << intid);                    /* ISENABLER0: enable */
}

/* Called from irq_entry (vectors.S). Ack -> dispatch -> EOI. */
void irq_handler(void)
{
    uint64_t iar;
    __asm__ volatile("mrs %0, " ICC_IAR1 : "=r"(iar));
    unsigned intid = (unsigned)(iar & 0xFFFFFF);
    if (intid == 30)             /* EL1 physical timer PPI */
        timer_irq();
    __asm__ volatile("msr " ICC_EOIR1 ", %0" :: "r"(iar));
}
