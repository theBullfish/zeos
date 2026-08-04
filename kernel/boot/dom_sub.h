/*
 * Zeos — Dom/Sub cooperative multi-chip PCIe compute fabric (spec:
 * specs/DOM_SUB_CHIPS.md). BUILD_MAP Q.1–Q.4.
 *
 * Multiple ARM co-processor chips (Goya-class today) that hot-plug via PCIe
 * recognize each other as one cooperative cohort, elect one Dom (gets THINK
 * work) and N Subs (get background work), and re-elect on membership change.
 *
 * This header covers Q.1 (chip-class table), Q.2 (cohort discovery: boot-seed
 * + live hotplug drain), Q.3 (identify handshake), Q.4 (election w/ hysteresis).
 * Q.5 (affinity routing), Q.6 (detach failure) and Q.7 (2+ real cards) are
 * separate. Real per-chip register identify + multi-card election land on
 * hardware in Q.7; the table + election *algorithm* are verifiable with no
 * hardware via dom_sub_selftest().
 */
#ifndef ZEOS_DOM_SUB_H
#define ZEOS_DOM_SUB_H

#include <stdint.h>

#define DOM_SUB_MAX_CHIPS   8      /* cohort capacity (mirrors GOYA_MAX_DEVICES) */
#define DOM_SUB_HYSTERESIS_PCT   10    /* challenger must beat Dom by >=10% ... */
#define DOM_SUB_HYSTERESIS_N      3    /* ... for N consecutive elections to unseat */

/* Q.1 — a recognized cooperative chip class (vendor:device allowlist). */
typedef struct {
    uint16_t    vendor_id;
    uint16_t    device_id;
    const char *label;
} chip_class_t;

/* Q.3 — identity read from a chip once it joins the cohort. */
typedef struct {
    uint32_t fw_version;
    uint32_t dram_mb;
    uint32_t thermal_margin;   /* 0 if unknown */
    uint32_t bench_score;      /* 0 until a real THINK job measures it */
} chip_identity_t;

typedef enum {
    DOM_SUB_ROLE_NONE = 0,
    DOM_SUB_ROLE_DOM,
    DOM_SUB_ROLE_SUB,
} dom_sub_role_t;

/* Q.5 — work-class tag set at chain-creation time by the enqueuing subsystem. */
typedef enum {
    DOM_SUB_CLASS_THINK,       /* high-priority: user is waiting on this */
    DOM_SUB_CLASS_BACKGROUND,  /* prefetch, batch, telemetry — "incidental shit" */
} dom_sub_class_t;

/* One cohort member. */
typedef struct {
    int             in_use;
    uint8_t         bus, dev, func;    /* PCI address (election tiebreak) */
    uint16_t        vendor_id, device_id;
    const char     *label;
    chip_identity_t id;
    dom_sub_role_t  role;
    int             goya_idx;          /* index into gpu_goya_device(), -1 if synthetic */
} cohort_member_t;

/* ── Q.1: chip-class recognition ───────────────────────────── */
/* Returns the matching class label, or 0 if (vendor,device) is not a
 * recognized cooperative chip. */
const char *dom_sub_chip_label(uint16_t vendor_id, uint16_t device_id);
int         dom_sub_is_cooperative_chip(uint16_t vendor_id, uint16_t device_id);

/* ── Q.2: discovery ────────────────────────────────────────── */
/* Boot-time seeding: adds every Goya card gpu_goya_init() already bound to the
 * cohort, identifies each, and runs the first election. Call once at boot
 * (from gpu_goya_init). Safe (no-op) when zero cards are present. */
void dom_sub_init(void);

/* Live drain of the hotplug event ring (rides CHAIN_HOTPLUG_PCI, no hotplug.c
 * edit — uses the public hotplug_event_copy API + a tsc cursor). Processes any
 * ATTACH/DETACH of a cooperative chip since the last call, updating cohort
 * membership + re-electing. Call periodically. */
void dom_sub_poll(void);

/* Membership handlers (also the synthetic-inject path for verification). */
int  dom_sub_on_attach(uint8_t bus, uint8_t dev, uint8_t func,
                       uint16_t vendor, uint16_t device, int goya_idx);
void dom_sub_on_detach(uint8_t bus, uint8_t dev, uint8_t func);

/* ── Q.4: election ─────────────────────────────────────────── */
/* Re-run the election over current members. Returns the Dom's member index, or
 * -1 for an empty cohort. Applies hysteresis so a marginal challenger does not
 * flap the incumbent Dom. */
int  dom_sub_elect(void);

/* ── Q.5: task classification & routing ────────────────────── */
/* Route a work class to a cohort chip slot. THINK → current Dom; BACKGROUND →
 * round-robin across Subs (a lone chip is both Dom and Sub; empty cohort → -1
 * so the caller uses its non-cooperative fallback). Returns a member index. */
int  dom_sub_route(dom_sub_class_t cls);

/* Current Dom member index (-1 if none) + accessors. */
int  dom_sub_dom_index(void);
int  dom_sub_count(void);
const cohort_member_t *dom_sub_member(int idx);

/* ── Verification / introspection ──────────────────────────── */
void dom_sub_dump(void);                 /* print the cohort + roles to serial */
void dom_sub_reset(void);                /* clear all members (test scaffolding) */
/* Inject a synthetic chip (no hardware) for election verification. */
int  dom_sub_inject_synthetic(uint8_t bus, uint8_t dev, uint8_t func,
                              uint32_t dram_mb, uint32_t bench_score);
/* Run the built-in synthetic election selftest. Returns 1 on PASS, 0 on FAIL,
 * logging each assertion to serial. */
int  dom_sub_selftest(void);

#endif /* ZEOS_DOM_SUB_H */
