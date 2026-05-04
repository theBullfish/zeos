/*
 * Zeos -- ACPI power button + lid switch.
 *
 * Polls PM1a/PM1b PWRBTN_STS each tick (cleared on read) and walks
 * DSDT/SSDT for the device HIDs (PNP0C0C power button, PNP0C0D lid,
 * PNP0C0E sleep button). Configurable actions persist in VAULT and
 * dispatch through suspend.c, lockscreen.c, notify.c, and idle.c.
 */

#ifndef ZEOS_POWER_BUTTONS_H
#define ZEOS_POWER_BUTTONS_H

#include <stdint.h>

typedef enum {
    POWER_ACT_SUSPEND  = 0,
    POWER_ACT_SHUTDOWN = 1,
    POWER_ACT_LOCK     = 2,
    POWER_ACT_ASK      = 3,
    POWER_ACT_NOTHING  = 4,
    POWER_ACT_WAKE     = 5,
} power_action_t;

/* Init: scan AML for PNP0C0C / PNP0C0D / PNP0C0E and load configured
 * actions from VAULT. Idempotent. */
void power_buttons_init(void);

/* Returns 1 if the fixed power button (PM1 PWRBTN_STS) was asserted
 * since the previous call. Clears the bit in PM1a/PM1b. */
int  power_button_pressed(void);

/* Returns 1 if a lid-state change happened since the previous call,
 * with `*new_state` set to 1=open, 0=closed (best-effort: PM1
 * status bit + cached _LID literal). Returns 0 if no change. */
int  lid_state_changed(int *new_state);

/* Snapshot accessors. */
int  power_btn_present(void);    /* PNP0C0C found */
int  sleep_btn_present(void);    /* PNP0C0E found */
int  lid_present(void);          /* PNP0C0D found */
int  lid_current_state(void);    /* 1=open, 0=closed, -1 unknown */

/* Configured actions. Setters persist to VAULT. */
power_action_t power_button_action(void);
power_action_t lid_close_action(void);
power_action_t lid_open_action(void);
int            power_button_set_action(power_action_t a);
int            lid_close_set_action(power_action_t a);
int            lid_open_set_action(power_action_t a);

/* Stats (selftest / debug). */
uint32_t power_button_event_count(void);
uint32_t lid_event_count(void);

/* Map enum <-> string. */
const char    *power_action_label(power_action_t a);
int            power_action_parse(const char *s, power_action_t *out);

/* Wire CHAIN_POWER_INPUT into the registry. Returns chain id, -1 on
 * failure. parent typically CHAIN_CPU. */
int            power_buttons_chain_register(int parent_chain_id);

extern int CHAIN_POWER_INPUT;

#endif /* ZEOS_POWER_BUTTONS_H */
