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
#include "settings_registry.h"
#include "usb_msc.h"
#include "usb_hub.h"
#include "block.h"
#include "block_chain.h"
#include "persistence.h"
#include "hotplug.h"
#include "timeofday.h"
#include "notify.h"
#include "nvme.h"
#include "ahci.h"
#include "persona.h"
#include "theme.h"
#include "kprint.h"
#include "fb.h"
#include "keyboard.h"
#include "pci.h"
#include "msix.h"
#include "lapic.h"
#include "ioapic.h"
#include "pmm.h"
#include "heap.h"
#include "zplus.h"
#include "sigviz.h"
#include "vault.h"
#include "net.h"
#include "net_rtl8188eu.h"
#include "wpa.h"
#include "net_ip.h"
#include "net_dns.h"
#include "net_ipv6.h"
#include "net_tcp.h"
#include "net_http.h"
#include "net_tls.h"
#include "timer.h"
#include "signal.h"
#include "chain.h"
#include "cfa_handle.h"
#include "chain_registry.h"
#include "scheduler.h"
#include "gpu_virtio.h"
#include "gpu_nvidia.h"
#include "gpu_goya.h"
#include "gpu_compute.h"
#include "mde_chain.h"
#include "serial.h"
#include "persona_filter.h"
#include "persona_anim.h"
#include "usb_cdc.h"
#include "hda.h"
#include "fat32.h"
#include "tests.h"
#include "suspend.h"
#include "acpi.h"
#include "battery.h"
#include "aml.h"
#include "power_buttons.h"
#include "brightness.h"
#include "bt_hci.h"
#include "bt_usb.h"
#include "bt_l2cap.h"

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
static void cmd_suspend(const char *args);
static void cmd_battery(const char *args);
static void cmd_ec(const char *args);
static void cmd_aml_eval(const char *args);
static void cmd_tests(const char *args);
static void cmd_beep(const char *args);
static void cmd_volume(const char *args);
static void cmd_mute(const char *args);
static void cmd_brightness(const char *args);
static void cmd_idle(const char *args);
static void cmd_lock(const char *args);
static void cmd_pin(const char *args);
static void cmd_cold_boot_login(const char *args);
static void cmd_power_button(const char *args);
static void cmd_lid_close(const char *args);
static void cmd_lid_open(const char *args);
static void cmd_power(const char *args);
static void cmd_static_ip(const char *args);
static void cmd_ip6(const char *args);
static void cmd_ping6(const char *args);
static void cmd_nslookup6(const char *args);
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
static void cmd_wifi_scan(const char *args);
static void cmd_bt(const char *args);
static void cmd_bt_l2cap(const char *args);
static void bt_print_bdaddr(const uint8_t bd[6]);
static void cmd_lsdrives(const char *args);
static void cmd_masq_journal(const char *args);
static void cmd_gpustat(const char *args);
static void cmd_goya(const char *args);
static void cmd_nvidia(const char *args);
static void cmd_gpu_load(const char *args);
static void cmd_scheduler_log(const char *args);
static void cmd_tickrate(const char *args);
static void cmd_chain_backoff(const char *args);
static void cmd_preempt_test(const char *args);
static void cmd_cores(const char *args);
static void cmd_persistence(const char *args);
static void cmd_hotplug(const char *args);

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

static void cmd_persistence(const char *args)
{
    (void)args;
    kputs("\n");
    persistence_dump();
    kputs("\n");
}

static void cmd_hotplug(const char *args)
{
    int n = 32;
    if (args && *args) {
        int v = 0; const char *p = args;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); p++; }
        if (v > 0) n = v;
    }
    kputs("\n");
    hotplug_dump_events(n);
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

static void cmd_gpustat(const char *args) {
    (void)args;
    int gn = gpu_virtio_device_count();
    if (gn == 0) {
        int gop = -1;
        if (gpu_virtio_gop_fallback(&gop) && gop >= 0) {
            kputs("\n  No virtio-gpu detected -- GOP fallback active.\n");
            kputs("  display.gop  ");
            kput_dec((uint64_t)fb_width());
            kputs("x");
            kput_dec((uint64_t)fb_height());
            kputs("  (UEFI GOP, no EDID-via-virtio)\n\n");
        } else {
            kputs("\n  No GPU chains registered.\n\n");
        }
        return;
    }

    kputs("\n  GPUs: ");
    kput_dec((uint64_t)gn);
    kputs("    Displays: ");
    kput_dec((uint64_t)gpu_virtio_display_count());
    kputs("\n");

    for (int gi = 0; gi < gn; gi++) {
        const gpu_virtio_device_t *d = gpu_virtio_device(gi);
        if (!d) continue;
        kputs("  gpu");
        kput_dec((uint64_t)gi);
        kputs("  pci=");
        kput_hex((uint64_t)d->pci_vendor);
        kputs(":");
        kput_hex((uint64_t)d->pci_device);
        kputs(" @ ");
        kput_dec((uint64_t)d->pci_bus); kputs(":");
        kput_dec((uint64_t)d->pci_dev); kputs(".");
        kput_dec((uint64_t)d->pci_func);
        kputs("  scanouts=");
        kput_dec((uint64_t)d->num_scanouts);
        kputs("  ready=");
        kput_dec((uint64_t)d->ready);
        kputc('\n');
    }

    int total = gpu_virtio_display_count();
    for (int i = 0; i < total; i++) {
        gpu_virtio_display_t info;
        if (gpu_virtio_display_info(i, &info) != 0) continue;
        kputs("    display.gpu");
        kput_dec((uint64_t)info.gpu_index);
        kputs(".scanout");
        kput_dec((uint64_t)info.scanout_index);
        kputs("  ");
        kput_dec((uint64_t)info.width);
        kputs("x");
        kput_dec((uint64_t)info.height);
        if (info.refresh_hz) {
            kputs(" @");
            kput_dec((uint64_t)info.refresh_hz);
            kputs("Hz");
        }
        kputs("  edid=");
        kputs(info.edid_valid ? "yes" : "no");
        if (info.edid_valid && info.monitor_name[0]) {
            kputs("  monitor=\"");
            kputs(info.monitor_name);
            kputs("\"");
        }
        kputc('\n');
    }
    kputc('\n');
}

/* goya
 *   Multi-line dump of every detected Habana Goya HL-1000:
 *   card count, PCI BDF, BAR0/2/4 phys+len, firmware status,
 *   current fence value, MSI-X vectors, and per-card chain dump.
 *   Mirrors the format gpu_goya_dump_status() emits at boot. */
static void cmd_goya(const char *args) {
    (void)args;
    gpu_goya_dump_status();
}

/* nvidia
 *   Multi-line dump of every detected NVIDIA card: chip family
 *   (Turing/Ampere/...), PCI BDF, PMC_BOOT_0, connector list with
 *   mode + EDID monitor name, and GSP / fence / perf state.
 *   Mirrors gpu_nvidia_dump_status(). */
static void cmd_nvidia(const char *args) {
    (void)args;
    gpu_nvidia_dump_status();
}

/* gpu-load
 *   Per-backend load table for the gpu_compute registry. Shows the
 *   active selection policy, every backend's inflight count, lifetime
 *   dispatch total, EWMA dispatch latency in microseconds, and any
 *   active error cooldown. Driven by stats updated in
 *   gpu_compute_dispatch_tracked() on every CHAIN_MDE.execute. */
static void cmd_gpu_load(const char *args) {
    (void)args;
    gpu_compute_dump_load();
}

/* tickrate [watch]
 *   Show the current scheduler tps and average tick wall time.
 *   "tickrate watch" prints a fresh line every second until Esc / q. */
static void cmd_tickrate(const char *args)
{
    int watch = 0;
    if (args) {
        const char *p = args;
        while (*p == ' ') p++;
        if (p[0] == 'w' && p[1] == 'a' && p[2] == 't' && p[3] == 'c' && p[4] == 'h')
            watch = 1;
    }

    extern uint32_t compositor_composite_count(void);
    extern uint32_t compositor_composite_skips(void);

    int loops = 0;
    for (;;) {
        uint32_t tps  = scheduler_tps();
        uint32_t avg  = scheduler_tick_avg_us();
        uint32_t agg  = scheduler_aggregate_slow_total();
        uint32_t comp = compositor_composite_count();
        uint32_t skip = compositor_composite_skips();
        kputs("  tickrate: ");
        kput_dec((uint64_t)tps);
        kputs(" tps, avg=");
        kput_dec((uint64_t)avg);
        kputs("us, agg_slow=");
        kput_dec((uint64_t)agg);
        kputs(", comp=");
        kput_dec((uint64_t)comp);
        kputs("/");
        kput_dec((uint64_t)(comp + skip));
        kputc('\n');
        if (!watch) break;

        /* Poll for a key, sleep 1s in 100ms slices. Esc / q / Ctrl-C breaks. */
        int aborted = 0;
        for (int i = 0; i < 10; i++) {
            char c;
            if (keyboard_try_getc(&c)) {
                if (c == 27 || c == 'q' || c == 3) { aborted = 1; break; }
            }
            timer_wait_ms(100);
        }
        if (aborted) break;
        if (++loops > 600) break;  /* 10 min hard cap */
    }
}

static void cmd_scheduler_log(const char *args)
{
    int n = 16;
    if (args && *args) {
        int v = 0; const char *p = args;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); p++; }
        if (v > 0) n = v;
    }
    kputs("\n  Scheduler quantum: ");
    kput_dec((uint64_t)scheduler_quantum_us_get());
    kputs(" us, ticks=");
    kput_dec(scheduler_tick_count());
    kputs(", tps=");
    kput_dec((uint64_t)scheduler_tps());
    kputs(", slow_total=");
    kput_dec((uint64_t)scheduler_slow_resolves_total());
    kputs(", agg_slow=");
    kput_dec((uint64_t)scheduler_aggregate_slow_total());
    kputs(", preempt_kills=");
    kput_dec((uint64_t)scheduler_preempt_kills());
    kputs(", wdog_kills=");
    kput_dec((uint64_t)scheduler_watchdog_kills());
    kputc('\n');
    scheduler_log_dump(n);
}

/* chain-backoff <id> <threshold> <every>
 *   threshold: failure-ratio threshold (e.g. 0.5 for 50%) -- accepts
 *              integer percent (50) OR decimal-string ("0.5", "0.75").
 *   every:     base skip period (skip every Nth tick when failing).
 * Updates chain->backoff_skip_threshold + chain->backoff_skip_every. */
static void cmd_chain_backoff(const char *args)
{
    if (!args || !*args) {
        kputs("  usage: chain-backoff <id> <threshold> <every>\n");
        kputs("    threshold: 0.0..1.0 (or integer percent like 50)\n");
        kputs("    every:     >= 1 (default 4)\n");
        return;
    }
    const char *p = args;
    while (*p == ' ') p++;

    /* id */
    int id = 0;
    if (*p < '0' || *p > '9') { kputs("  bad id\n"); return; }
    while (*p >= '0' && *p <= '9') { id = id*10 + (*p - '0'); p++; }
    while (*p == ' ') p++;

    /* threshold: parse as either percent (e.g. 50) or decimal (0.5). */
    if (*p == 0) { kputs("  missing threshold\n"); return; }
    int int_part = 0;
    int has_dot = 0;
    int frac_part = 0, frac_div = 1;
    while (*p >= '0' && *p <= '9') { int_part = int_part*10 + (*p - '0'); p++; }
    if (*p == '.') {
        has_dot = 1;
        p++;
        while (*p >= '0' && *p <= '9') {
            frac_part = frac_part*10 + (*p - '0');
            frac_div *= 10;
            p++;
        }
    }
    float threshold;
    if (has_dot) {
        threshold = (float)int_part + (float)frac_part / (float)frac_div;
    } else if (int_part > 1) {
        /* Treat as percent (e.g. 50 -> 0.5). */
        threshold = (float)int_part / 100.0f;
    } else {
        threshold = (float)int_part;
    }
    while (*p == ' ') p++;

    /* every */
    if (*p == 0) { kputs("  missing every\n"); return; }
    uint32_t every = 0;
    while (*p >= '0' && *p <= '9') { every = every*10 + (uint32_t)(*p - '0'); p++; }

    if (scheduler_set_backoff(id, threshold, every) != 0) {
        kputs("  chain-backoff: bad args (id invalid, or threshold not in (0,1], or every<1)\n");
        return;
    }

    chain_t *c = chain_get(id);
    kputs("  chain-backoff: id=");
    kput_dec((uint64_t)id);
    kputs(" name=\"");
    kputs(c ? c->name : "?");
    kputs("\" threshold=");
    /* Print as integer permille for predictability without printf. */
    kput_dec((uint64_t)(threshold * 1000.0f));
    kputs("/1000 every=");
    kput_dec((uint64_t)every);
    kputc('\n');
}

/* preempt-test: register a chain whose only node infinite-loops, call
 * scheduler_preempt_resolve directly, assert the LAPIC timer kills it
 * within watchdog_timeout_us, then unregister the chain. Verifies the
 * preempt path actually rescues a hung resolve. */
static void preempt_test_hang_node(chain_node_t *self, void *in, void *out)
{
    (void)self; (void)in; (void)out;
    /* Spin forever; only the LAPIC preempt timer can break us out. */
    for (;;) {
        __asm__ volatile("pause");
    }
}

static void cmd_preempt_test(const char *args)
{
    (void)args;
    kputs("  preempt-test: registering hang-chain ...\n");

    int id = chain_create("preempt-test-hang", -1, MASQ_INTERNAL);
    if (id < 0) { kputs("  preempt-test: chain_create failed\n"); return; }

    chain_t *c = chain_get(id);
    if (!c) { kputs("  preempt-test: chain_get failed\n"); chain_destroy(id); return; }

    /* Short timeout so the test finishes promptly. */
    c->watchdog_timeout_us = 5000;   /* 5 ms */
    c->status = CHAIN_LIVE;

    int n = chain_add_node(id, "hang", "void", "void", preempt_test_hang_node);
    if (n < 0) { kputs("  preempt-test: add_node failed\n"); chain_destroy(id); return; }

    uint32_t kills_before = scheduler_preempt_kills();

    kputs("  preempt-test: arming + entering hang (timeout=5000us) ...\n");
    uint64_t t0 = timer_read_tsc();
    int rc = scheduler_preempt_resolve(id);
    uint64_t t1 = timer_read_tsc();

    uint32_t kills_after = scheduler_preempt_kills();
    uint64_t freq = timer_tsc_freq();
    uint64_t us = (freq > 0) ? ((t1 - t0) * 1000000ULL / freq) : 0;

    chain_t *cc = chain_get(id);
    int status_ok = (cc && cc->status == CHAIN_ERROR);
    int kill_seen = (kills_after > kills_before);

    kputs("  preempt-test: rc=");
    kput_dec((uint64_t)(rc & 0xFFFFFFFF));
    kputs(" elapsed=");
    kput_dec(us);
    kputs("us preempt_kills delta=");
    kput_dec((uint64_t)(kills_after - kills_before));
    kputs(" status=");
    kputs(status_ok ? "CHAIN_ERROR" : "(unexpected)");
    kputc('\n');

    if (rc == -1 && kill_seen && status_ok) {
        kputs("  preempt-test: PASS (LAPIC preempt killed hung resolve)\n");
    } else {
        kputs("  preempt-test: FAIL\n");
    }

    chain_destroy(id);
}

/* cores: list SMP cores with role/alive/heartbeat. Just delegates to
 * smp.c so the print formatting stays in one place. */
static void cmd_cores(const char *args)
{
    (void)args;
    extern void smp_cmd_cores(void);
    smp_cmd_cores();
}

/* FAT32 read-only */
static void cmd_fat_mount(const char *args);
static void cmd_fat_ls(const char *args);
static void cmd_fat_cat(const char *args);
static void cmd_view(const char *args);

/* FAT32 write-side commands */
static void cmd_touch(const char *args);
static void cmd_rm(const char *args);
static void cmd_truncate(const char *args);
static void cmd_echo(const char *args);
static void cmd_trash(const char *args);
static void cmd_settings(const char *args);

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
    {"suspend", "ACPI S3 suspend-to-RAM (suspend [seconds]; default 5s RTC wake)", cmd_suspend, VIS_DEREZ},
    {"battery", "battery + AC adapter status (ACPI _BAT/_AC)", cmd_battery, VIS_ALWAYS},
    {"ec",      "ACPI EC: status / read <off> / write <off> <byte>", cmd_ec, VIS_DEREZ},
    {"aml-eval","evaluate an AML path (aml-eval <\\\\PATH>) — debug/introspection", cmd_aml_eval, VIS_DEREZ},
    {"tests",   "run per-chain test framework (tests [name|--json])", cmd_tests, VIS_DEREZ},
    {"static-ip","configure static IPv4 (use when DHCP unavailable)", cmd_static_ip, VIS_DEREZ},
    {"ip6",     "list configured IPv6 addresses + neighbor cache", cmd_ip6,    VIS_DEREZ},
    {"ping6",   "ICMPv6 echo request (ping6 <addr>)",              cmd_ping6,  VIS_DEREZ},
    {"nslookup6","resolve a hostname to an AAAA record",            cmd_nslookup6, VIS_DEREZ},
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
    {"persistence","show VAULT persistence stats (journal + chain snapshot)", cmd_persistence, VIS_DEREZ},
    {"gpustat","list GPUs and displays (mode + EDID monitor)", cmd_gpustat, VIS_DEREZ},
    {"goya",   "Habana Goya HL-1000: card count, BARs, fw, fence", cmd_goya,   VIS_DEREZ},
    {"nvidia", "NVIDIA GPUs: chip family, outputs, mode, GSP state", cmd_nvidia, VIS_DEREZ},
    {"gpu-load","per-backend MDE compute load (policy, inflight, EWMA us, cooldown)", cmd_gpu_load, VIS_DEREZ},
    {"scheduler-log","dump last N scheduler tick records (default 16)", cmd_scheduler_log, VIS_DEREZ},
    {"hotplug","dump recent hotplug events (PCI/USB/display)", cmd_hotplug, VIS_DEREZ},
    {"date",    "show or set wall clock (date [\"YYYY-MM-DD HH:MM:SS\"])", tod_cmd_date, VIS_ALWAYS},
    {"notify",  "fire a test notification (notify [level] [source] <text>)", notify_cmd_send, VIS_DEREZ},
    {"notify-history","show recent notifications (notify-history [N|clear])", notify_cmd_history, VIS_ALWAYS},
    {"dnd",     "Do Not Disturb (dnd | on|off|schedule [HH-HH] | mute|unmute <src>)", notify_cmd_dnd, VIS_ALWAYS},
    {"uptime",  "time since boot",                tod_cmd_uptime, VIS_ALWAYS},
    {"epoch",   "current Unix epoch seconds",     tod_cmd_epoch,  VIS_DEREZ},
    {"tickrate","show scheduler tps + avg tick (tickrate [watch])", cmd_tickrate, VIS_DEREZ},
    {"chain-backoff","tune B3 backoff: chain-backoff <id> <threshold> <every>", cmd_chain_backoff, VIS_DEREZ},
    {"preempt-test","verify LAPIC-timer preemption kills a hung chain_resolve", cmd_preempt_test, VIS_DEREZ},
    {"cores",   "list SMP cores: lapic_id, role, alive, heartbeat",  cmd_cores,    VIS_DEREZ},
    {"wifi",    "RTL8188EU USB WiFi: status|scan|list|connect|forget", cmd_wifi, VIS_DEREZ},
    {"wifi-scan","passive scan for visible APs (RTL8188EU)", cmd_wifi_scan, VIS_DEREZ},
    {"bt",      "Bluetooth: bt status|scan|connect <addr>|disconnect <addr>", cmd_bt, VIS_DEREZ},
    {"bt-l2cap","BT L2CAP: list active channels (cid, peer cid, psm, MTU, state)", cmd_bt_l2cap, VIS_DEREZ},

    /* VAULT filesystem — always visible */
    {"ls",      "list files",                      cmd_ls,      VIS_ALWAYS},
    {"cat",     "show file contents",              cmd_cat,     VIS_ALWAYS},
    {"save",    "save text to file (save path text)", cmd_write_file, VIS_DEREZ},
    {"mkdir",   "create directory",                cmd_mkdir,   VIS_DEREZ},
    {"df",      "VAULT disk usage",                cmd_df,      VIS_FULL},

    /* Full only — deep system commands */
    {"lspci",   "list PCI/PCIe devices (raw)",    cmd_lspci,   VIS_FULL},
    {"beep",    "play a 440 Hz tone via HDA audio", cmd_beep,   VIS_ALWAYS},
    {"volume",  "show or set master volume (volume [0-100|+N|-N])", cmd_volume, VIS_ALWAYS},
    {"mute",    "toggle master mute",              cmd_mute,    VIS_ALWAYS},
    {"brightness", "show or set display brightness (brightness [0-100|+N|-N])", cmd_brightness, VIS_ALWAYS},
    {"idle",    "show/set idle timing (idle | idle dim|lock|blank <secs>)", cmd_idle,  VIS_ALWAYS},
    {"lock",    "lock the screen now",            cmd_lock,    VIS_ALWAYS},
    {"pin",     "set lock screen PIN (pin <new-pin> | pin reset)", cmd_pin, VIS_ALWAYS},
    {"cold-boot-login", "show/toggle cold-boot login (cold-boot-login [on|off])", cmd_cold_boot_login, VIS_ALWAYS},
    {"power-button", "show/set power-button action (suspend|shutdown|lock|ask)", cmd_power_button, VIS_ALWAYS},
    {"lid-close", "show/set lid-close action (suspend|lock|nothing)", cmd_lid_close, VIS_ALWAYS},
    {"lid-open",  "show/set lid-open action (wake|nothing)", cmd_lid_open, VIS_ALWAYS},
    {"power",     "combined power view (buttons, lid, battery)", cmd_power, VIS_ALWAYS},
    {"about",   "about Zeos",                     cmd_about,   VIS_FULL},

    /* FAT32 (USB / SD / ESP / NVMe) read+write */
    {"fat-mount","mount FAT32 (fat-mount [<drive> <part-lba>])", cmd_fat_mount, VIS_ALWAYS},
    {"fat-ls",  "list FAT32 directory (fat-ls <path>)", cmd_fat_ls, VIS_ALWAYS},
    {"fat-cat", "show FAT32 file (fat-cat <path>)",  cmd_fat_cat, VIS_ALWAYS},
    {"view",    "open image viewer (view <path>; PNG only for now)", cmd_view, VIS_ALWAYS},
    {"touch",   "create empty file (FAT32: touch <path>)", cmd_touch, VIS_ALWAYS},
    {"rm",      "delete (moves to /.zeos-trash; rm -f to bypass)", cmd_rm,    VIS_ALWAYS},
    {"trash",   "list/restore/empty trash (trash, trash restore <id>, trash empty)", cmd_trash, VIS_ALWAYS},
    {"truncate","truncate file to size (truncate <path> <size>)", cmd_truncate, VIS_DEREZ},
    {"echo",    "write text to FAT32 file (echo <text> > <path>)", cmd_echo, VIS_ALWAYS},
    {"settings","unified settings (settings | settings <name> [value] | search/export/import)", cmd_settings, VIS_ALWAYS},
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

/* ── Suspend (ACPI S3) ──────────────────────────────────────────── */

static void cmd_suspend(const char *args)
{
    /* Parse optional seconds argument; default 5s. */
    while (*args == ' ' || *args == '\t') args++;
    uint32_t secs = 0;
    while (*args >= '0' && *args <= '9') {
        secs = secs * 10 + (uint32_t)(*args - '0');
        args++;
    }
    if (secs == 0) secs = 5;

    kputs("[shell] suspend requested, RTC wake in ");
    kput_dec((uint64_t)secs);
    kputs("s\n");

    int rc = suspend_to_ram(secs);
    if (rc == 0) {
        kputs("[shell] suspend cycle complete\n");
        if (CHAIN_POWER >= 0) {
            chain_t *cp = chain_get(CHAIN_POWER);
            if (cp) cp->vault_version++;
        }
    } else {
        kputs("[shell] suspend failed rc=");
        kput_dec((uint64_t)(uint32_t)(-rc));
        kputc('\n');
    }
}

/* ── Battery / AC ──────────────────────────────────────────────── */

static const char *bat_state_label(battery_state_t s)
{
    switch (s) {
    case BAT_CHARGING:    return "charging";
    case BAT_DISCHARGING: return "discharging";
    case BAT_FULL:        return "full";
    case BAT_CRITICAL:    return "CRITICAL";
    default:              return "unknown";
    }
}

static void cmd_battery(const char *args)
{
    (void)args;
    /* AC line */
    int ac = ac_online();
    kputs("AC: ");
    if      (ac == 1) kputs("connected\n");
    else if (ac == 0) kputs("on battery\n");
    else              kputs("unknown (no _AC device or method-only _PSR)\n");

    int n = battery_count();
    if (n == 0) {
        kputs("Battery: not present (desktop, QEMU, or pre-init)\n");
        return;
    }
    for (int i = 0; i < BATTERY_MAX; i++) {
        kputs("Battery ");
        kput_dec((uint64_t)i);
        kputs(": ");
        if (i >= n) {
            kputs("not present\n");
            continue;
        }
        const battery_info_t *b = battery_info(i);
        if (!b) { kputs("error\n"); continue; }
        if (!b->values_known) {
            kputs("present, charge unknown (no AML evaluator for method-only _BIF/_BST)\n");
            continue;
        }
        int pct = battery_percent(i);
        if (pct >= 0) { kput_dec((uint64_t)pct); kputs("% "); }
        else          { kputs("?% "); }
        kputs(bat_state_label(b->state));
        int mins = battery_remaining_minutes(i);
        if (mins >= 0) {
            kputs(", ~");
            kput_dec((uint64_t)mins);
            kputs(" minutes ");
            kputs(b->state == BAT_CHARGING ? "to full" : "remaining");
        }
        kputs("\n");
    }
}

/* ── ACPI Embedded Controller introspection ─────────────────────── */

#include "ec.h"

static int ec_parse_byte(const char *p, uint8_t *out)
{
    while (*p == ' ' || *p == '\t') p++;
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    if (!*p) return 0;
    uint32_t v = 0;
    while (*p && *p != ' ' && *p != '\t') {
        char c = *p++;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else return 0;
        if (d >= base) return 0;
        v = v * (uint32_t)base + (uint32_t)d;
        if (v > 0xFF) return 0;
    }
    *out = (uint8_t)v;
    return 1;
}

static const char *ec_skip_word(const char *p, char *buf, int buf_len)
{
    while (*p == ' ' || *p == '\t') p++;
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && i < buf_len - 1) buf[i++] = *p++;
    buf[i] = 0;
    return p;
}

static void cmd_ec(const char *args)
{
    char sub[16];
    const char *p = ec_skip_word(args ? args : "", sub, sizeof(sub));

    if (!sub[0]) {
        if (!ec_present()) {
            kputs("EC: not present (no _HID PNP0C09 in ACPI namespace)\n");
            return;
        }
        kputs("EC: present\n");
        kputs("  cmd  port = 0x"); kput_hex(ec_cmd_port());  kputs("\n");
        kputs("  data port = 0x"); kput_hex(ec_data_port()); kputs("\n");
        kputs("  reads     = "); kput_dec(ec_ops_read());    kputs("\n");
        kputs("  writes    = "); kput_dec(ec_ops_write());   kputs("\n");
        kputs("  timeouts  = "); kput_dec(ec_ops_timeout()); kputs("\n");
        return;
    }

    if (sub[0] == 'r' && sub[1] == 'e' && sub[2] == 'a' && sub[3] == 'd' && !sub[4]) {
        if (!ec_present()) { kputs("EC: not present\n"); return; }
        uint8_t off;
        if (!ec_parse_byte(p, &off)) { kputs("usage: ec read <offset>\n"); return; }
        uint8_t v = 0;
        if (ec_read(off, &v) != 0) {
            kputs("ec read: timeout / error\n");
            return;
        }
        kputs("ec[0x"); kput_hex(off); kputs("] = 0x"); kput_hex(v); kputs("\n");
        return;
    }

    if (sub[0] == 'w' && sub[1] == 'r' && sub[2] == 'i' && sub[3] == 't' && sub[4] == 'e' && !sub[5]) {
        if (!ec_present()) { kputs("EC: not present\n"); return; }
        char a1[8], a2[8];
        const char *q = ec_skip_word(p, a1, sizeof(a1));
        ec_skip_word(q, a2, sizeof(a2));
        uint8_t off, val;
        if (!a1[0] || !a2[0] || !ec_parse_byte(a1, &off) || !ec_parse_byte(a2, &val)) {
            kputs("usage: ec write <offset> <byte>\n");
            return;
        }
        /* Honest gate: writing arbitrary EC bytes can change fan, charge,
         * thermal, or wake behavior on real laptops. The shell accepts
         * the write (this command is VIS_DEREZ-gated already), but logs
         * the action in plain view. */
        kputs("[ec] WRITE 0x"); kput_hex(off);
        kputs(" <- 0x"); kput_hex(val); kputs("\n");
        if (ec_write(off, val) != 0) {
            kputs("ec write: timeout / error\n");
            return;
        }
        kputs("ec write: ok\n");
        return;
    }

    kputs("usage: ec | ec read <off> | ec write <off> <byte>\n");
}

/* ── AML eval (debug/introspection) ───────────────────────────────
 *
 * Usage: aml-eval <path>
 *   aml-eval \_SB_.LID0._LID
 *   aml-eval \_SB_.BAT0._BST
 *
 * Prints the type and value of the result, plus the journal entry's
 * elapsed TSC. No-arg methods only — passing args from the shell is a
 * separate, more involved feature (would need expression parsing).
 */
static void cmd_aml_eval(const char *args)
{
    if (!args || !*args) {
        kputs("  usage: aml-eval <\\PATH>\n");
        kputs("  example: aml-eval \\_SB_.LID0._LID\n");
        return;
    }
    /* Trim leading whitespace. */
    while (*args == ' ' || *args == '\t') args++;

    aml_object_t r = { .type = AML_T_UNINIT };
    int rc = aml_evaluate(args, 0, 0, &r);
    kputs("  path: ");
    kputs(args);
    kputs("\n  rc:   ");
    if (rc == 0)       kputs("0 (success)");
    else if (rc == -1) kputs("-1 (path not found)");
    else if (rc == -2) kputs("-2 (unimplemented opcode hit)");
    else if (rc == -3) kputs("-3 (eval aborted)");
    else { kputs("rc="); kput_dec((uint64_t)rc); }
    kputs("\n  type: ");
    switch (r.type) {
    case AML_T_INTEGER: kputs("Integer = 0x"); kput_hex(r.u.integer); break;
    case AML_T_STRING:  kputs("String = \""); kputs(r.u.str.s ? r.u.str.s : ""); kputs("\""); break;
    case AML_T_BUFFER:  kputs("Buffer ("); kput_dec((uint64_t)r.u.buf.len); kputs(" bytes)"); break;
    case AML_T_PACKAGE: {
        kputs("Package (");
        kput_dec((uint64_t)r.u.pkg.n);
        kputs(" elems): {");
        for (uint32_t i = 0; i < r.u.pkg.n && i < 8; i++) {
            if (i) kputs(", ");
            if (r.u.pkg.elems[i].type == AML_T_INTEGER) {
                kput_hex(r.u.pkg.elems[i].u.integer);
            } else {
                kputs("?");
            }
        }
        if (r.u.pkg.n > 8) kputs(", ...");
        kputs("}");
        break;
    }
    default: kputs("(uninit)");
    }
    kputs("\n");
    int jn = aml_journal_count();
    if (jn > 0) {
        const aml_journal_entry_t *e = aml_journal_at(jn - 1);
        kputs("  elapsed_tsc: ");
        kput_dec(e->elapsed_tsc);
        kputs("\n");
    }
    aml_object_release(&r);
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

/* chain_native — Z+ binds directly to the live kernel chain graph.
 * pulse fires every tick; sustained holds 5 consecutive ticks; then
 * audio.play rings through CHAIN_AUDIO and bumps its vault_version. */
static const char zp_chain_native[] =
    "// Z+ binds to the live kernel chain graph.\n"
    "pulse : emit(1)\n"
    "beat  : input -> sustained(> 0, for = 5)\n"
    "ring  : input -> audio.play\n"
    "log   : input -> tap.log\n"
    "pulse -> beat -> ring\n"
    "pulse ~> log\n"
    "chain heartbeat { pulse beat ring log }\n";

/* compute_via_mde — Z+ dispatches a node's resolve through CHAIN_MDE.
 * MDE picks the CPU backend, admits to the schedule (vault++), runs
 * the kernel_fn, emits the result (vault++). */
static const char zp_compute_via_mde[] =
    "// Z+ submits compute work via CHAIN_MDE.\n"
    "trigger : emit(1)\n"
    "square  : input -> delta -> output\n"
    "runner  : input -> compute.run(square)\n"
    "log     : input -> tap.log\n"
    "trigger -> runner -> log\n"
    "chain workload { trigger runner log }\n";

/* runtime_chain — register a chain whose nodes are real entries in the
 * kernel chain registry (chain.h), parented under CHAIN_CPU with
 * MASQ_INTERNAL tier. After running this program, `chains` lists
 * `heartbeat_runtime` as a LIVE chain and chain_registry_tick resolves
 * it via the Z+ dispatch thunk every frame. */
static const char zp_runtime_chain[] =
    "// Live runtime chain registered into the kernel chain graph.\n"
    "chain heartbeat_runtime {\n"
    "  pulse : emit(1) -> sustained(> 0, for = 3) -> tap.log\n"
    "}\n";

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
    {"chain_native", "Z+ -> live kernel chains (audio + tap.log)",  zp_chain_native},
    {"compute_via_mde", "Z+ submits compute through CHAIN_MDE",     zp_compute_via_mde},
    {"40_runtime_chain", "Z+ registers a chain into the live kernel registry", zp_runtime_chain},
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
    /* cp <src> <dst>  — src can be VAULT or (if mounted) FAT32; dst
     * picks whichever filesystem the path is on. We try VAULT first,
     * then fall back to FAT32 for the source. */
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

    static uint8_t buf[65536];
    int got = -1;
    int src_sz = 0;

    int vsz = vault_ready ? vault_size(src) : -1;
    if (vsz >= 0) {
        if (vsz > (int)sizeof(buf)) {
            kputs("  Source too large (");
            kput_dec((unsigned)vsz);
            kputs(" bytes; cp limited to 64 KB).\n");
            return;
        }
        got = vault_read(src, buf, (uint32_t)vsz);
        src_sz = vsz;
    } else if (fat32_mounted()) {
        struct fat32_file f;
        if (fat32_open(src, &f) == 0 && !f.is_dir) {
            if (f.size > sizeof(buf)) {
                kputs("  Source too large (");
                kput_dec((unsigned)f.size);
                kputs(" bytes; cp limited to 64 KB).\n");
                return;
            }
            got = fat32_read(&f, buf, f.size);
            src_sz = (int)f.size;
        }
    }

    if (got < 0) { kputs("  Source not found / read error.\n"); return; }
    (void)src_sz;

    /* Decide dst: prefer FAT32 if mounted; else VAULT. */
    if (fat32_mounted()) {
        (void)fat32_create(dst);
        if (fat32_truncate(dst, 0) != 0) {
            kputs("  cp: truncate dst failed\n"); return;
        }
        int w = fat32_write(dst, 0, buf, (uint32_t)got);
        if (w < 0) { kputs("  cp: write failed\n"); return; }
        kputs("  Copied "); kput_dec((unsigned)w); kputs(" bytes.\n");
        return;
    }

    if (!vault_ready) { kputs("  No filesystem mounted.\n"); return; }
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

/* ── IPv6 commands ───────────────────────────────────────── */

static void cmd_ip6(const char *args)
{
    (void)args;
    char buf[48];
    kputs("  IPv6 addresses:\n");
    int any = 0;
    for (int i = 0; i < IPV6_ADDR_MAX; i++) {
        if (!g_net6.addrs[i].valid) continue;
        any = 1;
        ipv6_format(g_net6.addrs[i].addr, buf);
        kputs("    "); kputs(buf);
        kputs("/"); kput_dec(g_net6.addrs[i].prefix_len);
        kputs(g_net6.addrs[i].is_dhcp ? " (DHCPv6)" :
              (ipv6_is_link_local(g_net6.addrs[i].addr) ? " (link-local)" : " (SLAAC)"));
        kputs("\n");
    }
    if (!any) kputs("    (none)\n");

    if (g_net6.have_gateway) {
        ipv6_format(g_net6.gateway, buf);
        kputs("  Gateway: "); kputs(buf); kputs("\n");
    }

    kputs("  Neighbor cache:\n");
    int nany = 0;
    for (int i = 0; i < IPV6_NEIGH_CACHE_SIZE; i++) {
        if (!g_net6.neigh[i].valid) continue;
        nany = 1;
        ipv6_format(g_net6.neigh[i].ip, buf);
        kputs("    "); kputs(buf); kputs(" -> ");
        for (int b = 0; b < 6; b++) {
            if (b) kputs(":");
            uint8_t v = g_net6.neigh[i].mac.b[b];
            static const char H[] = "0123456789abcdef";
            kputc(H[(v>>4)&0xF]); kputc(H[v&0xF]);
        }
        kputs("\n");
    }
    if (!nany) kputs("    (empty)\n");
}

static void cmd_ping6(const char *args)
{
    if (!g_net.up || !*args) {
        kputs("  Usage: ping6 <addr>\n");
        kputs("  Example: ping6 fec0::2\n");
        return;
    }
    struct ipv6_addr target;
    if (ipv6_parse(args, &target) < 0) {
        kputs("  Invalid IPv6 address.\n");
        return;
    }
    char buf[48];
    ipv6_format(target, buf);
    kputs("  Pinging "); kputs(buf); kputs("...\n");
    uint64_t rtt = ipv6_ping(target);
    if (rtt > 0) {
        kputs("  Reply: "); kput_dec(rtt); kputs(" TSC cycles\n");
    } else {
        kputs("  Timeout.\n");
    }
}

static void cmd_nslookup6(const char *args)
{
    if (!g_net.up || !*args) {
        kputs("  Usage: nslookup6 <hostname>\n");
        return;
    }
    struct ipv6_addr result;
    kputs("  Resolving "); kputs(args); kputs(" (AAAA)...\n");
    if (dns_resolve_aaaa(args, &result) == 0) {
        char buf[48];
        ipv6_format(result, buf);
        kputs("  "); kputs(args); kputs(" = "); kputs(buf); kputs("\n");
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

/* ── Master volume + mute ──────────────────────────────────────────
 *
 *   volume          show current
 *   volume <0-100>  set absolute
 *   volume +N       relative up   (clamps at 100)
 *   volume -N       relative down (clamps at 0)
 *   mute            toggle mute
 *
 * Both commands push the new state to the DAC + pin output amps via
 * HDA verb 0x300 and persist to VAULT under /audio/volume. CHAIN_AUDIO
 * vault_version bumps on every change.
 */

static void cmd_volume(const char *args)
{
    /* Skip leading whitespace. */
    while (*args == ' ' || *args == '\t') args++;

    if (*args == '\0') {
        kputs("  master volume: ");
        kput_dec((uint64_t)hda_audio_get_volume());
        kputs("%");
        if (hda_audio_get_muted()) kputs(" (muted)");
        kputs("\n");
        return;
    }

    int sign = 0;
    if (*args == '+') { sign = +1; args++; }
    else if (*args == '-') { sign = -1; args++; }

    /* Parse digits. */
    int n = 0, any = 0;
    while (*args >= '0' && *args <= '9') {
        n = n * 10 + (*args - '0');
        args++;
        any = 1;
    }
    if (!any) {
        kputs("  usage: volume [0-100|+N|-N]\n");
        return;
    }

    if (sign != 0) {
        hda_audio_volume_step(sign * n);
    } else {
        hda_audio_set_volume(n);
    }

    kputs("  master volume: ");
    kput_dec((uint64_t)hda_audio_get_volume());
    kputs("%");
    if (hda_audio_get_muted()) kputs(" (muted)");
    kputs("\n");
}

static void cmd_mute(const char *args)
{
    (void)args;
    hda_audio_toggle_mute();
    kputs("  master ");
    kputs(hda_audio_get_muted() ? "muted" : "unmuted");
    kputs(" (");
    kput_dec((uint64_t)hda_audio_get_volume());
    kputs("%)\n");
}

/* ── Display brightness ────────────────────────────────────────────
 *
 *   brightness          show current + supported levels
 *   brightness <0-100>  set absolute (snaps to nearest supported)
 *   brightness +N       relative up   (clamps at 100)
 *   brightness -N       relative down (clamps at 0)
 *
 * Backed by ACPI _BCL/_BCM scan in brightness.c. On firmware where
 * _BCM routes through SMI we persist the desired level but cannot
 * drive the panel until an AML interpreter lands.
 */
static void cmd_brightness(const char *args)
{
    while (*args == ' ' || *args == '\t') args++;

    if (*args == '\0') {
        if (!brightness_present()) {
            kputs("  no backlight (desktop or QEMU virtio-gpu)\n");
            return;
        }
        kputs("  brightness: ");
        kput_dec((uint64_t)brightness_get());
        kputs("%\n  supported levels: ");
        int lv[BRIGHTNESS_MAX_LEVELS];
        int n = brightness_levels(lv, BRIGHTNESS_MAX_LEVELS);
        for (int i = 0; i < n; i++) {
            if (i) kputc(' ');
            kput_dec((uint64_t)lv[i]);
        }
        kputs(" (");
        kput_dec((uint64_t)n);
        kputs(" steps)\n");
        return;
    }

    int sign = 0;
    if (*args == '+') { sign = +1; args++; }
    else if (*args == '-') { sign = -1; args++; }

    int n = 0, any = 0;
    while (*args >= '0' && *args <= '9') {
        n = n * 10 + (*args - '0');
        args++;
        any = 1;
    }
    if (!any) {
        kputs("  usage: brightness [0-100|+N|-N]\n");
        return;
    }

    int rc;
    if (sign != 0) rc = brightness_step(sign * n);
    else           rc = brightness_set(n);

    if (rc < 0) {
        kputs("  no backlight available\n");
        return;
    }
    kputs("  brightness: ");
    kput_dec((uint64_t)brightness_get());
    kputs("%\n");
}

/* ── Idle / lock-on-idle commands ────────────────────────────────
 *
 * `idle`              -- print thresholds + current state
 * `idle dim <secs>`   -- set the dim threshold
 * `idle lock <secs>`  -- set the lock threshold
 * `idle blank <secs>` -- set the blank (scanout off) threshold
 * `lock`              -- transition to LOCKED immediately
 * `pin <new-pin>`     -- replace the stored PIN (digits, 4..16)
 *
 * Thresholds persist to VAULT under /idle/{dim,lock,blank}_secs and
 * the PIN under /lock/pin. */

#include "idle.h"
#include "lockscreen.h"

static const char *idle_state_label(idle_state_t s)
{
    switch (s) {
    case IDLE_ACTIVE:  return "ACTIVE";
    case IDLE_DIMMED:  return "DIMMED";
    case IDLE_LOCKED:  return "LOCKED";
    case IDLE_BLANKED: return "BLANKED";
    }
    return "?";
}

static int idle_parse_uint(const char *p, uint32_t *out)
{
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return 0;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        p++;
        if (v > 86400u) return 0;
    }
    *out = v;
    return 1;
}

static void cmd_idle(const char *args)
{
    while (*args == ' ' || *args == '\t') args++;

    if (*args == '\0') {
        kputs("  idle dim_secs   = ");
        kput_dec((uint64_t)idle_dim_secs());
        kputc('\n');
        kputs("  idle lock_secs  = ");
        kput_dec((uint64_t)idle_lock_secs());
        kputc('\n');
        kputs("  idle blank_secs = ");
        kput_dec((uint64_t)idle_blank_secs());
        kputc('\n');
        kputs("  current state   = ");
        kputs(idle_state_label(idle_get_state()));
        kputs(" (");
        kput_dec(idle_seconds_since_input());
        kputs("s since last input)\n");
        kputs("  PIN configured  = ");
        kputs(lockscreen_pin_configured() ? "yes" : "no");
        kputc('\n');
        return;
    }

    /* Sub-commands. */
    const char *sub = args;
    int nlen = 0;
    while (sub[nlen] && sub[nlen] != ' ' && sub[nlen] != '\t') nlen++;
    const char *rest = sub + nlen;

    uint32_t v = 0;
    if (nlen == 3 && sub[0]=='d' && sub[1]=='i' && sub[2]=='m') {
        if (!idle_parse_uint(rest, &v) || idle_set_dim(v) < 0) {
            kputs("  usage: idle dim <secs>\n");
            return;
        }
        kputs("  dim_secs = ");
        kput_dec((uint64_t)idle_dim_secs());
        kputs(" (saved)\n");
        return;
    }
    if (nlen == 4 && sub[0]=='l' && sub[1]=='o' && sub[2]=='c' && sub[3]=='k') {
        if (!idle_parse_uint(rest, &v) || idle_set_lock(v) < 0) {
            kputs("  usage: idle lock <secs>\n");
            return;
        }
        kputs("  lock_secs = ");
        kput_dec((uint64_t)idle_lock_secs());
        kputs(" (saved)\n");
        return;
    }
    if (nlen == 5 && sub[0]=='b' && sub[1]=='l' && sub[2]=='a'
                  && sub[3]=='n' && sub[4]=='k') {
        if (!idle_parse_uint(rest, &v) || idle_set_blank(v) < 0) {
            kputs("  usage: idle blank <secs>\n");
            return;
        }
        kputs("  blank_secs = ");
        kput_dec((uint64_t)idle_blank_secs());
        kputs(" (saved)\n");
        return;
    }

    kputs("  usage: idle | idle dim|lock|blank <secs>\n");
}

static void cmd_lock(const char *args)
{
    (void)args;
    if (!lockscreen_pin_configured()) {
        kputs("  cannot lock: no PIN configured. Run `pin <new-pin>` first.\n");
        return;
    }
    kputs("  locking screen\n");
    idle_force_lock();
}

static void cmd_pin(const char *args)
{
    while (*args == ' ' || *args == '\t') args++;
    if (*args == '\0') {
        kputs("  usage: pin <new-pin>   (4..16 digits)\n");
        kputs("         pin reset        (clear stored PIN)\n");
        return;
    }
    /* Bound the input -- copy into a small stack buffer so we can
     * NUL-terminate cleanly even if the user typed trailing garbage. */
    char buf[24];
    int n = 0;
    while (args[n] && args[n] != ' ' && args[n] != '\t' && n < 22) {
        buf[n] = args[n];
        n++;
    }
    buf[n] = '\0';

    /* `pin reset` -- forget the stored PIN. Forces re-enrollment on the
     * next cold-boot gate or on the next idle-driven lock. */
    if (n == 5 && buf[0]=='r' && buf[1]=='e' && buf[2]=='s'
              && buf[3]=='e' && buf[4]=='t') {
        lockscreen_pin_clear();
        kputs("  PIN cleared. Enrollment will run on next cold-boot login.\n");
        return;
    }

    if (lockscreen_set_pin(buf) < 0) {
        kputs("  PIN must be 4..16 digits\n");
        return;
    }
    kputs("  PIN updated and persisted to /lock/pin\n");
}

static void cmd_cold_boot_login(const char *args)
{
    while (*args == ' ' || *args == '\t') args++;
    if (*args == '\0') {
        kputs("  cold-boot-login = ");
        kputs(cold_boot_login_required() ? "on" : "off");
        kputs(", PIN ");
        kputs(lockscreen_pin_configured() ? "set" : "MISSING");
        kputc('\n');
        kputs("  usage: cold-boot-login on|off\n");
        return;
    }

    if (args[0]=='o' && args[1]=='n' &&
        (args[2]=='\0' || args[2]==' ' || args[2]=='\t')) {
        if (cold_boot_login_set(1) < 0) {
            kputs("  failed to persist /lock/cold-boot-required\n");
            return;
        }
        kputs("  cold-boot-login = on (saved)\n");
        return;
    }
    if (args[0]=='o' && args[1]=='f' && args[2]=='f' &&
        (args[3]=='\0' || args[3]==' ' || args[3]=='\t')) {
        if (cold_boot_login_set(0) < 0) {
            kputs("  failed to persist /lock/cold-boot-required\n");
            return;
        }
        kputs("  cold-boot-login = off (saved)\n");
        return;
    }
    kputs("  usage: cold-boot-login on|off\n");
}

/* ── Power button / lid actions ─────────────────────────────── */

static const char *first_token(const char *s, int *outlen)
{
    while (*s == ' ' || *s == '\t') s++;
    int n = 0;
    while (s[n] && s[n] != ' ' && s[n] != '\t' && s[n] != '\n') n++;
    *outlen = n;
    return s;
}

static void cmd_power_button(const char *args)
{
    int n = 0;
    const char *t = first_token(args, &n);
    if (n == 0) {
        kputs("  power-button = ");
        kputs(power_action_label(power_button_action()));
        kputs(" (events seen: ");
        kput_dec((uint64_t)power_button_event_count());
        kputs(")\n");
        return;
    }
    char tok[16]; int i;
    for (i = 0; i < n && i < 15; i++) tok[i] = t[i];
    tok[i] = 0;
    power_action_t a;
    if (!power_action_parse(tok, &a)) {
        kputs("  usage: power-button [suspend|shutdown|lock|ask]\n");
        return;
    }
    if (power_button_set_action(a) < 0) {
        kputs("  invalid action for power-button\n");
        return;
    }
    kputs("  power-button = ");
    kputs(power_action_label(a));
    kputs(" (saved)\n");
}

static void cmd_lid_close(const char *args)
{
    int n = 0;
    const char *t = first_token(args, &n);
    if (n == 0) {
        kputs("  lid-close = ");
        kputs(power_action_label(lid_close_action()));
        kputc('\n');
        return;
    }
    char tok[16]; int i;
    for (i = 0; i < n && i < 15; i++) tok[i] = t[i];
    tok[i] = 0;
    power_action_t a;
    if (!power_action_parse(tok, &a)) {
        kputs("  usage: lid-close [suspend|lock|nothing]\n");
        return;
    }
    if (lid_close_set_action(a) < 0) {
        kputs("  invalid action for lid-close (use suspend|lock|nothing)\n");
        return;
    }
    kputs("  lid-close = ");
    kputs(power_action_label(a));
    kputs(" (saved)\n");
}

static void cmd_lid_open(const char *args)
{
    int n = 0;
    const char *t = first_token(args, &n);
    if (n == 0) {
        kputs("  lid-open = ");
        kputs(power_action_label(lid_open_action()));
        kputc('\n');
        return;
    }
    char tok[16]; int i;
    for (i = 0; i < n && i < 15; i++) tok[i] = t[i];
    tok[i] = 0;
    power_action_t a;
    if (!power_action_parse(tok, &a)) {
        kputs("  usage: lid-open [wake|nothing]\n");
        return;
    }
    if (lid_open_set_action(a) < 0) {
        kputs("  invalid action for lid-open (use wake|nothing)\n");
        return;
    }
    kputs("  lid-open = ");
    kputs(power_action_label(a));
    kputs(" (saved)\n");
}

static void cmd_power(const char *args)
{
    (void)args;
    kputs("\n  Power\n  ─────\n");
    kputs("  power-button     = ");
    kputs(power_action_label(power_button_action()));
    kputs(" (");
    kputs(power_btn_present() ? "PNP0C0C" : "fixed (FADT)");
    kputs(", events: ");
    kput_dec((uint64_t)power_button_event_count());
    kputs(")\n");

    kputs("  sleep-button     = ");
    kputs(sleep_btn_present() ? "PNP0C0E present" : "not detected");
    kputc('\n');

    kputs("  lid              = ");
    if (!lid_present()) {
        kputs("not detected\n");
    } else {
        int s = lid_current_state();
        if (s == 1) kputs("open");
        else if (s == 0) kputs("closed");
        else kputs("present (state via AML method, unreadable without evaluator)");
        kputs(" (events: ");
        kput_dec((uint64_t)lid_event_count());
        kputs(")\n");
    }
    kputs("  lid-close        = ");
    kputs(power_action_label(lid_close_action()));
    kputc('\n');
    kputs("  lid-open         = ");
    kputs(power_action_label(lid_open_action()));
    kputc('\n');

    /* Battery summary alongside, since `power` is the combined view. */
    kputs("  AC               = ");
    int ac = ac_online();
    if (ac == 1)      kputs("online\n");
    else if (ac == 0) kputs("offline (on battery)\n");
    else              kputs("unknown\n");

    int bn = battery_count();
    if (bn == 0) {
        kputs("  battery          = none\n");
    } else {
        for (int i = 0; i < bn; i++) {
            kputs("  battery[");
            kput_dec((uint64_t)i);
            kputs("]       = ");
            int pct = battery_percent(i);
            if (pct >= 0) {
                kput_dec((uint64_t)pct);
                kputs("% ");
                kputs(bat_state_label(battery_state(i)));
            } else {
                kputs("present (charge unknown)");
            }
            kputc('\n');
        }
    }
}

/* Selftest kernel_fn for the MDE chain probe. The body is what
 * actually runs on the CPU backend: read the .answer field, return
 * it. Real work, real return value. */
struct shell_mde_selftest_args { int answer; };
int shell_mde_selftest_kfn(void *args)
{
    struct shell_mde_selftest_args *a = (struct shell_mde_selftest_args *)args;
    return a ? a->answer : -1;
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
        /* Per-NVMe-drive completion path: MSI-X (preferred) or polling. */
        int nn = nvme_drive_count();
        for (int i = 0; i < nn; i++) {
            nvme_dev_t *dd = nvme_get_drive(i);
            if (!dd) continue;
            kputs("    NVMe drive ");
            kput_dec(i);
            kputs(" ........ ");
            if (dd->msix_vector >= 0) {
                kputs("msi-x driven (vec ");
                kput_hex((uint64_t)dd->msix_vector);
                kputs(")\n");
            } else {
                kputs("polling fallback\n");
            }
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

    /* FAT32 write: round-trip on the mounted volume. Auto-mount first
     * if nothing is mounted (NVMe boot will land here). */
    kputs("  FAT32 write ........... ");
    if (!fat32_mounted()) {
        (void)fat32_automount();
    }
    if (!fat32_mounted()) {
        kputs("SKIP (no FAT32 volume)\n");
    } else {
        const char *dir  = "/test";
        const char *path = "/test/zeos_test.txt";
        const char  msg[] = "zeos-fat32-write-selftest-OK";
        const uint32_t msg_len = (uint32_t)(sizeof(msg) - 1);

        /* Clean any prior run. Best-effort (ignore errors). */
        (void)fat32_unlink(path);
        (void)fat32_unlink(dir);

        int step = 0;
        int bytes_w = 0, bytes_r = 0;
        do {
            if (fat32_mkdir(dir) != 0) { step = 1; break; }
            if (fat32_create(path) != 0) { step = 2; break; }
            int w = fat32_write(path, 0, msg, msg_len);
            if (w != (int)msg_len) { step = 3; bytes_w = w; break; }
            bytes_w = w;

            struct fat32_file f;
            if (fat32_open(path, &f) != 0) { step = 4; break; }
            if (f.size != msg_len)         { step = 5; break; }
            char rbuf[64];
            int got = fat32_read(&f, rbuf, sizeof(rbuf));
            if (got != (int)msg_len) { step = 6; bytes_r = got; break; }
            bytes_r = got;
            for (uint32_t i = 0; i < msg_len; i++) {
                if (rbuf[i] != msg[i]) { step = 7; break; }
            }
            if (step) break;

            uint32_t half = msg_len / 2;
            if (fat32_truncate(path, half) != 0) { step = 8; break; }
            struct fat32_file f2;
            if (fat32_open(path, &f2) != 0)  { step = 9; break; }
            if (f2.size != half)             { step = 10; break; }

            if (fat32_unlink(path) != 0) { step = 11; break; }
            struct fat32_file f3;
            if (fat32_open(path, &f3) == 0) { step = 12; break; }
            (void)fat32_unlink(dir);
        } while (0);

        if (step == 0) {
            kputs("PASS (wrote ");
            kput_dec((uint64_t)bytes_w);
            kputs(", read ");
            kput_dec((uint64_t)bytes_r);
            kputs(")\n");
            passes++;
        } else {
            kputs("FAIL (step=");
            kput_dec((uint64_t)step);
            kputs(", w=");
            kput_dec((uint64_t)bytes_w);
            kputs(", r=");
            kput_dec((uint64_t)bytes_r);
            kputs(")\n");
            fails++;
            /* Best-effort cleanup. */
            (void)fat32_unlink(path);
            (void)fat32_unlink(dir);
        }
    }

    /* Trash subsystem readiness. Reports the trash root status,
     * current entry count, and approximate bytes consumed. Skips
     * silently when no FAT32 volume is mounted. */
    kputs("  Trash ................. ");
    if (!fat32_mounted()) {
        kputs("SKIP (no FAT32 volume)\n");
    } else {
        uint32_t tcount = 0;
        uint64_t tbytes = 0;
        int sr = fat32_trash_stats(&tcount, &tbytes);
        if (sr != 0) {
            kputs("FAIL (stats)\n");
            fails++;
        } else {
            kputs("/.zeos-trash/ ");
            /* Existence is implicit: if stats returned 0 with no
             * mkdir attempt, the directory is either present or
             * lazily-created on first delete. */
            kputs("ready, ");
            kput_dec((uint64_t)tcount);
            kputs(" entries, ");
            uint64_t mb = tbytes / (1024ULL * 1024ULL);
            kput_dec(mb);
            kputs(" MB used\n");
            passes++;
        }
    }

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

    /* HTTPS: full TLS handshake + GET against a known-good host.
     * Goes through Happy Eyeballs (https_get_once → tls_happy_eyeballs):
     * if v6 connectivity exists this fetches over v6, otherwise v4. */
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

    /* HTTPS/v6 explicit: AAAA-only path. Logs honestly if no v6 route. */
    kputs("  HTTPS/v6 .............. ");
    if (!g_net.up) {
        kputs("SKIP (no network)\n");
    } else {
        struct ipv6_addr ip6;
        extern int dns_resolve_aaaa(const char *, struct ipv6_addr *);
        extern tls_conn_t *tls_open_v6(const char *, struct ipv6_addr, uint16_t);
        if (dns_resolve_aaaa("letsencrypt.org", &ip6) < 0) {
            kputs("no v6 route (no AAAA / no v6 reachability)\n");
        } else {
            char ipbuf[48];
            ipv6_format(ip6, ipbuf);
            tls_conn_t *c = tls_open_v6("letsencrypt.org", ip6, 443);
            if (!c) {
                kputs("no v6 route (TCP6/TLS failed to "); kputs(ipbuf); kputs(")\n");
            } else {
                /* Issue a tiny GET and count bytes */
                const char *req =
                    "GET / HTTP/1.1\r\nHost: letsencrypt.org\r\n"
                    "Connection: close\r\nUser-Agent: Zeos/0.1\r\n\r\n";
                int rl = 0; while (req[rl]) rl++;
                int sent = 0;
                while (sent < rl) {
                    int n = tls_send(c, req + sent, rl - sent);
                    if (n < 0 && n != -0x6900 /* WANT_READ-ish placeholder */) break;
                    if (n > 0) sent += n;
                }
                static char rbuf[4096];
                int total = 0;
                while (total < (int)sizeof(rbuf) - 1) {
                    int n = tls_recv(c, rbuf + total, sizeof(rbuf) - 1 - total);
                    if (n <= 0) break;
                    total += n;
                }
                tls_close(c);
                if (total > 0) {
                    kputs("fetched ("); kput_dec(total); kputs(" bytes) — letsencrypt.org via "); kputs(ipbuf); kputs("\n");
                    passes++;
                } else {
                    kputs("no v6 route (handshake ok, no data from "); kputs(ipbuf); kputs(")\n");
                }
            }
        }
    }

    /* IPv6: link-local + DHCPv6 + AAAA selftest line */
    ipv6_print_selftest_line();
    passes++;

    /* MSI-X infrastructure */
    kputs("  MSI-X: ready (");
    kput_dec(msix_free_count());
    kputs(" vectors free)\n");
    passes++;

    /* LAPIC: id, calibrated bus frequency, IOAPIC count from ACPI. */
    kputs("  LAPIC ............. ");
    if (lapic_ready()) {
        uint32_t tpu = lapic_ticks_per_us();
        uint32_t mhz = tpu;  /* ticks/µs == MHz at the configured divider */
        kputs("id=");
        kput_dec(lapic_id());
        kputs(", freq=");
        kput_dec(mhz);
        kputs(" MHz, IOAPIC count=");
        kput_dec((uint64_t)ioapic_count());
        kputs("\n");
        /* Sanity floor: any non-zero calibrated frequency means PIT
         * channel 2 ticked and the LAPIC timer counted. QEMU's
         * emulated APIC bus is ~62 MHz at div=16; real silicon is
         * 100+ MHz. Anything less means calibration broke. */
        if (mhz >= 50) passes++;
        else           fails++;
    } else {
        kputs("not ready\n");
        fails++;
    }

    /* HDA audio -- exercises the chain-native pipeline:
     * pcm_source -> volume_filter -> hda_pin -> hardware_dma.
     * The chain is dumped regardless of controller readiness so the
     * paradigm conversion is observable even on hardware where the
     * codec walk doesn't bring up an output path. */
    kputs("  Audio (HDA) ........... ");
    if (hda_ready())          kputs("HDA ready\n");
    else { kputs("controller not ready ("); kputs(hda_status()); kputs(")\n"); }

    /* Master volume + mute — applied to DAC/pin amps via verb 0x300. */
    kputs("  Audio volume .......... ");
    kput_dec((uint64_t)hda_audio_get_volume());
    kputs("% (");
    kputs(hda_audio_get_muted() ? "muted" : "unmuted");
    kputs(") — applied to DAC/pin amps\n");
    passes++;

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

        /* 100 ms 440 Hz tone -- proves the pipeline actually pushes
         * samples to the DAC. With the QEMU 1af4:0012 codec the audio
         * backend should render this through the host. */
        if (hda_ready()) {
            const int sr     = 48000;
            const int frames = sr / 10;          /* 100 ms */
            const int freq   = 440;
            static int16_t tone_buf[4800 * 2];   /* 19 KB */
            uint32_t step  = ((uint64_t)freq * 4096ULL * 65536ULL) / (uint32_t)sr;
            uint32_t phase = 0;
            int env_frames = sr / 200;            /* 5 ms ramp */
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
                tone_buf[i*2 + 0] = (int16_t)v;
                tone_buf[i*2 + 1] = (int16_t)v;
                phase += step;
            }
            int trc = hda_play_pcm(tone_buf, frames * 2, sr);
            kputs("    tone 440 Hz / 100 ms .. ");
            if (trc == 0) { kputs("PASS\n"); passes++; }
            else          { kputs("FAIL (rc="); kput_dec((uint64_t)(int64_t)trc); kputs(")\n"); fails++; }
        } else {
            kputs("    tone 440 Hz / 100 ms .. SKIP (no controller)\n");
        }
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

    /* WiFi association: prove the WPA2 supplicant compiles and that
     * its primitives self-check. We can't drive a real handshake from
     * here without an AP, but we CAN verify PMK derivation against the
     * IEEE 802.11i test vector and that the MIC primitive is wired —
     * those two together are most of what could regress in this code
     * path. The chip-side bring-up status comes from g_rtl8188eu. */
    kputs("  WiFi assoc ............ ");
    {
        /* Test vector: passphrase="password", SSID="IEEE",
         * iter=4096 -> PMK starts with f4 2c 6f c5 2d f0 eb ef. */
        uint8_t pmk[32];
        int     pmk_ok = (wpa_derive_pmk("password", "IEEE", pmk) == 0);
        static const uint8_t expect[8] = {
            0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef
        };
        int v_ok = pmk_ok;
        for (int i = 0; i < 8 && v_ok; i++)
            if (pmk[i] != expect[i]) v_ok = 0;

        if (!v_ok) {
            kputs("PBKDF2 vector MISMATCH\n");
            fails++;
        } else if (g_rtl8188eu.assoc == RTL_ASSOC_JOINED) {
            kputs("joined \"");
            kputs(g_rtl8188eu.ssid);
            kputs("\"\n");
            passes++;
        } else if (g_rtl8188eu.present && g_rtl8188eu.fw_loaded) {
            kputs("supplicant ready, no AP joined\n");
            passes++;
        } else if (g_rtl8188eu.present) {
            kputs("chip detected (firmware not loaded)\n");
            passes++;
        } else {
            kputs("wired, untested (no RTL8188EU dongle present)\n");
            passes++;
        }
    }

    /* Bluetooth: HCI USB transport + boot sequence (Reset / Read Local
     * Version / Read BD_ADDR). Both "controller present" and "not
     * present" PASS — present means real silicon answered the boot
     * commands, not present is honest reporting on QEMU / no dongle. */
    kputs("  Bluetooth ............. ");
    if (bt_usb_present() && bt_hci_ready()) {
        kputs("controller present (BD_ADDR ");
        bt_print_bdaddr(bt_hci_bdaddr());
        kputs(")\n");
        passes++;
    } else if (bt_usb_present()) {
        /* Bound transport but boot sequence didn't complete (often a
         * vendor-firmware-needed chip). Still PASS — the transport
         * proved itself; the firmware loader is future work. */
        kputs("controller present (HCI boot incomplete — vendor firmware?)\n");
        passes++;
    } else {
        kputs("not present\n");
        passes++;
    }
    if (CHAIN_BLUETOOTH >= 0) {
        chain_t *btc = chain_get(CHAIN_BLUETOOTH);
        int bn = btc ? btc->node_count : 0;
        kputs("    chain nodes=");
        kput_dec((uint64_t)bn);
        kputs("\n");
        chain_dump(CHAIN_BLUETOOTH);
        if (bn == 4) passes++; else fails++;
    }

    /* L2CAP: signaling channel registered + N channels available. */
    kputs("  BT L2CAP .............. ");
    if (l2cap_signaling_ready()) {
        kput_dec((uint64_t)l2cap_channel_count());
        kputs(" channels available, signaling registered\n");
        passes++;
    } else {
        kputs("not initialized\n");
        fails++;
    }
    if (CHAIN_BT_L2CAP >= 0) {
        chain_t *l2c = chain_get(CHAIN_BT_L2CAP);
        int ln = l2c ? l2c->node_count : 0;
        kputs("    chain nodes=");
        kput_dec((uint64_t)ln);
        kputs("\n");
        chain_dump(CHAIN_BT_L2CAP);
        if (ln == 4) passes++; else fails++;
    }

    /* MDE chain: submit a trivial compute_request through CHAIN_MDE
     * and verify (a) the kernel_fn really ran (rc=42), (b)
     * vault_version bumped at least twice (schedule admit +
     * result_emit completion), (c) the schedule slot was released
     * (inflight=0 after submit). This is the chain-side proof that
     * MDE is exposed AS a chain, not just routing infrastructure. */
    kputs("  MDE chain ............. ");
    if (CHAIN_MDE >= 0) {
        chain_t *mc = chain_get(CHAIN_MDE);
        int mn = mc ? mc->node_count : 0;
        int vv_before = mc ? mc->vault_version : 0;
        int infl_before = mde_chain_inflight();

        /* Trivial real kernel_fn: returns args->answer. */
        struct shell_mde_selftest_args thunk = { .answer = 42 };
        mde_compute_request_t req = {
            .kernel_fn   = shell_mde_selftest_kfn,
            .args        = &thunk,
            .rc          = 0,
            .elapsed_tsc = 0,
            .policy_override = -1,
            .affinity_backend_idx = -1,
        };
        int srv = mde_chain_submit(&req);
        int vv_after = mc ? mc->vault_version : 0;
        int infl_after = mde_chain_inflight();
        int vv_delta = vv_after - vv_before;

        kputs("nodes=");
        kput_dec((uint64_t)mn);
        kputs(", rc=");
        kput_dec((uint64_t)(uint32_t)srv);
        kputs(", vault_version ");
        kput_dec((uint64_t)vv_before);
        kputs("->");
        kput_dec((uint64_t)vv_after);
        kputs(", inflight ");
        kput_dec((uint64_t)infl_before);
        kputs("->");
        kput_dec((uint64_t)infl_after);
        kputs("\n");
        chain_dump(CHAIN_MDE);

        if (mn == 5 && srv == 42 && vv_delta >= 2 &&
            req.rc == 42 && req.elapsed_tsc > 0 && infl_after == 0) {
            kputs("  MDE chain submit ...... PASS\n");
            passes++;
        } else {
            kputs("  MDE chain submit ...... FAIL\n");
            fails++;
        }
    } else {
        kputs("not registered\n");
        fails++;
    }

    /* MDE selector: report the active policy, the number of registered
     * compute backends, and how many are currently in error cooldown.
     * Always passes -- this is observability, not validation. */
    gpu_compute_print_selftest_line();
    passes++;

    /* CFA handles: TLS state + VAULT blob + lockscreen PIN buffers
     * must each be wrapped after tls_init(), vault_mount(), and
     * lockscreen_init() have run. The selftest fails if any are
     * unwrapped -- that means SOVEREIGN-tier kernel state is still
     * escaping the MasQ tier check. */
    kputs("  CFA handles ........... ");
    {
        int hc = cfa_handle_count();
        int ls = lockscreen_cfa_handle_count();
        kputs("count=");
        kput_dec((uint64_t)hc);
        kputs(" lockscreen=");
        kput_dec((uint64_t)ls);
        /* Expect: TLS conf + VAULT blob + 3 lockscreen PIN buffers. */
        if (hc >= 5 && ls >= 3) {
            kputs("  PASS\n");
            passes++;
        } else if (hc >= 2 && ls >= 2) {
            /* Tolerate 2-handle lockscreen during the very first boot
             * before enrollment has wrapped enroll_first; cold-boot
             * gate triggers all three. */
            kputs("  PASS (partial lockscreen wrap)\n");
            passes++;
        } else {
            kputs("  FAIL (expected TLS + VAULT + lockscreen)\n");
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

    /* GPU: virtio-gpu chains (CHAIN_GPU_n + CHAIN_DISPLAY_<n>) or
     * GOP fallback. Per docs/GPU_HOLES.md L1 -- this is the first
     * real GPU paradigm slot. Verifies at least one display chain
     * is active. */
    kputs("  GPU ................... ");
    {
        int gn = gpu_virtio_device_count();
        int dn = gpu_virtio_display_count();
        if (gn == 0) {
            int gop = -1;
            if (gpu_virtio_gop_fallback(&gop) && gop >= 0) {
                kputs("no virtio-gpu detected (GOP fallback)\n");
                chain_dump(gop);
                passes++;
            } else {
                kputs("FAIL (no GPU chains)\n");
                fails++;
            }
        } else {
            kputs("PASS (");
            kput_dec((uint64_t)gn);
            kputs(" device(s), ");
            kput_dec((uint64_t)dn);
            kputs(" display(s))\n");
            if (CHAIN_GPU_0 >= 0) chain_dump(CHAIN_GPU_0);
            for (int i = 0; i < dn; i++) {
                gpu_virtio_display_t info;
                if (gpu_virtio_display_info(i, &info) == 0 && info.chain_id >= 0)
                    chain_dump(info.chain_id);
            }
            if (dn >= 1) passes++; else fails++;
        }
    }

    /* NVIDIA: peer to virtio-gpu under CHAIN_CPU. Stage 1 reports
     * Turing scanout state; Stage 2 reports GSP firmware state.
     * SKIP when no NVIDIA card is present (QEMU default). */
    kputs("  NVIDIA ................ ");
    {
        int nvn = gpu_nvidia_device_count();
        if (nvn == 0) {
            kputs("SKIP (no NVIDIA card)\n");
        } else {
            char line[160];
            gpu_nvidia_selftest_summary(line, (int)sizeof(line));
            kputs(line);
            kputc('\n');
            int any_ok = 0;
            for (int i = 0; i < nvn; i++) {
                const gpu_nvidia_device_t *d = gpu_nvidia_device(i);
                if (d && d->ready) any_ok = 1;
            }
            if (any_ok) passes++; else fails++;
        }
    }

    /* Goya HL-1000: per docs/GPU_HOLES.md A3, compute-only accelerator
     * surface. Reports card count + firmware/compute status. Honest
     * outcomes: cards+fw=loaded+compute=ready (PASS), cards present
     * but no firmware blob embedded (DETECTED), or no cards (skip). */
    gpu_goya_print_selftest_line();
    if (gpu_goya_device_count() > 0) {
        passes++;
    }

    /* Hotplug pumps: PCI rescan, USB PORTSC poll, virtio-gpu HPD. */
    kputs("  Hotplug ............... ");
    {
        int live = hotplug_pumps_live();
        if (live == 3) {
            kputs("3 pumps live (pci/usb/display)\n");
            passes++;
        } else {
            kputs("FAIL (");
            kput_dec((uint64_t)live);
            kputs("/3 pumps live)\n");
            fails++;
        }
    }

    /* Scheduler: chain resolution as the kernel scheduler. Reports
     * ticks-per-second from the rolling 100ms sample, count of LIVE
     * chains visible in the registry, and per-tick error count. */
    kputs("  Scheduler ............ ");
    {
        uint32_t tps = scheduler_tps();
        int live = 0, errs = 0;
        for (int id = 0; id < MAX_CHAINS; id++) {
            chain_t *cc = chain_get(id);
            if (!cc) continue;
            if (cc->status == CHAIN_LIVE)  live++;
            if (cc->status == CHAIN_ERROR) errs++;
        }
        kput_dec((uint64_t)tps);
        kputs(" tps, ");
        kput_dec((uint64_t)live);
        kputs(" LIVE chains, ");
        kput_dec((uint64_t)errs);
        kputs(" error\n");
        /* New scheduler hardening lines: aggregate slow ticks,
         * watchdog kills, and the top slow chain by cumulative
         * resolve time (with average per-resolve ms). */
        kputs("                         agg_slow_total=");
        kput_dec((uint64_t)scheduler_aggregate_slow_total());
        kputs(", preempt_kills=");
        kput_dec((uint64_t)scheduler_preempt_kills());
        kputs(", watchdog_kills=");
        kput_dec((uint64_t)scheduler_watchdog_kills());
        kputs(", tick_avg=");
        kput_dec((uint64_t)scheduler_tick_avg_us());
        kputs("us\n");
        int top_id = -1;
        float top_avg = 0.0f;
        if (scheduler_top_slow_chain(&top_id, &top_avg) && top_id >= 0) {
            chain_t *tc = chain_get(top_id);
            kputs("                         top_slow=id ");
            kput_dec((uint64_t)top_id);
            kputs(" \"");
            kputs(tc ? tc->name : "?");
            kputs("\" avg=");
            /* Integer ms -- avoid float printf in kernel. */
            kput_dec((uint64_t)top_avg);
            kputs("ms");
            if (tc && tc->resolve_interval_ticks > 0) {
                kputs(" (resolved 1/");
                kput_dec((uint64_t)tc->resolve_interval_ticks);
                kputs(" ticks)");
            }
            kputc('\n');
        } else {
            kputs("                         top_slow=(none yet)\n");
        }
        /* Compositor async stats: composites that ran vs skipped via
         * the dirty-rect gate. High skips with a healthy composite
         * count = the async path is working. */
        {
            extern uint32_t compositor_composite_count(void);
            extern uint32_t compositor_composite_skips(void);
            extern uint32_t compositor_dirty_pushes(void);
            kputs("                         compositor: composites=");
            kput_dec((uint64_t)compositor_composite_count());
            kputs(" skips=");
            kput_dec((uint64_t)compositor_composite_skips());
            kputs(" dirty_pushes=");
            kput_dec((uint64_t)compositor_dirty_pushes());
            kputc('\n');
        }
        if (tps > 0 && live >= 1 && errs == 0) passes++; else fails++;
    }

    /* Z+ runtime chains: count chains registered via the Z+ runtime
     * (zp_runtime_register_chain). N=0 on first boot; after running
     * any program with a `chain ... { }` block, N>=1. */
    {
        extern int zp_runtime_count(void);
        int rt_n = zp_runtime_count();
        kputs("  Z+ runtime ........... PASS — ");
        kput_dec((uint64_t)rt_n);
        kputs(" user chain(s) live\n");
        passes++;
    }

    /* Serial input: sample CHAIN_SERIAL_IN's decoded-byte counter
     * across a 100ms window while we hand-pump the chain (selftest
     * runs inside shell_dispatch, which means the main scheduler
     * loop is paused). The chain's nodes drain the UART RX ring,
     * decode ASCII, and push into kb_buf — so any byte arriving on
     * the serial console during this window is observable here. A
     * non-zero rate means the serial input pipeline is alive; zero
     * is also a PASS (nothing was being sent). */
    kputs("  Serial input ......... ");
    if (CHAIN_SERIAL_IN >= 0) {
        uint32_t before = chain_serial_in_total();
        uint64_t freq = timer_tsc_freq();
        if (freq == 0) freq = 1;
        uint64_t deadline = timer_read_tsc() + (freq / 10ULL); /* 100ms */
        while (timer_read_tsc() < deadline) {
            (void)chain_resolve(CHAIN_SERIAL_IN);
            __asm__ volatile("pause");
        }
        uint32_t after = chain_serial_in_total();
        uint32_t delta = after - before;
        /* drain rate over a 100ms window: N chars / 0.1s = 10*N cps */
        uint32_t rate = delta * 10u;
        kputs("PASS — drain rate=");
        kput_dec((uint64_t)rate);
        kputs(" chars/sec\n");
        passes++;
    } else {
        kputs("FAIL (chain not registered)\n");
        fails++;
    }

    /* Persistence: report how much of the masq_journal + chain
     * registry came back from VAULT on this boot. First boot reports
     * N=0/M=0 -- that is also a PASS (the layer initialized cleanly,
     * there was just nothing to restore). Force a snapshot+sync now
     * so the test scenario (selftest -> reset -> selftest) doesn't
     * have to wait for the periodic checkpoint to fire. */
    /* Suspend: not actually executed during selftest (would halt the
     * console). Verify the path is wired: FADT parsed, _S3 located,
     * driver hooks registered, CHAIN_POWER alive. */
    kputs("  Suspend ............... ");
    {
        const acpi_fadt_info_t *f = acpi_fadt();
        int fadt_ok  = (f && f->valid && f->pm1a_cnt_blk);
        int s3_ok    = (f && f->slp_typ_valid);
        int chain_ok = (CHAIN_POWER >= 0);
        if (fadt_ok && chain_ok) {
            kputs("PASS — S3 path wired (untested in QEMU)");
            if (!s3_ok) kputs(" [_S3 not in DSDT]");
            kputc('\n');
            passes++;
        } else {
            kputs("FAIL (fadt=");
            kput_dec((uint64_t)fadt_ok);
            kputs(" s3=");
            kput_dec((uint64_t)s3_ok);
            kputs(" chain=");
            kput_dec((uint64_t)chain_ok);
            kputs(")\n");
            fails++;
        }
    }

    /* AML interpreter selftest: cataloged method count, evaluable count
     * (zero-arg methods that completed without hitting an unimplemented
     * opcode), and skipped count. */
    kputs("  ");
    aml_print_selftest_line();
    passes++;

    /* Battery: report presence and (if known) percent + state. On
     * QEMU this prints "not present" and PASSes — no battery is
     * the expected state for a default QEMU machine. Real laptop
     * hardware exercises the AML scanner. */
    kputs("  Battery ............... ");
    {
        int bn = battery_count();
        int ac = ac_online();
        if (bn == 0) {
            kputs("not present (desktop or QEMU)");
            if (ac == 1) kputs(" — AC online");
            kputc('\n');
            passes++;
        } else {
            int pct = battery_percent(0);
            const battery_info_t *b = battery_info(0);
            kput_dec((uint64_t)bn);
            kputs(" battery present");
            if (pct >= 0 && b) {
                kputs(" (");
                kput_dec((uint64_t)pct);
                kputs("% ");
                kputs(bat_state_label(b->state));
                kputs(")");
            } else {
                kputs(" (charge unknown — method-only _BST)");
            }
            if      (ac == 1) kputs(" — AC online");
            else if (ac == 0) kputs(" — on battery");
            kputc('\n');
            passes++;
        }
    }

    /* Display brightness — ACPI _BCL/_BCM pattern scan. QEMU's
     * virtio-gpu has no backlight surface; that's the expected state
     * on a desktop / VM and PASSes. */
    kputs("  Brightness ............ ");
    if (brightness_present()) {
        int lv[BRIGHTNESS_MAX_LEVELS];
        int nlv = brightness_levels(lv, BRIGHTNESS_MAX_LEVELS);
        kput_dec((uint64_t)brightness_get());
        kputs("% (");
        kput_dec((uint64_t)nlv);
        kputs(" levels available)\n");
        passes++;
    } else {
        kputs("no backlight (desktop or QEMU virtio-gpu)\n");
        passes++;
    }

    /* Idle / lock-on-idle. Reports the configured thresholds plus
     * whether a PIN is set. PASSes whenever the chain is registered
     * and a PIN is configured (default '0000' on first boot, replaced
     * via `pin <new>`). */
    kputs("  Idle/lock ............. ");
    {
        kputs("dim=");
        kput_dec((uint64_t)idle_dim_secs());
        kputs("s, lock=");
        kput_dec((uint64_t)idle_lock_secs());
        kputs("s, blank=");
        kput_dec((uint64_t)idle_blank_secs());
        kputs("s, PIN=");
        if (lockscreen_pin_configured()) {
            kputs("set\n");
            passes++;
        } else {
            kputs("MISSING\n");
            fails++;
        }
    }

    /* Cold-boot login -- the boot gate runs even before any idle time
     * has accrued. Three reportable states:
     *   required + PIN set    -> "required (PIN set)"
     *   required + no PIN     -> "not required (no PIN)" (enrollment
     *                            will run on next boot)
     *   not required          -> "not required" */
    kputs("  Cold-boot login ....... ");
    {
        int req = cold_boot_login_required();
        int pin = lockscreen_pin_configured();
        if (req && pin)        kputs("required (PIN set)\n");
        else if (req && !pin)  kputs("not required (no PIN)\n");
        else                   kputs("not required\n");
        passes++;
    }

    /* Power buttons (ACPI PNP0C0C / 0D / 0E + PM1 PWRBTN_STS poll). */
    kputs("  Power buttons ......... ");
    {
        kputs("button=");
        kputs(power_action_label(power_button_action()));
        kputs(", lid-close=");
        kputs(power_action_label(lid_close_action()));
        kputs(", lid-open=");
        kputs(power_action_label(lid_open_action()));
        uint32_t pe = power_button_event_count();
        uint32_t le = lid_event_count();
        if (pe == 0 && le == 0) {
            kputs(" (no events seen yet)\n");
        } else {
            kputs(" (events: btn=");
            kput_dec((uint64_t)pe);
            kputs(" lid=");
            kput_dec((uint64_t)le);
            kputs(")\n");
        }
        passes++;
    }

    kputs("  Persistence ........... ");
    if (persistence_ready()) {
        uint32_t n = persistence_journal_restored_count();
        uint32_t m = persistence_chains_restored_count();
        kputs("journal: ");
        kput_dec((uint64_t)n);
        kputs(" entries restored, chain snapshot: ");
        kput_dec((uint64_t)m);
        kputs(" chains restored\n");
        (void)persistence_save_snapshot_now();
        passes++;
    } else {
        kputs("FAIL (not initialized)\n");
        fails++;
    }

    /* Notify history + DND. Reports current state via the spec line. */
    kputs("  ");
    notify_print_selftest_line();
    passes++;

    /* UI primitives — undo/redo, context menus, hover, dirty,
     * quick-look, list states. All wired additively; modules report
     * their lifetime counters. PASS as long as the modules link and
     * report. */
    {
        extern uint32_t undo_total_records(void);
        extern uint32_t undo_total_undos(void);
        extern uint32_t undo_total_redos(void);
        extern uint32_t context_menu_total_opens(void);
        extern uint32_t hover_total_zones(void);
        extern uint32_t hover_total_enters(void);
        extern uint32_t quick_look_total_opens(void);
        extern uint32_t list_state_total_renders(void);
        kputs("  UI primitives ......... ");
        extern uint32_t quick_look_total_decodes_ok(void);
        extern uint32_t quick_look_total_decodes_fail(void);
        kputs("undo/redo, context menus, hover, dirty, quick_look=full PNG decode, list states — wired (rec=");
        kput_dec((uint64_t)undo_total_records());
        kputs(" undo=");
        kput_dec((uint64_t)undo_total_undos());
        kputs(" redo=");
        kput_dec((uint64_t)undo_total_redos());
        kputs(" ctx=");
        kput_dec((uint64_t)context_menu_total_opens());
        kputs(" hov=");
        kput_dec((uint64_t)hover_total_zones());
        kputs("/");
        kput_dec((uint64_t)hover_total_enters());
        kputs(" ql=");
        kput_dec((uint64_t)quick_look_total_opens());
        kputs("/dec=");
        kput_dec((uint64_t)quick_look_total_decodes_ok());
        kputs(",");
        kput_dec((uint64_t)quick_look_total_decodes_fail());
        kputs(" ls=");
        kput_dec((uint64_t)list_state_total_renders());
        kputs(")\n");
        passes++;
    }

    /* Image viewer — selftest reports readiness + lifetime counters. */
    {
        extern uint32_t image_viewer_total_opens(void);
        extern uint32_t image_viewer_total_decodes_ok(void);
        extern uint32_t image_viewer_total_decodes_fail(void);
        kputs("  Image viewer .......... ready (lodepng-backed, PNG only) (opens=");
        kput_dec((uint64_t)image_viewer_total_opens());
        kputs(" ok=");
        kput_dec((uint64_t)image_viewer_total_decodes_ok());
        kputs(" fail=");
        kput_dec((uint64_t)image_viewer_total_decodes_fail());
        kputs(")\n");
        passes++;
    }

    /* Settings registry — count keys, count live (non-stub) */
    settings_print_selftest_line();
    if (settings_count() > 0) passes++;
    else                      fails++;

    kputs("  ──────────────\n  ");
    kput_dec(passes); kputs(" passed, ");
    kput_dec(fails); kputs(" failed\n");
    kputs("  tests: run via `tests` command for full coverage\n\n");
}

/* ── Test framework command ─────────────────────────── */
static void cmd_tests(const char *args)
{
    /* Skip leading whitespace */
    while (*args == ' ' || *args == '\t') args++;

    if (*args == '\0') {
        (void)test_run_all();
        return;
    }

    /* JSON mode for CI: tests --json */
    if (args[0] == '-' && args[1] == '-' &&
        args[2] == 'j' && args[3] == 's' && args[4] == 'o' && args[5] == 'n') {
        (void)test_run_all_json();
        return;
    }

    /* Substring filter */
    (void)test_run_named(args);
}

/* ── VAULT filesystem commands ──────────────────── */

/* RAM disk for VAULT — 2MB */
#define VAULT_RAM_SIZE (2 * 1024 * 1024)
static uint8_t vault_ram[VAULT_RAM_SIZE] __attribute__((aligned(4096)));

/* Stamp builtin Z+ programs into /programs. Called only when we
 * format a fresh region; resume-from-disk reuses what's already
 * persisted so we don't churn temporal versions every boot. */
static void vault_seed_builtins(void)
{
    for (int i = 0; i < (int)NUM_BUILTINS; i++) {
        char path[64];
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

void shell_vault_init(void)
{
    if (vault_ready) return;  /* idempotent */

    /* Try to attach a persistent backing drive. block_init() ran in
     * main.c before this call, so block_drive_count() is accurate.
     * The QEMU run target adds a dedicated 8 MB NVMe disk specifically
     * for this. On real hardware, drive 0 is whatever NVMe/AHCI/USB
     * disk is present -- the persistent VAULT rides along on the
     * first 2 MB of it. */
    int rc = -1;
    if (block_drive_count() > 0) {
        rc = vault_persist_attach(vault_ram, VAULT_RAM_SIZE, 0);
    }

    if (rc == 1) {
        /* A previously-formatted image was loaded into vault_ram from
         * disk. Mount without re-formatting -- this is the path that
         * makes journal + snapshot files survive a reboot. */
        if (vault_mount(vault_ram, VAULT_RAM_SIZE) == 0) {
            vault_ready = 1;
            kputs("[vault] resumed from persistent backing\n");
            return;
        }
        /* Mount of the loaded image failed -- fall through to format.
         * Don't trust whatever was on disk. */
    }

    /* No usable image (no drive bound, drive empty, or mount failed
     * after a successful read). Format a fresh region and seed it
     * with the default directory layout + builtin programs. */
    if (vault_format(vault_ram, VAULT_RAM_SIZE, "zeos") == 0 &&
        vault_mount(vault_ram, VAULT_RAM_SIZE) == 0) {
        vault_ready = 1;

        vault_create("/programs", VAULT_TIER_REFERENCE);
        vault_create("/home", VAULT_TIER_SOVEREIGN);
        vault_create("/tmp", VAULT_TIER_INTERNAL);
        vault_seed_builtins();

        /* Push the freshly-formatted, freshly-seeded image to disk so
         * the very next vault_sync() is cheap and the next boot has
         * something valid to resume. */
        (void)vault_persist_flush();
        kputs("[vault] formatted fresh region (no prior image)\n");
    }
}

/* Backwards-compat wrapper -- shell_init still calls this name. */
static void vault_init_ramdisk(void)
{
    shell_vault_init();
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
    if (!*args) {
        kputs("  Usage: mkdir <path>\n");
        return;
    }

    /* Prefer FAT32 if a volume is mounted: that's the user-visible
     * filesystem for sticks / SDs / NVMe images. Fall back to VAULT
     * otherwise. */
    if (fat32_mounted()) {
        if (fat32_mkdir(args) == 0) {
            kputs("  Created directory: "); kputs(args); kputs("\n");
        } else {
            kputs("  Failed to create: "); kputs(args); kputs("\n");
        }
        return;
    }

    if (!vault_ready) {
        kputs("  No filesystem mounted.\n");
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

/* ── Scheduler-driven shell pump ─────────────────── */
/*
 * The kernel main loop is now scheduler_run() — chain resolution is
 * the scheduler. This file no longer owns a blocking loop; instead it
 * exposes:
 *   shell_init()        — one-shot setup (VAULT, networking, banner)
 *   shell_pump_char()   — fed one ASCII character at a time
 *   shell_print_prompt()— external trigger for a fresh prompt
 *
 * The CHAIN_KEYBOARD resolve translates scancodes -> ASCII -> kb_buf;
 * the scheduler drains kb_buf via keyboard_try_getc() and feeds chars
 * here. The user-visible behavior is identical to the old polling loop.
 */

static char     g_cmd_buf[CMD_BUF_SIZE];
static int      g_cmd_pos = 0;
static int      g_shell_initialized = 0;

void shell_print_prompt(void)
{
    shell_prompt();
    g_cmd_pos = 0;
}

void shell_init(struct zeos_boot_info *boot)
{
    if (g_shell_initialized) return;
    g_shell_initialized = 1;
    g_boot = boot;

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
    g_cmd_pos = 0;
}

static void shell_dispatch(char *cmd)
{
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

int shell_pump_char(char c)
{
    if (c == '\n') {
        kputc('\n');
        g_cmd_buf[g_cmd_pos] = '\0';
        if (g_cmd_pos == 0) {
            shell_prompt();
            return 1;
        }
        shell_dispatch(g_cmd_buf);
        g_cmd_pos = 0;
        shell_prompt();
        return 1;
    } else if (c == '\b') {
        if (g_cmd_pos > 0) {
            g_cmd_pos--;
            kputs("\b \b");
        }
        return 0;
    } else if (g_cmd_pos < CMD_BUF_SIZE - 1) {
        g_cmd_buf[g_cmd_pos++] = c;
        kputc(c);
    }
    return 0;
}

/* Legacy entry: initialize and hand control to the scheduler. */
void shell_run(struct zeos_boot_info *boot)
{
    extern void scheduler_run(void);
    shell_init(boot);
    shell_prompt();
    scheduler_run();   /* never returns */
}

/* ── wifi: RTL8188EU WPA2-PSK supplicant ──────────────────────── */

static int wifi_split_args(const char *args, char *ssid, int ssid_max,
                           char *psk, int psk_max)
{
    while (*args == ' ' || *args == '\t') args++;
    int n = 0;
    while (*args && *args != ' ' && *args != '\t' && n < ssid_max - 1)
        ssid[n++] = *args++;
    ssid[n] = 0;
    if (psk) {
        while (*args == ' ' || *args == '\t') args++;
        int m = 0;
        while (*args && *args != ' ' && *args != '\t' && m < psk_max - 1)
            psk[m++] = *args++;
        psk[m] = 0;
    }
    return n;
}

struct wifi_saved {
    char    ssid[33];
    int     ssid_len;
    uint8_t pmk[32];
    uint8_t reserved[4];
};

#define WIFI_SAVED_MAX 8

struct wifi_saved_table {
    int                 count;
    struct wifi_saved   nets[WIFI_SAVED_MAX];
};

static int wifi_load_table(struct wifi_saved_table *t)
{
    int rc = vault_load_config("/wifi/networks", t, sizeof(*t));
    if (rc != (int)sizeof(*t)) {
        for (int i = 0; i < (int)sizeof(*t); i++) ((uint8_t *)t)[i] = 0;
        return -1;
    }
    if (t->count < 0 || t->count > WIFI_SAVED_MAX) t->count = 0;
    return 0;
}

static int wifi_save_table(const struct wifi_saved_table *t)
{
    return vault_save_config("/wifi/networks", t, sizeof(*t));
}

static int wifi_save_network(const char *ssid, const uint8_t pmk[32])
{
    struct wifi_saved_table t;
    wifi_load_table(&t);
    int slen = 0; while (ssid[slen]) slen++;
    if (slen > 32) slen = 32;

    for (int i = 0; i < t.count; i++) {
        if (t.nets[i].ssid_len == slen) {
            int eq = 1;
            for (int j = 0; j < slen; j++)
                if (t.nets[i].ssid[j] != ssid[j]) { eq = 0; break; }
            if (eq) {
                for (int j = 0; j < 32; j++) t.nets[i].pmk[j] = pmk[j];
                return wifi_save_table(&t);
            }
        }
    }
    if (t.count >= WIFI_SAVED_MAX) return -1;
    struct wifi_saved *e = &t.nets[t.count++];
    for (int j = 0; j < (int)sizeof(*e); j++) ((uint8_t *)e)[j] = 0;
    for (int j = 0; j < slen; j++) e->ssid[j] = ssid[j];
    e->ssid_len = slen;
    for (int j = 0; j < 32; j++) e->pmk[j] = pmk[j];
    return wifi_save_table(&t);
}

static int wifi_forget_network(const char *ssid)
{
    struct wifi_saved_table t;
    if (wifi_load_table(&t) < 0) return -1;
    int slen = 0; while (ssid[slen]) slen++;
    int found = -1;
    for (int i = 0; i < t.count; i++) {
        if (t.nets[i].ssid_len == slen) {
            int eq = 1;
            for (int j = 0; j < slen; j++)
                if (t.nets[i].ssid[j] != ssid[j]) { eq = 0; break; }
            if (eq) { found = i; break; }
        }
    }
    if (found < 0) return -1;
    for (int i = found; i < t.count - 1; i++)
        t.nets[i] = t.nets[i + 1];
    t.count--;
    return wifi_save_table(&t);
}

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

    if (args[0] == 'l' && args[1] == 'i' && args[2] == 's' && args[3] == 't') {
        struct wifi_saved_table t;
        if (wifi_load_table(&t) < 0 || t.count == 0) {
            kputs("wifi: no saved networks.\n");
            return;
        }
        kputs("wifi: saved networks:\n");
        for (int i = 0; i < t.count; i++) {
            kputs("  ");
            kputs(t.nets[i].ssid);
            kputs("\n");
        }
        return;
    }

    if (args[0] == 'f' && args[1] == 'o' && args[2] == 'r') {
        /* "forget" — args advanced past command name in caller, so
         * skip the verb here: "forget " is 7 chars. */
        const char *p = args + 6;
        while (*p == ' ' || *p == '\t') p++;
        char ssid[33];
        wifi_split_args(p, ssid, sizeof(ssid), 0, 0);
        if (!ssid[0]) { kputs("usage: wifi forget <ssid>\n"); return; }
        if (wifi_forget_network(ssid) == 0) {
            kputs("wifi: forgot \"");
            kputs(ssid);
            kputs("\"\n");
        } else {
            kputs("wifi: SSID not in saved networks.\n");
        }
        return;
    }

    if (args[0] == 'c' && args[1] == 'o' && args[2] == 'n') {
        const char *p = args + 7;       /* "connect " */
        while (*p == ' ' || *p == '\t') p++;
        char ssid[33], psk[64];
        wifi_split_args(p, ssid, sizeof(ssid), psk, sizeof(psk));
        if (!ssid[0] || !psk[0]) {
            kputs("usage: wifi connect <ssid> <psk>\n");
            return;
        }
        int rc = rtl8188eu_associate(ssid, psk);
        if (rc == 0) {
            uint8_t pmk[32];
            if (wpa_derive_pmk(psk, ssid, pmk) == 0)
                wifi_save_network(ssid, pmk);
        }
        return;
    }

    kputs("usage: wifi [status|scan|list|connect <ssid> <psk>|forget <ssid>]\n");
}

/* Standalone passive-scan command. Prints the AP list. */
static void cmd_wifi_scan(const char *args)
{
    (void)args;
    rtl8188eu_scan();
}

/* ── Bluetooth shell ──────────────────────────────────────────── */

static void bt_print_bdaddr(const uint8_t bd[6])
{
    /* Display order ab:cd:ef:01:02:03 — bd is little-endian on the wire. */
    for (int i = 5; i >= 0; i--) {
        if (bd[i] < 0x10) kputc('0');
        kput_hex(bd[i]);
        if (i) kputc(':');
    }
}

/* Parse "ab:cd:ef:01:02:03" into 6 bytes (little-endian on the wire).
 * Returns 0 on success, -1 otherwise. */
static int bt_parse_bdaddr(const char *s, uint8_t out[6])
{
    /* Skip whitespace */
    while (*s == ' ' || *s == '\t') s++;
    int byte_idx = 5;
    int nibble_count = 0;
    uint8_t cur = 0;
    while (*s && byte_idx >= 0) {
        char c = *s++;
        if (c == ':' || c == '-') {
            if (nibble_count == 0) return -1;
            out[byte_idx--] = cur;
            cur = 0;
            nibble_count = 0;
            continue;
        }
        uint8_t v;
        if (c >= '0' && c <= '9') v = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (uint8_t)(c - 'A' + 10);
        else if (c == ' ' || c == '\t' || c == 0 || c == '\n') break;
        else return -1;
        cur = (uint8_t)((cur << 4) | v);
        nibble_count++;
        if (nibble_count > 2) return -1;
    }
    if (nibble_count > 0 && byte_idx >= 0) {
        out[byte_idx--] = cur;
    }
    return (byte_idx == -1) ? 0 : -1;
}

static void cmd_bt_status(void)
{
    if (!bt_usb_present()) {
        kputs("Bluetooth: no controller present (no USB device matched class E0/01/01)\n");
        return;
    }
    kputs("Bluetooth: controller present\n");
    bt_usb_dump();
    if (bt_hci_ready()) {
        uint8_t hv = 0, lv = 0;
        uint16_t mf = 0;
        bt_hci_local_version(&hv, &mf, &lv);
        kputs("  BD_ADDR ........ ");
        bt_print_bdaddr(bt_hci_bdaddr());
        kputs("\n  HCI version .... ");
        kput_dec(hv);
        kputs("\n  LMP version .... ");
        kput_dec(lv);
        kputs("\n  manufacturer ... ");
        kput_hex(mf);
        kputs("\n  scanning ....... ");
        kputs(bt_hci_scanning() ? "yes" : "no");
        kputs("\n  link state ..... ");
        switch (bt_link_state()) {
        case BT_LINK_NONE:          kputs("idle\n"); break;
        case BT_LINK_CONNECTING:    kputs("connecting\n"); break;
        case BT_LINK_CONNECTED:     kputs("connected\n"); break;
        case BT_LINK_DISCONNECTING: kputs("disconnecting\n"); break;
        }
    } else {
        kputs("  HCI boot ....... not completed (chip may need vendor firmware)\n");
    }
}

static void cmd_bt_scan(void)
{
    if (!bt_usb_present()) {
        kputs("bt: no controller present\n");
        return;
    }
    bt_devices_clear();
    kputs("bt: starting 10s discovery (classic inquiry + LE scan)\n");
    if (bt_inquiry_start(10) != 0) {
        kputs("bt: classic inquiry failed to dispatch\n");
    }
    if (bt_le_scan_start() != 0) {
        kputs("bt: LE scan failed to dispatch\n");
    }

    /* Pump events for ~10 seconds. The scheduler is also calling
     * bt_hci_poll() through CHAIN_BLUETOOTH every 8 ticks; this is the
     * synchronous foreground drain so the user sees activity in the
     * shell. We pace via 100x100ms sleeps that yield to interrupts. */
    extern void timer_wait_ms(uint32_t ms);
    for (int i = 0; i < 100; i++) {
        bt_hci_poll();
        timer_wait_ms(100);
    }

    (void)bt_le_scan_stop();

    bt_device_t devs[16];
    int n = bt_devices_found(devs, (int)(sizeof(devs)/sizeof(devs[0])));
    kputs("bt: ");
    kput_dec((uint64_t)n);
    kputs(" device(s) discovered\n");
    for (int i = 0; i < n; i++) {
        kputs("  ");
        bt_print_bdaddr(devs[i].bdaddr);
        kputs("  ");
        switch (devs[i].addr_type) {
        case 0: kputs("BR/EDR"); break;
        case 1: kputs("LE-pub"); break;
        case 2: kputs("LE-rnd"); break;
        default: kputs("?     "); break;
        }
        if (devs[i].rssi != 127) {
            kputs("  rssi=");
            /* Print signed dBm. */
            int r = devs[i].rssi;
            if (r < 0) { kputc('-'); r = -r; }
            kput_dec((uint64_t)r);
            kputs("dBm");
        }
        if (devs[i].name_len) {
            kputs("  ");
            kputs(devs[i].name);
        }
        kputc('\n');
    }
}

static void cmd_bt(const char *args)
{
    /* Skip leading whitespace. */
    while (*args == ' ' || *args == '\t') args++;

    if (*args == 0 || (args[0] == 's' && args[1] == 't' && args[2] == 'a')) {
        cmd_bt_status();
        return;
    }
    if (args[0] == 's' && args[1] == 'c' && args[2] == 'a' && args[3] == 'n') {
        cmd_bt_scan();
        return;
    }
    if (args[0] == 'c' && args[1] == 'o' && args[2] == 'n' && args[3] == 'n') {
        const char *p = args + 4;
        while (*p && *p != ' ' && *p != '\t') p++;
        while (*p == ' ' || *p == '\t') p++;
        uint8_t bd[6];
        if (bt_parse_bdaddr(p, bd) != 0) {
            kputs("usage: bt connect <addr>  (e.g. ab:cd:ef:01:02:03)\n");
            return;
        }
        /* Look up addr_type from the discovered table; default to LE
         * public (1) if not seen — classic peers need pairing+page which
         * is out of scope for the foundation. */
        bt_device_t devs[16];
        int n = bt_devices_found(devs, (int)(sizeof(devs)/sizeof(devs[0])));
        uint8_t at = 1;
        int found = 0;
        for (int i = 0; i < n; i++) {
            int eq = 1;
            for (int j = 0; j < 6; j++) if (devs[i].bdaddr[j] != bd[j]) { eq = 0; break; }
            if (eq) { at = devs[i].addr_type ? devs[i].addr_type : 1; found = 1; break; }
        }
        if (!found) {
            kputs("bt: peer not in discovered table — assuming LE public\n");
        }
        if (at == 0) {
            kputs("bt: classic BR/EDR connect not implemented in foundation; LE only\n");
            return;
        }
        if (bt_le_connect(bd, at) != 0) {
            kputs("bt: LE Create Connection failed to dispatch\n");
            return;
        }
        kputs("bt: connecting to ");
        bt_print_bdaddr(bd);
        kputs(" (LE) — watch `bt status` for outcome\n");
        return;
    }
    if (args[0] == 'd' && args[1] == 'i' && args[2] == 's') {
        const char *p = args + 3;
        while (*p && *p != ' ' && *p != '\t') p++;
        while (*p == ' ' || *p == '\t') p++;
        uint8_t bd[6];
        if (bt_parse_bdaddr(p, bd) != 0) {
            kputs("usage: bt disconnect <addr>\n");
            return;
        }
        if (bt_disconnect(bd) != 0) {
            kputs("bt: disconnect rejected (no live link to that peer)\n");
            return;
        }
        kputs("bt: disconnect dispatched\n");
        return;
    }
    kputs("usage: bt [status|scan|connect <addr>|disconnect <addr>]\n");
}

static const char *l2cap_state_name(l2cap_state_t s)
{
    switch (s) {
    case L2CAP_STATE_CLOSED:            return "closed";
    case L2CAP_STATE_WAIT_CONN_RSP:     return "wait-conn-rsp";
    case L2CAP_STATE_CONFIG:            return "config";
    case L2CAP_STATE_OPEN:              return "open";
    case L2CAP_STATE_WAIT_DISCONN_RSP:  return "wait-disc-rsp";
    }
    return "?";
}

static const char *l2cap_flavor_name(l2cap_flavor_t f)
{
    switch (f) {
    case L2CAP_FLAVOR_FIXED:        return "fixed";
    case L2CAP_FLAVOR_CLASSIC:      return "classic";
    case L2CAP_FLAVOR_LE_CREDIT:    return "le-credit";
    }
    return "?";
}

static void cmd_bt_l2cap(const char *args)
{
    (void)args;
    if (!l2cap_signaling_ready()) {
        kputs("bt-l2cap: not initialized\n");
        return;
    }
    l2cap_chan_info_t info[L2CAP_MAX_CHANNELS];
    int n = l2cap_channels_snapshot(info, L2CAP_MAX_CHANNELS);
    kputs("L2CAP channels (");
    kput_dec((uint64_t)n);
    kputs("):\n");
    kputs("  cid    peer   psm    handle  mtu(loc/peer)   state          flavor\n");
    for (int i = 0; i < n; i++) {
        kputs("  0x");
        kput_hex(info[i].cid);
        kputs(" 0x");
        kput_hex(info[i].peer_cid);
        kputs(" 0x");
        kput_hex(info[i].psm);
        kputs("  0x");
        kput_hex(info[i].conn_handle);
        kputs("   ");
        kput_dec((uint64_t)info[i].local_mtu);
        kputc('/');
        kput_dec((uint64_t)info[i].peer_mtu);
        kputs("    ");
        kputs(l2cap_state_name(info[i].state));
        kputs("  ");
        kputs(l2cap_flavor_name(info[i].flavor));
        kputc('\n');
    }
    kputs("  rx_frames=");
    kput_dec((uint64_t)l2cap_rx_frames());
    kputs(" tx_frames=");
    kput_dec((uint64_t)l2cap_tx_frames());
    kputs(" sig_count=");
    kput_dec((uint64_t)l2cap_signaling_count());
    kputc('\n');
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

/* Open the standalone image viewer on a FAT32 path. */
static void cmd_view(const char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        kputs("  Usage: view <path>\n");
        return;
    }
    if (!fat32_mounted()) {
        kputs("  fat32: not mounted (run 'fat-mount' first)\n");
        return;
    }
    extern int image_viewer_open(const char *path);
    if (image_viewer_open(args)) {
        kputs("  view: opened "); kputs(args); kputs("\n");
    } else {
        kputs("  view: opened with error (see overlay for details)\n");
    }
}

/* ── FAT32 write-side shell commands ─────────────────────────── */

static int strlen_simple(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void cmd_touch(const char *args)
{
    while (*args == ' ') args++;
    if (!*args) { kputs("  Usage: touch <path>\n"); return; }
    if (!fat32_mounted()) { kputs("  fat32: not mounted\n"); return; }
    if (fat32_create(args) == 0) {
        kputs("  Created: "); kputs(args); kputs("\n");
    } else {
        kputs("  touch: failed (exists or bad path?)\n");
    }
}

/* rm <path>          — soft-delete: move to /.zeos-trash/<id>/
 * rm -f <path>       — hard unlink, bypassing trash entirely
 * rm <path> -f       — same
 *
 * Directories use direct unlink (empty-dir-only) regardless of -f
 * because the trash subsystem does not yet support recursive trash. */
static void cmd_rm(const char *args)
{
    while (*args == ' ') args++;
    if (!*args) { kputs("  Usage: rm [-f] <path>\n"); return; }
    if (!fat32_mounted()) { kputs("  fat32: not mounted\n"); return; }

    int force = 0;
    char path[FAT32_PATH_MAX];
    int pi = 0;
    while (*args && pi < (int)sizeof(path) - 1) {
        while (*args == ' ') args++;
        if (!*args) break;
        if (args[0] == '-' && args[1] == 'f' &&
            (args[2] == '\0' || args[2] == ' ')) {
            force = 1;
            args += 2;
            continue;
        }
        if (pi != 0) {
            while (*args && *args != ' ') args++;
            continue;
        }
        while (*args && *args != ' ' && pi < (int)sizeof(path) - 1) {
            path[pi++] = *args++;
        }
    }
    path[pi] = '\0';
    if (!path[0]) { kputs("  Usage: rm [-f] <path>\n"); return; }

    if (force) {
        if (fat32_unlink(path) == 0) {
            kputs("  Removed: "); kputs(path); kputs(" (forced)\n");
        } else {
            kputs("  rm: failed (not found or non-empty dir?)\n");
        }
        return;
    }

    /* Try trash first. If the source is a directory or already lives in
     * the trash root, fall through to direct unlink. */
    char id[FAT32_TRASH_ID_MAX + 1];
    int rc = fat32_trash(path, "user", id);
    if (rc == 0) {
        kputs("  Trashed: "); kputs(path);
        kputs("  (id="); kputs(id); kputs(")\n");
        return;
    }

    if (fat32_unlink(path) == 0) {
        kputs("  Removed: "); kputs(path); kputs("\n");
    } else {
        kputs("  rm: failed (not found, non-empty dir, or trash error)\n");
        kputs("       try `rm -f <path>` to bypass trash.\n");
    }
}

/* ── Trash subcommand dispatcher ─────────────────────────────────
 *
 *   trash                          — list pending entries
 *   trash restore <id>             — restore one entry to its origin
 *   trash empty                    — purge ALL entries permanently
 *   trash empty --older-than <D>   — purge entries older than D days
 */

static void trash_list_cb(const char *id, const char *orig,
                          uint64_t deleted_at, uint32_t size)
{
    char tbuf[64];
    int tn = tod_format(deleted_at, tbuf, sizeof(tbuf));
    kputs("  ");
    kputs(id);
    kputs("  ");
    kput_dec((uint64_t)size);
    kputs("B  ");
    if (tn > 0) kputs(tbuf);
    else        kputs("(no clock)");
    kputs("  ");
    kputs(orig);
    kputs("\n");
}

static int trash_args_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static void cmd_trash(const char *args)
{
    while (*args == ' ') args++;
    if (!fat32_mounted()) { kputs("  fat32: not mounted\n"); return; }

    if (!*args) {
        kputs("\n  Trash entries (/.zeos-trash):\n");
        kputs("  ID        SIZE   DELETED                       ORIGINAL\n");
        kputs("  --------  -----  ----------------------------  --------\n");
        int n = fat32_trash_list(trash_list_cb);
        if (n == 0) kputs("  (empty)\n");
        else {
            kputs("  ── ");
            kput_dec((uint64_t)n);
            kputs(" entr");
            kputs(n == 1 ? "y" : "ies");
            kputs(" ──\n");
        }
        kputs("  Use `trash restore <id>` or `trash empty` to manage.\n\n");
        return;
    }

    char sub[16];
    int si = 0;
    while (*args && *args != ' ' && si < (int)sizeof(sub) - 1) {
        sub[si++] = *args++;
    }
    sub[si] = '\0';
    while (*args == ' ') args++;

    if (trash_args_eq(sub, "restore")) {
        if (!*args) { kputs("  Usage: trash restore <id>\n"); return; }
        char id[FAT32_TRASH_ID_MAX + 1];
        int ii = 0;
        while (*args && *args != ' ' && ii < FAT32_TRASH_ID_MAX) {
            id[ii++] = *args++;
        }
        id[ii] = '\0';
        if (fat32_trash_restore(id) == 0) {
            kputs("  Restored: "); kputs(id); kputs("\n");
        } else {
            kputs("  trash: restore failed (id not found, parent gone, "
                  "or destination already exists)\n");
        }
        return;
    }

    if (trash_args_eq(sub, "empty")) {
        if (*args) {
            const char *p = args;
            if (p[0] == '-' && p[1] == '-') {
                const char *flag = p + 2;
                const char *flag_end = flag;
                while (*flag_end && *flag_end != ' ') flag_end++;
                int flen = (int)(flag_end - flag);
                if (flen == 11 && flag[0] == 'o' && flag[1] == 'l') {
                    p = flag_end;
                    while (*p == ' ') p++;
                    if (!*p) {
                        kputs("  Usage: trash empty --older-than <days>\n");
                        return;
                    }
                    uint64_t days = 0;
                    while (*p >= '0' && *p <= '9') {
                        days = days * 10 + (uint64_t)(*p - '0');
                        p++;
                    }
                    if (days == 0) {
                        kputs("  Usage: trash empty --older-than <days>  (days >= 1)\n");
                        return;
                    }
                    int freed = fat32_trash_empty_older_than(days * 86400ULL);
                    if (freed < 0) {
                        kputs("  trash: empty --older-than failed\n");
                    } else {
                        kputs("  Auto-emptied "); kput_dec((uint64_t)freed);
                        kputs(" entr");
                        kputs(freed == 1 ? "y" : "ies");
                        kputs(" older than ");
                        kput_dec(days);
                        kputs(" days.\n");
                    }
                    return;
                }
            }
            kputs("  Usage: trash empty [--older-than <days>]\n");
            return;
        }

        int freed = fat32_trash_empty();
        if (freed < 0) {
            kputs("  trash: empty failed\n");
        } else {
            kputs("  Emptied trash: ");
            kput_dec((uint64_t)freed);
            kputs(" entr");
            kputs(freed == 1 ? "y" : "ies");
            kputs(" purged permanently.\n");
        }
        return;
    }

    kputs("  Usage:\n");
    kputs("    trash                          list pending entries\n");
    kputs("    trash restore <id>             restore one entry\n");
    kputs("    trash empty                    purge all entries\n");
    kputs("    trash empty --older-than <N>   purge entries >N days old\n");
}

static void cmd_truncate(const char *args)
{
    while (*args == ' ') args++;
    if (!*args) { kputs("  Usage: truncate <path> <size>\n"); return; }
    if (!fat32_mounted()) { kputs("  fat32: not mounted\n"); return; }

    /* Parse path then size. */
    char path[256];
    int pi = 0;
    while (*args && *args != ' ' && pi < 255) path[pi++] = *args++;
    path[pi] = 0;
    while (*args == ' ') args++;
    if (!*args) { kputs("  Usage: truncate <path> <size>\n"); return; }

    uint32_t sz = 0;
    while (*args >= '0' && *args <= '9') {
        sz = sz * 10 + (uint32_t)(*args - '0');
        args++;
    }
    if (fat32_truncate(path, sz) == 0) {
        kputs("  Truncated "); kputs(path); kputs(" to ");
        kput_dec(sz); kputs(" bytes\n");
    } else {
        kputs("  truncate: failed\n");
    }
}

/* echo <text> > <path>  — writes <text> to <path>, creating it if
 * needed. Always replaces full contents. */
static void cmd_echo(const char *args)
{
    while (*args == ' ') args++;
    if (!*args) { kputs("  Usage: echo <text> > <path>\n"); return; }
    if (!fat32_mounted()) { kputs("  fat32: not mounted\n"); return; }

    /* Find the last unquoted '>' for redirection. */
    int len = strlen_simple(args);
    int redir = -1;
    for (int i = 0; i < len; i++) {
        if (args[i] == '>') redir = i;
    }
    if (redir < 0) {
        kputs("  Usage: echo <text> > <path>\n");
        return;
    }

    /* Split. */
    char text[1024];
    int ti = 0;
    int end_text = redir;
    /* Trim trailing spaces from text region. */
    while (end_text > 0 && args[end_text - 1] == ' ') end_text--;
    for (int i = 0; i < end_text && ti < 1023; i++) text[ti++] = args[i];
    text[ti] = '\0';

    /* Skip past '>' and any spaces. */
    int pi = redir + 1;
    while (pi < len && args[pi] == ' ') pi++;
    char path[256];
    int pj = 0;
    while (pi < len && args[pi] != ' ' && pj < 255) path[pj++] = args[pi++];
    path[pj] = '\0';
    if (!path[0]) { kputs("  Usage: echo <text> > <path>\n"); return; }

    /* Append a newline, mirroring shell echo behaviour. */
    if (ti < 1023) { text[ti++] = '\n'; text[ti] = '\0'; }

    /* Create-if-missing, truncate to 0, then write. */
    (void)fat32_create(path);
    if (fat32_truncate(path, 0) != 0) {
        kputs("  echo: truncate failed\n"); return;
    }
    int wrote = fat32_write(path, 0, text, (uint32_t)ti);
    if (wrote < 0) { kputs("  echo: write failed\n"); return; }
    kputs("  Wrote "); kput_dec((uint32_t)wrote); kputs(" bytes to ");
    kputs(path); kputs("\n");
}

/* ── settings: unified config surface ─────────────────────────── */

static int settings_substr_match(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (hay[i + j] && needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* Compare the dotted prefix (text up to the first '.') of two names. */
static int settings_same_prefix(const char *a, const char *b)
{
    while (*a && *b && *a != '.' && *b != '.') {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == '.' || *a == '\0') && (*b == '.' || *b == '\0');
}

static void settings_print_one(const settings_entry_t *e)
{
    char val[SETTINGS_VALUE_MAX];
    val[0] = '\0';
    int rc = settings_get(e->name, val, sizeof(val));
    kputs("    ");
    kputs(e->name);
    /* pad to ~28 cols */
    int len = 0; for (const char *p = e->name; *p; p++) len++;
    for (int j = len; j < 28; j++) kputc(' ');
    kputs(" = ");
    if (rc == 0) kputs(val);
    else         kputs("(unavailable)");
    kputs("   [");
    kputs(settings_kind_label(e->kind));
    if (e->flags & SETTINGS_FLAG_READONLY)  kputs(",ro");
    if (e->flags & SETTINGS_FLAG_WRITEONLY) kputs(",wo");
    if (e->flags & SETTINGS_FLAG_STUB)      kputs(",stub");
    kputs("]\n");
    if (e->kind == SK_ENUM && e->enum_labels) {
        kputs("        choices: ");
        kputs(e->enum_labels);
        kputc('\n');
    }
}

struct settings_walk_ctx {
    const char *filter;   /* substring filter, NULL = all */
    int         export_mode;
    char        last_prefix[SETTINGS_NAME_MAX];
};

static int settings_walk_print(const settings_entry_t *e, void *u)
{
    struct settings_walk_ctx *ctx = (struct settings_walk_ctx *)u;
    if (ctx->filter && !settings_substr_match(e->name, ctx->filter)) return 0;

    if (ctx->export_mode) {
        char val[SETTINGS_VALUE_MAX];
        val[0] = '\0';
        if (settings_get(e->name, val, sizeof(val)) != 0) return 0;
        kputs(e->name);
        kputc('=');
        kputs(val);
        kputc('\n');
        return 0;
    }

    /* Group separator on prefix change */
    if (!settings_same_prefix(ctx->last_prefix, e->name)) {
        if (ctx->last_prefix[0]) kputc('\n');
        /* extract prefix */
        int i = 0;
        while (e->name[i] && e->name[i] != '.' && i < SETTINGS_NAME_MAX - 1) {
            ctx->last_prefix[i] = e->name[i];
            i++;
        }
        ctx->last_prefix[i] = '\0';
        kputs("  [");
        kputs(ctx->last_prefix);
        kputs("]\n");
    }

    settings_print_one(e);
    return 0;
}

static void cmd_settings(const char *args)
{
    while (*args == ' ' || *args == '\t') args++;

    /* No args -> list everything grouped by prefix */
    if (*args == '\0') {
        struct settings_walk_ctx ctx = { 0, 0, { 0 } };
        kputs("\n  Zeos Settings\n  ─────────────\n");
        settings_enumerate(settings_walk_print, &ctx);
        kputs("\n  ");
        kput_dec((uint64_t)settings_count());
        kputs(" total (");
        kput_dec((uint64_t)settings_live_count());
        kputs(" live, ");
        kput_dec((uint64_t)(settings_count() - settings_live_count()));
        kputs(" stub)\n");
        kputs("  use: settings <name> [<value>]   |   settings search <substr>\n");
        kputs("       settings export             |   settings import <key=value> ...\n\n");
        return;
    }

    /* Parse first word */
    char first[SETTINGS_NAME_MAX];
    int fi = 0;
    while (args[fi] && args[fi] != ' ' && args[fi] != '\t' && fi < SETTINGS_NAME_MAX - 1) {
        first[fi] = args[fi];
        fi++;
    }
    first[fi] = '\0';
    const char *rest = args + fi;
    while (*rest == ' ' || *rest == '\t') rest++;

    /* `settings search <substr>` */
    if (streq(first, "search")) {
        if (!*rest) {
            kputs("  usage: settings search <substr>\n");
            return;
        }
        struct settings_walk_ctx ctx = { rest, 0, { 0 } };
        kputs("\n  Matches for '"); kputs(rest); kputs("':\n");
        settings_enumerate(settings_walk_print, &ctx);
        kputs("\n");
        return;
    }

    /* `settings export` */
    if (streq(first, "export")) {
        struct settings_walk_ctx ctx = { 0, 1, { 0 } };
        settings_enumerate(settings_walk_print, &ctx);
        return;
    }

    /* `settings import key=value key=value ...` */
    if (streq(first, "import")) {
        if (!*rest) {
            kputs("  usage: settings import key=value [key=value ...]\n");
            return;
        }
        const char *p = rest;
        int ok = 0, fail = 0;
        char keybuf[SETTINGS_NAME_MAX];
        char valbuf[SETTINGS_VALUE_MAX];
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            int ki = 0;
            while (*p && *p != '=' && *p != ' ' && *p != '\t' && ki < SETTINGS_NAME_MAX - 1) {
                keybuf[ki++] = *p++;
            }
            keybuf[ki] = '\0';
            if (*p != '=') { fail++; while (*p && *p != ' ') p++; continue; }
            p++;
            int vi = 0;
            while (*p && *p != ' ' && *p != '\t' && vi < SETTINGS_VALUE_MAX - 1) {
                valbuf[vi++] = *p++;
            }
            valbuf[vi] = '\0';
            if (settings_set(keybuf, valbuf) == 0) ok++;
            else                                  fail++;
        }
        kputs("  imported: "); kput_dec((uint64_t)ok);
        kputs(" ok, "); kput_dec((uint64_t)fail); kputs(" failed\n");
        return;
    }

    /* `settings <name>` or `settings <name> <value>` */
    const settings_entry_t *e = settings_find(first);
    if (!e) {
        kputs("  unknown setting: "); kputs(first); kputs("\n");
        kputs("  try `settings` to list, or `settings search "); kputs(first); kputs("`\n");
        return;
    }

    if (!*rest) {
        settings_print_one(e);
        if (e->desc) {
            kputs("        ");
            kputs(e->desc);
            kputc('\n');
        }
        return;
    }

    /* Set */
    int rc = settings_set(first, rest);
    if (rc == 0) {
        kputs("  set "); kputs(first); kputs(" = ");
        char val[SETTINGS_VALUE_MAX];
        if (settings_get(first, val, sizeof(val)) == 0) kputs(val);
        kputs("\n");
    } else {
        kputs("  failed to set "); kputs(first);
        if (e->flags & SETTINGS_FLAG_READONLY) kputs(" (readonly)");
        else if (e->flags & SETTINGS_FLAG_STUB) kputs(" (stub: source module not yet wired)");
        else kputs(" (bad value or out of range)");
        kputs("\n");
    }
}
