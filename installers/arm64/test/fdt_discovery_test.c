/* Host test: run the REAL fdt.c parser over a REAL device tree blob and assert
 * it extracts this board's hardware addresses. Proves discovery logic without
 * needing the target: same source file, genuine DTB produced by QEMU virt. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern uint64_t g_dtb_ptr;
extern int fdt_init(void);
extern uint64_t fdt_base_of(const char *);
extern int fdt_find_reg(const char *, uint64_t *, int, int *);

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/virt.dtb";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 2; }
    static uint8_t buf[8u << 20];
    size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    printf("device tree: %s (%zu bytes)\n", path, n);

    g_dtb_ptr = (uint64_t)(uintptr_t)buf;
    if (!fdt_init()) { printf("FDT REJECTED -- parser could not validate\n"); return 1; }
    printf("FDT validated.\n");

    int fail = 0;
    struct { const char *compat; uint64_t want; const char *what; } t[] = {
        { "arm,pl011",              0x09000000UL,   "UART (PL011)" },
        { "arm,pl031",              0x09010000UL,   "RTC (PL031)" },
        { "pci-host-ecam-generic",  0x4010000000UL, "PCIe ECAM" },
    };
    for (unsigned i = 0; i < sizeof t/sizeof *t; i++) {
        uint64_t got = fdt_base_of(t[i].compat);
        int ok = (got == t[i].want);
        printf("  %-22s %-14s -> %#lx (want %#lx) %s\n", t[i].compat, t[i].what,
               (unsigned long)got, (unsigned long)t[i].want, ok ? "OK" : "FAIL");
        if (!ok) fail = 1;
    }
    /* GIC: distributor + redistributor from one node's reg list. */
    uint64_t r[8]; int nr = 0;
    if (fdt_find_reg("arm,gic-v3", r, 8, &nr) && nr >= 3) {
        int ok = (r[0] == 0x08000000UL && r[2] == 0x080A0000UL);
        printf("  %-22s %-14s -> gicd=%#lx gicr=%#lx %s\n", "arm,gic-v3", "GICv3",
               (unsigned long)r[0], (unsigned long)r[2], ok ? "OK" : "FAIL");
        if (!ok) fail = 1;
    } else { printf("  arm,gic-v3            GICv3          -> NOT FOUND  FAIL\n"); fail = 1; }

    printf(fail ? "RESULT: FAIL\n" : "RESULT: ALL DISCOVERED CORRECTLY\n");
    return fail;
}
