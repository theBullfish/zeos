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
#include "persona.h"
#include "theme.h"
#include "kprint.h"
#include "fb.h"
#include "keyboard.h"
#include "pci.h"
#include "pmm.h"
#include "heap.h"
#include "zplus.h"
#include "sigviz.h"
#include "vault.h"
#include "net.h"
#include "net_ip.h"
#include "net_dns.h"
#include "net_http.h"
#include "timer.h"
#include "signal.h"
#include "chain.h"
#include "serial.h"

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

/* Get current persona's accent color — used by sigviz and UI */
uint32_t theme_accent(void)
{
    return persona_accents[g_persona];
}

uint32_t theme_accent_dim(void)
{
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
static void cmd_netinfo(const char *args);

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
    {"netinfo", "show network configuration",      cmd_netinfo, VIS_ALWAYS},

    /* VAULT filesystem — always visible */
    {"ls",      "list files",                      cmd_ls,      VIS_ALWAYS},
    {"cat",     "show file contents",              cmd_cat,     VIS_ALWAYS},
    {"save",    "save text to file (save path text)", cmd_write_file, VIS_DEREZ},
    {"mkdir",   "create directory",                cmd_mkdir,   VIS_DEREZ},
    {"df",      "VAULT disk usage",                cmd_df,      VIS_FULL},

    /* Full only — deep system commands */
    {"lspci",   "list PCI/PCIe devices (raw)",    cmd_lspci,   VIS_FULL},
    {"about",   "about Zeos",                     cmd_about,   VIS_FULL},
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

static void cmd_lspci(const char *args)
{
    (void)args;
    int count = pci_device_count();
    if (count == 0) {
        kputs("No PCI devices found.\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;

        /* Bus:Dev.Func */
        fb_put_hex8(d->bus);
        kputs(":");
        fb_put_hex8(d->dev);
        kputs(".");
        kputc('0' + d->func);
        kputs("  ");

        /* Vendor:Device */
        fb_put_hex16(d->vendor_id);
        kputs(":");
        fb_put_hex16(d->device_id);
        kputs("  ");

        /* Class name */
        kputs(pci_class_name(d->class_code, d->subclass));

        /* Flag known devices */
        if (d->vendor_id == 0x1da3 && d->device_id == 0x0001)
            kputs("  ** GOYA HL-1000 **");
        else if (d->vendor_id == 0x1002)
            kputs("  [AMD/ATI]");
        else if (d->vendor_id == 0x8086)
            kputs("  [Intel]");
        else if (d->vendor_id == 0x10ee)
            kputs("  [Xilinx]");
        else if (d->vendor_id == 0x15b3)
            kputs("  [Mellanox]");
        else if (d->vendor_id == 0x10de)
            kputs("  [NVIDIA]");
        else if (d->vendor_id == 0x1022)
            kputs("  [AMD]");

        kputs("\n");
    }

    kput_dec(count);
    kputs(" devices total.\n");
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
    g_persona = PERSONA_FULL;
    kputs("\n");
    persona_banner_colored();
}

static void cmd_zeros(const char *args)
{
    (void)args;
    g_persona = PERSONA_ZEROS;
    kputs("\n");
    persona_banner_colored();
}

static void cmd_derez(const char *args)
{
    (void)args;
    g_persona = PERSONA_DEREZ;
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
        kputs("\n");
        zp_list_chains();
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
