/*
 * Zeos — Minimal shell
 *
 * Commands:
 *   help     — show available commands
 *   info     — system info (framebuffer, memory, ACPI)
 *   tsc      — read the TSC (Zixel timing)
 *   clear    — clear screen
 *   zeos     — about
 */

#include "shell.h"
#include "fb.h"
#include "keyboard.h"
#include "pci.h"
#include "pmm.h"
#include "heap.h"
#include "timer.h"
#include "signal.h"

#define CMD_BUF_SIZE 256

static struct zeos_boot_info *g_boot;

/*
 * Simple string comparison.
 */
static int streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void cmd_help(void)
{
    fb_puts("  help     show this message\n");
    fb_puts("  info     system information\n");
    fb_puts("  mem      memory stats\n");
    fb_puts("  heap     heap stats\n");
    fb_puts("  lspci    list PCI/PCIe devices\n");
    fb_puts("  tsc      read TSC (Zixel timing)\n");
    fb_puts("  delta    measure TSC delta (two reads)\n");
    fb_puts("  signal   run signal chain demo\n");
    fb_puts("  clear    clear screen\n");
    fb_puts("  zeos     about\n");
}

static void cmd_info(void)
{
    fb_puts("Framebuffer: ");
    fb_put_dec(g_boot->fb.width);
    fb_puts("x");
    fb_put_dec(g_boot->fb.height);
    fb_puts(" pitch=");
    fb_put_dec(g_boot->fb.pitch);
    fb_puts("\n");

    fb_puts("ACPI RSDP:   ");
    if (g_boot->rsdp) {
        fb_puts("0x");
        fb_put_hex((uint64_t)(unsigned long)g_boot->rsdp);
    } else {
        fb_puts("not found");
    }
    fb_puts("\n");

    /* Count memory */
    uint64_t usable = 0;
    uint8_t *entry = (uint8_t *)g_boot->mmap.entries;
    uint8_t *end = entry + g_boot->mmap.size;
    while (entry < end) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)entry;
        if (desc->Type == EfiConventionalMemory ||
            desc->Type == EfiBootServicesCode ||
            desc->Type == EfiBootServicesData) {
            usable += desc->NumberOfPages * 4096;
        }
        entry += g_boot->mmap.desc_size;
    }
    fb_puts("Memory:      ");
    fb_put_dec(usable / (1024 * 1024));
    fb_puts(" MB usable\n");
}

static uint64_t read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void cmd_tsc(void)
{
    uint64_t tsc = read_tsc();
    fb_puts("TSC: 0x");
    fb_put_hex(tsc);
    fb_puts("\n");
}

static void cmd_delta(void)
{
    fb_puts("Reading TSC delta (two sequential reads)...\n");
    uint64_t t1 = read_tsc();
    uint64_t t2 = read_tsc();
    uint64_t delta = t2 - t1;
    fb_puts("  t1:    0x");
    fb_put_hex(t1);
    fb_puts("\n  t2:    0x");
    fb_put_hex(t2);
    fb_puts("\n  delta: ");
    fb_put_dec(delta);
    fb_puts(" cycles\n");
    fb_puts("  Zixel: timing granularity = ");
    fb_put_dec(delta);
    fb_puts(" TSC ticks\n");
}

static void fb_put_hex8(uint8_t val)
{
    static const char hex[] = "0123456789abcdef";
    fb_putc(hex[(val >> 4) & 0xf]);
    fb_putc(hex[val & 0xf]);
}

static void fb_put_hex16(uint16_t val)
{
    fb_put_hex8((val >> 8) & 0xff);
    fb_put_hex8(val & 0xff);
}

static void cmd_mem(void)
{
    fb_puts("Physical pages: ");
    fb_put_dec(pmm_total_pages());
    fb_puts(" total, ");
    fb_put_dec(pmm_free_pages());
    fb_puts(" free, ");
    fb_put_dec(pmm_used_pages());
    fb_puts(" used\n");

    fb_puts("Memory:         ");
    fb_put_dec(pmm_free_pages() * 4 / 1024);
    fb_puts(" MB free / ");
    fb_put_dec(pmm_total_pages() * 4 / 1024);
    fb_puts(" MB total\n");
}

static void cmd_alloc(void)
{
    uint64_t page = pmm_alloc();
    if (page) {
        fb_puts("Allocated page at 0x");
        fb_put_hex(page);
        fb_puts(" (");
        fb_put_dec(pmm_free_pages());
        fb_puts(" pages remaining)\n");
    } else {
        fb_puts("Out of memory!\n");
    }
}

static void cmd_lspci(void)
{
    int count = pci_device_count();
    if (count == 0) {
        fb_puts("No PCI devices found.\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;

        /* Bus:Dev.Func */
        fb_put_hex8(d->bus);
        fb_puts(":");
        fb_put_hex8(d->dev);
        fb_puts(".");
        fb_putc('0' + d->func);
        fb_puts("  ");

        /* Vendor:Device */
        fb_put_hex16(d->vendor_id);
        fb_puts(":");
        fb_put_hex16(d->device_id);
        fb_puts("  ");

        /* Class name */
        fb_puts(pci_class_name(d->class_code, d->subclass));

        /* Flag known devices */
        if (d->vendor_id == 0x1da3 && d->device_id == 0x0001)
            fb_puts("  ** GOYA HL-1000 **");
        else if (d->vendor_id == 0x1002)
            fb_puts("  [AMD/ATI]");
        else if (d->vendor_id == 0x8086)
            fb_puts("  [Intel]");
        else if (d->vendor_id == 0x10ee)
            fb_puts("  [Xilinx]");
        else if (d->vendor_id == 0x15b3)
            fb_puts("  [Mellanox]");
        else if (d->vendor_id == 0x10de)
            fb_puts("  [NVIDIA]");
        else if (d->vendor_id == 0x1022)
            fb_puts("  [AMD]");

        fb_puts("\n");
    }

    fb_put_dec(count);
    fb_puts(" devices total.\n");
}

static void cmd_heap(void)
{
    fb_puts("Heap: ");
    fb_put_dec(heap_used_bytes());
    fb_puts(" used / ");
    fb_put_dec(heap_total_bytes());
    fb_puts(" total (");
    fb_put_dec(heap_free_bytes());
    fb_puts(" free)\n");
}

/*
 * Signal chain demo: three-node pipeline
 *   [Source] → [Double] → [Display]
 *
 * Source produces a number. Double multiplies by 2. Display shows it.
 * This is TRISA's pattern: detect → transform → output.
 */
static int demo_source(struct sig_node *node, struct sig_data *in,
                        struct sig_data *out)
{
    (void)node;
    (void)in;
    /* Produce the number 42 */
    uint32_t val = 42;
    out->data[0] = val & 0xFF;
    out->data[1] = (val >> 8) & 0xFF;
    out->data[2] = (val >> 16) & 0xFF;
    out->data[3] = (val >> 24) & 0xFF;
    out->size = 4;
    out->type = 1;  /* uint32 */
    return 0;
}

static int demo_double(struct sig_node *node, struct sig_data *in,
                        struct sig_data *out)
{
    (void)node;
    uint32_t val = in->data[0] | (in->data[1] << 8) |
                   (in->data[2] << 16) | (in->data[3] << 24);
    val *= 2;
    out->data[0] = val & 0xFF;
    out->data[1] = (val >> 8) & 0xFF;
    out->data[2] = (val >> 16) & 0xFF;
    out->data[3] = (val >> 24) & 0xFF;
    out->size = 4;
    out->type = 1;
    return 0;
}

static int demo_display(struct sig_node *node, struct sig_data *in,
                         struct sig_data *out)
{
    (void)node;
    (void)out;
    uint32_t val = in->data[0] | (in->data[1] << 8) |
                   (in->data[2] << 16) | (in->data[3] << 24);
    fb_puts("  [Display] received: ");
    fb_put_dec(val);
    fb_puts("\n");
    return 0;
}

static void cmd_signal(void)
{
    fb_puts("Signal chain demo: [Source:42] -> [Double] -> [Display]\n\n");

    /* Create chain */
    int chain = sig_chain_create("demo");
    if (chain < 0) {
        fb_puts("Failed to create chain!\n");
        return;
    }

    /* Add nodes */
    int src = sig_node_add(chain, "Source", demo_source, 0);
    int dbl = sig_node_add(chain, "Double", demo_double, 0);
    int dsp = sig_node_add(chain, "Display", demo_display, 0);

    /* Connect: Source → Double → Display */
    sig_edge_add(chain, src, dbl);
    sig_edge_add(chain, dbl, dsp);

    /* Inject empty data to trigger Source */
    struct sig_data trigger = {.size = 0, .type = 0};
    sig_inject(chain, src, &trigger);

    /* Resolve the chain */
    fb_puts("  Resolving...\n");
    int fired = sig_resolve(chain);

    fb_puts("  ");
    fb_put_dec(fired);
    fb_puts(" nodes fired.\n\n");

    /* Print timing for each node */
    struct sig_chain *c = sig_get_chain(chain);
    if (c) {
        fb_puts("  Node timing (TSC cycles):\n");
        for (int i = 0; i < c->node_count; i++) {
            struct sig_node *n = &c->nodes[i];
            fb_puts("    ");
            fb_puts(n->name);
            fb_puts(": ");
            fb_put_dec(n->tsc_end - n->tsc_start);
            fb_puts(" cycles\n");
        }
        fb_puts("\n  Chain total: ");
        fb_put_dec(c->tsc_end - c->tsc_start);
        fb_puts(" cycles (");
        fb_put_dec(c->resolve_count);
        fb_puts(" resolutions)\n");
    }
}

static void cmd_zeos(void)
{
    fb_puts("\n");
    fb_puts("  Zeos\n");
    fb_puts("  The first operating system with proprioception.\n");
    fb_puts("  Built by Codex Labs LLC.\n\n");
    fb_puts("  Signal chains, not processes.\n");
    fb_puts("  CFA addressing, not flat memory.\n");
    fb_puts("  TRISA decides. The machine feels.\n\n");
}

void shell_run(struct zeos_boot_info *boot)
{
    g_boot = boot;
    char cmd[CMD_BUF_SIZE];
    int pos;

    fb_puts("Type 'help' for commands.\n\n");

    for (;;) {
        fb_puts("zeos> ");
        pos = 0;

        /* Read a line */
        for (;;) {
            char c = keyboard_getc();

            if (c == '\n') {
                fb_putc('\n');
                cmd[pos] = '\0';
                break;
            } else if (c == '\b') {
                if (pos > 0) {
                    pos--;
                    /* Erase character on screen */
                    fb_puts("\b \b");
                }
            } else if (pos < CMD_BUF_SIZE - 1) {
                cmd[pos++] = c;
                fb_putc(c);
            }
        }

        /* Skip empty lines */
        if (pos == 0)
            continue;

        /* Dispatch */
        if (streq(cmd, "help"))
            cmd_help();
        else if (streq(cmd, "info"))
            cmd_info();
        else if (streq(cmd, "mem"))
            cmd_mem();
        else if (streq(cmd, "heap"))
            cmd_heap();
        else if (streq(cmd, "lspci"))
            cmd_lspci();
        else if (streq(cmd, "tsc"))
            cmd_tsc();
        else if (streq(cmd, "delta"))
            cmd_delta();
        else if (streq(cmd, "signal"))
            cmd_signal();
        else if (streq(cmd, "clear"))
            fb_clear(0x001A1A1A);
        else if (streq(cmd, "zeos"))
            cmd_zeos();
        else {
            fb_puts("unknown command: ");
            fb_puts(cmd);
            fb_puts("\n");
        }
    }
}
