/*
 * Zeos -- Chain Registry
 *
 * Wires every subsystem into the chain/MDE graph at boot.
 * Each subsystem's existing code runs inside chain node resolve
 * functions. MDE decides execution order via topological sort.
 * B3 tracks per-chain reliability. MasQ applies to everything.
 *
 * Static state, no malloc, bare-metal C.
 */

#include "chain_registry.h"
#include "chain.h"
#include "mde.h"
#include "hw_discover.h"
#include "compositor.h"
#include "panel.h"
#include "dock.h"
#include "desktop.h"
#include "wm.h"
#include "inspector.h"
#include "palette.h"
#include "hda.h"
#include "net_chain.h"
#include "block_chain.h"
#include "mde_chain.h"
#include "gpu_compute.h"
#include "gpu_virtio.h"
#include "keyboard.h"
#include "mouse.h"
#include "serial.h"
#include "hotplug.h"
#include "suspend.h"
#include "battery.h"
#include "fat32.h"
#include "vault.h"
#include "kprint.h"

/* Forward decls for resolves defined later in this file. */
static void battery_acpi_poll_resolve(chain_node_t *self, void *input, void *output);
static void battery_state_resolve(chain_node_t *self, void *input, void *output);
static void battery_ac_resolve(chain_node_t *self, void *input, void *output);
static void battery_power_event_resolve(chain_node_t *self, void *input, void *output);

/* ── System chain IDs ──────────────────────────────────────────── */

int CHAIN_CPU        = -1;
int CHAIN_COMPOSITOR = -1;
int CHAIN_PANEL      = -1;
int CHAIN_DOCK       = -1;
int CHAIN_DESKTOP    = -1;
int CHAIN_SHELL      = -1;
int CHAIN_BROWSER    = -1;
int CHAIN_INSPECTOR  = -1;
int CHAIN_PALETTE    = -1;
int CHAIN_AUDIO      = -1;
int CHAIN_GPU_0          = -1;
int CHAIN_GPU_0_RENDER   = -1;
int CHAIN_GPU_0_DISPLAY  = -1;
int CHAIN_DISPLAY_GOP    = -1;
int CHAIN_KEYBOARD       = -1;
int CHAIN_MOUSE          = -1;
int CHAIN_SERIAL_IN      = -1;
int CHAIN_POWER          = -1;
int CHAIN_BATTERY        = -1;
int CHAIN_TRASH_GC       = -1;

/* Per-tick counters published by the serial chain so cmd_selftest
 * can sample drain rate over a 100ms window without reaching into
 * the UART itself. */
static volatile uint32_t s_serial_chars_decoded;

/* ── Node resolve functions ────────────────────────────────────── */
/*
 * Each resolve function wraps the existing subsystem code.
 * The chain system calls these during mde_resolve_all().
 * Input/output buffers are scratch space -- the real work
 * happens through the existing global state in each module.
 */

static void compositor_mix_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;

    /* Dirty-rect gate: if nothing has been pushed since the last
     * resolve, skip the entire composite. wm_draw_all() is the
     * 12-31ms hot spot — bypassing it when the UI is idle is what
     * brings the scheduler tick back under budget. */
    int dirty = compositor_consume_dirty();
    int *ok = (int *)output;

    if (dirty == 0) {
        /* Tick the skip counter via the compositor module. */
        extern void compositor_note_skip(void);
        compositor_note_skip();
        if (ok) *ok = 0;
        return;
    }

    /* Real composite. */
    wm_draw_all();

    extern void compositor_note_composite(void);
    compositor_note_composite();
    if (ok) *ok = 1;
}

static void panel_render_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    (void)output;

    panel_update();
    panel_draw();
}

static void dock_render_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    (void)output;

    dock_update();
    dock_draw();
}

static void desktop_render_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    (void)output;

    desktop_draw();
}

static void shell_interpret_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    (void)output;

    /*
     * Shell runs in its own loop (shell_run never returns).
     * This node exists so the shell is visible in the chain graph,
     * inspectable, and has B3 tracking. Actual interpretation
     * happens in the shell's own event loop.
     */
}

static void inspector_inspect_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    (void)output;

    inspector_draw();
}

static void palette_search_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    (void)output;

    palette_draw();
}

/* ── Keyboard chain resolves ───────────────────────────────────── */

static void kbd_irq_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    /* The IRQ handler fills the scancode ring asynchronously. Here we
     * simply publish the pending count so downstream nodes see "how
     * many scancodes are queued" as their typed input. */
    int *out = (int *)output;
    *out = keyboard_chain_pending();
}

static void kbd_decode_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    int pending = input ? *(int *)input : keyboard_chain_pending();
    int *out = (int *)output;
    if (pending <= 0) {
        *out = 0;
        return;
    }
    /* keyboard_chain_drain runs each scancode through the existing
     * keybinds + ASCII pipeline, pushing characters into kb_buf. */
    *out = (int)keyboard_chain_drain();
    if (*out > 0) {
        chain_t *c = chain_get(CHAIN_KEYBOARD);
        if (c) c->vault_version++;
    }
}

static void kbd_event_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)output;
    /* Terminal node: events are now in kb_buf, ready for the scheduler
     * to feed the shell pump. The 'input_event' output type is what
     * MDE auto-routes to the shell, palette, browser etc. */
    int decoded = input ? *(int *)input : 0;
    int *out = (int *)output;
    *out = decoded;
}

/* ── Mouse chain resolves ──────────────────────────────────────── */

static void mouse_irq_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    int *out = (int *)output;
    *out = mouse_chain_pending();
}

static void mouse_decode_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    int pending = input ? *(int *)input : mouse_chain_pending();
    int *out = (int *)output;
    if (pending <= 0) { *out = 0; return; }
    *out = (int)mouse_chain_drain();
    if (*out > 0) {
        chain_t *c = chain_get(CHAIN_MOUSE);
        if (c) c->vault_version++;
    }
}

static void mouse_event_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)output;
    int decoded = input ? *(int *)input : 0;
    int *out = (int *)output;
    *out = decoded;
}

/* ── Serial-input chain resolves ───────────────────────────────── */
/*
 * CHAIN_SERIAL_IN: drains the COM1 RX ring (filled by the serial IRQ
 * and a polling fallback inside serial_rx_pending), translates each
 * byte to an ASCII input_event, and pushes it into the shared shell
 * input buffer (kb_buf via keyboard_inject_char). MDE auto-routes
 * input_event to CHAIN_SHELL and CHAIN_PALETTE the same way it does
 * for CHAIN_KEYBOARD.
 *
 *   Pipeline:  serial_poll  →  ascii_decode  →  input_event
 */

#define SERIAL_CHAIN_RING 16
typedef struct {
    uint8_t  buf[SERIAL_CHAIN_RING];
    int      count;
} serial_poll_state_t;
static serial_poll_state_t s_serial_poll;

static void serial_poll_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;

    /* Calling serial_rx_pending() also tops up the RX ring from the
     * UART FIFO so the polling fallback works on hosts where the
     * COM1 IRQ never fires. */
    int avail = serial_rx_pending();
    s_serial_poll.count = 0;
    if (avail <= 0) {
        int *out = (int *)output;
        if (out) *out = 0;
        return;
    }
    while (s_serial_poll.count < SERIAL_CHAIN_RING) {
        uint8_t b;
        if (!serial_rx_pop(&b)) break;
        s_serial_poll.buf[s_serial_poll.count++] = b;
    }
    int *out = (int *)output;
    if (out) *out = s_serial_poll.count;
}

static void serial_ascii_decode_resolve(chain_node_t *self,
                                        void *input, void *output)
{
    (void)self;
    int n = input ? *(int *)input : s_serial_poll.count;
    int produced = 0;
    for (int i = 0; i < n && i < SERIAL_CHAIN_RING; i++) {
        uint8_t b = s_serial_poll.buf[i];
        char c;
        /* Normalize line endings: bare CR or LF both map to '\n'. The
         * shell pump treats '\n' as the dispatch trigger. */
        if (b == 0x0D || b == 0x0A) {
            c = '\n';
        } else if (b == 0x7F) {
            /* DEL → backspace; shell_pump_char handles '\b'. */
            c = '\b';
        } else if (b == 0x08) {
            c = '\b';
        } else if (b < 0x20 || b > 0x7E) {
            /* Drop non-printable control bytes we don't translate. */
            continue;
        } else {
            c = (char)b;
        }
        keyboard_inject_char(c);
        produced++;
    }
    s_serial_poll.count = 0;
    s_serial_chars_decoded += (uint32_t)produced;

    int *out = (int *)output;
    if (out) *out = produced;

    if (produced > 0) {
        chain_t *cc = chain_get(CHAIN_SERIAL_IN);
        if (cc) cc->vault_version++;
    }
}

static void serial_input_event_resolve(chain_node_t *self,
                                       void *input, void *output)
{
    (void)self;
    int decoded = input ? *(int *)input : 0;
    int *out = (int *)output;
    if (out) *out = decoded;
}

uint32_t chain_serial_in_total(void)
{
    return s_serial_chars_decoded;
}

/* ── Battery chain resolves ─────────────────────────────────────── */
/*
 * Single poller wrapped as a 4-node pipeline so the chain reads
 * naturally in the inspector:
 *   acpi_poll       → calls battery_refresh() once per 4s
 *   battery_state   → publishes per-battery state
 *   ac_state        → publishes AC online flag
 *   power_event_emit→ if any change, bumps CHAIN_BATTERY's vault_version
 *
 * Real work is in node 0; the rest are observation nodes.
 */

static int s_battery_changed;

static void battery_acpi_poll_resolve(chain_node_t *self,
                                      void *input, void *output)
{
    (void)self; (void)input;
    int *out = (int *)output;
    s_battery_changed = battery_refresh();
    if (out) *out = s_battery_changed;
}

static void battery_state_resolve(chain_node_t *self,
                                  void *input, void *output)
{
    (void)self;
    int changed = input ? *(int *)input : s_battery_changed;
    int *out = (int *)output;
    if (out) *out = changed;
}

static void battery_ac_resolve(chain_node_t *self,
                               void *input, void *output)
{
    (void)self;
    int changed = input ? *(int *)input : s_battery_changed;
    int *out = (int *)output;
    if (out) *out = changed;
}

static void battery_power_event_resolve(chain_node_t *self,
                                        void *input, void *output)
{
    (void)self;
    int changed = input ? *(int *)input : s_battery_changed;
    int *out = (int *)output;
    if (out) *out = changed;
    if (changed && CHAIN_BATTERY >= 0) {
        chain_t *cb = chain_get(CHAIN_BATTERY);
        if (cb) cb->vault_version++;
    }
}

/* ── Trash garbage collector ─────────────────────────────────────
 *
 * CHAIN_TRASH_GC: low-frequency poller. Default cadence is ~1 hour
 * with the current scheduler tps; resolve_interval_ticks is sized
 * conservatively from the compositor's measured 432-800 tps. Each
 * resolve checks the configured "/trash/auto-empty-days" key (32-bit
 * little-endian days, default 30) and calls
 * fat32_trash_empty_older_than(days * 86400). If FAT32 is not
 * mounted or no wall clock is present, the resolve is a no-op. */

static uint32_t s_trash_gc_default_days = 30;

static uint32_t trash_gc_load_days(void)
{
    uint32_t days = 0;
    if (vault_load_config("/trash/auto-empty-days", &days,
                          sizeof(days)) == (int)sizeof(days)) {
        if (days == 0 || days > 3650) days = s_trash_gc_default_days;
        return days;
    }
    return s_trash_gc_default_days;
}

static void trash_gc_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self;
    (void)input;
    int *out = (int *)output;
    if (out) *out = 0;

    if (!fat32_mounted()) return;

    uint32_t days = trash_gc_load_days();
    uint64_t cutoff = (uint64_t)days * 86400ULL;

    int freed = fat32_trash_empty_older_than(cutoff);
    if (freed > 0) {
        if (CHAIN_TRASH_GC >= 0) {
            chain_t *c = chain_get(CHAIN_TRASH_GC);
            if (c) c->vault_version++;
        }
        kputs("[trash-gc] auto-emptied ");
        kput_dec((uint64_t)freed);
        kputs(" entries (>");
        kput_dec((uint64_t)days);
        kputs(" days)\n");
    }
    if (out) *out = freed;
}

/* ── Init ──────────────────────────────────────────────────────── */

int chain_registry_init(void)
{
    int hw_count;

    kputs("[chain_registry] initializing system chain graph\n");

    /* Step 1: Reset the chain registry */
    chain_init();

    /* Step 2: Initialize MDE routing engine */
    mde_init();

    /* Step 3: Discover hardware -- creates CPU, memory, GPU, NIC, etc. chains */
    hw_count = hw_discover_all();
    kputs("[chain_registry] hardware chains: ");
    kput_dec((uint64_t)hw_count);
    kputc('\n');

    /* Get the CPU chain -- all system chains parent from it */
    CHAIN_CPU = hw_find_chain("hw.cpu");
    if (CHAIN_CPU < 0) {
        kputs("[chain_registry] ERROR: no CPU chain from hw_discover\n");
        return -1;
    }

    /* ── Step 4: Register system chains ─────────────────────────── */

    /* Compositor: mixes all surface outputs into the framebuffer.
     *
     * Async: resolve_interval_ticks=16. With ~432-800 tps measured,
     * 16 ticks ≈ 20-37ms ≈ 27-50 fps for the full composite, which is
     * fine for desktop UI. The dirty-rect gate inside the resolve
     * skips even that when nothing changed. The two together drop the
     * compositor's per-tick cost from 12-31ms to <1us in the common
     * idle case. */
    CHAIN_COMPOSITOR = chain_create("compositor", CHAIN_CPU, MASQ_INTERNAL);
    if (CHAIN_COMPOSITOR >= 0) {
        chain_add_node(CHAIN_COMPOSITOR, "mix",
                       "surface_output", "framebuffer",
                       compositor_mix_resolve);
        chain_t *cc = chain_get(CHAIN_COMPOSITOR);
        if (cc) cc->resolve_interval_ticks = 16;
    }

    /* Panel: renders the top bar from chain status data.
     * Tied to compositor cadence (16 ticks ~ 27-50fps) — there is no
     * point re-rendering the panel faster than the compositor flips. */
    CHAIN_PANEL = chain_create("panel", CHAIN_COMPOSITOR, MASQ_INTERNAL);
    if (CHAIN_PANEL >= 0) {
        chain_add_node(CHAIN_PANEL, "render",
                       "chain_status", "surface_output",
                       panel_render_resolve);
        chain_t *cc = chain_get(CHAIN_PANEL);
        if (cc) cc->resolve_interval_ticks = 16;
    }

    /* Dock: renders the bottom launcher from chain list. Same cadence. */
    CHAIN_DOCK = chain_create("dock", CHAIN_COMPOSITOR, MASQ_INTERNAL);
    if (CHAIN_DOCK >= 0) {
        chain_add_node(CHAIN_DOCK, "render",
                       "chain_list", "surface_output",
                       dock_render_resolve);
        chain_t *cc = chain_get(CHAIN_DOCK);
        if (cc) cc->resolve_interval_ticks = 16;
    }

    /* Desktop: renders wallpaper + icons from icon data. Same cadence. */
    CHAIN_DESKTOP = chain_create("desktop", CHAIN_COMPOSITOR, MASQ_INTERNAL);
    if (CHAIN_DESKTOP >= 0) {
        chain_add_node(CHAIN_DESKTOP, "render",
                       "icon_data", "surface_output",
                       desktop_render_resolve);
        chain_t *cc = chain_get(CHAIN_DESKTOP);
        if (cc) cc->resolve_interval_ticks = 16;
    }

    /* Shell: interprets input events into text output (standalone loop) */
    CHAIN_SHELL = chain_create("shell", -1, MASQ_INTERNAL);
    if (CHAIN_SHELL >= 0) {
        chain_add_node(CHAIN_SHELL, "interpret",
                       "input_event", "text_output",
                       shell_interpret_resolve);
    }

    /* Browser: placeholder -- will be wired when browser becomes chain-aware */
    CHAIN_BROWSER = chain_create("browser", -1, MASQ_INTERNAL);
    /* No node yet -- browser doesn't have chain-aware resolve */

    /* Inspector: renders chain state inspection panel */
    CHAIN_INSPECTOR = chain_create("inspector", -1, MASQ_INTERNAL);
    if (CHAIN_INSPECTOR >= 0) {
        chain_add_node(CHAIN_INSPECTOR, "inspect",
                       "chain_id", "surface_output",
                       inspector_inspect_resolve);
    }

    /* Palette: renders command palette overlay from input events */
    CHAIN_PALETTE = chain_create("palette", -1, MASQ_INTERNAL);
    if (CHAIN_PALETTE >= 0) {
        chain_add_node(CHAIN_PALETTE, "search",
                       "input_event", "surface_output",
                       palette_search_resolve);
    }

    /* Keyboard: input pipeline. The IRQ handler queues scancodes in a
     * lock-free ring; this chain drains the ring on every scheduler
     * tick and produces input_event so MDE can route keystrokes to
     * the shell, palette, browser, etc. */
    CHAIN_KEYBOARD = chain_create("keyboard", CHAIN_CPU, MASQ_INTERNAL);
    if (CHAIN_KEYBOARD >= 0) {
        chain_add_node(CHAIN_KEYBOARD, "kbd_irq",
                       "kbd_irq_event",  "scancode_count",
                       kbd_irq_resolve);
        chain_add_node(CHAIN_KEYBOARD, "scancode_decode",
                       "scancode_count", "kbd_decoded_count",
                       kbd_decode_resolve);
        chain_add_node(CHAIN_KEYBOARD, "input_event",
                       "kbd_decoded_count", "input_event",
                       kbd_event_resolve);
    }

    /* Serial input: COM1 UART RX. The IRQ4 ISR (and a polling
     * fallback inside serial_rx_pending) feed a small ring; the
     * chain's three nodes drain it, decode ASCII, and emit
     * input_event so MDE auto-routes serial keystrokes to the
     * same downstream consumers as PS/2. Critical for headless /
     * QEMU -serial stdio boots where the only console IS serial. */
    CHAIN_SERIAL_IN = chain_create("serial_in", CHAIN_CPU, MASQ_INTERNAL);
    if (CHAIN_SERIAL_IN >= 0) {
        chain_add_node(CHAIN_SERIAL_IN, "serial_poll",
                       "uart_rx_event",  "byte_count",
                       serial_poll_resolve);
        chain_add_node(CHAIN_SERIAL_IN, "ascii_decode",
                       "byte_count", "ser_decoded_count",
                       serial_ascii_decode_resolve);
        chain_add_node(CHAIN_SERIAL_IN, "input_event",
                       "ser_decoded_count", "input_event",
                       serial_input_event_resolve);
    }

    /* Mouse: same shape as keyboard but for PS/2 mouse packets. */
    CHAIN_MOUSE = chain_create("mouse", CHAIN_CPU, MASQ_INTERNAL);
    if (CHAIN_MOUSE >= 0) {
        chain_add_node(CHAIN_MOUSE, "mouse_irq",
                       "mouse_irq_event",  "packet_count",
                       mouse_irq_resolve);
        chain_add_node(CHAIN_MOUSE, "packet_decode",
                       "packet_count", "mouse_decoded_count",
                       mouse_decode_resolve);
        chain_add_node(CHAIN_MOUSE, "mouse_event",
                       "mouse_decoded_count", "mouse_event",
                       mouse_event_resolve);
    }

    /* Audio (HDA): first hardware driver converted to native chain
     * paradigm. Pipeline: pcm_source -> volume_filter -> hda_pin ->
     * hardware_dma. hda_init() ran during boot so the controller is
     * already programmed; these nodes wrap the per-PCM playback path. */
    CHAIN_AUDIO = chain_create("audio", CHAIN_CPU, MASQ_INTERNAL);
    if (CHAIN_AUDIO >= 0) {
        extern void *hda_pcm_source_state(void);
        extern void *hda_volume_filter_state(void);
        extern void *hda_pin_state(void);
        extern void *hda_dma_state(void);

        int n0 = chain_add_node(CHAIN_AUDIO, "pcm_source",
                                "pcm_request", "pcm_frame",
                                hda_pcm_source_resolve);
        int n1 = chain_add_node(CHAIN_AUDIO, "volume_filter",
                                "pcm_frame", "pcm_frame",
                                hda_volume_filter_resolve);
        int n2 = chain_add_node(CHAIN_AUDIO, "hda_pin",
                                "pcm_frame", "dma_descriptor",
                                hda_pin_resolve);
        int n3 = chain_add_node(CHAIN_AUDIO, "hardware_dma",
                                "dma_descriptor", "tx_completion",
                                hda_dma_resolve);

        /* Wire each node's state pointer to the right module-private
         * struct. The accessor functions live in hda.c. */
        chain_t *c = chain_get(CHAIN_AUDIO);
        if (c) {
            if (n0 >= 0) c->nodes[n0].state = hda_pcm_source_state();
            if (n1 >= 0) c->nodes[n1].state = hda_volume_filter_state();
            if (n2 >= 0) c->nodes[n2].state = hda_pin_state();
            if (n3 >= 0) c->nodes[n3].state = hda_dma_state();
        }
    }

    /* Networking: chain-native NIC pipeline. The active driver
     * (virtio / e1000 / rtl8169 / rtl8139 / usb_eth) registered itself
     * as the hardware_dma backend during net_init(). The chain layer
     * is generic; the driver never appears in this file. */
    if (net_chain_register(CHAIN_CPU) != 0) {
        kputs("[chain_registry] WARN: net chain registration failed\n");
    }

    /* Block I/O: chain-native storage pipeline. NVMe / AHCI / USB-MSC
     * drivers are the hardware_dma backend, dispatched by the
     * addressing node. masq_journal records every write with prior
     * vault_version so temporal provenance is preserved across the
     * whole storage tier. block_init() already ran during boot, so the
     * drives are enumerated when this chain registers. */
    if (block_chain_register(CHAIN_CPU) != 0) {
        kputs("[chain_registry] WARN: block chain registration failed\n");
    }

    /* MDE-as-a-chain: the routing engine itself exposed as a chain so
     * userspace + Z+ can submit generic compute work via the same
     * pipeline contract. CPU backend wired today; GPU/Goya/FPGA slots
     * present in device_select for future drivers.
     *
     * gpu_compute_init() must run BEFORE mde_chain_register so the
     * device_select node finds at least the CPU backend in the
     * registry on its first resolve. GPU backends register later
     * during gpu_virtio_init. */
    gpu_compute_init();
    if (mde_chain_register(CHAIN_CPU) != 0) {
        kputs("[chain_registry] WARN: mde chain registration failed\n");
    }

    /* GPU: virtio-gpu first (per docs/GPU_HOLES.md L1). Registers
     * CHAIN_GPU_<n> + render/display sub-chains + per-scanout display
     * chains. Falls back to CHAIN_DISPLAY_GOP if no virtio-gpu is
     * present so headless / non-virtio QEMU still surfaces a display
     * chain. Real Intel/AMD/NVIDIA drivers will plug into this same
     * shape later (per CHAIN_CONTRACT addendum). */
    {
        int gpus = gpu_virtio_init(CHAIN_CPU);
        if (gpus > 0) {
            const gpu_virtio_device_t *d0 = gpu_virtio_device(0);
            if (d0) {
                CHAIN_GPU_0         = d0->chain_id;
                CHAIN_GPU_0_RENDER  = d0->chain_render_id;
                CHAIN_GPU_0_DISPLAY = d0->chain_display_id;
            }
        } else {
            int gop_id = -1;
            (void)gpu_virtio_gop_fallback(&gop_id);
            CHAIN_DISPLAY_GOP = gop_id;
        }
    }

    /* Hotplug pumps: PCI rescan, USB PORTSC poll, virtio-gpu HPD.
     * Each runs every 4 ticks (~4ms) and emits attach/detach events
     * into the hotplug ring with MasQ provenance. The boot-time
     * enumeration paths (pci_enumerate, xhci_init, gpu_virtio_init)
     * remain authoritative for first-boot discovery; this layer is
     * purely additive. */
    {
        int hp = hotplug_init(CHAIN_CPU);
        kputs("[chain_registry] hotplug pumps: ");
        kput_dec((uint64_t)hp);
        kputs("\n");
    }

    /* Power: ACPI S3 suspend/resume orchestrator. The chain itself is
     * a state machine — its single node walks the pipeline:
     *   power_event → quiesce_chains → save_devices → s3_enter →
     *   s3_resume → restore_devices → unquiesce_chains
     *
     * The actual work is dispatched via suspend_to_ram() at command
     * time; the chain exists so the suspend cycle is visible in the
     * graph (panel, inspector, B3) and so future power policy can
     * route into it. */
    extern void suspend_register_default_drivers(void);
    suspend_register_default_drivers();
    CHAIN_POWER = chain_create("power", CHAIN_CPU, MASQ_INTERNAL);
    if (CHAIN_POWER >= 0) {
        /* Single passthrough node — the chain's purpose is provenance,
         * not per-tick work. The suspend command pokes the cycle
         * counter via vault_version so MasQ records each S3 entry. */
        chain_t *cp = chain_get(CHAIN_POWER);
        if (cp) cp->resolve_interval_ticks = 256;
    }

    /* Battery: ACPI _BAT / _AC pattern-scan poller. Resolves slowly
     * (every 240 ticks ≈ 4s) — battery state changes on a scale of
     * minutes, no point burning ticks on it. The pipeline:
     *   acpi_poll → battery_state → ac_state → power_event_emit
     * battery_init() ran before chain_registry_init via main.c so
     * presence is already determined; the resolve walks battery_refresh
     * and bumps vault_version on a real change. */
    battery_init();
    CHAIN_BATTERY = chain_create("battery", CHAIN_CPU, MASQ_INTERNAL);
    if (CHAIN_BATTERY >= 0) {
        chain_add_node(CHAIN_BATTERY, "acpi_poll",
                       "acpi_tick", "battery_raw",
                       battery_acpi_poll_resolve);
        chain_add_node(CHAIN_BATTERY, "battery_state",
                       "battery_raw", "battery_state",
                       battery_state_resolve);
        chain_add_node(CHAIN_BATTERY, "ac_state",
                       "battery_state", "ac_state",
                       battery_ac_resolve);
        chain_add_node(CHAIN_BATTERY, "power_event_emit",
                       "ac_state", "power_event",
                       battery_power_event_resolve);
        chain_t *cb = chain_get(CHAIN_BATTERY);
        if (cb) cb->resolve_interval_ticks = 240;
    }

    /* ── Step 5: Auto-route by type matching ────────────────────── */
    /*
     * MDE scans all chains, finds output_type == input_type matches,
     * and wires routes automatically. Panel, dock, desktop, inspector,
     * and palette all produce "surface_output" -- compositor accepts
     * "surface_output" -- so MDE routes them all into the compositor.
     */
    int routes = mde_auto_route();
    kputs("[chain_registry] MDE auto-routed ");
    kput_dec((uint64_t)routes);
    kputs(" connections\n");

    /* ── Step 6: Dump the full graph ────────────────────────────── */
    mde_dump_graph();

    /* Summary */
    int total = chain_count();
    kputs("[chain_registry] system ready: ");
    kput_dec((uint64_t)total);
    kputs(" chains, ");
    kput_dec((uint64_t)routes);
    kputs(" routes\n");

    return total;
}

/* ── Tick ───────────────────────────────────────────────────────── */

int chain_registry_tick(void)
{
    /*
     * Resolve the entire chain graph in dependency order.
     * MDE's topological sort ensures:
     *   1. Hardware chains resolve first (data producers)
     *   2. Desktop/panel/dock/inspector/palette resolve (surface producers)
     *   3. Compositor resolves last (consumes surfaces, produces framebuffer)
     *
     * Each chain's resolve function calls the real subsystem code.
     * B3 belief updates automatically on success/failure.
     * Timing is measured per-chain by chain_resolve().
     */
    return mde_resolve_all();
}
