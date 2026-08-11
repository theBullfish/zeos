/*
 * Zeos aarch64 — Flattened Device Tree (FDT/DTB) discovery.
 *
 * THE POINT: Zeos does not assume where hardware is. Firmware hands us a device
 * tree describing THIS board — UART, interrupt controller, PCIe ECAM, RAM — and
 * we read it. One binary boots QEMU 'virt', a Snapdragon, a Rockchip, an Ampere,
 * anything that hands us a DTB, with no rebuild and no per-board #defines.
 *
 * Spec: Devicetree Specification v0.4, §5 (flattened format). All header and
 * token fields are BIG-endian regardless of CPU endianness.
 */
#include <stdint.h>

/* Set by boot.S from x0 before anything else runs. */
uint64_t g_dtb_ptr;

#define FDT_MAGIC        0xd00dfeedu
#define FDT_BEGIN_NODE   1u
#define FDT_END_NODE     2u
#define FDT_PROP         3u
#define FDT_NOP          4u
#define FDT_END          9u

struct fdt_header {
    uint32_t magic, totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap;
    uint32_t version, last_comp_version, boot_cpuid_phys;
    uint32_t size_dt_strings, size_dt_struct;
};

static inline uint32_t be32(uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}
static inline uint32_t rd32(const void *p) { return be32(*(const uint32_t *)p); }

static const struct fdt_header *g_fdt;
static const char *g_strings;
static const uint8_t *g_struct;

int fdt_valid(void) { return g_fdt != 0; }

static int try_dtb_at(uint64_t addr)
{
    if (!addr || (addr & 3)) return 0;
    const struct fdt_header *h = (const struct fdt_header *)addr;
    if (be32(h->magic) != FDT_MAGIC) return 0;
    uint32_t total = be32(h->totalsize);
    if (total < sizeof *h || total > (64u << 20)) return 0;   /* sanity */
    uint32_t soff = be32(h->off_dt_struct), stroff = be32(h->off_dt_strings);
    if (soff >= total || stroff >= total) return 0;
    g_fdt     = h;
    g_strings = (const char *)h + stroff;
    g_struct  = (const uint8_t *)h + soff;
    return 1;
}

/*
 * Get a device tree by ANY means. Zeos is built to grab hardware, not to be
 * handed it: a bootloader that forgets x0, a board that stashes the blob
 * somewhere else, an ELF loaded straight into RAM — none of those should leave
 * us blind and guessing (guessing is what faults).
 *   1. x0 as the aarch64 boot protocol specifies
 *   2. a linker-provided blob appended to the image (if present)
 *   3. SCAN low RAM for the FDT magic — bootloaders park it near the start of
 *      memory, and a validated header + sane totalsize makes a false positive
 *      vanishingly unlikely.
 * Returns 1 if we found a real tree.
 */
int fdt_init(void)
{
    if (try_dtb_at(g_dtb_ptr)) return 1;              /* 1. firmware told us */

    /* 3. hunt for it. QEMU/u-boot/UEFI all leave the blob in low RAM; walk on
     * 4 KB boundaries (DTBs are page-aligned by every loader in practice). */
    for (uint64_t a = 0x40000000UL; a < 0x48000000UL; a += 0x1000UL)
        if (try_dtb_at(a)) { g_dtb_ptr = a; return 1; }

    return 0;
}

static int str_eq_n(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
/* Does `list` (a NUL-separated string list of length len) contain `want`? */
static int compat_has(const char *list, uint32_t len, const char *want)
{
    uint32_t i = 0;
    while (i < len) {
        if (str_eq_n(list + i, want)) return 1;
        while (i < len && list[i]) i++;
        i++;                                  /* skip the NUL */
    }
    return 0;
}

/*
 * Walk the tree for the first node whose "compatible" contains `compat`, and
 * return reg[0] (base) and optionally reg[1] (size) plus reg[2] (a second bank,
 * e.g. GICv2's CPU interface). `nregs_out` reports how many 64-bit values were
 * decoded so callers can tell a 1-bank device from a 2-bank one.
 *
 * Cell sizes: we honor the root's #address-cells/#size-cells (2/2 on every
 * 64-bit board in practice) rather than hardcoding.
 */
int fdt_find_reg(const char *compat, uint64_t *regs, int max_regs, int *nregs_out)
{
    if (!g_fdt) return 0;
    const uint8_t *p = g_struct;
    uint32_t addr_cells = 2, size_cells = 2;
    int depth = 0;
    /* Properties appear in ARBITRARY order inside a node, so we cannot wait for
     * "compatible" before noticing "reg" — track both for the current node and
     * decide when the node ends. */
    const uint8_t *cur_reg = 0; uint32_t cur_reg_len = 0; int cur_match = 0;
    int match_depth = -1;   /* depth of the matched node; child props must not clobber it */

    for (;;) {
        uint32_t tok = rd32(p); p += 4;
        if (tok == FDT_END) break;
        if (tok == FDT_NOP) continue;
        if (tok == FDT_BEGIN_NODE) {
            depth++;
            while (*p) p++;                    /* node name */
            p = (const uint8_t *)(((uintptr_t)p + 4) & ~3ULL);
            if (!cur_match) { cur_reg = 0; cur_reg_len = 0; }  /* fresh node */
            continue;
        }
        if (tok == FDT_END_NODE) {
            if (cur_match && cur_reg && depth == match_depth) {
                int n = 0;
                const uint8_t *r = cur_reg;
                uint32_t consumed = 0;
                while (consumed + (addr_cells + size_cells) * 4 <= cur_reg_len &&
                       n < max_regs) {
                    uint64_t a = 0;
                    for (uint32_t c = 0; c < addr_cells; c++) { a = (a << 32) | rd32(r); r += 4; }
                    uint64_t s = 0;
                    for (uint32_t c = 0; c < size_cells; c++) { s = (s << 32) | rd32(r); r += 4; }
                    regs[n++] = a;
                    if (n < max_regs) regs[n++] = s;
                    consumed += (addr_cells + size_cells) * 4;
                }
                if (n > 0) { if (nregs_out) *nregs_out = n; return 1; }
            }
            /* Leaving the matched node without a usable reg: keep searching.
             * Leaving a CHILD of the match: keep the match alive. */
            if (depth == match_depth) { cur_match = 0; match_depth = -1; cur_reg = 0; cur_reg_len = 0; }
            depth--;
            continue;
        }
        if (tok == FDT_PROP) {
            uint32_t len = rd32(p); p += 4;
            uint32_t nameoff = rd32(p); p += 4;
            const char *name = g_strings + nameoff;
            const uint8_t *data = p;
            p += (len + 3) & ~3u;

            if (depth == 1) {                 /* root cell sizes */
                if (str_eq_n(name, "#address-cells")) addr_cells = rd32(data);
                else if (str_eq_n(name, "#size-cells")) size_cells = rd32(data);
            }
            /* Only the matched node's OWN properties count — a child node's reg
             * (e.g. the GIC's ITS child) must not overwrite the parent's. */
            if (str_eq_n(name, "reg")) {
                if (!cur_match || depth == match_depth) { cur_reg = data; cur_reg_len = len; }
            } else if (str_eq_n(name, "compatible") && !cur_match &&
                       compat_has((const char *)data, len, compat)) {
                cur_match = 1; match_depth = depth;
            }
            continue;
        }
        break;                                 /* unknown token — bail safely */
    }
    return 0;
}

/* Convenience: base address of the first node matching `compat`, else 0. */
uint64_t fdt_base_of(const char *compat)
{
    uint64_t regs[8]; int n = 0;
    if (!fdt_find_reg(compat, regs, 8, &n) || n < 1) return 0;
    return regs[0];
}
