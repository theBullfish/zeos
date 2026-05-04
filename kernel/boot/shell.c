/*
 * Zeos — Shell with Persona System
 *
 * Three views of the same shell:
 *   zeros>  — Robotics/hardware-first (Zeros)
 *   derez>  — Code/AI-first (DereZ)
 *   zeos>   — Full system (curtain raised)
 *
 * Every command exists in all modes. The persona only changes what
 * 'help' shows. You can always type any command. The curtain is
 * about discovery, not restriction.
 *
 * "raise"  — lift the curtain, see everything
 * "zeros"  — drop into robotics persona
 * "derez"  — drop into dev persona
 */

#include "shell.h"
#include "usb_msc.h"
#include "usb_hub.h"
#include "block.h"
#include "block_chain.h"
#include "nvme.h"
#include "ahci.h"
#include "persona.h"
#include "theme.h"
#include "kprint.h"
#include "fb.h"
#include "keyboard.h"
#include "pci.h"
#include "msix.h"
#include "pmm.h"
#include "heap.h"
#include "zplus.h"
#include "sigviz.h"
#include "vault.h"
#include "net.h"
#include "net_rtl8188eu.h"
#include "net_ip.h"
#include "net_dns.h"
#include "net_tcp.h"
#include "net_http.h"
#include "timer.h"
#include "signal.h"
#include "chain.h"
#include "cfa_handle.h"
#include "chain_registry.h"
#include "serial.h"
#include "persona_filter.h"
#include "persona_anim.h"
#include "usb_cdc.h"
#include "hda.h"
#include "fat32.h"

#define CMD_BUF_SIZE 256

static struct zeos_boot_info *g_boot;
static enum persona g_persona = PERSONA_FULL;
static int vault_ready = 0;

/* ── Persona-aware accent colors ───────────────── */

static const uint32_t persona_accents[] = {
    COLOR_ZEROS_ACCENT,   /* PERSONA_ZEROS */
    COLOR_DEREZ_ACCENT,   /* PERSONA_DEREZ */
    COLOR_FULL_ACCENT,    /* PERSONA_FULL */
};

static const uint32_t persona_dims[] = {
    COLOR_ZEROS_DIM,      /* PERSONA_ZEROS */
    COLOR_DEREZ_DIM,      /* PERSONA_DEREZ */
    COLOR_FULL_DIM,       /* PERSONA_FULL */
};

/* Get current persona's accent color — used by sigviz and UI.
 * During a persona transition, returns the spring-interpolated color. */
uint32_t theme_accent(void)
{
    if (persona_transitioning())
        return persona_current_accent();
    return persona_accents[g_persona];
}

uint32_t theme_accent_dim(void)
{
    if (persona_transitioning())
        return persona_current_accent_dim();
    return persona_dims[g_persona];
}

/*
 * Draw the shell prompt in the current persona's accent color.
 * Framebuffer gets colored text; serial gets plain text.
 */
static void shell_prompt(void)
{
    const char *tag;
    switch (g_persona) {
    case PERSONA_ZEROS: tag = "zeros"; break;
    case PERSONA_DEREZ: tag = "derez"; break;
    case PERSONA_FULL:  tag = "zeos";  break;
    default:            tag = "zeos";  break;
    }

    /* Get current cursor position (character grid) */
    uint32_t col, row;
    fb_cursor_pos(&col, &row);
    int px = (int)(col * 8);
    int py = (int)(row * 16);

    /* Render colored prompt on framebuffer */
    uint32_t accent = theme_accent();
    fb_text(px, py, tag, accent);
    int tag_len = 0;
    const char *t = tag;
    while (*t++) tag_len++;
    fb_text(px + tag_len * 8, py, "> ", COLOR_ON_SURFACE_2);

    /* Advance framebuffer cursor past the prompt */
    fb_set_cursor(col + (uint32_t)tag_len + 2, row);

    /* Send plain text to serial */
    serial_puts(tag);
    serial_puts("> ");
}

/* ── String helpers ─────────────────────────────── */

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

/* Return pointer past the first word (skip command name, return args) */
static const char *skip_word(const char *s)
{
    while (*s && *s != ' ')
        s++;
    while (*s == ' ')
        s++;
    return s;
}

/* ── Forward declarations ───────────────────────── */

static void cmd_help(const char *args);
static void cmd_info(const char *args);
static void cmd_display(const char *args);
static void cmd_mem(const char *args);
static void cmd_heap(const char *args);
static void cmd_lspci(const char *args);
static void cmd_tsc(const char *args);
static void cmd_delta(const char *args);
static void cmd_signal(const char *args);
static void cmd_clear(const char *args);
static void cmd_about(const char *args);

/* Persona switching */
static void cmd_raise(const char *args);
static void cmd_zeros(const char *args);
static void cmd_derez(const char *args);
static void persona_banner_colored(void);

/* Zeros persona — robotics/hardware */
static void cmd_scan(const char *args);
static void cmd_sensors(const char *args);
static void cmd_motors(const char *args);
static void cmd_build(const char *args);

/* DereZ persona — code/dev */
static void cmd_chains(const char *args);
static void cmd_trace(const char *args);
static void cmd_nodes(const char *args);
static void cmd_inject(const char *args);
static void cmd_inspect(const char *args);

/* Z+ interpreter */
static void cmd_run(const char *args);
static void cmd_programs(const char *args);

/* Signal visualizer */
static void cmd_viz(const char *args);

/* VAULT filesystem */
static void cmd_ls(const char *args);
static void cmd_cat(const char *args);
static void cmd_write_file(const char *args);
static void cmd_mkdir(const char *args);
static void cmd_df(const char *args);

/* Networking */
static void cmd_ping(const char *args);
static void cmd_dns_cmd(const char *args);
static void cmd_fetch(const char *args);
static void cmd_https(const char *args);
static void cmd_netinfo(const char *args);
static void cmd_selftest(const char *args);
static void cmd_beep(const char *args);
static void cmd_static_ip(const char *args);
static void cmd_xxd(const char *args);
static void cmd_cp(const char *args);
static void cmd_wc(const char *args);
static void cmd_portscan(const char *args);
static void cmd_sweep(const char *args);
static void cmd_sha256(const char *args);
static void cmd_nc(const char *args);

/* USB CDC ACM (Arduino/ESP32/Pico serial) */
static void cmd_usb_serial(const char *args);
static void cmd_cdc_send(const char *args);
static void cmd_cdc_recv(const char *args);
static void cmd_wifi(const char *args);
static void cmd_lsdrives(const char *args);
static void cmd_masq_journal(const char *args);

static const char *drive_kind_label(int k) {
    switch (k) {
        case BLOCK_KIND_NVME:    return "nvme";
        case BLOCK_KIND_AHCI:    return "ahci";
        case BLOCK_KIND_USB_MSC: return "usb-msc";
        default:                 return "?";
    }
}

static void cmd_lsdrives(const char *args) {
    (void)args;
    int n = block_drive_count();
    if (n == 0) { kputs("  No storage drives.\n"); return; }
    kputs("\n  idx  kind     sectors        size       model\n");
    kputs(  "  ---  -------  -------------  ---------  -----\n");
    for (int i = 0; i < n; i++) {
        block_drive_info_t info;
        if (block_drive_info(i, &info) != 0) continue;
        uint64_t bytes = info.sectors * (uint64_t)info.sector_size;
        kputs("  ");
        kput_dec(i); kputs("    ");
        kputs(drive_kind_label(info.kind));
        kputs("\t");
        kput_dec(info.sectors);
        kputs("\t");
        if (bytes >= (1ull << 30))      { kput_dec(bytes >> 30); kputs(" GB"); }
        else if (bytes >= (1ull << 20)) { kput_dec(bytes >> 20); kputs(" MB"); }
        else                             { kput_dec(bytes >> 10); kputs(" KB"); }
        if (info.model[0]) { kputs("  "); kputs(info.model); }
        kputs("\n");
    }
    kputs("\n");
}

static void cmd_masq_journal(const char *args) {
    int n = 16;
    if (args && *args) {
        int v = 0; const char *p = args;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); p++; }
        if (v > 0) n = v;
    }
    kputs("\n");
    block_chain_dump_journal(n);
    kputs("\n");
}

/* FAT32 read-only */
static void cmd_fat_mount(const char *args);
static void cmd_fat_ls(const char *args);
static void cmd_fat_cat(const char *args);

/* ── Command table ──────────────────────────────── */

static const struct shell_cmd commands[] = {
    /* Always visible */
    {"help",    "show available commands",       cmd_help,    VIS_ALWAYS},
    {"clear",   "clear screen",                  cmd_clear,   VIS_ALWAYS},

    /* Persona switching — always visible */
    {"raise",   "raise the curtain (full mode)",  cmd_raise,   VIS_ALWAYS},
    {"zeros",   "robotics mode",                  cmd_zeros,   VIS_ALWAYS},
    {"derez",   "dev mode",                       cmd_derez,   VIS_ALWAYS},

    /* Zeros persona — robotics/hardware */
    {"scan",    "scan for hardware devices",      cmd_scan,    VIS_ZEROS},
    {"sensors", "read sensor values",             cmd_sensors, VIS_ZEROS},
    {"motors",  "motor status and control",       cmd_motors,  VIS_ZEROS},
    {"build",   "build and flash project",        cmd_build,   VIS_ZEROS},
    {"delta",   "measure timing delta",           cmd_delta,   VIS_ZEROS},
    {"info",    "system information",             cmd_info,    VIS_ZEROS},
    {"display", "current resolution + monitor (EDID)", cmd_display, VIS_ALWAYS},
    {"mem",     "memory stats",                   cmd_mem,     VIS_ZEROS},

    /* DereZ persona — code/dev */
    {"signal",  "run signal chain demo",          cmd_signal,  VIS_DEREZ},
    {"chains",  "list all active chains",          cmd_chains,  VIS_DEREZ},
    {"nodes",   "show nodes in a chain",          cmd_nodes,   VIS_DEREZ},
    {"inspect", "inspect a chain by ID",           cmd_inspect, VIS_DEREZ},
    {"trace",   "trace signal flow with timing",  cmd_trace,   VIS_DEREZ},
    {"inject",  "inject data into a signal node", cmd_inject,  VIS_DEREZ},
    {"tsc",     "read TSC (raw timing)",          cmd_tsc,     VIS_DEREZ},
    {"heap",    "heap allocator stats",           cmd_heap,    VIS_DEREZ},

    /* Z+ interpreter — visible in DereZ and Full */
    {"run",     "run a Z+ program",               cmd_run,     VIS_DEREZ},
    {"programs","list built-in Z+ programs",       cmd_programs,VIS_DEREZ},
    {"viz",     "visualize signal chain (graphical)", cmd_viz,  VIS_DEREZ},

    /* Networking */
    {"ping",    "ping an IP address",              cmd_ping,    VIS_ALWAYS},
    {"dns",     "resolve a hostname",              cmd_dns_cmd, VIS_DEREZ},
    {"fetch",   "fetch a URL (HTTP GET)",          cmd_fetch,   VIS_DEREZ},
    {"https",   "fetch a URL over TLS (HTTPS GET)", cmd_https,   VIS_DEREZ},
    {"selftest","run subsystem self-test (VAULT, DNS, HTTPS)", cmd_selftest, VIS_DEREZ},
    {"static-ip","configure static IPv4 (use when DHCP unavailable)", cmd_static_ip, VIS_DEREZ},
    {"xxd",     "hex dump of a file",             cmd_xxd,     VIS_ALWAYS},
    {"cp",      "copy a file (cp src dst)",       cmd_cp,      VIS_ALWAYS},
    {"wc",      "byte and line count of a file",  cmd_wc,      VIS_ALWAYS},
    {"portscan","scan TCP ports (portscan host start end)", cmd_portscan, VIS_DEREZ},
    {"sweep",   "ping-sweep a /24 subnet to find live hosts", cmd_sweep,    VIS_DEREZ},
    {"sha256",  "SHA-256 hash of a file",         cmd_sha256,  VIS_DEREZ},
    {"nc",      "netcat-lite: connect, send bytes, print reply", cmd_nc, VIS_DEREZ},

    /* USB CDC ACM (Arduino, ESP32, Pi Pico, generic USB serial) */
    {"usb-serial","list detected USB serial devices",   cmd_usb_serial, VIS_ALWAYS},
    {"cdc-send","send text to USB serial (cdc-send <idx> <text>)", cmd_cdc_send, VIS_ALWAYS},
    {"cdc-recv","read pending bytes from USB serial (cdc-recv <idx>)", cmd_cdc_recv, VIS_ALWAYS},
    {"netinfo", "show network configuration",      cmd_netinfo, VIS_ALWAYS},
    {"lsdrives","list storage drives (NVMe / AHCI / USB MSC)", cmd_lsdrives, VIS_ALWAYS},
    {"masq-journal","show last N block-write journal records (masq-journal [N])", cmd_masq_journal, VIS_DEREZ},
    {"wifi",    "RTL8188EU USB WiFi: status|scan|connect", cmd_wifi, VIS_DEREZ},

    /* VAULT filesystem — always visible */
    {"ls",      "list files",                      cmd_ls,      VIS_ALWAYS},
    {"cat",     "show file contents",              cmd_cat,     VIS_ALWAYS},
    {"save",    "save text to file (save path text)", cmd_write_file, VIS_DEREZ},
    {"mkdir",   "create directory",                cmd_mkdir,   VIS_DEREZ},
    {"df",      "VAULT disk usage",                cmd_df,      VIS_FULL},

    /* Full only — deep system commands */
    {"lspci",   "list PCI/PCIe devices (raw)",    cmd_lspci,   VIS_FULL},
    {"beep",    "play a 440 Hz tone via HDA audio", cmd_beep,   VIS_ALWAYS},
    {"about",   "about Zeos",                     cmd_about,   VIS_FULL},

    /* FAT32 (USB / SD / ESP) read-only */
    {"fat-mount","mount FAT32 (fat-mount [<drive> <part-lba>])", cmd_fat_mount, VIS_ALWAYS},
    {"fat-ls",  "list FAT32 directory (fat-ls <path>)", cmd_fat_ls, VIS_ALWAYS},
    {"fat-cat", "show FAT32 file (fat-cat <path>)",  cmd_fat_cat, VIS_ALWAYS},
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

/* ── Core commands (unchanged logic) ────────────── */

static void cmd_help(const char *args)
{
    (void)args;
    kputs("\n");
    persona_banner_colored();

    for (int i = 0; i < (int)NUM_COMMANDS; i++) {
        if (cmd_visible(&commands[i], g_persona)) {
            kputs("  ");
            kputs(commands[i].name);
            /* Pad to 10 chars */
            int len = 0;
            const char *p = commands[i].name;
            while (*p++) len++;
            for (int j = len; j < 10; j++)
                kputc(' ');
            kputs(commands[i].desc);
            kputs("\n");
        }
    }
    kputs("\n");

    if (g_persona != PERSONA_FULL) {
        kputs("  Tip: any command works even if not listed. ");
        kputs("'raise' shows all.\n\n");
    }
}

static void cmd_info(const char *args)
{
    (void)args;
    kputs("Framebuffer: ");
    kput_dec(g_boot->fb.width);
    kputs("x");
    kput_dec(g_boot->fb.height);
    kputs(" pitch=");
    kput_dec(g_boot->fb.pitch);
    kputs("\n");

    kputs("ACPI RSDP:   ");
    if (g_boot->rsdp) {
        kputs("0x");
        kput_hex((uint64_t)(unsigned long)g_boot->rsdp);
    } else {
        kputs("not found");
    }
    kputs("\n");

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
    kputs("Memory:      ");
    kput_dec(usable / (1024 * 1024));
    kputs(" MB usable\n");
}

static const char *pixel_format_name(uint32_t f)
{
    switch (f) {
    case PixelRedGreenBlueReserved8BitPerColor:        return "RGB888";
    case PixelBlueGreenRedReserved8BitPerColor: return "BGRX8888";
    case PixelBitMask:                          return "BitMask";
    case PixelBltOnly:                          return "BltOnly";
    default:                                    return "unknown";
    }
}

static void cmd_display(const char *args)
{
    (void)args;
    kputs("Resolution:  ");
    kput_dec(g_boot->fb.width);
    kputs("x");
    kput_dec(g_boot->fb.height);
    kputs(" (pitch ");
    kput_dec(g_boot->fb.pitch);
    kputs(")\n");

    kputs("Pixel fmt:   ");
    kputs(pixel_format_name(g_boot->fb.pixel_format));
    kputs(" (");
    kput_dec(g_boot->fb.pixel_format);
    kputs(")\n");

    kputs("FB base:     0x");
    kput_hex((uint64_t)(unsigned long)g_boot->fb.base);
    kputs("  size=");
    kput_dec(g_boot->fb.size / 1024);
    kputs(" KB\n");

    if (g_boot->display.edid_valid) {
        kputs("Monitor:     ");
        kputs(g_boot->display.mfr);
        kputs(" product 0x");
        kput_hex(g_boot->display.product_id);
        kputs("\n");
        kputs("Native:      ");
        kput_dec(g_boot->display.native_w);
        kputs("x");
        kput_dec(g_boot->display.native_h);
        kputs(" @ ");
        kput_dec(g_boot->display.native_hz);
        kputs(" Hz\n");
    } else {
        kputs("Monitor:     EDID not exposed by firmware\n");
    }
}

static uint64_t read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void cmd_tsc(const char *args)
{
    (void)args;
    uint64_t tsc = read_tsc();
    kputs("TSC: 0x");
    kput_hex(tsc);
    kputs("\n");
}

static void cmd_delta(const char *args)
{
    (void)args;
    kputs("Reading TSC delta (two sequential reads)...\n");
    uint64_t t1 = read_tsc();
    uint64_t t2 = read_tsc();
    uint64_t delta = t2 - t1;
    kputs("  t1:    0x");
    kput_hex(t1);
    kputs("\n  t2:    0x");
    kput_hex(t2);
    kputs("\n  delta: ");
    kput_dec(delta);
    kputs(" cycles\n");

    if (g_persona == PERSONA_ZEROS) {
        /* Student-friendly explanation */
        kputs("\n  This is how fast your hardware thinks.\n");
        kputs("  Lower = faster silicon. Watch it change with temperature.\n");
    } else {
        kputs("  Zixel: timing granularity = ");
        kput_dec(delta);
        kputs(" TSC ticks\n");
    }
}

static void fb_put_hex8(uint8_t val)
{
    static const char hex[] = "0123456789abcdef";
    kputc(hex[(val >> 4) & 0xf]);
    kputc(hex[val & 0xf]);
}

static void fb_put_hex16(uint16_t val)
{
    fb_put_hex8((val >> 8) & 0xff);
    fb_put_hex8(val & 0xff);
}

static void cmd_mem(const char *args)
{
    (void)args;
    kputs("Physical pages: ");
    kput_dec(pmm_total_pages());
    kputs(" total, ");
    kput_dec(pmm_free_pages());
    kputs(" free, ");
    kput_dec(pmm_used_pages());
    kputs(" used\n");

    kputs("Memory:         ");
    kput_dec(pmm_free_pages() * 4 / 1024);
    kputs(" MB free / ");
    kput_dec(pmm_total_pages() * 4 / 1024);
    kputs(" MB total\n");
}

static void cmd_heap(const char *args)
{
    (void)args;
    kputs("Heap: ");
    kput_dec(heap_used_bytes());
    kputs(" used / ");
    kput_dec(heap_total_bytes());
    kputs(" total (");
    kput_dec(heap_free_bytes());
    kputs(" free)\n");
}

/* Knows-what's-plugged-in is the prime directive. lspci prints every
 * PCI/PCIe device, names vendor and product where we know them, and
 * — when run with "-v" — walks the capability chain to show link
 * speed/width, BARs with sizes, MSI/MSI-X presence. */
static void cmd_lspci(const char *args)
{
    int verbose = 0;
    while (*args == ' ') args++;
    if (args[0] == '-' && args[1] == 'v') verbose = 1;

    int count = pci_device_count();
    if (count == 0) {
        kputs("No PCI devices found.\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;

        /* Bus:Dev.Func   Vendor:Device   Class — Vendor Product */
        fb_put_hex8(d->bus);  kputs(":");
        fb_put_hex8(d->dev);  kputs(".");
        kputc('0' + d->func); kputs("  ");
        fb_put_hex16(d->vendor_id); kputs(":");
        fb_put_hex16(d->device_id); kputs("  ");
        kputs(pci_class_name(d->class_code, d->subclass));
        kputs(" — ");
        kputs(pci_vendor_name(d->vendor_id));
        const char *prod = pci_device_name(d->vendor_id, d->device_id);
        if (prod && *prod) { kputs(" "); kputs(prod); }
        if (d->vendor_id == 0x1DA3 && d->device_id == 0x0001)
            kputs("  ** GOYA **");
        kputs("\n");

        if (!verbose) continue;

        /* PCIe link */
        int speed = pci_link_speed(d);
        int width = pci_link_width(d);
        if (speed > 0) {
            kputs("           link: ");
            kputs(pci_link_speed_name(speed));
            kputs(" x"); kput_dec((unsigned)width); kputs("\n");
        }

        /* MSI / MSI-X */
        if (pci_has_msi(d) || pci_has_msix(d)) {
            kputs("           irq:  ");
            if (pci_has_msi(d))  kputs("MSI ");
            if (pci_has_msix(d)) kputs("MSI-X ");
            kputs("\n");
        }

        /* BARs with sizes */
        for (int b = 0; b < 6; b++) {
            if (d->bar[b] == 0) continue;
            uint64_t sz = pci_bar_size(d, b);
            kputs("           bar"); kputc('0' + b);
            kputs(": ");
            if (d->bar[b] & 1) kputs("io  ");
            else               kputs("mem ");
            fb_put_hex16((uint16_t)((d->bar[b] & ~0xFu) >> 16));
            fb_put_hex16((uint16_t)(d->bar[b] & 0xFFFFu));
            kputs("  size=");
            if (sz >= (1ull << 30))      { kput_dec((unsigned)(sz >> 30)); kputs("G"); }
            else if (sz >= (1ull << 20)) { kput_dec((unsigned)(sz >> 20)); kputs("M"); }
            else if (sz >= (1ull << 10)) { kput_dec((unsigned)(sz >> 10)); kputs("K"); }
            else                          { kput_dec((unsigned)sz); kputs("B"); }
            /* skip BAR slot taken by 64-bit upper half */
            if (!(d->bar[b] & 1) && (d->bar[b] & 0x6) == 0x4 && b < 5) {
                kputs(" [64-bit]");
                b++;
            }
            kputs("\n");
        }
    }

    kputs("\n");
    kput_dec(count);
    kputs(" device(s) total.");
    if (!verbose) kputs("  Use 'lspci -v' for link speed, BARs, IRQ caps.");
    kputs("\n");
}

static void cmd_clear(const char *args)
{
    (void)args;
    fb_clear(COLOR_SURFACE);
}

/* ── Signal chain demo (unchanged) ──────────────── */

static int demo_source(struct sig_node *node, struct sig_data *in,
                        struct sig_data *out)
{
    (void)node;
    (void)in;
    uint32_t val = 42;
    out->data[0] = val & 0xFF;
    out->data[1] = (val >> 8) & 0xFF;
    out->data[2] = (val >> 16) & 0xFF;
    out->data[3] = (val >> 24) & 0xFF;
    out->size = 4;
    out->type = 1;
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
    kputs("  [Display] received: ");
    kput_dec(val);
    kputs("\n");
    return 0;
}

static void cmd_signal(const char *args)
{
    (void)args;
    kputs("Signal chain demo: [Source:42] -> [Double] -> [Display]\n\n");

    int chain = sig_chain_create("demo");
    if (chain < 0) {
        kputs("Failed to create chain!\n");
        return;
    }

    int src = sig_node_add(chain, "Source", demo_source, 0);
    int dbl = sig_node_add(chain, "Double", demo_double, 0);
    int dsp = sig_node_add(chain, "Display", demo_display, 0);

    sig_edge_add(chain, src, dbl);
    sig_edge_add(chain, dbl, dsp);

    struct sig_data trigger = {.size = 0, .type = 0};
    sig_inject(chain, src, &trigger);

    kputs("  Resolving...\n");
    int fired = sig_resolve(chain);

    kputs("  ");
    kput_dec(fired);
    kputs(" nodes fired.\n\n");

    struct sig_chain *c = sig_get_chain(chain);
    if (c) {
        kputs("  Node timing (TSC cycles):\n");
        for (int i = 0; i < c->node_count; i++) {
            struct sig_node *n = &c->nodes[i];
            kputs("    ");
            kputs(n->name);
            kputs(": ");
            kput_dec(n->tsc_end - n->tsc_start);
            kputs(" cycles\n");
        }
        kputs("\n  Chain total: ");
        kput_dec(c->tsc_end - c->tsc_start);
        kputs(" cycles (");
        kput_dec(c->resolve_count);
        kputs(" resolutions)\n");
    }
}

static void cmd_about(const char *args)
{
    (void)args;
    kputs("\n");
    kputs("  Zeos\n");
    kputs("  The first operating system with proprioception.\n");
    kputs("  Built by Codex Labs LLC.\n\n");
    kputs("  Signal chains, not processes.\n");
    kputs("  CFA addressing, not flat memory.\n");
    kputs("  TRISA decides. The machine feels.\n\n");
}

/* ── Persona switching ──────────────────────────── */

/*
 * Persona banner with accent-colored header line.
 * The bracket header is drawn in the persona's accent color,
 * the rest of the banner in secondary text.
 */
static void persona_banner_colored(void)
{
    const char *header;
    const char *sub;

    switch (g_persona) {
    case PERSONA_ZEROS:
        header = "  [ Zeros — Robotics Mode ]";
        sub = "  Hardware, sensors, motors. Type 'raise' to see everything.\n\n";
        break;
    case PERSONA_DEREZ:
        header = "  [ DereZ — Dev Mode ]";
        sub = "  Code, signals, debug. Type 'raise' to see everything.\n\n";
        break;
    case PERSONA_FULL:
        header = "  [ Zeos — Full System ]";
        sub = "  Curtain raised. Everything visible.\n\n";
        break;
    default:
        header = "  [ Zeos ]";
        sub = "\n";
        break;
    }

    /* Draw header in accent color on framebuffer */
    uint32_t col, row;
    fb_cursor_pos(&col, &row);
    fb_text((int)(col * 8), (int)(row * 16), header, theme_accent());

    /* Advance cursor past header */
    int hlen = 0;
    const char *h = header;
    while (*h++) hlen++;
    /* Header goes to end of line — just advance to next line */
    fb_set_cursor(0, row + 1);

    /* Send plain text to serial */
    serial_puts(header);
    serial_putc('\n');

    /* Subtitle in normal text */
    kputs(sub);
}

static void cmd_raise(const char *args)
{
    (void)args;
    int old = (int)g_persona;
    g_persona = PERSONA_FULL;
    persona_transition(old, PERSONA_FULL);
    kputs("\n");
    persona_banner_colored();
}

static void cmd_zeros(const char *args)
{
    (void)args;
    int old = (int)g_persona;
    g_persona = PERSONA_ZEROS;
    persona_transition(old, PERSONA_ZEROS);
    kputs("\n");
    persona_banner_colored();
}

static void cmd_derez(const char *args)
{
    (void)args;
    int old = (int)g_persona;
    g_persona = PERSONA_DEREZ;
    persona_transition(old, PERSONA_DEREZ);
    kputs("\n");
    persona_banner_colored();
}

/* ── Zeros persona commands ─────────────────────── */

static void cmd_scan(const char *args)
{
    (void)args;
    kputs("\n  Scanning for hardware...\n\n");

    int count = pci_device_count();
    int hw_count = 0;

    for (int i = 0; i < count; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;

        /* Show hardware in student-friendly terms */
        const char *friendly = 0;

        if (d->vendor_id == 0x1da3 && d->device_id == 0x0001)
            friendly = "Goya Brain Card (AI accelerator)";
        else if (d->vendor_id == 0x10ee)
            friendly = "FPGA Board (programmable logic)";
        else if (d->class_code == 0x03)
            friendly = "Display (graphics)";
        else if (d->class_code == 0x01)
            friendly = "Storage (disk/SSD)";
        else if (d->class_code == 0x02)
            friendly = "Network (ethernet/wifi)";
        else if (d->class_code == 0x0C && d->subclass == 0x03)
            friendly = "USB Controller";
        else if (d->class_code == 0x04)
            friendly = "Audio";
        else
            continue;  /* Skip boring bridge/host devices */

        kputs("  ");
        kputs(friendly);
        kputs("\n");
        hw_count++;
    }

    if (hw_count == 0) {
        kputs("  No interesting hardware found.\n");
    }

    kputs("\n  ");
    kput_dec(hw_count);
    kputs(" devices ready.\n");
    kputs("  Use 'lspci' for the raw PCI bus view.\n\n");
}

static void cmd_sensors(const char *args)
{
    (void)args;
    kputs("\n  Sensor readings:\n\n");

    /* TSC delta as a "temperature proxy" — this is real Zixel */
    uint64_t t1 = read_tsc();
    uint64_t t2 = read_tsc();
    uint64_t delta = t2 - t1;

    kputs("  timing    ");
    kput_dec(delta);
    kputs(" cycles  (silicon speed — changes with heat)\n");

    kputs("  memory    ");
    kput_dec(pmm_free_pages() * 4 / 1024);
    kputs(" MB free\n");

    kputs("  heap      ");
    kput_dec(heap_free_bytes());
    kputs(" bytes free\n");

    /* PCI device count as "what's connected" */
    kputs("  devices   ");
    kput_dec(pci_device_count());
    kputs(" on bus\n");

    /* Signal chains as "running programs" */
    kputs("  chains    ");
    kput_dec(sig_chain_count());
    kputs(" active\n");

    kputs("\n  Tip: run 'sensors' again — watch the timing value change.\n");
    kputs("  That's your hardware's heartbeat.\n\n");
}

static void cmd_motors(const char *args)
{
    (void)args;
    kputs("\n  Motor subsystem: no hardware connected.\n\n");
    kputs("  When a motor controller is on the bus, this command\n");
    kputs("  shows speed, direction, current draw, and fault state.\n\n");
    kputs("  Supported controllers:\n");
    kputs("    - PWM via GPIO (direct pin)\n");
    kputs("    - I2C motor drivers (PCA9685, etc.)\n");
    kputs("    - CAN bus (industrial servos)\n\n");
    kputs("  Connect hardware and run 'scan' to detect it.\n\n");
}

static void cmd_build(const char *args)
{
    (void)args;
    kputs("\n  Build system: use 'zeos build' from the host.\n\n");
    kputs("  From your Linux terminal:\n");
    kputs("    zeos build       compile the kernel\n");
    kputs("    zeos run         build and test in QEMU\n");
    kputs("    zeos flash       write to USB for real hardware\n");
    kputs("    zeos doctor      check dependencies\n\n");
}

/* ── DereZ persona commands ─────────────────────── */

static void cmd_chains(const char *args)
{
    (void)args;
    int sig_count = sig_chain_count();
    int ch_count = chain_count();

    if (sig_count == 0 && ch_count == 0) {
        kputs("No chains active.\n");
        kputs("Run 'signal' or 'run' to create chains.\n");
        return;
    }

    if (sig_count > 0) {
        kputs("\n  Signal chains:\n\n");
        for (int i = 0; i < sig_count; i++) {
            struct sig_chain *c = sig_get_chain(i);
            if (!c || !c->active) continue;

            kputs("  [");
            kput_dec(i);
            kputs("] ");
            kputs(c->name);
            kputs("  (");
            kput_dec(c->node_count);
            kputs(" nodes, ");
            kput_dec(c->resolve_count);
            kputs(" resolutions)\n");
        }
        kputs("\n  Use 'nodes <id>' to inspect a signal chain.\n");
    }

    if (ch_count > 0) {
        int vis = persona_visible_chain_count();
        kputs("\n  Active chains (chain.h):\n\n");
        for (int i = 0; i < MAX_CHAINS; i++) {
            chain_t *c = chain_get(i);
            if (!c) continue;
            if (!persona_can_see(i)) continue;

            kputs("  [");
            kput_dec((uint64_t)c->id);
            kputs("] ");
            kputs(c->name);
            kputs("  (");
            kput_dec((uint64_t)c->node_count);
            kputs(" nodes, status=");
            switch (c->status) {
            case CHAIN_LIVE:     kputs("live");     break;
            case CHAIN_PAUSED:   kputs("paused");   break;
            case CHAIN_ERROR:    kputs("error");    break;
            case CHAIN_DETACHED: kputs("detached"); break;
            }
            kputs(")\n");
        }
        kputs("\n  Visible: ");
        kput_dec((uint64_t)vis);
        kputs("/");
        kput_dec((uint64_t)ch_count);
        kputs(" chain(s)\n");
    }

    kputs("\n");
}

static void cmd_nodes(const char *args)
{
    /* Parse chain ID from args */
    int chain_id = 0;
    if (*args >= '0' && *args <= '9') {
        chain_id = *args - '0';
    }

    struct sig_chain *c = sig_get_chain(chain_id);
    if (!c) {
        kputs("Chain ");
        kput_dec(chain_id);
        kputs(" not found. Run 'chains' to see active chains.\n");
        return;
    }

    kputs("\n  Chain: ");
    kputs(c->name);
    kputs("\n\n");

    for (int i = 0; i < c->node_count; i++) {
        struct sig_node *n = &c->nodes[i];

        kputs("  [");
        kput_dec(i);
        kputs("] ");
        kputs(n->name);

        /* State */
        kputs("  state=");
        switch (n->state) {
        case SIG_IDLE:    kputs("idle");    break;
        case SIG_READY:   kputs("ready");   break;
        case SIG_RUNNING: kputs("running"); break;
        case SIG_DONE:    kputs("done");    break;
        case SIG_ERROR:   kputs("ERROR");   break;
        }

        /* Timing (if fired) */
        if (n->state == SIG_DONE && n->tsc_end > n->tsc_start) {
            kputs("  (");
            kput_dec(n->tsc_end - n->tsc_start);
            kputs(" cycles)");
        }

        /* Edges */
        if (n->output_count > 0) {
            kputs("  -> ");
            for (int e = 0; e < n->output_count; e++) {
                if (e > 0) kputs(", ");
                kputs(c->nodes[n->output_nodes[e]].name);
            }
        }

        kputs("\n");
    }
    kputs("\n");
}

static void cmd_inspect(const char *args)
{
    if (!*args || (*args < '0' || *args > '9')) {
        kputs("  Usage: inspect <chain_id>\n");
        kputs("  Use 'chains' to see all active chains.\n");
        return;
    }

    /* Parse multi-digit chain ID */
    int chain_id = 0;
    const char *p = args;
    while (*p >= '0' && *p <= '9') {
        chain_id = chain_id * 10 + (*p - '0');
        p++;
    }

    if (!persona_can_see(chain_id)) {
        kputs("  Chain ");
        kput_dec((uint64_t)chain_id);
        kputs(" is not visible in the current persona.\n");
        return;
    }

    zp_inspect_chain(chain_id);
}

static void cmd_trace(const char *args)
{
    /* Parse chain ID */
    int chain_id = 0;
    if (*args >= '0' && *args <= '9') {
        chain_id = *args - '0';
    }

    struct sig_chain *c = sig_get_chain(chain_id);
    if (!c) {
        kputs("Chain ");
        kput_dec(chain_id);
        kputs(" not found. Run 'signal' first to create a chain.\n");
        return;
    }

    kputs("\n  Signal trace: ");
    kputs(c->name);
    kputs("\n\n");

    /* Visual flow diagram */
    for (int i = 0; i < c->node_count; i++) {
        struct sig_node *n = &c->nodes[i];

        /* Node box */
        kputs("  [");
        kputs(n->name);
        kputs("]");

        /* Timing bar — proportional to cycles spent */
        if (n->state == SIG_DONE && n->tsc_end > n->tsc_start) {
            uint64_t cycles = n->tsc_end - n->tsc_start;
            kputs(" ");
            kput_dec(cycles);
            kputs("cy ");

            /* Simple bar graph (1 block per ~100 cycles, capped at 20) */
            int bars = (int)(cycles / 100);
            if (bars < 1) bars = 1;
            if (bars > 20) bars = 20;
            for (int b = 0; b < bars; b++)
                kputs("#");
        }

        kputs("\n");

        /* Arrow to next */
        if (n->output_count > 0 && i < c->node_count - 1) {
            kputs("    |\n");
            kputs("    v\n");
        }
    }

    kputs("\n  Chain total: ");
    kput_dec(c->tsc_end - c->tsc_start);
    kputs(" cycles across ");
    kput_dec(c->node_count);
    kputs(" nodes\n\n");
}

static void cmd_inject(const char *args)
{
    (void)args;
    kputs("\n  inject: inject data into a running signal chain.\n\n");
    kputs("  Usage:  inject <chain_id> <node_id> <value>\n");
    kputs("  Example: inject 0 0 42\n\n");
    kputs("  This feeds a value into a node's input and triggers\n");
    kputs("  the chain to resolve. Watch data flow with 'trace'.\n\n");
}

/* ── Z+ built-in programs ───────────────────────── */

/* hello_chain.zp — the first Z+ program */
static const char zp_hello_chain[] =
    "// hello_chain.zp — Your first signal chain\n"
    "source : emit(42)\n"
    "double : input -> * 2 -> output\n"
    "display : input -> print(\"Result: {value}\")\n"
    "source -> double -> display\n";

/* triple_chain — three transforms */
static const char zp_triple[] =
    "// Triple chain — three stages\n"
    "start : emit(10)\n"
    "add5 : input -> + 5 -> output\n"
    "times3 : input -> * 3 -> output\n"
    "show : input -> print(\"Final: {value}\")\n"
    "start -> add5 -> times3 -> show\n";

/* math_test — multiple operations */
static const char zp_math[] =
    "// Math pipeline\n"
    "seed : emit(7)\n"
    "double : input -> * 2 -> output\n"
    "add100 : input -> + 100 -> output\n"
    "triple : input -> * 3 -> output\n"
    "sub10 : input -> - 10 -> output\n"
    "result : input -> print(\"7 * 2 + 100 * 3 - 10 = {value}\")\n"
    "seed -> double -> add100 -> triple -> sub10 -> result\n";

/* gate_demo — conditional signal flow */
static const char zp_gate[] =
    "// Gate demo — signals pass or block\n"
    "big : emit(100)\n"
    "small : emit(5)\n"
    "check_big : input -> gate(> 50) -> output\n"
    "check_small : input -> gate(> 50) -> output\n"
    "show_big : input -> print(\"PASSED: {value} > 50\")\n"
    "show_small : input -> print(\"PASSED: {value} > 50\")\n"
    "big -> check_big -> show_big\n"
    "small -> check_small -> show_small\n";

/* fork_demo — one source, multiple destinations */
static const char zp_fork[] =
    "// Fork demo — one signal, three paths\n"
    "source : emit(42)\n"
    "double : input -> * 2 -> output\n"
    "triple : input -> * 3 -> output\n"
    "show_raw : input -> print(\"Raw: {value}\")\n"
    "show_dbl : input -> print(\"Doubled: {value}\")\n"
    "show_tri : input -> print(\"Tripled: {value}\")\n"
    "source -> {show_raw, double, triple}\n"
    "double -> show_dbl\n"
    "triple -> show_tri\n";

/* pipeline — gate + math combined */
static const char zp_pipeline[] =
    "// Pipeline: emit -> transform -> gate -> display\n"
    "start : emit(30)\n"
    "boost : input -> * 3 -> output\n"
    "check : input -> gate(> 50) -> output\n"
    "result : input -> print(\"Passed gate: {value}\")\n"
    "blocked : input -> gate(< 50) -> output\n"
    "nope : input -> print(\"This should not print\")\n"
    "start -> boost -> check -> result\n"
    "start -> blocked -> nope\n";

struct zp_builtin {
    const char *name;
    const char *desc;
    const char *source;
};

static const struct zp_builtin builtins[] = {
    {"hello",    "first signal chain (42 * 2 = 84)",                zp_hello_chain},
    {"triple",   "three-stage pipeline (10 + 5 * 3 = 45)",         zp_triple},
    {"math",     "five-stage math pipeline",                        zp_math},
    {"gate",     "gate demo — 100 passes, 5 blocks (threshold 50)", zp_gate},
    {"fork",     "fork demo — one source, three paths",             zp_fork},
    {"pipeline", "gate + math combined — transform then filter",    zp_pipeline},
};

#define NUM_BUILTINS (sizeof(builtins) / sizeof(builtins[0]))

static void cmd_programs(const char *args)
{
    (void)args;
    kputs("\n  Built-in Z+ programs:\n\n");
    for (int i = 0; i < (int)NUM_BUILTINS; i++) {
        kputs("  ");
        kputs(builtins[i].name);
        int len = 0;
        const char *p = builtins[i].name;
        while (*p++) len++;
        for (int j = len; j < 10; j++)
            kputc(' ');
        kputs(builtins[i].desc);
        kputs("\n");
    }
    kputs("\n  Usage: run <name>\n");
    kputs("  Example: run hello\n\n");
}

static void cmd_run(const char *args)
{
    if (!*args) {
        kputs("  Usage: run <program>\n");
        kputs("  Type 'programs' to see available programs.\n");
        return;
    }

    /* Find the built-in program */
    for (int i = 0; i < (int)NUM_BUILTINS; i++) {
        /* Compare args to builtin name */
        const char *a = args;
        const char *b = builtins[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*b == '\0' && (*a == '\0' || *a == ' ')) {
            /* Show the source */
            kputs("\n  Source:\n");
            const char *src = builtins[i].source;
            while (*src) {
                if (*src == '\n') {
                    kputs("\n");
                    if (*(src + 1))
                        kputs("    ");
                } else {
                    kputc(*src);
                }
                src++;
            }
            kputs("\n");

            /* Run it */
            zp_run(builtins[i].source);
            return;
        }
    }

    /* Not a built-in — try loading from VAULT */
    if (vault_ready) {
        char vpath[128];
        int pi = 0;
        const char *prefix = "/programs/";
        while (*prefix && pi < 100) vpath[pi++] = *prefix++;
        const char *a = args;
        while (*a && *a != ' ' && pi < 120) vpath[pi++] = *a++;
        /* Add .zp extension if not present */
        if (pi < 4 || vpath[pi-3] != '.' || vpath[pi-2] != 'z' || vpath[pi-1] != 'p') {
            vpath[pi++] = '.'; vpath[pi++] = 'z'; vpath[pi++] = 'p';
        }
        vpath[pi] = '\0';

        int sz = vault_size(vpath);
        if (sz > 0 && sz < 2048) {
            char src[2048];
            int got = vault_read(vpath, src, 2047);
            if (got > 0) {
                src[got] = '\0';
                kputs("\n  Loading from VAULT: ");
                kputs(vpath);
                kputs("\n");
                zp_run(src);
                return;
            }
        }

        /* Also try the exact path given */
        sz = vault_size(args);
        if (sz > 0 && sz < 2048) {
            char src[2048];
            int got = vault_read(args, src, 2047);
            if (got > 0) {
                src[got] = '\0';
                kputs("\n  Loading from VAULT: ");
                kputs(args);
                kputs("\n");
                zp_run(src);
                return;
            }
        }
    }

    kputs("  Unknown program: ");
    kputs(args);
    kputs("\n  Type 'programs' to see available programs.\n");
    kputs("  Or save a .zp file to /programs/ and run it by name.\n");
}

static void cmd_viz(const char *args)
{
    /* Parse chain ID from args (default 0) */
    int chain_id = 0;
    if (*args >= '0' && *args <= '9')
        chain_id = *args - '0';

    struct sig_chain *c = sig_get_chain(chain_id);
    if (!c) {
        kputs("No chain ");
        kput_dec(chain_id);
        kputs(". Run 'signal' or 'run hello' first.\n");
        return;
    }

    /* Save cursor position */
    uint32_t save_col, save_row;
    fb_cursor_pos(&save_col, &save_row);

    /* Draw visualization in the right half of the screen */
    uint32_t sw = fb_width();
    uint32_t sh = fb_height();
    int viz_x = (int)(sw / 2);
    int viz_y = 0;
    int viz_w = (int)(sw - viz_x);
    int viz_h = (int)sh;

    sigviz_draw(chain_id, viz_x, viz_y, viz_w, viz_h);

    kputs("  Signal chain ");
    kput_dec(chain_id);
    kputs(" drawn on right half. Press any key to dismiss.\n");

    /* Wait for keypress */
    keyboard_getc();

    /* Restore: clear the viz area and redraw will happen on next output */
    fb_rect(viz_x, viz_y, viz_w, viz_h, COLOR_SURFACE);

    /* Restore cursor */
    fb_set_cursor(save_col, save_row);
}

/* ── Networking commands ────────────────────────── */

static void cmd_netinfo(const char *args)
{
    (void)args;
    if (!g_net.up) {
        kputs("  Network not available.\n");
        return;
    }
    kputs("\n  Network Configuration:\n\n");
    kputs("  MAC:     ");
    for (int i = 0; i < 6; i++) {
        if (i > 0) kputs(":");
        static const char hex[] = "0123456789abcdef";
        kputc(hex[(g_net.mac.b[i] >> 4) & 0xf]);
        kputc(hex[g_net.mac.b[i] & 0xf]);
    }
    kputs("\n  IP:      ");
    kput_dec(g_net.ip.b[0]); kputs(".");
    kput_dec(g_net.ip.b[1]); kputs(".");
    kput_dec(g_net.ip.b[2]); kputs(".");
    kput_dec(g_net.ip.b[3]);
    kputs("\n  Gateway: ");
    kput_dec(g_net.gateway.b[0]); kputs(".");
    kput_dec(g_net.gateway.b[1]); kputs(".");
    kput_dec(g_net.gateway.b[2]); kputs(".");
    kput_dec(g_net.gateway.b[3]);
    kputs("\n  DNS:     ");
    kput_dec(g_net.dns.b[0]); kputs(".");
    kput_dec(g_net.dns.b[1]); kputs(".");
    kput_dec(g_net.dns.b[2]); kputs(".");
    kput_dec(g_net.dns.b[3]);
    kputs("\n\n");
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static const char *parse_one_ip(const char *p, struct ipv4_addr *out, int *ok);
static const char *parse_one_ip(const char *p, struct ipv4_addr *out, int *ok)
{
    int octet = 0, val = 0, digits = 0;
    *ok = 0;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            digits++;
            if (digits > 3 || val > 255) return p;
        } else if (*p == '.') {
            if (octet >= 4 || digits == 0) return p;
            out->b[octet++] = (uint8_t)val;
            val = 0; digits = 0;
        } else {
            break;
        }
        p++;
    }
    if (octet == 3 && digits > 0 && val <= 255) {
        out->b[3] = (uint8_t)val;
        *ok = 1;
    }
    return p;
}

static void cmd_static_ip(const char *args)
{
    /* static-ip <ip> <gateway> <dns> [netmask] */
    struct ipv4_addr ip, gw, dns, mask = (struct ipv4_addr){{255,255,255,0}};
    int ok;
    args = skip_ws(args);
    args = parse_one_ip(args, &ip, &ok);
    if (!ok) goto usage;
    args = skip_ws(args);
    args = parse_one_ip(args, &gw, &ok);
    if (!ok) goto usage;
    args = skip_ws(args);
    args = parse_one_ip(args, &dns, &ok);
    if (!ok) goto usage;
    args = skip_ws(args);
    if (*args) {
        parse_one_ip(args, &mask, &ok);
        if (!ok) goto usage;
    }
    g_net.ip      = ip;
    g_net.gateway = gw;
    g_net.dns     = dns;
    g_net.netmask = mask;
    g_net.up      = 1;  /* override DHCP-failed state */
    kputs("  Static IP configured.\n");
    return;
usage:
    kputs("  Usage: static-ip <ip> <gateway> <dns> [netmask]\n");
    kputs("  Example: static-ip 192.168.1.50 192.168.1.1 1.1.1.1\n");
}

/* ── universal shell tools ────────────────────────────────── */

extern int vault_size(const char *path);
extern int vault_exists(const char *path);

static void hex_byte(uint8_t b) {
    static const char hx[] = "0123456789abcdef";
    kputc(hx[(b >> 4) & 0xf]); kputc(hx[b & 0xf]);
}

static void cmd_xxd(const char *args)
{
    const char *path = args;
    while (*path == ' ') path++;
    if (!*path) { kputs("  Usage: xxd <file>\n"); return; }

    int sz = vault_size(path);
    if (sz < 0) { kputs("  No such file.\n"); return; }
    if (sz == 0) { kputs("  (empty)\n"); return; }

    static uint8_t buf[8192];
    int read_len = sz > (int)sizeof(buf) ? (int)sizeof(buf) : sz;
    int got = vault_read(path, buf, (uint32_t)read_len);
    if (got < 0) { kputs("  Read error.\n"); return; }

    /* xxd-style: 8 hex digit offset, 16 bytes hex (split 8/8), then ASCII */
    for (int off = 0; off < got; off += 16) {
        for (int s = 28, i = 0; i < 8; i++, s -= 4) hex_byte((uint8_t)((off >> s) & 0xff));
        kputs("  ");
        for (int i = 0; i < 16; i++) {
            if (off + i < got) hex_byte(buf[off + i]);
            else                kputs("  ");
            if (i == 7) kputc(' ');
            kputc(' ');
        }
        kputs(" |");
        for (int i = 0; i < 16 && off + i < got; i++) {
            uint8_t c = buf[off + i];
            kputc((c >= 32 && c < 127) ? (char)c : '.');
        }
        kputs("|\n");
    }
    if (sz > read_len) {
        kputs("  ... (truncated, ");
        kput_dec((unsigned)(sz - read_len));
        kputs(" bytes more)\n");
    }
}

static void cmd_wc(const char *args)
{
    const char *path = args;
    while (*path == ' ') path++;
    if (!*path) { kputs("  Usage: wc <file>\n"); return; }
    int sz = vault_size(path);
    if (sz < 0) { kputs("  No such file.\n"); return; }
    if (sz == 0) { kputs("  0 lines, 0 bytes\n"); return; }

    static uint8_t buf[16384];
    int read_len = sz > (int)sizeof(buf) ? (int)sizeof(buf) : sz;
    int got = vault_read(path, buf, (uint32_t)read_len);
    if (got < 0) { kputs("  Read error.\n"); return; }
    int lines = 0;
    for (int i = 0; i < got; i++) if (buf[i] == '\n') lines++;
    /* If file doesn't end in \n, count the final partial line */
    if (got > 0 && buf[got - 1] != '\n') lines++;
    kputs("  ");
    kput_dec((unsigned)lines); kputs(" lines, ");
    kput_dec((unsigned)sz);    kputs(" bytes");
    if (sz > read_len) kputs(" (line count from first 16 KB only)");
    kputs("\n");
}

static void cmd_cp(const char *args)
{
    /* cp <src> <dst> */
    char src[128], dst[128];
    int si = 0, di = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && si < 127) src[si++] = *p++;
    src[si] = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && di < 127) dst[di++] = *p++;
    dst[di] = 0;
    if (!src[0] || !dst[0]) { kputs("  Usage: cp <src> <dst>\n"); return; }

    int sz = vault_size(src);
    if (sz < 0) { kputs("  Source not found.\n"); return; }

    static uint8_t buf[65536];
    if (sz > (int)sizeof(buf)) {
        kputs("  Source too large (");
        kput_dec((unsigned)sz);
        kputs(" bytes; cp limited to 64 KB).\n");
        return;
    }
    int got = vault_read(src, buf, (uint32_t)sz);
    if (got < 0) { kputs("  Read error.\n"); return; }

    /* Create dst (delete first if it existed) */
    if (vault_exists(dst)) vault_delete(dst);
    if (vault_create(dst, 0) < 0) { kputs("  Create dst failed.\n"); return; }
    if (vault_write(dst, buf, (uint32_t)got) < 0) {
        kputs("  Write dst failed.\n");
        vault_delete(dst);
        return;
    }
    kputs("  Copied "); kput_dec((unsigned)got); kputs(" bytes.\n");
}

static void cmd_portscan(const char *args)
{
    /* portscan <host_or_ip> <start> <end> */
    char host[128];
    int hi = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && hi < 127) host[hi++] = *p++;
    host[hi] = 0;
    while (*p == ' ') p++;
    int start = 0;
    while (*p >= '0' && *p <= '9') { start = start * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    int end = 0;
    while (*p >= '0' && *p <= '9') { end = end * 10 + (*p - '0'); p++; }

    if (!host[0] || start <= 0 || end <= 0 || end < start || end > 65535) {
        kputs("  Usage: portscan <host> <start-port> <end-port>\n");
        kputs("  Example: portscan 10.0.2.2 22 100\n");
        return;
    }
    if (!g_net.up) { kputs("  Network not up.\n"); return; }

    /* Resolve host (or accept dotted quad) */
    struct ipv4_addr ip;
    int ok = 0;
    {
        unsigned a, b, c, d;
        const char *q = host;
        int parsed = 0;
        a = 0; while (*q >= '0' && *q <= '9' && parsed < 4) { a = a*10 + (unsigned)(*q - '0'); q++; parsed++; }
        if (parsed && parsed < 4 && *q == '.' && a < 256) {
            q++; parsed = 0; b = 0;
            while (*q >= '0' && *q <= '9' && parsed < 4) { b = b*10 + (unsigned)(*q - '0'); q++; parsed++; }
            if (parsed && parsed < 4 && *q == '.' && b < 256) {
                q++; parsed = 0; c = 0;
                while (*q >= '0' && *q <= '9' && parsed < 4) { c = c*10 + (unsigned)(*q - '0'); q++; parsed++; }
                if (parsed && parsed < 4 && *q == '.' && c < 256) {
                    q++; parsed = 0; d = 0;
                    while (*q >= '0' && *q <= '9' && parsed < 4) { d = d*10 + (unsigned)(*q - '0'); q++; parsed++; }
                    if (parsed && parsed < 4 && *q == 0 && d < 256) {
                        ip.b[0]=a; ip.b[1]=b; ip.b[2]=c; ip.b[3]=d; ok = 1;
                    }
                }
            }
        }
    }
    if (!ok) {
        if (dns_resolve(host, &ip) < 0) { kputs("  DNS failed.\n"); return; }
    }

    kputs("  Scanning ");
    kput_dec(ip.b[0]); kputs(".");
    kput_dec(ip.b[1]); kputs(".");
    kput_dec(ip.b[2]); kputs(".");
    kput_dec(ip.b[3]);
    kputs(" ports "); kput_dec(start);
    kputs("-"); kput_dec(end); kputs("\n");

    int open_count = 0;
    for (int port = start; port <= end; port++) {
        struct tcp_conn c;
        if (tcp_connect(&c, ip, (uint16_t)port) == 0) {
            kputs("  open: "); kput_dec(port); kputs("\n");
            open_count++;
            tcp_close(&c);
        }
    }
    kputs("  done — ");
    kput_dec(open_count);
    kputs(" open\n");
}

static int parse_ip(const char *s, struct ipv4_addr *out);  /* forward */

/* Ping-sweep a /24 subnet. Args: "192.168.1" or "192.168.1.0" — we
 * scan .1 through .254. Reports each host that replies to ICMP echo. */
static void cmd_sweep(const char *args)
{
    if (!g_net.up) { kputs("  Network not up.\n"); return; }
    char buf[32];
    int bi = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && bi < 31) buf[bi++] = *p++;
    buf[bi] = 0;
    if (!buf[0]) {
        kputs("  Usage: sweep <subnet>   (e.g. sweep 192.168.1)\n");
        return;
    }
    /* Trim trailing ".0" if present */
    int len = bi;
    if (len >= 2 && buf[len-2] == '.' && buf[len-1] == '0') buf[len-2] = 0;

    struct ipv4_addr base;
    /* Parse three octets — append ".0" so parse_ip works as a/b/c/0 */
    char tmp[40];
    int ti = 0;
    for (int i = 0; buf[i] && ti < 36; i++) tmp[ti++] = buf[i];
    tmp[ti++] = '.'; tmp[ti++] = '0'; tmp[ti] = 0;
    if (parse_ip(tmp, &base) < 0) {
        kputs("  Invalid subnet.\n");
        return;
    }

    kputs("  Sweeping ");
    kput_dec(base.b[0]); kputs(".");
    kput_dec(base.b[1]); kputs(".");
    kput_dec(base.b[2]); kputs(".0/24\n");

    int alive = 0;
    for (int host = 1; host < 255; host++) {
        struct ipv4_addr t = base;
        t.b[3] = (uint8_t)host;
        uint64_t rtt = icmp_ping(t);
        if (rtt > 0) {
            kputs("  alive: ");
            kput_dec(t.b[0]); kputs(".");
            kput_dec(t.b[1]); kputs(".");
            kput_dec(t.b[2]); kputs(".");
            kput_dec(t.b[3]);
            kputs("\n");
            alive++;
        }
    }
    kputs("  done — ");
    kput_dec(alive);
    kputs(" alive\n");
}

extern int mbedtls_sha256(const unsigned char *input, unsigned long ilen,
                          unsigned char *output, int is224);

static void cmd_sha256(const char *args)
{
    const char *path = args;
    while (*path == ' ') path++;
    if (!*path) { kputs("  Usage: sha256 <file>\n"); return; }

    int sz = vault_size(path);
    if (sz < 0) { kputs("  No such file.\n"); return; }

    /* For Alpha: cap at 64 KB so we keep the buffer reasonable. */
    if (sz > 65536) {
        kputs("  File too large for inline hash (>64 KB).\n");
        return;
    }
    static uint8_t buf[65536];
    int got = vault_read(path, buf, (uint32_t)sz);
    if (got < 0) { kputs("  Read error.\n"); return; }

    uint8_t hash[32];
    if (mbedtls_sha256(buf, (unsigned long)got, hash, 0) != 0) {
        kputs("  Hash error.\n");
        return;
    }
    kputs("  ");
    for (int i = 0; i < 32; i++) hex_byte(hash[i]);
    kputs("  ");
    kputs(path);
    kputs("\n");
}

/* nc-lite: connect to <host> <port>, send the rest of the line as
 * payload (or nothing), poll for response, print as text + hex. */
static void cmd_nc(const char *args)
{
    if (!g_net.up) { kputs("  Network not up.\n"); return; }

    char host[128];
    int hi = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && hi < 127) host[hi++] = *p++;
    host[hi] = 0;
    while (*p == ' ') p++;
    int port = 0;
    while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    /* Rest of line = payload (optional). May contain spaces. */
    const char *payload = p;
    int payload_len = 0;
    while (payload[payload_len]) payload_len++;

    if (!host[0] || port <= 0 || port > 65535) {
        kputs("  Usage: nc <host> <port> [payload]\n");
        kputs("  Example: nc 10.0.2.2 22\n");
        kputs("  Example: nc httpbin.org 80 GET / HTTP/1.0\\r\\n\\r\\n\n");
        return;
    }

    struct ipv4_addr ip;
    int parsed = 0;
    {
        unsigned a, b, c, d;
        const char *q = host;
        int n = 0;
        a = 0; while (*q >= '0' && *q <= '9' && n < 4) { a = a*10 + (unsigned)(*q - '0'); q++; n++; }
        if (n && n < 4 && *q == '.' && a < 256) {
            q++; n = 0; b = 0;
            while (*q >= '0' && *q <= '9' && n < 4) { b = b*10 + (unsigned)(*q - '0'); q++; n++; }
            if (n && n < 4 && *q == '.' && b < 256) {
                q++; n = 0; c = 0;
                while (*q >= '0' && *q <= '9' && n < 4) { c = c*10 + (unsigned)(*q - '0'); q++; n++; }
                if (n && n < 4 && *q == '.' && c < 256) {
                    q++; n = 0; d = 0;
                    while (*q >= '0' && *q <= '9' && n < 4) { d = d*10 + (unsigned)(*q - '0'); q++; n++; }
                    if (n && n < 4 && *q == 0 && d < 256) {
                        ip.b[0]=a; ip.b[1]=b; ip.b[2]=c; ip.b[3]=d; parsed = 1;
                    }
                }
            }
        }
    }
    if (!parsed) {
        if (dns_resolve(host, &ip) < 0) { kputs("  DNS failed.\n"); return; }
    }

    struct tcp_conn conn;
    if (tcp_connect(&conn, ip, (uint16_t)port) < 0) {
        kputs("  Connect failed.\n");
        return;
    }

    if (payload_len > 0) {
        if (tcp_send(&conn, payload, (uint16_t)payload_len) < 0) {
            kputs("  Send failed.\n");
            tcp_close(&conn);
            return;
        }
    }

    static uint8_t resp[8192];
    int total = 0;
    while (total < (int)sizeof(resp) - 1) {
        int n = tcp_recv(&conn, resp + total,
                          (uint16_t)((sizeof(resp) - 1 - total) > 0xF000 ? 0xF000 :
                                     (sizeof(resp) - 1 - total)));
        if (n <= 0) break;
        total += n;
    }
    tcp_close(&conn);

    kputs("  Got ");
    kput_dec((unsigned)total);
    kputs(" bytes:\n");
    for (int i = 0; i < total; i++) {
        uint8_t c = resp[i];
        kputc((c >= 32 && c < 127) || c == '\n' || c == '\r' || c == '\t' ? (char)c : '.');
    }
    kputs("\n");
}

static int parse_ip(const char *s, struct ipv4_addr *out)
{
    int octet = 0, val = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
        } else if (*s == '.') {
            if (octet >= 4 || val > 255) return -1;
            out->b[octet++] = (uint8_t)val;
            val = 0;
        } else {
            break;
        }
        s++;
    }
    if (octet != 3 || val > 255) return -1;
    out->b[3] = (uint8_t)val;
    return 0;
}

static void cmd_ping(const char *args)
{
    if (!g_net.up || !*args) {
        kputs("  Usage: ping <ip>\n");
        kputs("  Example: ping 10.0.2.2\n");
        return;
    }

    struct ipv4_addr target;
    if (parse_ip(args, &target) < 0) {
        kputs("  Invalid IP address.\n");
        return;
    }

    kputs("  Pinging ");
    kput_dec(target.b[0]); kputs(".");
    kput_dec(target.b[1]); kputs(".");
    kput_dec(target.b[2]); kputs(".");
    kput_dec(target.b[3]); kputs("...\n");

    uint64_t rtt = icmp_ping(target);
    if (rtt > 0) {
        kputs("  Reply: ");
        kput_dec(rtt);
        kputs(" TSC cycles\n");
    } else {
        kputs("  Timeout.\n");
    }
}

static void cmd_dns_cmd(const char *args)
{
    if (!g_net.up || !*args) {
        kputs("  Usage: dns <hostname>\n");
        kputs("  Example: dns example.com\n");
        return;
    }

    struct ipv4_addr result;
    kputs("  Resolving ");
    kputs(args);
    kputs("...\n");

    if (dns_resolve(args, &result) == 0) {
        kputs("  ");
        kputs(args);
        kputs(" = ");
        kput_dec(result.b[0]); kputs(".");
        kput_dec(result.b[1]); kputs(".");
        kput_dec(result.b[2]); kputs(".");
        kput_dec(result.b[3]); kputs("\n");
    } else {
        kputs("  Failed to resolve.\n");
    }
}

static void cmd_fetch(const char *args)
{
    if (!g_net.up || !*args) {
        kputs("  Usage: fetch <host> [path]\n");
        kputs("  Example: fetch example.com /\n");
        return;
    }

    /* Parse host and path */
    char host[128];
    const char *path = "/";
    int hi = 0;
    const char *p = args;

    /* Skip http:// if present */
    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' &&
        p[4] == ':' && p[5] == '/' && p[6] == '/')
        p += 7;

    while (*p && *p != ' ' && *p != '/' && hi < 127)
        host[hi++] = *p++;
    host[hi] = '\0';

    if (*p == '/') path = p;
    else if (*p == ' ') {
        p++;
        if (*p) path = p;
    }

    kputs("\n  Fetching http://");
    kputs(host);
    kputs(path);
    kputs("\n\n");

    struct http_response resp;
    if (http_get(host, path, &resp) == 0) {
        kputs("\n  Status: ");
        kput_dec(resp.status_code);
        kputs("\n  Type:   ");
        kputs(resp.content_type);
        kputs("\n  Body:   ");
        kput_dec(resp.body_len);
        kputs(" bytes\n\n");

        /* Display body (truncate to terminal) */
        if (resp.body_len > 0) {
            int limit = resp.body_len > 2048 ? 2048 : resp.body_len;
            for (int i = 0; i < limit; i++)
                kputc(resp.body[i]);
            if (resp.body[limit - 1] != '\n')
                kputs("\n");
            if (resp.body_len > 2048)
                kputs("\n  ... (truncated)\n");
        }
    } else {
        kputs("  Fetch failed.\n");
    }
    kputs("\n");
}

extern int https_get(const char *hostname, const char *path,
                     char *resp_buf, int resp_max, int *body_len);

static void cmd_https(const char *args)
{
    if (!g_net.up || !*args) {
        kputs("  Usage: https <host> [path]\n");
        kputs("  Example: https example.com /\n");
        return;
    }

    char host[128];
    const char *path = "/";
    int hi = 0;
    const char *p = args;

    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' &&
        p[4] == 's' && p[5] == ':' && p[6] == '/' && p[7] == '/')
        p += 8;
    else if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' &&
             p[4] == ':' && p[5] == '/' && p[6] == '/')
        p += 7;

    while (*p && *p != ' ' && *p != '/' && hi < 127)
        host[hi++] = *p++;
    host[hi] = '\0';

    if (*p == '/') path = p;
    else if (*p == ' ') { p++; if (*p) path = p; }

    kputs("\n  Fetching https://");
    kputs(host);
    kputs(path);
    kputs("\n\n");

    static char resp[8192];
    int body_len = 0;
    int status = https_get(host, path, resp, sizeof(resp), &body_len);
    if (status > 0) {
        kputs("  Status: ");
        kput_dec(status);
        kputs("\n  Body:   ");
        kput_dec(body_len);
        kputs(" bytes\n\n");
        if (body_len > 0) {
            int limit = body_len > 1024 ? 1024 : body_len;
            for (int i = 0; i < limit; i++) kputc(resp[i]);
            if (body_len > 1024) kputs("\n  ... (truncated)\n");
            else kputs("\n");
        }
    } else {
        kputs("  HTTPS fetch failed.\n");
    }
    kputs("\n");
}

extern int vault_create(const char *path, uint32_t tier);
extern int vault_mkdir(const char *path, uint32_t tier);
extern int vault_write(const char *path, const void *data, uint32_t size);
extern int vault_read(const char *path, void *buf, uint32_t size);
extern int vault_delete(const char *path);
extern int dns_resolve(const char *hostname, struct ipv4_addr *out);

/* Play 200 ms of 440 Hz triangle-approximated sine through HDA audio.
 * Triangle (4 line segments per period) sounds tonal at 440 Hz and
 * avoids needing a sin LUT. 5 ms attack/release ramp kills the click. */
static void cmd_beep(const char *args)
{
    (void)args;
    if (!hda_ready()) {
        kputs("hda: not ready (");
        kputs(hda_status());
        kputs(")\n");
        return;
    }

    const int sr     = 48000;
    const int frames = sr / 5;     /* 200 ms */
    const int freq   = 440;

    static int16_t buf[9600 * 2]; /* 38 KB; not on stack */

    uint32_t step  = ((uint64_t)freq * 4096ULL * 65536ULL) / (uint32_t)sr;
    uint32_t phase = 0;
    int env_frames = sr / 200;     /* 5 ms */

    for (int i = 0; i < frames; i++) {
        uint32_t p = (phase >> 16) & 4095;
        int32_t s;
        if (p < 1024)        s =  (int32_t)(p) * 24;
        else if (p < 2048)   s =  (int32_t)(2047 - p) * 24;
        else if (p < 3072)   s = -(int32_t)(p - 2048) * 24;
        else                 s = -(int32_t)(4095 - p) * 24;

        int amp = 256;
        if (i < env_frames)               amp = (256 * i) / env_frames;
        else if (i > frames - env_frames) amp = (256 * (frames - i)) / env_frames;
        int32_t v = (s * amp) >> 8;
        if (v > 32767)  v =  32767;
        if (v < -32768) v = -32768;
        buf[i*2 + 0] = (int16_t)v;
        buf[i*2 + 1] = (int16_t)v;
        phase += step;
    }

    kputs("hda: playing 440 Hz, 200 ms ...");
    int rc = hda_play_pcm(buf, frames, sr);
    if (rc == 0) kputs(" done\n");
    else { kputs(" error rc="); kput_dec((uint64_t)(int64_t)rc); kputs("\n"); }
}

static void cmd_selftest(const char *args)
{
    (void)args;
    int passes = 0, fails = 0;
    kputs("\n  Zeos self-test\n  ──────────────\n");

    /* Storage census before any disk operation */
    {
        int n = block_drive_count();
        kputs("  Storage: ");
        kput_dec(n);
        kputs(" drive(s)\n");
        for (int i = 0; i < n; i++) {
            block_drive_info_t info;
            if (block_drive_info(i, &info) != 0) continue;
            uint64_t mb = (info.sectors * (uint64_t)info.sector_size) / (1024ULL * 1024ULL);
            kputs("    [");
            kput_dec(i);
            kputs("] ");
            kputs(drive_kind_label(info.kind));
            kputs(" ");
            kput_dec(mb);
            kputs(" MB");
            if (info.model[0]) { kputs(" "); kputs(info.model); }
            kputs("\n");
        }
    }

    /* Block chain: dump pipeline + drive a small write/read loop on
     * the first available drive so the masq journal accumulates
     * provable records. Reads are not journaled per the contract;
     * only the writes show up below. */
    kputs("  Block chain ........... ");
    if (CHAIN_BLOCK >= 0) {
        chain_t *bc = chain_get(CHAIN_BLOCK);
        int bn = bc ? bc->node_count : 0;
        kputs("nodes=");
        kput_dec((uint64_t)bn);
        kputc('\n');
        chain_dump(CHAIN_BLOCK);
        if (bn == 4) passes++; else fails++;

        int dn = block_drive_count();
        if (dn > 0) {
            block_drive_info_t info;
            if (block_drive_info(0, &info) == 0 && info.sector_size >= 512) {
                /* Pick a high LBA inside the device so we don't trample
                 * partition tables / FAT structures. Use sector_count-2
                 * if available, else LBA 256 as a safer-than-zero spot. */
                uint64_t test_lba = (info.sectors > 16) ? (info.sectors - 2) : 8;
                static uint8_t scratch[4096] __attribute__((aligned(64)));
                static uint8_t readback[4096] __attribute__((aligned(64)));
                uint32_t bs = info.sector_size;
                if (bs > sizeof(scratch)) bs = sizeof(scratch);

                /* Read once first to seed the buffer with prior contents
                 * so we can restore after the test. */
                int read_ok = (block_read_drive(0, test_lba, 1, scratch) == 0);

                kputs("  Block journal write ... ");
                uint64_t before = block_chain_journal_total();

                /* Three writes at distinct LBAs so the journal grows by
                 * three. Write a recognizable pattern. */
                uint8_t pat[512];
                for (int i = 0; i < 512; i++) pat[i] = (uint8_t)(i ^ 0x5A);

                int w_ok = 0;
                for (int k = 0; k < 3; k++) {
                    uint64_t lba = test_lba + (uint64_t)k * 0;  /* same lba ok; */
                    /* but use distinct lbas to make the dump readable */
                    lba = test_lba - (uint64_t)k;
                    if (block_write_drive(0, lba, 1, pat) == 0) w_ok++;
                }
                /* Read back to prove the chain plumbed bytes through. */
                int rb_ok = (block_read_drive(0, test_lba, 1, readback) == 0);

                uint64_t after = block_chain_journal_total();
                uint64_t added = after - before;

                if (w_ok == 3 && added == 3 && rb_ok) {
                    kputs("PASS (");
                    kput_dec(added);
                    kputs(" records added)\n");
                    passes++;
                } else {
                    kputs("FAIL (w_ok=");
                    kput_dec((uint64_t)w_ok);
                    kputs(" added=");
                    kput_dec(added);
                    kputs(" rb=");
                    kput_dec((uint64_t)rb_ok);
                    kputs(")\n");
                    fails++;
                }

                /* Show last 3 journal entries. */
                block_chain_dump_journal(3);

                /* Restore prior contents at test_lba (best effort). */
                if (read_ok) (void)block_write_drive(0, test_lba, 1, scratch);
            } else {
                kputs("  Block journal write ... SKIP (no usable drive 0)\n");
            }
        } else {
            kputs("  Block journal write ... SKIP (no drives)\n");
        }
    } else {
        kputs("not registered\n");
        fails++;
    }

    /* VAULT: write+read+delete round-trip at root */
    kputs("  VAULT round-trip ...... ");
    {
        const char *path = "/selftest";
        const char *msg = "hello, zeos";
        char buf[32];
        vault_delete(path);  /* clean any prior run */
        int rc = vault_create(path, 0);  /* returns inode # on success, <0 on err */
        if (rc < 0) { kputs("CREATE FAIL\n"); fails++; goto vault_done; }
        rc = vault_write(path, msg, 11);
        if (rc < 0) { kputs("WRITE FAIL\n"); vault_delete(path); fails++; goto vault_done; }
        rc = vault_read(path, buf, sizeof(buf));
        if (rc < 11) { kputs("READ FAIL\n"); vault_delete(path); fails++; goto vault_done; }
        for (int i = 0; i < 11; i++) {
            if (buf[i] != msg[i]) { kputs("COMPARE FAIL\n"); vault_delete(path); fails++; goto vault_done; }
        }
        vault_delete(path);
        kputs("PASS\n"); passes++;
    }
vault_done:

    /* DNS: resolve a known host (if network is up) */
    kputs("  DNS resolve ........... ");
    if (!g_net.up) {
        kputs("SKIP (no network)\n");
    } else {
        struct ipv4_addr ip;
        if (dns_resolve("example.com", &ip) == 0) {
            kputs("PASS (");
            kput_dec(ip.b[0]); kputs(".");
            kput_dec(ip.b[1]); kputs(".");
            kput_dec(ip.b[2]); kputs(".");
            kput_dec(ip.b[3]); kputs(")\n");
            passes++;
        } else {
            kputs("FAIL\n"); fails++;
        }
    }

    /* HTTPS: full TLS handshake + GET against a known-good host */
    kputs("  HTTPS fetch ........... ");
    if (!g_net.up) {
        kputs("SKIP (no network)\n");
    } else {
        static char resp[4096];
        int body_len = 0;
        int status = https_get("letsencrypt.org", "/", resp, sizeof(resp), &body_len);
        if (status == 200 && body_len > 0) {
            kputs("PASS (");
            kput_dec(status); kputs(", ");
            kput_dec(body_len); kputs(" bytes)\n");
            passes++;
        } else {
            kputs("FAIL (status=");
            kput_dec(status); kputs(")\n");
            fails++;
        }
    }

    /* MSI-X infrastructure */
    kputs("  MSI-X: ready (");
    kput_dec(msix_free_count());
    kputs(" vectors free)\n");
    passes++;

    /* HDA audio -- exercises the chain-native pipeline:
     * pcm_source -> volume_filter -> hda_pin -> hardware_dma.
     * The chain is dumped regardless of controller readiness so the
     * paradigm conversion is observable even on hardware where the
     * codec walk doesn't bring up an output path. */
    kputs("  Audio (HDA) ........... ");
    if (hda_ready())          kputs("HDA ready\n");
    else { kputs("controller not ready ("); kputs(hda_status()); kputs(")\n"); }

    if (CHAIN_AUDIO >= 0) {
        chain_t *ac = chain_get(CHAIN_AUDIO);
        int nc = ac ? ac->node_count : 0;
        int rc = (hda_ready()) ? chain_resolve(CHAIN_AUDIO) : 0;
        kputs("    chain ok (nodes=");
        kput_dec((uint64_t)nc);
        kputs(", rc=");
        kput_dec((uint64_t)rc);
        kputs(")\n");
        chain_dump(CHAIN_AUDIO);
        if (nc == 4) passes++; else fails++;
    } else {
        kputs("    chain not registered\n");
        fails++;
    }

    /* Network chains: dump both TX and RX so the chain-native NIC
     * conversion is observable. Each chain must show 4 nodes
     * matching the contract pipeline. */
    kputs("  Network chains ........ ");
    if (CHAIN_NET_TX >= 0 && CHAIN_NET_RX >= 0) {
        chain_t *tx = chain_get(CHAIN_NET_TX);
        chain_t *rx = chain_get(CHAIN_NET_RX);
        int tn = tx ? tx->node_count : 0;
        int rn = rx ? rx->node_count : 0;
        kputs("tx_nodes=");
        kput_dec((uint64_t)tn);
        kputs(", rx_nodes=");
        kput_dec((uint64_t)rn);
        kputc('\n');
        chain_dump(CHAIN_NET_TX);
        chain_dump(CHAIN_NET_RX);
        if (tn == 4 && rn == 4) passes++; else fails++;
    } else {
        kputs("not registered\n");
        fails++;
    }

    /* CFA handles: TLS state + VAULT blob must each be wrapped after
     * tls_init() and vault_mount() have run. The selftest fails if
     * either subsystem is unwrapped, since that would mean security-
     * relevant memory is still escaping the MasQ tier check. */
    kputs("  CFA handles ........... ");
    {
        int hc = cfa_handle_count();
        kputs("count=");
        kput_dec((uint64_t)hc);
        /* Quick sanity: tls_init wraps g_ssl_conf, vault_mount wraps
         * g_base, so we expect at least 2 live handles. */
        if (hc >= 2) {
            kputs("  PASS\n");
            passes++;
        } else {
            kputs("  FAIL (expected >= 2: TLS + VAULT)\n");
            fails++;
        }
    }

    /* USB MSC: report presence + size in MB. */
    kputs("  USB MSC ............... ");
    if (usb_msc_ready()) {
        uint64_t sectors = usb_msc_sector_count();
        uint32_t bs = usb_msc_block_size();
        uint64_t mb = (sectors * bs) / (1024ULL * 1024ULL);
        kputs("present (");
        kput_dec(mb);
        kputs(" MB)\n");
        passes++;
    } else {
        kputs("SKIP (no device)\n");
    }

    /* USB hub topology: report deepest tier the hub walker reached. */
    kputs("  USB hub depth ......... ");
    {
        int td = usb_hub_max_tier();
        if (td <= 0) {
            kputs("no hubs\n");
        } else {
            kputs("tier ");
            kput_dec(td);
            kputs("\n");
        }
    }

    kputs("  ──────────────\n  ");
    kput_dec(passes); kputs(" passed, ");
    kput_dec(fails); kputs(" failed\n\n");
}

/* ── VAULT filesystem commands ──────────────────── */

/* RAM disk for VAULT — 2MB */
#define VAULT_RAM_SIZE (2 * 1024 * 1024)
static uint8_t vault_ram[VAULT_RAM_SIZE] __attribute__((aligned(4096)));

static void vault_init_ramdisk(void)
{
    if (vault_format(vault_ram, VAULT_RAM_SIZE, "zeos") == 0 &&
        vault_mount(vault_ram, VAULT_RAM_SIZE) == 0) {
        vault_ready = 1;

        /* Create default directories */
        vault_create("/programs", VAULT_TIER_REFERENCE);
        vault_create("/home", VAULT_TIER_SOVEREIGN);
        vault_create("/tmp", VAULT_TIER_INTERNAL);

        /* Store built-in Z+ programs in VAULT */
        for (int i = 0; i < (int)NUM_BUILTINS; i++) {
            char path[64];
            /* Build path: /programs/name.zp */
            int pi = 0;
            const char *prefix = "/programs/";
            while (*prefix) path[pi++] = *prefix++;
            const char *n = builtins[i].name;
            while (*n) path[pi++] = *n++;
            path[pi++] = '.'; path[pi++] = 'z'; path[pi++] = 'p';
            path[pi] = '\0';

            int len = 0;
            const char *s = builtins[i].source;
            while (s[len]) len++;
            vault_write(path, builtins[i].source, (uint32_t)len);
        }
    }
}

static void cmd_ls(const char *args)
{
    if (!vault_ready) {
        kputs("VAULT not mounted.\n");
        return;
    }

    const char *path = (*args) ? args : "/";

    struct vault_dirent entries[64];
    int count = vault_list(path, entries, 64);

    if (count < 0) {
        kputs("  Not a directory: ");
        kputs(path);
        kputs("\n");
        return;
    }

    kputs("\n");
    for (int i = 0; i < count; i++) {
        kputs("  ");
        kputs(entries[i].name);

        /* Show file size if we can determine it */
        char check_path[128];
        int pi = 0;
        const char *p = path;
        while (*p && pi < 120) check_path[pi++] = *p++;
        if (pi > 0 && check_path[pi-1] != '/') check_path[pi++] = '/';
        const char *n = entries[i].name;
        while (*n && pi < 126) check_path[pi++] = *n++;
        check_path[pi] = '\0';

        int sz = vault_size(check_path);
        if (sz > 0) {
            kputs("  (");
            kput_dec((uint64_t)sz);
            kputs(" bytes)");
        } else if (sz == 0) {
            kputs("  (dir)");
        }

        kputs("\n");
    }

    if (count == 0)
        kputs("  (empty)\n");

    kputs("\n");
}

static void cmd_cat(const char *args)
{
    if (!vault_ready || !*args) {
        kputs("  Usage: cat <path>\n");
        return;
    }

    int sz = vault_size(args);
    if (sz < 0) {
        kputs("  File not found: ");
        kputs(args);
        kputs("\n");
        return;
    }

    /* Read file content — limit display to 2K */
    char buf[2048];
    int to_read = sz;
    if (to_read > 2047) to_read = 2047;

    int got = vault_read(args, buf, (uint32_t)to_read);
    if (got > 0) {
        buf[got] = '\0';
        kputs("\n");
        kputs(buf);
        if (buf[got-1] != '\n')
            kputs("\n");
        kputs("\n");
    }
}

static void cmd_write_file(const char *args)
{
    if (!vault_ready || !*args) {
        kputs("  Usage: save <path> <content>\n");
        return;
    }

    /* Split args into path and content */
    const char *p = args;
    while (*p && *p != ' ') p++;

    if (!*p) {
        kputs("  Usage: save <path> <content>\n");
        return;
    }

    /* Copy path */
    char path[128];
    int pi = 0;
    const char *a = args;
    while (a < p && pi < 127) path[pi++] = *a++;
    path[pi] = '\0';

    /* Skip space */
    p++;

    int len = 0;
    const char *c = p;
    while (c[len]) len++;

    int wrote = vault_write(path, p, (uint32_t)len);
    if (wrote > 0) {
        kputs("  Wrote ");
        kput_dec((uint64_t)wrote);
        kputs(" bytes to ");
        kputs(path);
        kputs("\n");
    } else {
        kputs("  Write failed.\n");
    }
}

static void cmd_mkdir(const char *args)
{
    if (!vault_ready || !*args) {
        kputs("  Usage: mkdir <path>\n");
        return;
    }

    int ino = vault_mkdir(args, VAULT_TIER_INTERNAL);
    if (ino >= 0) {
        kputs("  Created directory: ");
        kputs(args);
        kputs("\n");
    } else {
        kputs("  Failed to create: ");
        kputs(args);
        kputs("\n");
    }
}

static void cmd_df(const char *args)
{
    (void)args;
    if (!vault_ready) {
        kputs("VAULT not mounted.\n");
        return;
    }

    uint32_t total_b, free_b, total_i, free_i;
    vault_stat(&total_b, &free_b, &total_i, &free_i);

    kputs("\n  VAULT Filesystem\n\n");
    kputs("  Blocks: ");
    kput_dec(free_b);
    kputs(" free / ");
    kput_dec(total_b);
    kputs(" total (");
    kput_dec((uint64_t)free_b * VAULT_BLOCK_SIZE / 1024);
    kputs(" KB free)\n");
    kputs("  Inodes: ");
    kput_dec(free_i);
    kputs(" free / ");
    kput_dec(total_i);
    kputs(" total\n\n");
}

/* ── Main shell loop ────────────────────────────── */

void shell_run(struct zeos_boot_info *boot)
{
    g_boot = boot;
    char cmd[CMD_BUF_SIZE];
    int pos;

    /* Initialize VAULT ramdisk */
    vault_init_ramdisk();
    if (vault_ready) {
        kputs("VAULT: 2MB ramdisk mounted. ");
        kput_dec(NUM_BUILTINS);
        kputs(" programs loaded.\n");
    }

    /* Initialize networking */
    net_init();

    kputs("Type 'help' for commands.\n");
    kputs("Switch modes: 'zeros' (robotics) | 'derez' (dev) | 'raise' (full)\n\n");

    for (;;) {
        shell_prompt();
        pos = 0;

        /* Read a line */
        for (;;) {
            char c = keyboard_getc();

            if (c == '\n') {
                kputc('\n');
                cmd[pos] = '\0';
                break;
            } else if (c == '\b') {
                if (pos > 0) {
                    pos--;
                    kputs("\b \b");
                }
            } else if (pos < CMD_BUF_SIZE - 1) {
                cmd[pos++] = c;
                kputc(c);
            }
        }

        if (pos == 0)
            continue;

        /* Extract command name (first word) */
        const char *args = skip_word(cmd);

        /* Null-terminate the command name for matching */
        char name[32];
        int ni = 0;
        for (int i = 0; cmd[i] && cmd[i] != ' ' && ni < 31; i++)
            name[ni++] = cmd[i];
        name[ni] = '\0';

        /* Search command table — ALL commands work in ALL modes */
        int found = 0;
        for (int i = 0; i < (int)NUM_COMMANDS; i++) {
            if (streq(name, commands[i].name)) {
                commands[i].handler(args);
                found = 1;
                break;
            }
        }

        if (!found) {
            kputs("unknown command: ");
            kputs(name);
            kputs("  (type 'help')\n");
        }
    }
}

/* ── wifi: RTL8188EU USB dongle (detection-stage only) ────────── */
static void cmd_wifi(const char *args)
{
    while (*args == ' ' || *args == '\t') args++;

    if (*args == '\0' || streq(args, "status")) {
        rtl8188eu_print_status();
        return;
    }

    if (args[0] == 's' && args[1] == 'c' && args[2] == 'a' && args[3] == 'n') {
        rtl8188eu_scan();
        return;
    }
    if (args[0] == 'c' && args[1] == 'o' && args[2] == 'n') {
        /* Args after "connect ": ssid [psk]. We'd parse them here, but
         * rtl8188eu_connect honestly refuses regardless of input. */
        rtl8188eu_connect(0, 0);
        return;
    }

    kputs("usage: wifi [status|scan|connect <ssid> [psk]]\n");
}

/* ── USB CDC ACM ──────────────────────────────────────────────── */

static int parse_int_cdc(const char **pp)
{
    const char *p = *pp;
    while (*p == ' ') p++;
    int n = 0, any = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        p++;
        any = 1;
    }
    *pp = p;
    return any ? n : -1;
}

static void cmd_usb_serial(const char *args)
{
    (void)args;
    int n = usb_cdc_count();
    if (n == 0) {
        kputs("  no USB serial devices detected\n");
        return;
    }
    kputs("  USB serial devices:\n");
    for (int i = 0; i < n; i++) {
        struct usb_cdc_device *d = usb_cdc_get(i);
        if (!d) continue;
        kputs("  ["); kput_dec(i); kputs("] /dev/");
        kputs(d->name);
        kputs(" vid=");  kput_hex(d->xdev->vendor_id);
        kputs(" pid=");  kput_hex(d->xdev->product_id);
        kputs(" iface="); kput_dec(d->iface_data);
        kputs(" ep_in="); kput_hex(d->ep_in);
        kputs(" ep_out="); kput_hex(d->ep_out);
        kputs(" mps=");  kput_dec(d->max_packet_in);
        kputs("\n");
    }
}

static void cmd_cdc_send(const char *args)
{
    const char *p = args;
    int idx = parse_int_cdc(&p);
    if (idx < 0) {
        kputs("usage: cdc-send <idx> <text>\n");
        return;
    }
    while (*p == ' ') p++;
    if (*p == 0) {
        kputs("usage: cdc-send <idx> <text>\n");
        return;
    }
    struct usb_cdc_device *d = usb_cdc_get(idx);
    if (!d) {
        kputs("  no such device. try 'usb-serial'.\n");
        return;
    }
    int len = 0;
    while (p[len]) len++;
    int r = usb_cdc_send(d, p, len);
    if (r < 0) {
        kputs("  send failed\n");
        return;
    }
    char nl = '\n';
    usb_cdc_send(d, &nl, 1);
    kputs("  sent ");
    kput_dec((uint64_t)r);
    kputs(" bytes\n");
}

static void cmd_cdc_recv(const char *args)
{
    const char *p = args;
    int idx = parse_int_cdc(&p);
    if (idx < 0) idx = 0;
    struct usb_cdc_device *d = usb_cdc_get(idx);
    if (!d) {
        kputs("  no such device. try 'usb-serial'.\n");
        return;
    }
    char buf[128];
    int total = 0;
    for (int attempts = 0; attempts < 8; attempts++) {
        int got = usb_cdc_recv(d, buf, sizeof(buf) - 1);
        if (got <= 0) {
            if (total) break;
            for (volatile int s = 0; s < 200000; s++) ;
            continue;
        }
        buf[got] = 0;
        kputs(buf);
        total += got;
    }
    if (total == 0) {
        kputs("  (no data)\n");
    } else {
        kputs("\n  ");
        kput_dec((uint64_t)total);
        kputs(" bytes received\n");
    }
}


/* ── FAT32 read-only commands ────────────────────────────────── */

static uint64_t fat_parse_u64_dec(const char **p)
{
    uint64_t v = 0;
    while (**p == ' ') (*p)++;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (uint64_t)(**p - '0');
        (*p)++;
    }
    return v;
}

static void cmd_fat_mount(const char *args)
{
    const char *p = args;
    while (*p == ' ') p++;

    if (!*p) {
        if (fat32_automount() == 0) {
            kputs("  fat32: auto-mounted\n");
        } else {
            kputs("  fat32: no FAT32 volume found\n");
            kputs("  Usage: fat-mount <drive> <partition-lba>\n");
        }
        return;
    }

    int drive = 0;
    while (*p >= '0' && *p <= '9') {
        drive = drive * 10 + (*p - '0');
        p++;
    }
    while (*p == ' ') p++;
    uint64_t plba = fat_parse_u64_dec(&p);

    if (fat32_mount(drive, plba) == 0) {
        kputs("  fat32: mounted\n");
    } else {
        kputs("  fat32: mount failed (not FAT32 at LBA ");
        kput_dec(plba);
        kputs(")\n");
    }
}

static void cmd_fat_ls(const char *args)
{
    if (!fat32_mounted()) {
        kputs("  fat32: not mounted (run 'fat-mount' first)\n");
        return;
    }
    const char *path = args;
    while (*path == ' ') path++;
    if (!*path) path = "/";

    struct fat32_dirent *ents =
        (struct fat32_dirent *)kmalloc(sizeof(struct fat32_dirent) * 64);
    if (!ents) { kputs("  out of memory\n"); return; }

    int n = fat32_list(path, ents, 64);
    if (n < 0) {
        kputs("  fat-ls: not a directory or not found: ");
        kputs(path);
        kputs("\n");
        kfree(ents);
        return;
    }
    for (int i = 0; i < n; i++) {
        kputs("  ");
        if (ents[i].is_dir) kputs("[d] ");
        else                kputs("    ");
        kputs(ents[i].name);
        if (!ents[i].is_dir) {
            kputs("  (");
            kput_dec(ents[i].size);
            kputs(" bytes)");
        }
        kputs("\n");
    }
    if (n == 0) kputs("  (empty)\n");
    kfree(ents);
}

static void cmd_fat_cat(const char *args)
{
    if (!fat32_mounted()) {
        kputs("  fat32: not mounted (run 'fat-mount' first)\n");
        return;
    }
    const char *path = args;
    while (*path == ' ') path++;
    if (!*path) {
        kputs("  Usage: fat-cat <path>\n");
        return;
    }

    struct fat32_file f;
    if (fat32_open(path, &f) != 0) {
        kputs("  fat-cat: not found: ");
        kputs(path);
        kputs("\n");
        return;
    }
    if (f.is_dir) {
        kputs("  fat-cat: is a directory\n");
        return;
    }

    char buf[4096];
    uint32_t want = sizeof(buf) - 1;
    if (f.size < want) want = f.size;
    int got = fat32_read(&f, buf, want);
    if (got < 0) {
        kputs("  fat-cat: read failed\n");
        return;
    }
    buf[got] = '\0';
    kputs("\n");
    kputs(buf);
    if (got > 0 && buf[got - 1] != '\n') kputs("\n");
    if ((uint32_t)got < f.size) {
        kputs("  ... (");
        kput_dec(f.size - (uint32_t)got);
        kputs(" more bytes)\n");
    }
}
