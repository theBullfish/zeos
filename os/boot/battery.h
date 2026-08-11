/*
 * Zeos — ACPI battery + AC adapter detection.
 *
 * Pattern-scans DSDT/SSDT AML for `_BAT`/`_AC` device blocks and
 * extracts static `_BIF` (Battery Information) + `_BST` (Battery
 * Status) Package fields where the firmware inlined them. Does NOT
 * implement a full AML evaluator: any battery whose values are
 * computed by AML methods (the common case on modern laptops with
 * an EC) reports BAT_UNKNOWN until a real evaluator lands.
 *
 * Smart Battery System (SBS / I²C) is a separate path — not handled
 * here.
 */

#ifndef ZEOS_BATTERY_H
#define ZEOS_BATTERY_H

#include <stdint.h>

#define BATTERY_MAX 4

typedef enum {
    BAT_UNKNOWN = 0,
    BAT_CHARGING,
    BAT_DISCHARGING,
    BAT_FULL,
    BAT_CRITICAL
} battery_state_t;

typedef struct {
    int             present;        /* 1 if a _BAT device was located */
    int             values_known;   /* 1 if _BIF/_BST static fields decoded */
    uint32_t        design_capacity;/* mWh or mAh; 0 if unknown */
    uint32_t        last_full;      /* mWh or mAh; 0 if unknown */
    uint32_t        remaining;      /* mWh or mAh; 0 if unknown */
    uint32_t        rate;           /* mW or mA; 0 if unknown / not draining */
    int             power_unit;     /* 0 = mW/mWh, 1 = mA/mAh; -1 unknown */
    battery_state_t state;
} battery_info_t;

void battery_init(void);
int  battery_present(void);          /* >=1 = at least one battery */
int  battery_count(void);

int  battery_percent(int idx);       /* 0..100, or -1 unknown */
int  battery_remaining_minutes(int idx); /* >=0, or -1 unknown */
battery_state_t battery_state(int idx);

int  ac_online(void);                /* 1 = on AC, 0 = on battery, -1 unknown */

/* Refresh cached values. Called from CHAIN_BATTERY's resolve every ~4s.
 * Returns 1 if any cached value changed (caller should bump vault_version). */
int  battery_refresh(void);

const battery_info_t *battery_info(int idx);

#endif /* ZEOS_BATTERY_H */
