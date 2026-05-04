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
#include "kprint.h"

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
    (void)output;

    /* The compositor's job: draw all WM surfaces to the framebuffer */
    wm_draw_all();
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

    /* Compositor: mixes all surface outputs into the framebuffer */
    CHAIN_COMPOSITOR = chain_create("compositor", CHAIN_CPU, MASQ_INTERNAL);
    if (CHAIN_COMPOSITOR >= 0) {
        chain_add_node(CHAIN_COMPOSITOR, "mix",
                       "surface_output", "framebuffer",
                       compositor_mix_resolve);
    }

    /* Panel: renders the top bar from chain status data */
    CHAIN_PANEL = chain_create("panel", CHAIN_COMPOSITOR, MASQ_INTERNAL);
    if (CHAIN_PANEL >= 0) {
        chain_add_node(CHAIN_PANEL, "render",
                       "chain_status", "surface_output",
                       panel_render_resolve);
    }

    /* Dock: renders the bottom launcher from chain list */
    CHAIN_DOCK = chain_create("dock", CHAIN_COMPOSITOR, MASQ_INTERNAL);
    if (CHAIN_DOCK >= 0) {
        chain_add_node(CHAIN_DOCK, "render",
                       "chain_list", "surface_output",
                       dock_render_resolve);
    }

    /* Desktop: renders wallpaper + icons from icon data */
    CHAIN_DESKTOP = chain_create("desktop", CHAIN_COMPOSITOR, MASQ_INTERNAL);
    if (CHAIN_DESKTOP >= 0) {
        chain_add_node(CHAIN_DESKTOP, "render",
                       "icon_data", "surface_output",
                       desktop_render_resolve);
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
