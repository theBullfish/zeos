/*
 * Zeos -- Hotplug Pumps
 *
 * Three chain-resolved pumps that turn hardware deltas into chain
 * create/destroy events with MasQ provenance:
 *
 *   CHAIN_HOTPLUG_PCI     -- PCI bus rescan (config-space walk + diff)
 *   CHAIN_HOTPLUG_USB     -- xHCI root-port PORTSC poll + diff
 *   CHAIN_HOTPLUG_DISPLAY -- virtio-gpu GET_DISPLAY_INFO + diff
 *
 * Boot-time enumeration paths (pci_enumerate, xhci_init,
 * gpu_virtio_init) remain authoritative for first-boot discovery.
 * Hotplug is purely additive: it diffs the current hardware state
 * against the snapshot it captured the previous tick.
 */

#ifndef ZEOS_HOTPLUG_H
#define ZEOS_HOTPLUG_H

#include <stdint.h>

/* Event kinds recorded into the hotplug ring. */
typedef enum {
    HOTPLUG_EVT_NONE        = 0,
    HOTPLUG_EVT_PCI_ATTACH  = 1,
    HOTPLUG_EVT_PCI_DETACH  = 2,
    HOTPLUG_EVT_USB_ATTACH  = 3,
    HOTPLUG_EVT_USB_DETACH  = 4,
    HOTPLUG_EVT_DISP_ATTACH = 5,
    HOTPLUG_EVT_DISP_DETACH = 6,
} hotplug_evt_kind_t;

typedef struct {
    uint64_t tsc;               /* timer_read_tsc() at emit time */
    uint16_t kind;              /* hotplug_evt_kind_t */
    uint16_t port_or_slot;      /* USB port / scanout idx / 0 for PCI */
    uint16_t bus;               /* PCI bus  / 0 for USB,DISP */
    uint16_t devfn;             /* (dev << 3) | func / 0 for USB,DISP */
    uint16_t vendor_id;         /* PCI/USB vendor */
    uint16_t product_id;        /* PCI device / USB product */
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  speed;             /* USB only: PORTSC speed nibble */
    uint8_t  _pad;
    uint32_t width;             /* DISPLAY only */
    uint32_t height;            /* DISPLAY only */
} hotplug_event_t;

/* Chain IDs (set by hotplug_init, -1 if not created). */
extern int CHAIN_HOTPLUG_PCI;
extern int CHAIN_HOTPLUG_USB;
extern int CHAIN_HOTPLUG_DISPLAY;

/* Register the three pumps. parent_id is CHAIN_CPU. Returns
 * the number of chains successfully created (0..3). */
int  hotplug_init(int parent_id);

/* How many events are in the ring (capped at ring capacity). */
int  hotplug_event_count(void);

/* Copy out up to max_n events newest-last. Returns number copied. */
int  hotplug_event_copy(hotplug_event_t *out, int max_n);

/* Print last n events via kputs (used by the shell `hotplug` cmd). */
void hotplug_dump_events(int n);

/* Liveness: true if all three chains are registered. */
int  hotplug_pumps_live(void);

#endif /* ZEOS_HOTPLUG_H */
