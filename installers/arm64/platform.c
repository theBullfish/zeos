/*
 * Zeos aarch64 — runtime platform discovery.
 *
 * Fills in WHERE this board's hardware is, from the device tree firmware gave
 * us. No per-board #defines, no rebuild per SoC: the same image adapts to
 * QEMU 'virt', Snapdragon, Rockchip, Ampere — anything that hands us a DTB.
 *
 * The QEMU 'virt' values appear ONLY as a last-resort fallback for the case
 * where we were launched with no DTB at all (bare -kernel with no -dtb). If
 * discovery worked, nothing here is guessed — and plat_discovered() says which.
 */
#include <stdint.h>
#include "platform.h"

extern int      fdt_init(void);
extern int      fdt_valid(void);
extern uint64_t fdt_base_of(const char *compat);
extern int      fdt_find_reg(const char *compat, uint64_t *regs, int max, int *n);

struct arm_platform g_plat;
static int g_from_dt;

int plat_discovered(void) { return g_from_dt; }

/* UART: try the common aarch64 console controllers in order. Each entry is a
 * devicetree "compatible" string; the first one present on this board wins. */
static const char *const uart_compat[] = {
    "arm,pl011",            /* QEMU virt, many ARM dev boards */
    "qcom,geni-uart",       /* Snapdragon (GENI QUP) — e.g. Q6A/8cx */
    "qcom,msm-uartdm",      /* older Qualcomm */
    "snps,dw-apb-uart",     /* Synopsys DesignWare (Rockchip, Goya SoC, ...) */
    "ns16550a",             /* generic 16550 */
    0
};
static const char *const gic_compat[] = {
    "arm,gic-v3", "arm,gic-v3-its", "arm,cortex-a15-gic", "arm,gic-400", 0
};
static const char *const rtc_compat[] = { "arm,pl031", "snps,dw-apb-rtc", 0 };

void plat_init(void)
{
    g_from_dt = fdt_init();

    if (g_from_dt) {
        for (int i = 0; uart_compat[i] && !g_plat.uart; i++) {
            uint64_t b = fdt_base_of(uart_compat[i]);
            if (b) { g_plat.uart = b; g_plat.uart_compat = uart_compat[i]; }
        }
        /* GIC: reg[0]=distributor, reg[2]=second bank (GICv2 CPU interface or
         * GICv3 redistributor) since reg pairs are (addr,size). */
        for (int i = 0; gic_compat[i] && !g_plat.gicd; i++) {
            uint64_t r[8]; int n = 0;
            if (fdt_find_reg(gic_compat[i], r, 8, &n) && n >= 1) {
                g_plat.gicd = r[0];
                if (n >= 3) g_plat.gicc = r[2];   /* v3: redistributor; v2: CPU if */
                g_plat.gic_compat = gic_compat[i];
                /* v3 and v2 have INCOMPATIBLE second banks — a v2 CPU-interface
                 * address on a v3 machine is an unmapped hole (observed: data
                 * abort at 0x08010014). Record which we actually found. */
                g_plat.gic_v3 = (gic_compat[i][8] == '3');  /* "arm,gic-v3..." */
            }
        }
        for (int i = 0; rtc_compat[i] && !g_plat.rtc; i++)
            g_plat.rtc = fdt_base_of(rtc_compat[i]);

        /* PCIe config space (ECAM). */
        g_plat.ecam = fdt_base_of("pci-host-ecam-generic");
        if (!g_plat.ecam) g_plat.ecam = fdt_base_of("pci-host-generic");
    }

    /* Last-resort fallback: no DTB was provided at all. Marked so callers and
     * the boot log can say plainly that these were NOT discovered. */
    if (!g_plat.uart) { g_plat.uart = 0x09000000UL; g_plat.uart_compat = "arm,pl011 (assumed)"; }
    if (!g_plat.gicd)  g_plat.gicd = 0x08000000UL;
    /* Do NOT invent a second GIC bank: v2's CPU interface and v3's redistributor
     * live at different places, and picking wrong faults. With no DTB we assume
     * the GICv3 layout QEMU virt uses (redistributor at GICD+0xA0000). */
    if (!g_plat.gicc) { g_plat.gicc = g_plat.gicd + 0xA0000UL; g_plat.gic_v3 = 1; }
    if (!g_plat.rtc)   g_plat.rtc  = 0x09010000UL;
    if (!g_plat.ecam)  g_plat.ecam = 0x4010000000UL;
}
