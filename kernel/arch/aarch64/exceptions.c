/* Zeos aarch64 — exception reporter (M1). Fatal: print and halt. */
#include <stdint.h>
extern void kputs(const char *);
extern void kput_hex(uint64_t);

void exc_dispatch(uint64_t esr, uint64_t elr, uint64_t far)
{
    kputs("\n[EXCEPTION] EC=");
    kput_hex((esr >> 26) & 0x3F);   /* exception class */
    kputs(" ESR=");  kput_hex(esr);
    kputs(" ELR=");  kput_hex(elr);
    kputs(" FAR=");  kput_hex(far);
    kputs("\n  halted.\n");
}
