/*
 * Zeos -- Idle / lock-on-idle subsystem.
 *
 * Tracks the timestamp (TSC) of the most recent input event from any
 * source -- PS/2 keyboard, PS/2 mouse, USB HID, UART RX. The CHAIN_IDLE
 * resolve consults it on a 120ms cadence to drive ACTIVE -> DIMMED ->
 * LOCKED -> BLANKED transitions. The lock screen and PIN prompt live
 * in lockscreen.{c,h}; this file is just the timer + state machine.
 */

#ifndef ZEOS_IDLE_H
#define ZEOS_IDLE_H

#include <stdint.h>

typedef enum {
    IDLE_ACTIVE  = 0,
    IDLE_DIMMED  = 1,
    IDLE_LOCKED  = 2,
    IDLE_BLANKED = 3,
} idle_state_t;

/* Single chokepoint -- every input source calls this on every event. */
void          idle_mark_active(void);

/* Wire CHAIN_IDLE into the registry. Called from chain_registry_init.
 * Returns the chain id, or -1 on failure. parent is typically CHAIN_CPU. */
int           idle_chain_register(int parent_chain_id);

/* Snapshot accessors for shell + selftest. */
idle_state_t  idle_get_state(void);
uint32_t      idle_dim_secs(void);
uint32_t      idle_lock_secs(void);
uint32_t      idle_blank_secs(void);
uint64_t      idle_seconds_since_input(void);

/* Configuration -- persisted to VAULT under /idle/{dim,lock,blank}_secs. */
int           idle_set_dim(uint32_t secs);
int           idle_set_lock(uint32_t secs);
int           idle_set_blank(uint32_t secs);

/* Force an immediate transition into LOCKED. Equivalent to the shell
 * `lock` command. Idempotent. */
void          idle_force_lock(void);

/* Force LOCKED/BLANKED -> ACTIVE. Called by the lock screen after a
 * successful PIN unlock; nothing else should call this. */
void          idle_force_unlock(void);

/* Returns IDLE chain id (-1 if not yet registered). */
int           idle_chain_id(void);

#endif /* ZEOS_IDLE_H */
