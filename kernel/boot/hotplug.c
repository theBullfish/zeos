/*
 * Zeos -- Hotplug Pumps
 *
 * Three hotplug pumps registered as chains. Each diffs the current
 * hardware state against the last-known and emits attach/detach
 * events. Per-class init functions are reused; this layer only
 * decides WHEN to call them.
 *
 *   CHAIN_HOTPLUG_PCI     -- bus walk + diff
 *   CHAIN_HOTPLUG_USB     -- xHCI PORTSC poll + diff
 *   CHAIN_HOTPLUG_DISPLAY -- virtio-gpu GET_DISPLAY_INFO + diff
 *
 * Boot-time enumeration paths (pci_enumerate, xhci_init,
 * gpu_virtio_init) remain authoritative for first-boot discovery.
 * Hotplug runs additively after boot and detects deltas only.
 *
 * Each resolve function exits in ~microseconds when nothing has
 * changed: the diff is a single u32 vendor/device read per slot
 * for PCI, a single u32 PORTSC read per port for USB, and a memcmp
 * of the cached scanout array for display.
 */

#include "hotplug.h"
#include "chain.h"
#include "chain_registry.h"
#include "pci.h"
#include "usb_xhci.h"
#include "gpu_virtio.h"
#include "timer.h"
#include "kprint.h"

extern void *memset(void *s, int c, unsigned long n);

/* ── Chain IDs ─────────────────────────────────────────────────── */

int CHAIN_HOTPLUG_PCI     = -1;
int CHAIN_HOTPLUG_USB     = -1;
int CHAIN_HOTPLUG_DISPLAY = -1;

/* ── Event ring (32 entries, MasQ-tier INTERNAL via chain.tier) ── */

#define HOTPLUG_RING 32

static hotplug_event_t s_ring[HOTPLUG_RING];
static uint32_t        s_head;     /* next slot to write */
static uint32_t        s_total;    /* total events emitted (saturating) */

static void emit(hotplug_event_t *e)
{
    e->tsc = timer_read_tsc();
    s_ring[s_head % HOTPLUG_RING] = *e;
    s_head++;
    if (s_total < 0xFFFFFFFFu) s_total++;

    /* MasQ: bump the chain's vault_version so observers see the change. */
    chain_t *c = 0;
    switch (e->kind) {
        case HOTPLUG_EVT_PCI_ATTACH:
        case HOTPLUG_EVT_PCI_DETACH:
            c = chain_get(CHAIN_HOTPLUG_PCI); break;
        case HOTPLUG_EVT_USB_ATTACH:
        case HOTPLUG_EVT_USB_DETACH:
            c = chain_get(CHAIN_HOTPLUG_USB); break;
        case HOTPLUG_EVT_DISP_ATTACH:
        case HOTPLUG_EVT_DISP_DETACH:
            c = chain_get(CHAIN_HOTPLUG_DISPLAY); break;
        default: break;
    }
    if (c) c->vault_version++;

    /* Live MasQ trace line so the boot console / serial log captures
     * each event the moment it fires (no batching). */
    switch (e->kind) {
        case HOTPLUG_EVT_PCI_ATTACH:
            kputs("[hotplug] PCI attach ");
            kput_hex(e->bus); kputs(":");
            kput_hex((e->devfn >> 3) & 0x1F); kputs(".");
            kput_hex(e->devfn & 0x07);
            kputs(" vid="); kput_hex(e->vendor_id);
            kputs(" did="); kput_hex(e->product_id);
            kputs(" class="); kput_hex(e->class_code);
            kputs("\n");
            break;
        case HOTPLUG_EVT_PCI_DETACH:
            kputs("[hotplug] PCI detach ");
            kput_hex(e->bus); kputs(":");
            kput_hex((e->devfn >> 3) & 0x1F); kputs(".");
            kput_hex(e->devfn & 0x07);
            kputs(" vid="); kput_hex(e->vendor_id);
            kputs(" did="); kput_hex(e->product_id);
            kputs("\n");
            break;
        case HOTPLUG_EVT_USB_ATTACH:
            kputs("[hotplug] USB attach port ");
            kput_dec(e->port_or_slot);
            kputs(" speed="); kput_dec(e->speed);
            kputs("\n");
            break;
        case HOTPLUG_EVT_USB_DETACH:
            kputs("[hotplug] USB detach port ");
            kput_dec(e->port_or_slot);
            kputs("\n");
            break;
        case HOTPLUG_EVT_DISP_ATTACH:
            kputs("[hotplug] display attach scanout ");
            kput_dec(e->port_or_slot);
            kputs(" "); kput_dec(e->width);
            kputs("x"); kput_dec(e->height);
            kputs("\n");
            break;
        case HOTPLUG_EVT_DISP_DETACH:
            kputs("[hotplug] display detach scanout ");
            kput_dec(e->port_or_slot);
            kputs("\n");
            break;
        default: break;
    }
}

/* ── PCI rescan pump ───────────────────────────────────────────── */

/* Snapshot of (bus,dev,func) -> vendor_id|device_id|class|subclass.
 * We don't keep the whole pci_device array; we only diff the identity
 * tuple. Indexed by (bus<<8)|(dev<<3)|func — 256 buses * 32 dev * 8 fn
 * is way too large; instead we use a flat list of "known" entries up
 * to MAX_PCI_KNOWN. New attaches push, detaches splice out. */

#define MAX_PCI_KNOWN 64

typedef struct {
    uint8_t  bus, dev, func;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  in_use;
} pci_known_t;

static pci_known_t s_pci_known[MAX_PCI_KNOWN];
static int         s_pci_seeded = 0;

static int pci_known_find(uint8_t bus, uint8_t dev, uint8_t func)
{
    for (int i = 0; i < MAX_PCI_KNOWN; i++) {
        if (!s_pci_known[i].in_use) continue;
        if (s_pci_known[i].bus == bus && s_pci_known[i].dev == dev &&
            s_pci_known[i].func == func) return i;
    }
    return -1;
}

static int pci_known_add(uint8_t bus, uint8_t dev, uint8_t func,
                         uint16_t vid, uint16_t did,
                         uint8_t class_code, uint8_t subclass, uint8_t prog_if)
{
    for (int i = 0; i < MAX_PCI_KNOWN; i++) {
        if (s_pci_known[i].in_use) continue;
        s_pci_known[i].bus        = bus;
        s_pci_known[i].dev        = dev;
        s_pci_known[i].func       = func;
        s_pci_known[i].vendor_id  = vid;
        s_pci_known[i].device_id  = did;
        s_pci_known[i].class_code = class_code;
        s_pci_known[i].subclass   = subclass;
        s_pci_known[i].prog_if    = prog_if;
        s_pci_known[i].in_use     = 1;
        return i;
    }
    return -1;
}

/* Bus walk that reads vendor/device IDs only — does NOT touch the
 * boot-time `devices[]` cache in pci.c. Returns -1 if pci is absent. */
/* id=29 fix (2026-07-25): a full 256-bus config-space walk is ~8192 port-I/O
 * probes, each a VM-exit under QEMU -> ~139ms in ONE resolve, which blew the
 * scheduler budget every time this chain fired (it was the top-slow chain by
 * far -- confirmed id=29=hotplug.pci from the boot log). Zeos fix: AMORTIZE the
 * scan across resolves -- probe a window of HOTPLUG_BUSES_PER_FIRE buses per
 * fire, advancing a cursor, so each resolve costs ~window/256 of the old spike
 * with NO loss of coverage. The detach sweep (a device is gone if it wasn't
 * seen) must run against a FULL scan, so `visited` is accumulated across the
 * whole cycle and the sweep + seed flag only run when the cursor wraps. */
#define HOTPLUG_BUSES_PER_FIRE 16
static uint8_t s_cycle_visited[MAX_PCI_KNOWN];
static int     s_scan_bus;   /* next bus to probe this cycle [0,256) */

static void pci_walk_and_diff(int *touched_count)
{
    int bus_end = s_scan_bus + HOTPLUG_BUSES_PER_FIRE;
    if (bus_end > 256) bus_end = 256;

    for (int bus = s_scan_bus; bus < bus_end; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_config_read32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;

            uint32_t hdr0 = pci_config_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            int multi = ((hdr0 >> 16) & 0x80) ? 1 : 0;
            int max_func = multi ? 8 : 1;

            for (int func = 0; func < max_func; func++) {
                uint32_t id = (func == 0) ? id0
                            : pci_config_read32((uint8_t)bus, (uint8_t)dev,
                                                (uint8_t)func, 0x00);
                uint16_t vid = id & 0xFFFF;
                if (vid == 0xFFFF || vid == 0x0000) continue;
                uint16_t did = (id >> 16) & 0xFFFF;
                uint32_t cls = pci_config_read32((uint8_t)bus, (uint8_t)dev,
                                                 (uint8_t)func, 0x08);
                uint8_t  cc  = (cls >> 24) & 0xFF;
                uint8_t  sc  = (cls >> 16) & 0xFF;
                uint8_t  pi  = (cls >>  8) & 0xFF;

                int known_idx = pci_known_find((uint8_t)bus, (uint8_t)dev,
                                               (uint8_t)func);
                if (known_idx < 0) {
                    /* Attach */
                    int ki = pci_known_add((uint8_t)bus, (uint8_t)dev,
                                           (uint8_t)func, vid, did,
                                           cc, sc, pi);
                    if (ki >= 0 && ki < (int)sizeof(s_cycle_visited))
                        s_cycle_visited[ki] = 1;
                    if (s_pci_seeded) {
                        hotplug_event_t e;
                        memset(&e, 0, sizeof(e));
                        e.kind = HOTPLUG_EVT_PCI_ATTACH;
                        e.bus = (uint16_t)bus;
                        e.devfn = (uint16_t)((dev << 3) | func);
                        e.vendor_id = vid;
                        e.product_id = did;
                        e.class_code = cc;
                        e.subclass = sc;
                        emit(&e);
                    }
                    if (touched_count) (*touched_count)++;
                } else {
                    if (known_idx < (int)sizeof(s_cycle_visited))
                        s_cycle_visited[known_idx] = 1;
                    /* Identity changed at same (b,d,f) -- treat as
                     * detach + attach. Vanishingly rare on real HW
                     * but cheap to handle. */
                    if (s_pci_known[known_idx].vendor_id != vid ||
                        s_pci_known[known_idx].device_id != did) {
                        if (s_pci_seeded) {
                            hotplug_event_t e;
                            memset(&e, 0, sizeof(e));
                            e.kind = HOTPLUG_EVT_PCI_DETACH;
                            e.bus = (uint16_t)bus;
                            e.devfn = (uint16_t)((dev << 3) | func);
                            e.vendor_id = s_pci_known[known_idx].vendor_id;
                            e.product_id = s_pci_known[known_idx].device_id;
                            emit(&e);
                            e.kind = HOTPLUG_EVT_PCI_ATTACH;
                            e.vendor_id = vid;
                            e.product_id = did;
                            e.class_code = cc;
                            e.subclass = sc;
                            emit(&e);
                        }
                        s_pci_known[known_idx].vendor_id  = vid;
                        s_pci_known[known_idx].device_id  = did;
                        s_pci_known[known_idx].class_code = cc;
                        s_pci_known[known_idx].subclass   = sc;
                        s_pci_known[known_idx].prog_if    = pi;
                    }
                }
            }
        }
    }

    /* Advance the scan cursor. The detach sweep + seed flag only run when a
     * FULL cycle (all 256 buses) has been probed, so a device on a bus not yet
     * reached this cycle is never falsely detached. */
    s_scan_bus = bus_end;
    if (s_scan_bus < 256)
        return;   /* cycle not complete yet -- defer detach sweep */

    /* Full cycle done: anything not seen anywhere this cycle is gone. */
    for (int i = 0; i < MAX_PCI_KNOWN; i++) {
        if (!s_pci_known[i].in_use) continue;
        if (i < (int)sizeof(s_cycle_visited) && s_cycle_visited[i]) continue;
        if (s_pci_seeded) {
            hotplug_event_t e;
            memset(&e, 0, sizeof(e));
            e.kind = HOTPLUG_EVT_PCI_DETACH;
            e.bus = s_pci_known[i].bus;
            e.devfn = (uint16_t)((s_pci_known[i].dev << 3) |
                                 s_pci_known[i].func);
            e.vendor_id = s_pci_known[i].vendor_id;
            e.product_id = s_pci_known[i].device_id;
            e.class_code = s_pci_known[i].class_code;
            e.subclass = s_pci_known[i].subclass;
            emit(&e);
        }
        s_pci_known[i].in_use = 0;
        if (touched_count) (*touched_count)++;
    }

    /* Reset for the next cycle. Seed flag flips only after the first FULL
     * cycle, so initial devices are recorded silently (no spurious attach). */
    memset(s_cycle_visited, 0, sizeof(s_cycle_visited));
    s_scan_bus = 0;
    if (!s_pci_seeded) s_pci_seeded = 1;
}

static void pci_pump_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self; (void)input;
    int touched = 0;
    pci_walk_and_diff(&touched);
    /* seed flag is now managed inside pci_walk_and_diff at cycle completion */
    if (output) *(int *)output = touched;
}

/* ── USB hotplug pump ──────────────────────────────────────────── */

#define MAX_USB_PORTS 32

typedef struct {
    uint8_t connected;
    uint8_t speed;
} usb_port_known_t;

static usb_port_known_t s_usb_known[MAX_USB_PORTS];
static int              s_usb_seeded = 0;

static void usb_pump_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self; (void)input;
    int max_p = xhci_hotplug_max_ports();
    if (max_p > MAX_USB_PORTS) max_p = MAX_USB_PORTS;
    int touched = 0;


    for (int p = 1; p <= max_p; p++) {
        int connected = 0, speed = 0, csc = 0;
        if (xhci_hotplug_port_status(p, &connected, &speed, &csc) != 0)
            continue;

        int prev_conn = s_usb_known[p - 1].connected;

        /* Use CSC OR a state mismatch as the change trigger. CSC catches
         * fast attach/detach pairs that flip back before the next poll;
         * the state mismatch catches the slow-poll case where CSC was
         * already cleared by another path. */
        if (csc || (s_usb_seeded && connected != prev_conn)) {
            if (s_usb_seeded) {
                hotplug_event_t e;
                memset(&e, 0, sizeof(e));
                e.port_or_slot = (uint16_t)p;
                e.speed = (uint8_t)speed;
                if (connected && !prev_conn) {
                    e.kind = HOTPLUG_EVT_USB_ATTACH;
                    emit(&e);
                } else if (!connected && prev_conn) {
                    e.kind = HOTPLUG_EVT_USB_DETACH;
                    emit(&e);
                } else if (connected && prev_conn) {
                    /* Bounce: count as detach+attach so observers see
                     * the cycle. */
                    e.kind = HOTPLUG_EVT_USB_DETACH;
                    emit(&e);
                    e.kind = HOTPLUG_EVT_USB_ATTACH;
                    emit(&e);
                }
                touched++;
            }
            xhci_hotplug_ack_csc(p);
        }

        s_usb_known[p - 1].connected = (uint8_t)connected;
        s_usb_known[p - 1].speed     = (uint8_t)speed;
    }
    if (!s_usb_seeded) s_usb_seeded = 1;
    if (output) *(int *)output = touched;
}

/* ── Display HPD pump ──────────────────────────────────────────── */

#define MAX_DISP_DEVS     4
#define MAX_DISP_SCANOUTS 16

typedef struct {
    uint8_t  enabled;
    uint32_t width;
    uint32_t height;
} disp_known_t;

static disp_known_t s_disp_known[MAX_DISP_DEVS][MAX_DISP_SCANOUTS];
static int          s_disp_seeded = 0;

static void disp_pump_resolve(chain_node_t *self, void *input, void *output)
{
    (void)self; (void)input;
    int touched = 0;

    /* Re-poll GET_DISPLAY_INFO. If no virtio-gpu device exists this is
     * a no-op (returns 0). */
    (void)gpu_virtio_hotplug_repoll();

    for (int d = 0; d < MAX_DISP_DEVS; d++) {
        for (int si = 0; si < MAX_DISP_SCANOUTS; si++) {
            int enabled = 0;
            uint32_t w = 0, h = 0;
            if (gpu_virtio_hotplug_scanout(d, si, &enabled, &w, &h) != 0) {
                /* Out of range — treat as not-enabled; if previously
                 * enabled, emit detach. */
                enabled = 0;
                w = 0; h = 0;
            }
            int prev = s_disp_known[d][si].enabled;
            if (s_disp_seeded && enabled != prev) {
                hotplug_event_t e;
                memset(&e, 0, sizeof(e));
                e.port_or_slot = (uint16_t)((d << 8) | si);
                e.width  = w;
                e.height = h;
                if (enabled && !prev) {
                    e.kind = HOTPLUG_EVT_DISP_ATTACH;
                    emit(&e);
                } else {
                    e.kind = HOTPLUG_EVT_DISP_DETACH;
                    e.width = s_disp_known[d][si].width;
                    e.height = s_disp_known[d][si].height;
                    emit(&e);
                }
                touched++;
            } else if (s_disp_seeded && enabled &&
                       (w != s_disp_known[d][si].width ||
                        h != s_disp_known[d][si].height)) {
                /* Mode changed in place — treat as detach+attach. */
                hotplug_event_t e;
                memset(&e, 0, sizeof(e));
                e.port_or_slot = (uint16_t)((d << 8) | si);
                e.kind = HOTPLUG_EVT_DISP_DETACH;
                e.width = s_disp_known[d][si].width;
                e.height = s_disp_known[d][si].height;
                emit(&e);
                e.kind = HOTPLUG_EVT_DISP_ATTACH;
                e.width = w;
                e.height = h;
                emit(&e);
                touched++;
            }
            s_disp_known[d][si].enabled = (uint8_t)enabled;
            s_disp_known[d][si].width   = w;
            s_disp_known[d][si].height  = h;
        }
    }
    if (!s_disp_seeded) s_disp_seeded = 1;
    if (output) *(int *)output = touched;
}

/* ── Public API ─────────────────────────────────────────────────── */

int hotplug_init(int parent_id)
{
    int created = 0;

    memset(s_ring, 0, sizeof(s_ring));
    s_head = 0;
    s_total = 0;
    memset(s_pci_known, 0, sizeof(s_pci_known));
    memset(s_usb_known, 0, sizeof(s_usb_known));
    memset(s_disp_known, 0, sizeof(s_disp_known));
    s_pci_seeded = 0;
    s_usb_seeded = 0;
    s_disp_seeded = 0;

    /* PCI rescan chain. resolve_interval_ticks=4 keeps the per-tick
     * cost amortized. */
    CHAIN_HOTPLUG_PCI = chain_create("hotplug.pci", parent_id, MASQ_INTERNAL);
    if (CHAIN_HOTPLUG_PCI >= 0) {
        chain_add_node(CHAIN_HOTPLUG_PCI, "pci_rescan",
                       "hotplug_request", "hotplug_delta",
                       pci_pump_resolve);
        chain_t *c = chain_get(CHAIN_HOTPLUG_PCI);
        if (c) c->resolve_interval_ticks = 4;
        created++;
    }

    /* Power on every xHCI root-hub port so post-boot attaches register.
     * Boot's xhci_init only inspected ports with CCS already set at
     * power-up; PP=0 ports stay invisible until we assert it. */
    xhci_hotplug_power_on_all_ports();

    /* USB hotplug chain. */
    CHAIN_HOTPLUG_USB = chain_create("hotplug.usb", parent_id, MASQ_INTERNAL);
    if (CHAIN_HOTPLUG_USB >= 0) {
        chain_add_node(CHAIN_HOTPLUG_USB, "xhci_port_poll",
                       "hotplug_request", "hotplug_delta",
                       usb_pump_resolve);
        chain_t *c = chain_get(CHAIN_HOTPLUG_USB);
        if (c) c->resolve_interval_ticks = 4;
        created++;
    }

    /* Display HPD chain. */
    CHAIN_HOTPLUG_DISPLAY = chain_create("hotplug.display", parent_id, MASQ_INTERNAL);
    if (CHAIN_HOTPLUG_DISPLAY >= 0) {
        chain_add_node(CHAIN_HOTPLUG_DISPLAY, "display_repoll",
                       "hotplug_request", "hotplug_delta",
                       disp_pump_resolve);
        chain_t *c = chain_get(CHAIN_HOTPLUG_DISPLAY);
        if (c) c->resolve_interval_ticks = 4;
        created++;
    }

    /* Run each pump once now to seed the known state with the
     * boot-time hardware list. The "seeded" flag suppresses event
     * emission on this first pass — we only want to emit when there's
     * a delta against a real prior snapshot. */
    if (CHAIN_HOTPLUG_PCI >= 0)     chain_resolve(CHAIN_HOTPLUG_PCI);
    if (CHAIN_HOTPLUG_USB >= 0)     chain_resolve(CHAIN_HOTPLUG_USB);
    if (CHAIN_HOTPLUG_DISPLAY >= 0) chain_resolve(CHAIN_HOTPLUG_DISPLAY);

    kputs("[hotplug] pumps live: pci=");
    kput_dec(CHAIN_HOTPLUG_PCI >= 0);
    kputs(" usb=");
    kput_dec(CHAIN_HOTPLUG_USB >= 0);
    kputs(" display=");
    kput_dec(CHAIN_HOTPLUG_DISPLAY >= 0);
    kputs("\n");

    return created;
}

int hotplug_pumps_live(void)
{
    return (CHAIN_HOTPLUG_PCI     >= 0) +
           (CHAIN_HOTPLUG_USB     >= 0) +
           (CHAIN_HOTPLUG_DISPLAY >= 0);
}

int hotplug_event_count(void)
{
    int n = (int)s_total;
    if (n > HOTPLUG_RING) n = HOTPLUG_RING;
    return n;
}

int hotplug_event_copy(hotplug_event_t *out, int max_n)
{
    if (!out || max_n <= 0) return 0;
    int n = hotplug_event_count();
    if (n > max_n) n = max_n;
    /* Newest-last: walk back N from head. */
    uint32_t start = (s_head >= (uint32_t)n) ? (s_head - (uint32_t)n)
                                              : 0;
    for (int i = 0; i < n; i++) {
        out[i] = s_ring[(start + (uint32_t)i) % HOTPLUG_RING];
    }
    return n;
}

static const char *kind_label(uint16_t k)
{
    switch (k) {
        case HOTPLUG_EVT_PCI_ATTACH:  return "PCI ATTACH ";
        case HOTPLUG_EVT_PCI_DETACH:  return "PCI DETACH ";
        case HOTPLUG_EVT_USB_ATTACH:  return "USB ATTACH ";
        case HOTPLUG_EVT_USB_DETACH:  return "USB DETACH ";
        case HOTPLUG_EVT_DISP_ATTACH: return "DSP ATTACH ";
        case HOTPLUG_EVT_DISP_DETACH: return "DSP DETACH ";
        default: return "?          ";
    }
}

void hotplug_dump_events(int n)
{
    if (n <= 0) n = 32;
    if (n > HOTPLUG_RING) n = HOTPLUG_RING;
    int have = hotplug_event_count();
    if (have == 0) {
        kputs("  (no hotplug events recorded)\n");
        return;
    }
    if (have < n) n = have;
    static hotplug_event_t buf[HOTPLUG_RING];
    int got = hotplug_event_copy(buf, n);

    kputs("  Hotplug event log (newest last, ");
    kput_dec((uint64_t)got);
    kputs(" of ");
    kput_dec((uint64_t)s_total);
    kputs(" total)\n");
    kputs("  ──────────────────────────────────────────────────\n");
    for (int i = 0; i < got; i++) {
        hotplug_event_t *e = &buf[i];
        kputs("  tsc=");
        kput_hex(e->tsc);
        kputs(" ");
        kputs(kind_label(e->kind));
        switch (e->kind) {
            case HOTPLUG_EVT_PCI_ATTACH:
            case HOTPLUG_EVT_PCI_DETACH:
                kputs("bdf=");
                kput_hex(e->bus); kputs(":");
                kput_hex((e->devfn >> 3) & 0x1F); kputs(".");
                kput_hex(e->devfn & 0x07);
                kputs(" vid="); kput_hex(e->vendor_id);
                kputs(" did="); kput_hex(e->product_id);
                kputs(" class="); kput_hex(e->class_code);
                kputs(":"); kput_hex(e->subclass);
                break;
            case HOTPLUG_EVT_USB_ATTACH:
            case HOTPLUG_EVT_USB_DETACH:
                kputs("port="); kput_dec(e->port_or_slot);
                kputs(" speed="); kput_dec(e->speed);
                break;
            case HOTPLUG_EVT_DISP_ATTACH:
            case HOTPLUG_EVT_DISP_DETACH:
                kputs("scanout=");
                kput_dec((e->port_or_slot >> 8) & 0xFF);
                kputs(".");
                kput_dec(e->port_or_slot & 0xFF);
                kputs(" ");
                kput_dec(e->width); kputs("x");
                kput_dec(e->height);
                break;
            default: break;
        }
        kputs("\n");
    }
}
