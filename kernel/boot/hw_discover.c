/*
 * Zeos -- Hardware Discovery
 *
 * Scans PCI bus (and implicit CPU/memory) to build the hardware
 * chain graph at boot. Every device becomes a chain with typed
 * inputs and outputs -- no driver model, just chains and routes.
 *
 * NOTE: Add kernel/boot/hw_discover.c to SRCS in the Makefile.
 */

#include "hw_discover.h"
#include "chain.h"
#include "pci.h"
#include "kprint.h"
#include "timer.h"

/* ── Internal state ─────────────────────────────────────────────── */

#define HW_MAX_CHAINS 64

/* Map from hw chain index to chain registry id */
static int   hw_chain_ids[HW_MAX_CHAINS];
static char  hw_chain_types[HW_MAX_CHAINS][32];
static int   hw_count;

/* CPU chain is always id 0 in the hw table */
static int   cpu_chain_id = -1;

/* ── String helpers (no libc) ───────────────────────────────────── */

static void hw_strcpy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int hw_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ── PCI class to hardware type mapping ─────────────────────────── */

static const char *pci_class_to_hw_type(uint8_t class_code, uint8_t subclass)
{
    switch (class_code) {
    case 0x01:  return HW_TYPE_STORAGE;
    case 0x02:  return HW_TYPE_NIC;
    case 0x03:  return HW_TYPE_GPU;
    case 0x04:  return HW_TYPE_AUDIO;
    case 0x06:  return HW_TYPE_BRIDGE;
    case 0x07:  return HW_TYPE_SERIAL;
    case 0x09:  return HW_TYPE_INPUT;
    case 0x0C:
        if (subclass == 0x03) return HW_TYPE_USB;
        return HW_TYPE_UNKNOWN;
    case 0x0D:  return HW_TYPE_WIRELESS;
    case 0x10:  return HW_TYPE_CRYPTO;
    case 0x12:  return HW_TYPE_ACCEL;
    case 0x40:  return HW_TYPE_ACCEL;
    default:    return HW_TYPE_UNKNOWN;
    }
}

/* ── Input/output type mapping for MDE routing ──────────────────── */

static const char *hw_input_type(const char *hw_type)
{
    if (hw_streq(hw_type, HW_TYPE_GPU))      return "render_command";
    if (hw_streq(hw_type, HW_TYPE_NIC))      return "network_frame";
    if (hw_streq(hw_type, HW_TYPE_STORAGE))  return "block_request";
    if (hw_streq(hw_type, HW_TYPE_AUDIO))    return "audio_command";
    if (hw_streq(hw_type, HW_TYPE_USB))      return "usb_transfer";
    if (hw_streq(hw_type, HW_TYPE_WIRELESS)) return "network_frame";
    if (hw_streq(hw_type, HW_TYPE_CRYPTO))   return "crypto_request";
    if (hw_streq(hw_type, HW_TYPE_ACCEL))    return "compute_task";
    return "raw_data";
}

static const char *hw_output_type(const char *hw_type)
{
    if (hw_streq(hw_type, HW_TYPE_GPU))      return "framebuffer";
    if (hw_streq(hw_type, HW_TYPE_NIC))      return "network_frame";
    if (hw_streq(hw_type, HW_TYPE_STORAGE))  return "block_data";
    if (hw_streq(hw_type, HW_TYPE_AUDIO))    return "audio_pcm";
    if (hw_streq(hw_type, HW_TYPE_INPUT))    return "input_event";
    if (hw_streq(hw_type, HW_TYPE_USB))      return "usb_data";
    if (hw_streq(hw_type, HW_TYPE_WIRELESS)) return "network_frame";
    if (hw_streq(hw_type, HW_TYPE_CRYPTO))   return "crypto_result";
    if (hw_streq(hw_type, HW_TYPE_ACCEL))    return "compute_result";
    return "raw_data";
}

/* ── Chain name builder ─────────────────────────────────────────── */

/*
 * Build a name like "gpu:1002:7480" from hw_type + vendor + device.
 * Short prefix pulled from the type string after the dot.
 */
static void build_chain_name(char *buf, int max, const char *hw_type,
                              uint16_t vendor, uint16_t device)
{
    /* Find the part after "hw." */
    const char *short_name = hw_type;
    while (*short_name && *short_name != '.') short_name++;
    if (*short_name == '.') short_name++;

    int i = 0;

    /* Copy short name */
    while (i < max - 1 && *short_name) {
        buf[i++] = *short_name++;
    }

    /* Separator */
    if (i < max - 1) buf[i++] = ':';

    /* Vendor hex (4 digits) */
    static const char hex[] = "0123456789abcdef";
    if (i + 4 < max) {
        buf[i++] = hex[(vendor >> 12) & 0xF];
        buf[i++] = hex[(vendor >> 8) & 0xF];
        buf[i++] = hex[(vendor >> 4) & 0xF];
        buf[i++] = hex[vendor & 0xF];
    }

    if (i < max - 1) buf[i++] = ':';

    /* Device hex (4 digits) */
    if (i + 4 < max) {
        buf[i++] = hex[(device >> 12) & 0xF];
        buf[i++] = hex[(device >> 8) & 0xF];
        buf[i++] = hex[(device >> 4) & 0xF];
        buf[i++] = hex[device & 0xF];
    }

    buf[i] = '\0';
}

/* ── Register a hardware chain ──────────────────────────────────── */

static int hw_register(const char *name, const char *hw_type,
                       int parent_id, masq_tier_t tier,
                       const char *in_type, const char *out_type)
{
    if (hw_count >= HW_MAX_CHAINS)
        return -1;

    int id = chain_create(name, parent_id, tier);
    if (id < 0)
        return -1;

    /* Add a single processing node with the device's I/O types */
    chain_add_node(id, name, in_type, out_type, 0);

    hw_chain_ids[hw_count] = id;
    hw_strcpy(hw_chain_types[hw_count], hw_type, 32);
    hw_count++;

    return id;
}

/* ── Discovery ──────────────────────────────────────────────────── */

int hw_discover_all(void)
{
    hw_count = 0;

    kputs("hw: discovering hardware...\n");
    uint64_t t0 = timer_read_tsc();

    /* ── Chain zero: CPU (always exists) ─────────────────────────── */
    cpu_chain_id = hw_register("cpu:0", HW_TYPE_CPU,
                                -1, MASQ_SOVEREIGN,
                                "instruction", "result");
    if (cpu_chain_id < 0) {
        kputs("hw: FATAL -- could not create CPU chain\n");
        return 0;
    }

    /* ── Memory chain (child of CPU, sovereign) ──────────────────── */
    hw_register("memory:0", HW_TYPE_MEMORY,
                cpu_chain_id, MASQ_SOVEREIGN,
                "address", "data");

    /* ── Scan PCI bus ────────────────────────────────────────────── */
    int pci_count = pci_device_count();
    if (pci_count == 0) {
        /* PCI hasn't been enumerated yet -- do it now */
        pci_count = pci_enumerate();
    }

    kputs("hw: PCI devices found: ");
    kput_dec((uint64_t)pci_count);
    kputs("\n");

    for (int i = 0; i < pci_count; i++) {
        struct pci_device *dev = pci_get_device(i);
        if (!dev) continue;

        const char *hw_type = pci_class_to_hw_type(dev->class_code,
                                                     dev->subclass);

        /* Skip bridges -- they're infrastructure, not data chains */
        if (hw_streq(hw_type, HW_TYPE_BRIDGE))
            continue;

        /* Skip unknown class 0x00 (host bridge junk) */
        if (dev->class_code == 0x00)
            continue;

        /* Skip system peripherals (class 0x08) -- not routable */
        if (dev->class_code == 0x08)
            continue;

        /* Skip memory controllers (class 0x05) -- handled above */
        if (dev->class_code == 0x05)
            continue;

        /* Build chain name from type + PCI IDs */
        char name[64];
        build_chain_name(name, 64, hw_type,
                         dev->vendor_id, dev->device_id);

        const char *in_t  = hw_input_type(hw_type);
        const char *out_t = hw_output_type(hw_type);

        int id = hw_register(name, hw_type,
                              cpu_chain_id, MASQ_INTERNAL,
                              in_t, out_t);

        if (id >= 0) {
            kputs("  chain ");
            kput_dec((uint64_t)id);
            kputs(": ");
            kputs(name);
            kputs(" [");
            kputs(pci_class_name(dev->class_code, dev->subclass));
            kputs("] ");
            kputs(in_t);
            kputs(" -> ");
            kputs(out_t);
            kputs("\n");
        }
    }

    /* ── Summary ─────────────────────────────────────────────────── */
    uint64_t t1 = timer_read_tsc();
    uint64_t freq = timer_tsc_freq();
    uint64_t us = 0;
    if (freq > 0)
        us = ((t1 - t0) * 1000000) / freq;

    kputs("hw: ");
    kput_dec((uint64_t)hw_count);
    kputs(" chains created in ");
    kput_dec(us);
    kputs(" us\n");

    /* Print the chain tree */
    kputs("hw: chain graph:\n");
    kputs("  [");
    kput_dec((uint64_t)cpu_chain_id);
    kputs("] cpu:0 (sovereign)\n");

    for (int i = 1; i < hw_count; i++) {
        chain_t *c = chain_get(hw_chain_ids[i]);
        if (!c) continue;

        kputs("    +-- [");
        kput_dec((uint64_t)hw_chain_ids[i]);
        kputs("] ");
        kputs(c->name);
        if (c->tier == MASQ_SOVEREIGN)
            kputs(" (sovereign)");
        else
            kputs(" (internal)");
        kputs("\n");
    }

    return hw_count;
}

/* ── Lookup ─────────────────────────────────────────────────────── */

int hw_find_chain(const char *hw_type)
{
    for (int i = 0; i < hw_count; i++) {
        if (hw_streq(hw_chain_types[i], hw_type))
            return hw_chain_ids[i];
    }
    return -1;
}

int hw_chain_count(void)
{
    return hw_count;
}
