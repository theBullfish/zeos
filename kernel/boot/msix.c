/*
 * Zeos — MSI-X interrupt delivery (implementation)
 *
 * Honest accounting:
 *  - APIC ID detection is NOT implemented. Message Address is hardcoded
 *    to 0xFEE00000 with dest=0 (physical, no RH, no DM). On a single-CPU
 *    QEMU guest this routes to the BSP and works. On multi-CPU systems,
 *    interrupts will still be delivered to the BSP's LAPIC. That is fine
 *    for current Zeos, which has no SMP scheduler.
 *  - LAPIC EOI: the existing isr_dispatch() in idt.c calls pic_eoi() only
 *    for legacy IRQ vectors 0x20-0x2F. For MSI-X vectors (0x40-0x7F),
 *    the EOI must go to the LAPIC, not the PIC. We have no LAPIC driver
 *    yet (no MADT parser, no enable path), so EOI is left as a no-op.
 *    First MSI-X interrupt on a vector will fire; subsequent ones may
 *    queue at the LAPIC ISR until a future LAPIC subsystem enables EOI.
 *  - We assume xAPIC mode; x2APIC EOI via MSR is a separate path.
 *
 * The vector pool, table programming, capability locating, and dispatch
 * machinery are real.
 */

#include "msix.h"
#include "pci.h"
#include "idt.h"
#include "vmm.h"
#include "lapic.h"
#include "kprint.h"

#define MSIX_TBL_OFFSET        0x04
#define MSIX_PBA_OFFSET        0x08

#define MSIX_CTRL_TBL_SIZE_MASK 0x07ffu
#define MSIX_CTRL_FUNC_MASK     (1u << 14)
#define MSIX_CTRL_ENABLE        (1u << 15)

#define MSIX_ENTRY_VEC_CTRL_MASK 0x1u

#define MSIX_MSG_ADDR_BASE      0xFEE00000ULL  /* xAPIC interrupt redirection */

#define MSIX_MAX_DEVICES 16

struct msix_device_state {
    struct pci_device *dev;
    uint8_t  cap_off;
    uint8_t  table_bir;
    uint32_t table_offset;
    uint64_t table_phys;
    int      num_entries;
    int      vectors[MSIX_VECTOR_COUNT];
};

static struct msix_device_state g_devices[MSIX_MAX_DEVICES];
static int g_device_count = 0;

static void (*g_vector_handlers[MSIX_VECTOR_COUNT])(void);
static int g_vector_used[MSIX_VECTOR_COUNT];

static int g_initialized = 0;

void msix_init(void)
{
    if (g_initialized) return;
    for (int i = 0; i < MSIX_VECTOR_COUNT; i++) {
        g_vector_handlers[i] = 0;
        g_vector_used[i] = 0;
    }
    for (int i = 0; i < MSIX_MAX_DEVICES; i++) {
        g_devices[i].dev = 0;
    }
    g_device_count = 0;
    g_initialized = 1;
}

int msix_free_count(void)
{
    int c = 0;
    for (int i = 0; i < MSIX_VECTOR_COUNT; i++)
        if (!g_vector_used[i]) c++;
    return c;
}

int msix_alloc_vector(void (*handler)(void))
{
    if (!g_initialized) msix_init();
    for (int i = 0; i < MSIX_VECTOR_COUNT; i++) {
        if (!g_vector_used[i]) {
            g_vector_used[i] = 1;
            g_vector_handlers[i] = handler;
            return MSIX_VECTOR_BASE + i;
        }
    }
    return -1;
}

void msix_free_vector(int vec)
{
    if (vec < MSIX_VECTOR_BASE || vec >= MSIX_VECTOR_TOP) return;
    int i = vec - MSIX_VECTOR_BASE;
    g_vector_used[i] = 0;
    g_vector_handlers[i] = 0;
}

void msix_dispatch(int vec)
{
    if (vec < MSIX_VECTOR_BASE || vec >= MSIX_VECTOR_TOP) {
        /* Still EOI: the LAPIC accepted this vector. */
        lapic_eoi();
        return;
    }
    int i = vec - MSIX_VECTOR_BASE;
    if (g_vector_handlers[i]) {
        g_vector_handlers[i]();
    }
    /* Acknowledge to the LAPIC so the next interrupt on this vector
     * can be delivered. Without this, the LAPIC's ISR keeps the bit
     * set and subsequent interrupts at the same priority queue. */
    lapic_eoi();
}

static struct msix_device_state *find_state(struct pci_device *dev)
{
    for (int i = 0; i < g_device_count; i++)
        if (g_devices[i].dev == dev) return &g_devices[i];
    return 0;
}

static struct msix_device_state *alloc_state(struct pci_device *dev)
{
    if (g_device_count >= MSIX_MAX_DEVICES) return 0;
    struct msix_device_state *s = &g_devices[g_device_count++];
    s->dev = dev;
    s->cap_off = 0;
    s->num_entries = 0;
    for (int i = 0; i < MSIX_VECTOR_COUNT; i++) s->vectors[i] = -1;
    return s;
}

static uint64_t read_bar64_local(struct pci_device *dev, int idx)
{
    uint32_t lo = pci_config_read32(dev->bus, dev->dev, dev->func, 0x10 + idx * 4);
    if ((lo & 0x06) == 0x04) {
        uint32_t hi = pci_config_read32(dev->bus, dev->dev, dev->func, 0x10 + (idx + 1) * 4);
        return ((uint64_t)hi << 32) | (lo & ~0xFULL);
    }
    return lo & ~0xFULL;
}

static uint16_t read_msgctrl(struct pci_device *dev, uint8_t cap_off)
{
    uint32_t w = pci_config_read32(dev->bus, dev->dev, dev->func, cap_off);
    return (uint16_t)(w >> 16);
}

static void write_msgctrl(struct pci_device *dev, uint8_t cap_off, uint16_t val)
{
    uint32_t w = pci_config_read32(dev->bus, dev->dev, dev->func, cap_off);
    w = (w & 0x0000FFFFu) | ((uint32_t)val << 16);
    pci_config_write32(dev->bus, dev->dev, dev->func, cap_off, w);
}

int msix_enable(struct pci_device *dev, int num_vectors)
{
    if (!g_initialized) msix_init();
    if (!dev || num_vectors < 1) return -2;

    uint8_t cap = pci_find_capability(dev, 0x11);
    if (!cap) return -1;

    uint16_t ctrl = read_msgctrl(dev, cap);
    int table_size = (ctrl & MSIX_CTRL_TBL_SIZE_MASK) + 1;
    if (num_vectors > table_size) return -2;
    if (num_vectors > MSIX_VECTOR_COUNT) return -2;

    uint32_t tbl = pci_config_read32(dev->bus, dev->dev, dev->func, cap + MSIX_TBL_OFFSET);
    uint8_t  bir = (uint8_t)(tbl & 0x7);
    uint32_t off = tbl & ~0x7u;

    uint64_t bar_base = read_bar64_local(dev, bir);
    if (!bar_base) return -3;

    uint64_t table_phys = bar_base + off;

    /* Map the MSI-X table region. Max 2048 entries × 16 B = 32 KiB. */
    {
        uint64_t base_page = table_phys & ~0xFFFULL;
        vmm_map_range(base_page, base_page, 8, PTE_WRITABLE | PTE_NOCACHE);
    }

    struct msix_device_state *st = find_state(dev);
    if (!st) st = alloc_state(dev);
    if (!st) return -2;
    st->cap_off = cap;
    st->table_bir = bir;
    st->table_offset = off;
    st->table_phys = table_phys;

    volatile uint32_t *table = (volatile uint32_t *)(uintptr_t)table_phys;
    for (int e = 0; e < num_vectors; e++) {
        int v = msix_alloc_vector(0);
        if (v < 0) {
            for (int j = 0; j < e; j++) {
                msix_free_vector(st->vectors[j]);
                st->vectors[j] = -1;
            }
            st->num_entries = 0;
            return -4;
        }
        st->vectors[e] = v;

        volatile uint32_t *entry = table + (e * 4);
        entry[0] = (uint32_t)(MSIX_MSG_ADDR_BASE & 0xFFFFFFFFu);
        entry[1] = (uint32_t)(MSIX_MSG_ADDR_BASE >> 32);
        entry[2] = (uint32_t)v;
        entry[3] = 0;
    }
    st->num_entries = num_vectors;

    for (int e = num_vectors; e < table_size; e++) {
        volatile uint32_t *entry = table + (e * 4);
        entry[3] = MSIX_ENTRY_VEC_CTRL_MASK;
    }

    /* Bus Master + Memory Space — required for MSI writes to land. */
    {
        uint32_t cmd = pci_config_read32(dev->bus, dev->dev, dev->func, 0x04);
        cmd |= (1u << 2) | (1u << 1);
        pci_config_write32(dev->bus, dev->dev, dev->func, 0x04, cmd);
    }

    ctrl &= ~MSIX_CTRL_FUNC_MASK;
    ctrl |= MSIX_CTRL_ENABLE;
    write_msgctrl(dev, cap, ctrl);

    return 0;
}

int msix_set_handler(struct pci_device *dev, int entry, void (*handler)(void))
{
    struct msix_device_state *st = find_state(dev);
    if (!st) return -1;
    if (entry < 0 || entry >= st->num_entries) return -1;
    int v = st->vectors[entry];
    if (v < MSIX_VECTOR_BASE || v >= MSIX_VECTOR_TOP) return -1;
    g_vector_handlers[v - MSIX_VECTOR_BASE] = handler;
    return 0;
}

void msix_disable(struct pci_device *dev)
{
    struct msix_device_state *st = find_state(dev);
    if (!st || !st->cap_off) return;

    volatile uint32_t *table = (volatile uint32_t *)(uintptr_t)st->table_phys;
    for (int e = 0; e < st->num_entries; e++) {
        volatile uint32_t *entry = table + (e * 4);
        entry[3] = MSIX_ENTRY_VEC_CTRL_MASK;
    }

    uint16_t ctrl = read_msgctrl(dev, st->cap_off);
    ctrl &= ~MSIX_CTRL_ENABLE;
    ctrl |= MSIX_CTRL_FUNC_MASK;
    write_msgctrl(dev, st->cap_off, ctrl);

    for (int e = 0; e < st->num_entries; e++) {
        if (st->vectors[e] >= 0) msix_free_vector(st->vectors[e]);
        st->vectors[e] = -1;
    }
    st->num_entries = 0;
}
