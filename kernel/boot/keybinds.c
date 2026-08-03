/*
 * Zeos — Keyboard Shortcuts System
 *
 * Tracks modifier state from raw scancodes, matches key combos
 * against the binding table, and dispatches actions to the WM,
 * compositor, and shell.
 *
 * Scan code set 1 conventions:
 *   - Make (press):   scancode with bit 7 clear
 *   - Break (release): scancode with bit 7 set (scancode | 0x80)
 *   - Extended keys:   preceded by 0xE0 prefix byte
 *
 * Extended scancodes (after 0xE0 prefix):
 *   0x5B = Left Super (Windows key)
 *   0x5C = Right Super
 *   0x4B = Left arrow
 *   0x4D = Right arrow
 *   0x48 = Up arrow
 *   0x50 = Down arrow
 */

#include "keybinds.h"
#include "wm.h"
#include "workspaces.h"
#include "compositor.h"
#include "io.h"

/* ── Extended scan codes (after 0xE0 prefix) ── */
#define SC_EXT_LEFT_SUPER   0x5B
#define SC_EXT_RIGHT_SUPER  0x5C
#define SC_EXT_LEFT_ARROW   0x4B
#define SC_EXT_RIGHT_ARROW  0x4D
#define SC_EXT_UP_ARROW     0x48
#define SC_EXT_DOWN_ARROW   0x50

/* ── Standard scan codes ── */
#define SC_LSHIFT     0x2A
#define SC_RSHIFT     0x36
#define SC_LCTRL      0x1D  /* Left Ctrl (standard) */
#define SC_LALT       0x38  /* Left Alt (standard)  */
#define SC_EXT_RCTRL  0x1D  /* Right Ctrl (extended) */
#define SC_EXT_RALT   0x38  /* Right Alt (extended)  */
#define SC_TAB        0x0F
#define SC_ENTER      0x1C
#define SC_SPACE      0x39
#define SC_1          0x02
#define SC_2          0x03
#define SC_3          0x04
#define SC_4          0x05
#define SC_D          0x20
#define SC_G          0x22
#define SC_I          0x17
#define SC_T          0x14
#define SC_W          0x11
#define SC_5          0x06
#define SC_6          0x07
#define SC_7          0x08
#define SC_8          0x09
#define SC_F1         0x3B
#define SC_F2         0x3C
#define SC_ESC        0x01

/* ── Modifier state ── */
static uint8_t mod_state;

/* ── Binding table ── */
static keybind_t bindings[KEYBINDS_MAX];
static int       bind_count;

/* ── Helpers ── */

static void bind(uint8_t scancode, uint8_t mods, int extended, action_id_t action)
{
    if (bind_count >= KEYBINDS_MAX)
        return;
    bindings[bind_count].scancode  = scancode;
    bindings[bind_count].modifiers = mods;
    bindings[bind_count].extended  = (uint8_t)extended;
    bindings[bind_count].action    = action;
    bind_count++;
}

/* ── Initialization ── */

void keybinds_init(void)
{
    mod_state  = MOD_NONE;
    bind_count = 0;

    /* Window management — Super + arrow keys (extended scancodes).
     * Super + Left/Right cycle: half -> two-thirds -> restore.
     * Super + Up maximizes. Super + Down restores (un-maximize / un-snap). */
    bind(SC_EXT_LEFT_ARROW,  MOD_SUPER, 1, ACTION_SNAP_LEFT);
    bind(SC_EXT_RIGHT_ARROW, MOD_SUPER, 1, ACTION_SNAP_RIGHT);
    bind(SC_EXT_UP_ARROW,    MOD_SUPER, 1, ACTION_MAXIMIZE);
    bind(SC_EXT_DOWN_ARROW,  MOD_SUPER, 1, ACTION_RESTORE);

    /* Quadrant snapping — Super + 1/2/3/4, also Super + Shift + arrows. */
    bind(SC_1, MOD_SUPER, 0, ACTION_SNAP_TL);
    bind(SC_2, MOD_SUPER, 0, ACTION_SNAP_TR);
    bind(SC_3, MOD_SUPER, 0, ACTION_SNAP_BL);
    bind(SC_4, MOD_SUPER, 0, ACTION_SNAP_BR);
    bind(SC_EXT_LEFT_ARROW,  MOD_SUPER | MOD_SHIFT, 1, ACTION_SNAP_TL);
    bind(SC_EXT_RIGHT_ARROW, MOD_SUPER | MOD_SHIFT, 1, ACTION_SNAP_TR);
    bind(SC_EXT_DOWN_ARROW,  MOD_SUPER | MOD_SHIFT, 1, ACTION_MOVE_TO_NEXT_DISPLAY);

    /* Escape cancels an in-progress drag (no-op when no drag is active). */
    bind(SC_ESC, MOD_NONE, 0, ACTION_CANCEL_DRAG);

    /* Super + T — toggle tiling */
    bind(SC_T, MOD_SUPER, 0, ACTION_TOGGLE_TILING);

    /* Super + W — close (detach) surface */
    bind(SC_W, MOD_SUPER, 0, ACTION_CLOSE_SURFACE);

    /* Alt + Tab — cycle surfaces */
    bind(SC_TAB, MOD_ALT, 0, ACTION_CYCLE_SURFACE);

    /* Desktop shortcuts */
    bind(SC_D,     MOD_SUPER,               0, ACTION_SHOW_DESKTOP);
    bind(SC_SPACE, MOD_SUPER,               0, ACTION_COMMAND_PALETTE);
    bind(SC_D,     MOD_SUPER | MOD_SHIFT,   0, ACTION_SHOW_DOCK);

    /* Workspaces */
    bind(SC_F1, MOD_SUPER, 0, ACTION_WORKSPACE_1);
    bind(SC_F2, MOD_SUPER, 0, ACTION_WORKSPACE_2);
    /* Workspace prev/next on Super+Ctrl+arrow; up = overview. */
    bind(SC_EXT_RIGHT_ARROW, MOD_SUPER | MOD_CTRL, 1, ACTION_WORKSPACE_NEXT);
    bind(SC_EXT_LEFT_ARROW,  MOD_SUPER | MOD_CTRL, 1, ACTION_WORKSPACE_PREV);
    bind(SC_EXT_UP_ARROW,    MOD_SUPER | MOD_CTRL, 1, ACTION_WORKSPACE_OVERVIEW);

    /* Super+Ctrl+1..8 — switch directly to workspace N */
    bind(SC_1, MOD_SUPER | MOD_CTRL, 0, ACTION_WORKSPACE_1);
    bind(SC_2, MOD_SUPER | MOD_CTRL, 0, ACTION_WORKSPACE_2);
    bind(SC_3, MOD_SUPER | MOD_CTRL, 0, ACTION_WORKSPACE_3);
    bind(SC_4, MOD_SUPER | MOD_CTRL, 0, ACTION_WORKSPACE_4);
    bind(SC_5, MOD_SUPER | MOD_CTRL, 0, ACTION_WORKSPACE_5);
    bind(SC_6, MOD_SUPER | MOD_CTRL, 0, ACTION_WORKSPACE_6);
    bind(SC_7, MOD_SUPER | MOD_CTRL, 0, ACTION_WORKSPACE_7);
    bind(SC_8, MOD_SUPER | MOD_CTRL, 0, ACTION_WORKSPACE_8);

    /* Super+Ctrl+Shift+1..8 — move focused window then switch */
    bind(SC_1, MOD_SUPER | MOD_CTRL | MOD_SHIFT, 0, ACTION_MOVE_WINDOW_WS_1);
    bind(SC_2, MOD_SUPER | MOD_CTRL | MOD_SHIFT, 0, ACTION_MOVE_WINDOW_WS_2);
    bind(SC_3, MOD_SUPER | MOD_CTRL | MOD_SHIFT, 0, ACTION_MOVE_WINDOW_WS_3);
    bind(SC_4, MOD_SUPER | MOD_CTRL | MOD_SHIFT, 0, ACTION_MOVE_WINDOW_WS_4);
    bind(SC_5, MOD_SUPER | MOD_CTRL | MOD_SHIFT, 0, ACTION_MOVE_WINDOW_WS_5);
    bind(SC_6, MOD_SUPER | MOD_CTRL | MOD_SHIFT, 0, ACTION_MOVE_WINDOW_WS_6);
    bind(SC_7, MOD_SUPER | MOD_CTRL | MOD_SHIFT, 0, ACTION_MOVE_WINDOW_WS_7);
    bind(SC_8, MOD_SUPER | MOD_CTRL | MOD_SHIFT, 0, ACTION_MOVE_WINDOW_WS_8);

    /* System */
    bind(SC_ENTER, MOD_SUPER, 0, ACTION_OPEN_TERMINAL);
    bind(SC_I,     MOD_SUPER, 0, ACTION_INSPECT_CHAIN);
    bind(SC_G,     MOD_SUPER, 0, ACTION_CHAIN_GRAPH);
}

/* ── Modifier tracking ── */

/*
 * Update modifier state from a raw scancode.
 * Called for every scancode (make and break) before matching.
 * Returns 1 if this scancode was purely a modifier (should not match bindings).
 */
static int update_modifiers(uint8_t scancode, int extended)
{
    int is_break = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;  /* Strip break bit */

    if (!extended) {
        /* Standard modifiers */
        if (code == SC_LSHIFT || code == SC_RSHIFT) {
            if (is_break)
                mod_state &= ~MOD_SHIFT;
            else
                mod_state |= MOD_SHIFT;
            return 1;
        }
        if (code == SC_LCTRL) {
            if (is_break)
                mod_state &= ~MOD_CTRL;
            else
                mod_state |= MOD_CTRL;
            return 1;
        }
        if (code == SC_LALT) {
            if (is_break)
                mod_state &= ~MOD_ALT;
            else
                mod_state |= MOD_ALT;
            return 1;
        }
    } else {
        /* Extended modifiers */
        if (code == SC_EXT_LEFT_SUPER || code == SC_EXT_RIGHT_SUPER) {
            if (is_break)
                mod_state &= ~MOD_SUPER;
            else
                mod_state |= MOD_SUPER;
            return 1;
        }
        if (code == SC_EXT_RCTRL) {
            if (is_break)
                mod_state &= ~MOD_CTRL;
            else
                mod_state |= MOD_CTRL;
            return 1;
        }
        if (code == SC_EXT_RALT) {
            if (is_break)
                mod_state &= ~MOD_ALT;
            else
                mod_state |= MOD_ALT;
            return 1;
        }
    }

    return 0;
}

/* ── Process a scancode ── */

action_id_t keybinds_process(uint8_t scancode, uint8_t modifiers, int extended)
{
    (void)modifiers;  /* We use our own tracked mod_state */

    /* Update modifier state first */
    if (update_modifiers(scancode, extended))
        return ACTION_NONE;

    /* Ignore break (release) codes for action matching */
    if (scancode & 0x80)
        return ACTION_NONE;

    /* Search bindings for a match */
    for (int i = 0; i < bind_count; i++) {
        if (bindings[i].scancode  == scancode &&
            bindings[i].modifiers == mod_state &&
            bindings[i].extended  == (uint8_t)extended)
        {
            keybinds_execute(bindings[i].action);
            return bindings[i].action;
        }
    }

    return ACTION_NONE;
}

/* ── Execute an action ── */

void keybinds_execute(action_id_t action)
{
    int focused;

    switch (action) {
    /* ── Window management ── */
    case ACTION_SNAP_LEFT:
        focused = wm_get_focused();
        if (focused >= 0) wm_snap_cycle_left(focused);
        break;

    case ACTION_SNAP_RIGHT:
        focused = wm_get_focused();
        if (focused >= 0) wm_snap_cycle_right(focused);
        break;

    case ACTION_MAXIMIZE:
        focused = wm_get_focused();
        if (focused >= 0) wm_maximize_surface(focused);
        break;

    case ACTION_RESTORE:
        focused = wm_get_focused();
        if (focused >= 0) {
            chain_surface_t *s = wm_get_surface(focused);
            if (s) {
                if (s->state == SURFACE_MAXIMIZED) {
                    /* Unmaximize: maximize toggle restores. */
                    wm_maximize_surface(focused);
                } else if (s->state != SURFACE_NORMAL && s->state != SURFACE_TILED) {
                    wm_snap(focused, SURFACE_NORMAL);
                }
                /* If already NORMAL, do nothing (don't minimize on Down). */
            }
        }
        break;

    case ACTION_MINIMIZE:
        focused = wm_get_focused();
        if (focused >= 0) wm_minimize_surface(focused);
        break;

    case ACTION_MOVE_TO_NEXT_DISPLAY:
        focused = wm_get_focused();
        if (focused >= 0) wm_move_to_next_display(focused);
        break;

    case ACTION_CANCEL_DRAG:
        /* Esc also exits workspace overview */
        if (workspaces_overview_handle_escape()) break;
        (void)wm_cancel_drag();
        break;

    case ACTION_SNAP_TL:
        focused = wm_get_focused();
        if (focused >= 0) wm_snap_surface(focused, SURFACE_SNAPPED_TL);
        break;

    case ACTION_SNAP_TR:
        focused = wm_get_focused();
        if (focused >= 0) wm_snap_surface(focused, SURFACE_SNAPPED_TR);
        break;

    case ACTION_SNAP_BL:
        focused = wm_get_focused();
        if (focused >= 0) wm_snap_surface(focused, SURFACE_SNAPPED_BL);
        break;

    case ACTION_SNAP_BR:
        focused = wm_get_focused();
        if (focused >= 0) wm_snap_surface(focused, SURFACE_SNAPPED_BR);
        break;

    case ACTION_TOGGLE_TILING:
        wm_toggle_tiling();
        break;

    case ACTION_CLOSE_SURFACE:
        focused = wm_get_focused();
        if (focused >= 0) wm_detach_surface(focused);
        break;

    case ACTION_CYCLE_SURFACE:
        /*
         * Cycle focus to the next visible surface on the current workspace.
         * Simple round-robin: find current focused, advance to next visible.
         */
        {
            int count = wm_surface_count();
            int current = wm_get_focused();
            int ws = wm_get_workspace();
            int started = 0;

            for (int i = 0; i < count; i++) {
                chain_surface_t *s = wm_get_surface(i);
                if (!s || !s->visible || s->workspace != ws)
                    continue;
                if (!started) {
                    if (s->id == current)
                        started = 1;
                    continue;
                }
                /* Focus next visible surface after current */
                wm_focus_surface(s->id);
                return;
            }
            /* Wrap around: focus first visible surface */
            for (int i = 0; i < count; i++) {
                chain_surface_t *s = wm_get_surface(i);
                if (s && s->visible && s->workspace == ws) {
                    wm_focus_surface(s->id);
                    return;
                }
            }
        }
        break;

    /* ── Desktop ── */
    case ACTION_SHOW_DESKTOP:
        /*
         * Spring-animate all surfaces: scale to 0.9 + fade out.
         * Toggle: if already shown, spring back.
         */
        if (wm_is_desktop_shown())
            wm_restore_desktop();
        else
            wm_show_desktop();
        break;

    case ACTION_COMMAND_PALETTE:
        { extern void palette_toggle(void); palette_toggle(); }
        break;

    case ACTION_SHOW_DOCK:
        {
            compositor_t *comp = compositor_get_state();
            compositor_show_dock(comp ? !comp->dock_visible : 1);
        }
        break;

    /* ── Workspaces ── */
    case ACTION_WORKSPACE_1: workspace_switch(0); break;
    case ACTION_WORKSPACE_2: workspace_switch(1); break;
    case ACTION_WORKSPACE_3: workspace_switch(2); break;
    case ACTION_WORKSPACE_4: workspace_switch(3); break;
    case ACTION_WORKSPACE_5: workspace_switch(4); break;
    case ACTION_WORKSPACE_6: workspace_switch(5); break;
    case ACTION_WORKSPACE_7: workspace_switch(6); break;
    case ACTION_WORKSPACE_8: workspace_switch(7); break;

    case ACTION_WORKSPACE_NEXT:
        {
            int ws = workspace_active();
            int n = workspaces_get_count();
            if (ws < n - 1) workspace_switch(ws + 1);
        }
        break;

    case ACTION_WORKSPACE_PREV:
        {
            int ws = workspace_active();
            if (ws > 0) workspace_switch(ws - 1);
        }
        break;

    case ACTION_WORKSPACE_OVERVIEW:
        if (workspaces_overview_active())
            workspaces_overview_exit();
        else
            workspaces_overview_enter();
        break;

    case ACTION_MOVE_WINDOW_WS_1: workspace_move_focused_and_switch(0); break;
    case ACTION_MOVE_WINDOW_WS_2: workspace_move_focused_and_switch(1); break;
    case ACTION_MOVE_WINDOW_WS_3: workspace_move_focused_and_switch(2); break;
    case ACTION_MOVE_WINDOW_WS_4: workspace_move_focused_and_switch(3); break;
    case ACTION_MOVE_WINDOW_WS_5: workspace_move_focused_and_switch(4); break;
    case ACTION_MOVE_WINDOW_WS_6: workspace_move_focused_and_switch(5); break;
    case ACTION_MOVE_WINDOW_WS_7: workspace_move_focused_and_switch(6); break;
    case ACTION_MOVE_WINDOW_WS_8: workspace_move_focused_and_switch(7); break;

    /* ── System ── */
    case ACTION_OPEN_TERMINAL:
        /* TODO: create terminal surface — needs shell_create_surface() */
        break;

    case ACTION_INSPECT_CHAIN:
        /* TODO: open signal inspector overlay */
        break;

    case ACTION_CHAIN_GRAPH:
        /* K.1: toggle the chain-graph (sigviz) overlay. */
        { extern void sigviz_overlay_toggle(void);
          extern void compositor_dirty_all(void);
          sigviz_overlay_toggle();
          compositor_dirty_all(); }
        break;

    case ACTION_NONE:
    case ACTION_COUNT:
        break;
    }
}

/* ── Rebind ── */

void keybinds_set(action_id_t action, uint8_t scancode, uint8_t modifiers, int extended)
{
    /* Find existing binding for this action and update it */
    for (int i = 0; i < bind_count; i++) {
        if (bindings[i].action == action) {
            bindings[i].scancode  = scancode;
            bindings[i].modifiers = modifiers;
            bindings[i].extended  = (uint8_t)extended;
            return;
        }
    }

    /* No existing binding — add a new one */
    bind(scancode, modifiers, extended, action);
}

/* ── Query ── */

uint8_t keybinds_get_modifiers(void)
{
    return mod_state;
}
