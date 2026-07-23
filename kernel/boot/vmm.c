/*
 * Zeos — Virtual Memory Manager
 *
 * Creates new 4-level page tables for the kernel.
 * Strategy:
 *   - Identity map the first 4GB (hardware access, MMIO, framebuffer)
 *   - Map kernel at higher-half (KERNEL_VBASE + physical)
 *   - All page table pages allocated from PMM
 *
 * We stay on the UEFI identity mapping until vmm_init() switches CR3.
 * After that, we own the page tables completely.
 */

#include "vmm.h"
#include "pmm.h"
#include "fb.h"
#include "smp.h"

/* The PML4 table — root of our page hierarchy */
static uint64_t *pml4;
static uint64_t pml4_phys;

/*
 * Allocate a zeroed page for a page table.
 */
static uint64_t *alloc_table(void)
{
    uint64_t phys = pmm_alloc();
    if (!phys)
        return 0;

    /* Zero it — page tables must be zeroed (non-present entries = 0) */
    uint64_t *table = (uint64_t *)phys;  /* Identity mapped */
    for (int i = 0; i < 512; i++)
        table[i] = 0;

    return table;
}

/*
 * Get or create a page table entry at a given level.
 * If the entry doesn't exist and create=1, allocate a new table.
 */
static uint64_t *get_or_create_entry(uint64_t *table, int index, int create)
{
    if (table[index] & PTE_PRESENT) {
        return (uint64_t *)(table[index] & 0x000FFFFFFFFFF000ULL);
    }

    if (!create)
        return 0;

    uint64_t *new_table = alloc_table();
    if (!new_table)
        return 0;

    table[index] = (uint64_t)new_table | PTE_PRESENT | PTE_WRITABLE;
    return new_table;
}

/*
 * Extract page table indices from a virtual address.
 * 48-bit virtual: [47:39]=PML4, [38:30]=PDPT, [29:21]=PD, [20:12]=PT, [11:0]=offset
 */
static inline int pml4_index(uint64_t virt) { return (virt >> 39) & 0x1FF; }
static inline int pdpt_index(uint64_t virt) { return (virt >> 30) & 0x1FF; }
static inline int pd_index(uint64_t virt)   { return (virt >> 21) & 0x1FF; }
static inline int pt_index(uint64_t virt)   { return (virt >> 12) & 0x1FF; }

void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pdpt = get_or_create_entry(pml4, pml4_index(virt), 1);
    if (!pdpt) return;

    uint64_t *pd = get_or_create_entry(pdpt, pdpt_index(virt), 1);
    if (!pd) return;

    uint64_t *pt = get_or_create_entry(pd, pd_index(virt), 1);
    if (!pt) return;

    pt[pt_index(virt)] = (phys & 0x000FFFFFFFFFF000ULL) | flags | PTE_PRESENT;
}

void vmm_unmap(uint64_t virt)
{
    uint64_t *pdpt = get_or_create_entry(pml4, pml4_index(virt), 0);
    if (!pdpt) return;

    uint64_t *pd = get_or_create_entry(pdpt, pdpt_index(virt), 0);
    if (!pd) return;

    uint64_t *pt = get_or_create_entry(pd, pd_index(virt), 0);
    if (!pt) return;

    pt[pt_index(virt)] = 0;

    /* Local INVLPG + IPI shootdown to every AP so a stale entry on
     * another core doesn't keep the unmapped page reachable. */
    tlb_shootdown_range(virt & ~0xFFFULL, 1);
}

uint64_t vmm_virt_to_phys(uint64_t virt)
{
    uint64_t *pdpt = get_or_create_entry(pml4, pml4_index(virt), 0);
    if (!pdpt) return 0;

    uint64_t *pd = get_or_create_entry(pdpt, pdpt_index(virt), 0);
    if (!pd) return 0;

    uint64_t *pt = get_or_create_entry(pd, pd_index(virt), 0);
    if (!pt) return 0;

    uint64_t entry = pt[pt_index(virt)];
    if (!(entry & PTE_PRESENT))
        return 0;

    return (entry & 0x000FFFFFFFFFF000ULL) | (virt & 0xFFF);
}

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t count, uint64_t flags)
{
    for (uint64_t i = 0; i < count; i++) {
        vmm_map(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags);
    }
    /* Re-mapping an existing virtual range with new flags or a new
     * physical backing requires every core to drop its stale PTE
     * cache. INVLPG locally + IPI broadcast on the BSP. */
    tlb_shootdown_range(virt & ~0xFFFULL, count);
}

/*
 * B.7 ROOT-CAUSE FIX — map the framebuffer WRITE-COMBINING (the field-standard
 * framebuffer memory type: writes bypass the cache so the display sees them, but
 * the CPU batches sequential writes into burst transactions so it stays fast).
 *
 * vmm_init() identity-maps the whole low 4GB write-back cached (P|W|HUGE, no cache
 * bits => default WB), silently re-caching the FB the firmware handed us. Direct
 * compositor writes then sit in CPU cache and the display misses them => "windows
 * vanish". The fix everyone converges on: FB = WC, via PAT (or MTRR on old HW),
 * plus a cached back buffer + one bulk present (B.6). Here: reprogram PAT slot 1
 * to WC, then tag the FB's 2MB pages to select it (PWT=1, PCD=0, PAT=0 -> index 1).
 */
#define IA32_PAT 0x277u

static inline uint64_t rd_msr(uint32_t m) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wr_msr(uint32_t m, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* Set PAT entry 1 (default WT) to Write-Combining (0x01). Intel SDM 11.11.8
 * cache-safe sequence: CD=1 + WBINVD + TLB flush around the MSR write. BSP, boot. */
static void pat_enable_wc(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
    __asm__ volatile("cli");

    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" :: "r"((cr0 & ~(1ULL << 29)) | (1ULL << 30))); /* CD=1,NW=0 */
    __asm__ volatile("wbinvd");
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");   /* flush TLB */

    uint64_t pat = rd_msr(IA32_PAT);
    pat = (pat & ~(0xFFULL << 8)) | (0x01ULL << 8);            /* PA1 = WC */
    wr_msr(IA32_PAT, pat);

    __asm__ volatile("wbinvd");
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));             /* restore caching */

    if (flags & (1ULL << 9)) __asm__ volatile("sti");
}

void vmm_set_range_wc(uint64_t phys, uint64_t size)
{
    if (!pml4 || !size) return;
    pat_enable_wc();

    uint64_t start = phys & ~0x1FFFFFULL;                        /* round down 2MB */
    uint64_t end   = (phys + size + 0x1FFFFFULL) & ~0x1FFFFFULL;  /* round up 2MB */

    for (uint64_t a = start; a < end; a += 0x200000ULL) {
        for (int half = 0; half < 2; half++) {
            uint64_t v = a + (half ? KERNEL_VBASE : 0ULL);
            uint64_t *pdpt = get_or_create_entry(pml4, pml4_index(v), 0);
            if (!pdpt) continue;
            uint64_t *pd = get_or_create_entry(pdpt, pdpt_index(v), 0);
            if (!pd) continue;
            uint64_t *e = &pd[pd_index(v)];
            if (!(*e & PTE_PRESENT) || !(*e & PTE_HUGE)) continue;
            *e |=  PTE_WRITETHROUGH;    /* PWT (bit 3) -> select PAT index 1 (WC) */
            *e &= ~PTE_NOCACHE;         /* PCD (bit 4) = 0 */
            *e &= ~(1ULL << 12);        /* PAT bit (huge page) = 0 */
            __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory");
        }
    }
}

void vmm_init(void)
{
    /* Allocate PML4 */
    pml4 = alloc_table();
    if (!pml4) {
        fb_puts("VMM: FATAL — cannot allocate PML4!\n");
        return;
    }
    pml4_phys = (uint64_t)pml4;

    /*
     * Identity map the first 4GB using 2MB huge pages for speed.
     * This covers all MMIO, framebuffer, and low memory.
     * We use PD-level huge pages (bit 7 in PD entry = 2MB page).
     */
    for (uint64_t gb = 0; gb < 4; gb++) {
        uint64_t virt_base = gb * 0x40000000ULL;  /* 1GB per iteration */

        /* Get/create PDPT entry */
        uint64_t *pdpt = get_or_create_entry(pml4, pml4_index(virt_base), 1);
        if (!pdpt) continue;

        /* Get/create PD */
        uint64_t *pd = get_or_create_entry(pdpt, pdpt_index(virt_base), 1);
        if (!pd) continue;

        /* Fill PD with 512 x 2MB huge pages = 1GB */
        for (int i = 0; i < 512; i++) {
            uint64_t phys = virt_base + (uint64_t)i * 0x200000ULL;
            pd[i] = phys | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;
        }
    }

    /*
     * Map kernel higher-half: KERNEL_VBASE → 0 (first 4GB mirrored).
     * Same 2MB huge pages, but in the upper-half PML4 entries.
     */
    for (uint64_t gb = 0; gb < 4; gb++) {
        uint64_t virt_base = KERNEL_VBASE + gb * 0x40000000ULL;
        uint64_t phys_base = gb * 0x40000000ULL;

        uint64_t *pdpt = get_or_create_entry(pml4, pml4_index(virt_base), 1);
        if (!pdpt) continue;

        uint64_t *pd = get_or_create_entry(pdpt, pdpt_index(virt_base), 1);
        if (!pd) continue;

        for (int i = 0; i < 512; i++) {
            uint64_t phys = phys_base + (uint64_t)i * 0x200000ULL;
            pd[i] = phys | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;
        }
    }

    /* Switch to our page tables */
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

uint64_t vmm_get_pml4(void)
{
    return pml4_phys;
}
