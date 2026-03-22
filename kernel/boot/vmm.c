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

    /* Invalidate TLB for this address */
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
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
