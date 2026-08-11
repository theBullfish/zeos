/*
 * Zeos aarch64 — discovered platform description.
 *
 * Every address in here is READ FROM THE DEVICE TREE at boot, not compiled in.
 * Code must use g_plat.<x>; a literal MMIO address anywhere in the ARM installer
 * is a bug — it locks the image to one board and breaks the point of Zeos.
 */
#ifndef ZEOS_ARM_PLATFORM_H
#define ZEOS_ARM_PLATFORM_H

#include <stdint.h>

struct arm_platform {
    uint64_t    uart;          /* console UART register base */
    const char *uart_compat;   /* which controller we matched (drives the driver) */
    uint64_t    gicd;          /* GIC distributor */
    uint64_t    gicc;          /* GICv2 CPU interface / GICv3 redistributor */
    const char *gic_compat;
    int         gic_v3;        /* 1 = GICv3 (gicc is a redistributor), 0 = v2 */
    uint64_t    rtc;           /* real-time clock */
    uint64_t    ecam;          /* PCIe ECAM config space */
};

extern struct arm_platform g_plat;

void plat_init(void);        /* parse the DTB firmware handed us */
int  plat_discovered(void);  /* 1 = values came from the device tree, 0 = no DTB */

#endif
