/*
 * Zeos — Keyboard Shortcuts System
 *
 * Maps key combinations (scancode + modifiers) to desktop actions.
 * This is the layer between the raw keyboard driver and the WM/compositor/shell.
 *
 * Modifier tracking: shift/ctrl/alt/super state maintained from
 * make/break scancodes. Super = 0xE0 0x5B (left) / 0xE0 0x5C (right).
 *
 * All bindings stored in a static table — no heap allocation.
 * Rebindable at runtime via keybinds_set().
 */

#ifndef ZEOS_KEYBINDS_H
#define ZEOS_KEYBINDS_H

#include <stdint.h>

/* ── Modifier flags ── */
#define MOD_NONE   0x00
#define MOD_SHIFT  0x01
#define MOD_CTRL   0x02
#define MOD_ALT    0x04
#define MOD_SUPER  0x08  /* "Windows key" / Meta */

/* ── Action IDs ── */
typedef enum {
    ACTION_NONE = 0,

    /* Window management */
    ACTION_SNAP_LEFT,           /* Super + Left  (cycle: half -> 2/3 -> restore) */
    ACTION_SNAP_RIGHT,          /* Super + Right (cycle: half -> 2/3 -> restore) */
    ACTION_MAXIMIZE,            /* Super + Up         */
    ACTION_RESTORE,             /* Super + Down  (un-maximize / un-snap)        */
    ACTION_MINIMIZE,            /* (no default binding — kept for palette use)  */
    ACTION_SNAP_TL,             /* Super + 1, Super + Shift + Left */
    ACTION_SNAP_TR,             /* Super + 2, Super + Shift + Right */
    ACTION_SNAP_BL,             /* Super + 3 */
    ACTION_SNAP_BR,             /* Super + 4 */
    ACTION_MOVE_TO_NEXT_DISPLAY,/* Super + Shift + Down */
    ACTION_CANCEL_DRAG,         /* Escape — cancel an in-progress window drag */
    ACTION_TOGGLE_TILING,       /* Super + T          */
    ACTION_CLOSE_SURFACE,       /* Super + W (detach) */
    ACTION_CYCLE_SURFACE,       /* Alt + Tab          */

    /* Desktop */
    ACTION_SHOW_DESKTOP,        /* Super + D          */
    ACTION_COMMAND_PALETTE,     /* Super + Space      */
    ACTION_SHOW_DOCK,           /* Super + Shift + D  */

    /* Workspaces */
    ACTION_WORKSPACE_1,         /* Super + F1         */
    ACTION_WORKSPACE_2,         /* Super + F2         */
    ACTION_WORKSPACE_NEXT,      /* Super + Shift + Right */
    ACTION_WORKSPACE_PREV,      /* Super + Shift + Left  */

    /* System */
    ACTION_OPEN_TERMINAL,       /* Super + Enter      */
    ACTION_INSPECT_CHAIN,       /* Super + I (signal inspector) */
    ACTION_CHAIN_GRAPH,         /* Super + G (show chain graph) */

    ACTION_COUNT
} action_id_t;

/* ── Keybind entry ── */
typedef struct {
    uint8_t     scancode;       /* Scan code set 1 (0xE0 prefix stripped, flagged separately) */
    uint8_t     modifiers;      /* Bitmask of MOD_* flags */
    uint8_t     extended;       /* 1 if scancode was preceded by 0xE0 */
    action_id_t action;
} keybind_t;

/* ── Maximum bindings ── */
#define KEYBINDS_MAX 64

/* ── API ── */

/* Initialize default key bindings */
void keybinds_init(void);

/*
 * Process a raw scancode + current modifier state.
 * Called from the keyboard ISR after modifier tracking.
 * Returns the action_id if a binding matched and was executed, ACTION_NONE otherwise.
 * If a binding matches, the key is consumed (not passed to the shell).
 */
action_id_t keybinds_process(uint8_t scancode, uint8_t modifiers, int extended);

/* Execute a specific action by ID (calls into wm, compositor, shell) */
void keybinds_execute(action_id_t action);

/* Rebind a key: assign a new scancode + modifiers to an action */
void keybinds_set(action_id_t action, uint8_t scancode, uint8_t modifiers, int extended);

/* Get current modifier state (maintained by keybinds from make/break codes) */
uint8_t keybinds_get_modifiers(void);

#endif /* ZEOS_KEYBINDS_H */
