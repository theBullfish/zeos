/*
 * Zeos — PS/2 keyboard driver
 *
 * Minimal scan code set 1 translation.
 * Handles shift, caps lock, and basic ASCII.
 */

#include "keyboard.h"
#include "keybinds.h"
#include "idt.h"
#include "io.h"
#include "hda.h"
#include "brightness.h"
#include "idle.h"
#include "lockscreen.h"

/* Media-key scancodes (set 1, with the 0xE0 prefix flagged via the
 * `extended` arg to process_scancode). These are the "Multimedia"
 * codes used by virtually every modern keyboard for volume +/-/mute,
 * including the F-key overlays on most laptops. */
#define SC_EXT_VOL_UP     0x30
#define SC_EXT_VOL_DOWN   0x2E
#define SC_EXT_MUTE       0x20

/* XF86MonBrightnessUp / XF86MonBrightnessDown — laptop firmware
 * convention varies wildly; the most commonly observed PS/2 set-1
 * extended codes are 0xE0 0x04 (up) and 0xE0 0x03 (down). Real
 * laptops often emit the ACPI Notify path instead, which lands as a
 * separate event — those will route through here once the ACPI
 * hotkey driver lands. */
#define SC_EXT_BRIGHT_UP    0x04
#define SC_EXT_BRIGHT_DOWN  0x03

#define KB_DATA_PORT    0x60
#define KB_STATUS_PORT  0x64
#define KB_BUF_SIZE     256

/* Circular buffer */
static char kb_buf[KB_BUF_SIZE];
static volatile uint32_t kb_head;
static volatile uint32_t kb_tail;

/* Raw scancode ring for the CHAIN_KEYBOARD pipeline. The IRQ handler
 * pushes scancodes here so the chain resolve can drain them on the
 * scheduler tick — the chain becomes the source of truth for keyboard
 * events visible to MasQ / B3, while the IRQ-side fast-path still keeps
 * the existing kb_buf alive for blocking shell paths during early boot. */
#define KB_SC_RING 256
static volatile uint8_t  kb_sc_ring[KB_SC_RING];
static volatile uint8_t  kb_sc_ext[KB_SC_RING];
static volatile uint32_t kb_sc_head;
static volatile uint32_t kb_sc_tail;
static volatile uint32_t kb_sc_drained;   /* events drained by chain */
static volatile uint32_t kb_pending_irqs; /* scheduler wake counter  */

static void kb_sc_push(uint8_t sc, int ext)
{
    uint32_t next = (kb_sc_head + 1) % KB_SC_RING;
    if (next != kb_sc_tail) {
        kb_sc_ring[kb_sc_head] = sc;
        kb_sc_ext[kb_sc_head]  = (uint8_t)(ext ? 1 : 0);
        kb_sc_head = next;
        kb_pending_irqs++;
    }
}

int keyboard_chain_pending(void)
{
    return (int)((kb_sc_head - kb_sc_tail) & (KB_SC_RING - 1));
}

uint32_t keyboard_chain_drain(void)
{
    /* Called by CHAIN_KEYBOARD's resolve. Drains scancodes through the
     * existing process_scancode pipeline (which pushes ASCII into
     * kb_buf for the shell). Returns count drained this tick. */
    uint32_t n = 0;
    while (kb_sc_tail != kb_sc_head) {
        uint8_t sc = kb_sc_ring[kb_sc_tail];
        uint8_t ex = kb_sc_ext[kb_sc_tail];
        kb_sc_tail = (kb_sc_tail + 1) % KB_SC_RING;
        /* process_scancode is the shared decode path; calling it from
         * the chain resolve makes the chain the canonical event source. */
        extern void keyboard_inject_scancode(uint8_t scancode, int extended);
        keyboard_inject_scancode(sc, (int)ex);
        n++;
        kb_sc_drained++;
    }
    return n;
}

uint32_t keyboard_chain_total(void)
{
    return kb_sc_drained;
}

/* Modifier state */
static int shift_held;
static int caps_lock;

/* Extended scancode prefix tracking */
static int e0_prefix;

/* Scan code set 1 → ASCII (unshifted) */
static const char scancode_to_ascii[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6',  /* 0x00-0x07 */
    '7', '8', '9', '0', '-', '=', '\b', '\t', /* 0x08-0x0F */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',  /* 0x10-0x17 */
    'o', 'p', '[', ']', '\n', 0,   'a', 's',  /* 0x18-0x1F */
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',  /* 0x20-0x27 */
    '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v', /* 0x28-0x2F */
    'b', 'n', 'm', ',', '.', '/', 0,   '*',   /* 0x30-0x37 */
    0,   ' ', 0,   0,   0,   0,   0,   0,     /* 0x38-0x3F */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 0x40-0x47 */
    0,   0,   '-', 0,   0,   0,   '+', 0,     /* 0x48-0x4F */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 0x50-0x57 */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 0x58-0x5F */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 0x60-0x67 */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 0x68-0x6F */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 0x70-0x77 */
    0,   0,   0,   0,   0,   0,   0,   0,     /* 0x78-0x7F */
};

/* Shifted versions */
static const char scancode_to_ascii_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^',
    '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0,  '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*',
    0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   '-', 0,   0,   0,   '+', 0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
};

/* Scan codes for modifier keys */
#define SC_LSHIFT_PRESS   0x2A
#define SC_RSHIFT_PRESS   0x36
#define SC_LSHIFT_RELEASE 0xAA
#define SC_RSHIFT_RELEASE 0xB6
#define SC_CAPS_LOCK      0x3A

static void kb_buf_push(char c)
{
    uint32_t next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) {
        kb_buf[kb_head] = c;
        kb_head = next;
    }
}

/*
 * Process one scancode byte through the full keybinds + ASCII pipeline.
 * Shared between the PS/2 IRQ path (which reads from port 0x60) and the
 * USB HID boot-keyboard path (which translates HID usage codes to set-1
 * scancodes and feeds them through here).
 */
static void process_scancode(uint8_t scancode, int extended)
{
    /*
     * Media keys (XF86AudioRaiseVolume / LowerVolume / Mute) are
     * extended scancodes 0xE0 0x30 / 0x2E / 0x20. We dispatch them
     * directly into the audio driver before anything else so they
     * work regardless of focus or modifier state. Releases (bit 7
     * set) are ignored — only act on the press edge.
     */
    if (extended && !(scancode & 0x80)) {
        if (scancode == SC_EXT_VOL_UP)   { hda_audio_volume_step(+5); return; }
        if (scancode == SC_EXT_VOL_DOWN) { hda_audio_volume_step(-5); return; }
        if (scancode == SC_EXT_MUTE)     { hda_audio_toggle_mute();   return; }
        if (scancode == SC_EXT_BRIGHT_UP)   { brightness_step(+10); return; }
        if (scancode == SC_EXT_BRIGHT_DOWN) { brightness_step(-10); return; }
    }

    /*
     * UI overlay intercept (additive, runs BEFORE keybinds dispatch).
     * If a modal / quick look / context menu owns the focus, route the
     * keystroke into it. Also handle Ctrl-Z / Ctrl-Shift-Z / Ctrl-Y
     * for the focused undo buffer.
     */
    if (!extended && !(scancode & 0x80)) {
        extern int dirty_modal_active(void);
        extern int dirty_modal_key(int);
        extern int quick_look_active(void);
        extern int quick_look_key(int);
        extern int context_menu_active(void);
        extern int context_menu_key(int);
        extern int undo_handle_key(int, int);
        extern int image_viewer_active(void);
        extern int image_viewer_key(int);
        extern int calculator_active(void);
        extern int calculator_key(int);
        extern int editor_active(void);
        extern int editor_key(int);
        extern int file_mgr_active(void);
        extern int file_mgr_key(int);
        extern int activity_active(void);
        extern int activity_key(int);

        uint8_t mods_now = keybinds_get_modifiers();
        int sh = (mods_now & MOD_SHIFT) ? 1 : 0;

        /* Esc (scancode 0x01) closes overlays before anything else. */
        if (scancode == 0x01) {
            if (dirty_modal_active())  { dirty_modal_key(27);  return; }
            if (calculator_active())   { calculator_key(27);   return; }
            if (editor_active())       { editor_key(27);       return; }
            if (file_mgr_active())     { file_mgr_key(27);     return; }
            if (activity_active())     { activity_key(27);     return; }
            if (image_viewer_active()) { image_viewer_key(27); return; }
            if (quick_look_active())   { quick_look_key(27);   return; }
            if (context_menu_active()) { context_menu_key(27); return; }
        }

        /* Calculator: Ctrl+1/2/3 mode switch (uses the same scancodes for
         * '1'/'2'/'3': 0x02/0x03/0x04). We translate to 0x11/0x12/0x13
         * which the calculator's key handler interprets as mode commands. */
        if (calculator_active() && (mods_now & MOD_CTRL)) {
            if (scancode == 0x02) { calculator_key(0x11); return; }
            if (scancode == 0x03) { calculator_key(0x12); return; }
            if (scancode == 0x04) { calculator_key(0x13); return; }
        }

        /* Space (0x39) — close if already open, otherwise open Quick
         * Look on the currently-selected list item. The desktop is the
         * only built-in list with a selectable file path right now;
         * other lists can register their own hook here later. */
        if (scancode == 0x39) {
            if (quick_look_active()) { quick_look_key(' '); return; }
            extern void desktop_quick_look_selected(void);
            desktop_quick_look_selected();
            if (quick_look_active()) return;
        }

        /* Ctrl-Z (scancode 0x2C='z') / Ctrl-Y (scancode 0x15='y'). */
        if ((mods_now & MOD_CTRL)) {
            if (scancode == 0x2C) {  /* z */
                if (undo_handle_key(0x1A, sh)) return;
            }
            if (scancode == 0x15) {  /* y */
                if (undo_handle_key(0x19, 0)) return;
            }
        }

        /* Editor: Ctrl+S/A/F/C/V/X — translate scancode to control byte
         * before falling into the generic dispatch (which only handles
         * printable + Enter + Backspace). */
        if (editor_active() && (mods_now & MOD_CTRL)) {
            if (scancode == 0x1F) { editor_key(0x13); return; } /* Ctrl-S */
            if (scancode == 0x1E) { editor_key(0x01); return; } /* Ctrl-A */
            if (scancode == 0x21) { editor_key(0x06); return; } /* Ctrl-F */
            if (scancode == 0x2E) { editor_key(0x03); return; } /* Ctrl-C */
            if (scancode == 0x2F) { editor_key(0x16); return; } /* Ctrl-V */
            if (scancode == 0x2D) { editor_key(0x18); return; } /* Ctrl-X */
        }

        /* Generic key dispatch into modal / quick look / context menu
         * for printable characters (Enter / S / D shortcuts). */
        if (dirty_modal_active() || calculator_active() ||
            editor_active() ||
            file_mgr_active() ||
            activity_active() ||
            image_viewer_active() ||
            quick_look_active() || context_menu_active()) {
            char ch = sh ? scancode_to_ascii_shift[scancode]
                         : scancode_to_ascii[scancode];
            /* Backspace / Enter come through scancodes 0x0E / 0x1C with
             * ASCII 0x08 / 0x0D in the table; route them too. */
            if (!ch) {
                if (scancode == 0x0E) ch = 8;       /* backspace */
                else if (scancode == 0x1C) ch = 13; /* enter */
            }
            if (ch) {
                if (dirty_modal_active())  { dirty_modal_key(ch);  return; }
                if (calculator_active())   { calculator_key(ch);   return; }
                if (editor_active())       { editor_key(ch);       return; }
                if (file_mgr_active())     { file_mgr_key(ch);     return; }
                if (activity_active())     { activity_key(ch);     return; }
                if (image_viewer_active()) { image_viewer_key(ch); return; }
                if (quick_look_active())   { quick_look_key(ch);   return; }
                if (context_menu_active()) { context_menu_key(ch); return; }
            }
        }
    }

    /* Extended arrow keys → image viewer pan. We re-check here because
     * the block above is gated on !extended. */
    if (extended && !(scancode & 0x80)) {
        extern int image_viewer_active(void);
        extern int image_viewer_key_arrow(int dir);
        extern int calculator_active(void);
        extern int calculator_key_arrow(int dir);
        extern int editor_active(void);
        extern int editor_key_arrow(int dir);
        extern int file_mgr_active(void);
        extern int file_mgr_key_arrow(int dir);
        extern int activity_active(void);
        extern int activity_key_arrow(int dir);
        if (calculator_active()) {
            if (scancode == 0x4B) { calculator_key_arrow(0); return; }
            if (scancode == 0x4D) { calculator_key_arrow(1); return; }
            if (scancode == 0x48) { calculator_key_arrow(2); return; }
            if (scancode == 0x50) { calculator_key_arrow(3); return; }
        }
        if (editor_active()) {
            if (scancode == 0x4B) { editor_key_arrow(0); return; }
            if (scancode == 0x4D) { editor_key_arrow(1); return; }
            if (scancode == 0x48) { editor_key_arrow(2); return; }
            if (scancode == 0x50) { editor_key_arrow(3); return; }
        }
        if (file_mgr_active()) {
            if (scancode == 0x4B) { file_mgr_key_arrow(0); return; }
            if (scancode == 0x4D) { file_mgr_key_arrow(1); return; }
            if (scancode == 0x48) { file_mgr_key_arrow(2); return; }
            if (scancode == 0x50) { file_mgr_key_arrow(3); return; }
        }
        if (activity_active()) {
            if (scancode == 0x4B) { activity_key_arrow(0); return; }
            if (scancode == 0x4D) { activity_key_arrow(1); return; }
            if (scancode == 0x48) { activity_key_arrow(2); return; }
            if (scancode == 0x50) { activity_key_arrow(3); return; }
        }
        if (image_viewer_active()) {
            if (scancode == 0x4B) { image_viewer_key_arrow(0); return; } /* left */
            if (scancode == 0x4D) { image_viewer_key_arrow(1); return; } /* right */
            if (scancode == 0x48) { image_viewer_key_arrow(2); return; } /* up */
            if (scancode == 0x50) { image_viewer_key_arrow(3); return; } /* down */
        }
    }

    /*
     * Pass every scancode to the keybinds system first.
     * It tracks modifier state (shift/ctrl/alt/super) internally
     * and matches key combos against the binding table.
     * If a shortcut fired, consume the key — don't pass to shell.
     */
    action_id_t action = keybinds_process(scancode, 0, extended);
    if (action != ACTION_NONE)
        return;  /* Shortcut consumed this key */

    /*
     * Keybinds didn't consume it — update local shift state and
     * translate to ASCII for the shell buffer.
     * Extended keys (arrows, etc.) don't produce ASCII, so skip them.
     */
    if (extended)
        return;

    /* Handle shift modifier keys */
    if (scancode == SC_LSHIFT_PRESS || scancode == SC_RSHIFT_PRESS) {
        shift_held = 1;
        return;
    }
    if (scancode == SC_LSHIFT_RELEASE || scancode == SC_RSHIFT_RELEASE) {
        shift_held = 0;
        return;
    }
    if (scancode == SC_CAPS_LOCK) {
        caps_lock = !caps_lock;
        return;
    }

    /* Ignore key releases (bit 7 set) */
    if (scancode & 0x80)
        return;

    /*
     * Don't pass keys to shell when Super/Ctrl/Alt are held —
     * those combos are for shortcuts, even if unbound.
     * Only allow Shift through (it modifies ASCII output).
     */
    uint8_t mods = keybinds_get_modifiers();
    if (mods & (MOD_SUPER | MOD_CTRL | MOD_ALT))
        return;

    /* Keep shift_held in sync with keybinds modifier state */
    shift_held = (mods & MOD_SHIFT) ? 1 : 0;

    /* Translate to ASCII */
    char c;
    if (shift_held) {
        c = scancode_to_ascii_shift[scancode];
    } else {
        c = scancode_to_ascii[scancode];
    }

    /* Apply caps lock to letters */
    if (caps_lock && c >= 'a' && c <= 'z')
        c -= 32;
    else if (caps_lock && c >= 'A' && c <= 'Z')
        c += 32;

    if (c) {
        /* Every produced ASCII character is an input event. Bump the
         * idle timer so the lock-on-idle state machine sees activity. */
        idle_mark_active();

        /* When the lock screen is the active modal, intercept input
         * directly into the PIN entry instead of feeding kb_buf. The
         * shell pump must not see characters typed at the lock prompt. */
        if (lockscreen_active()) {
            lockscreen_input(c);
            return;
        }
        kb_buf_push(c);
    }
}

/*
 * Keyboard IRQ handler (IRQ1 = vector 0x21).
 * Reads a scancode from the PS/2 port, tracks the 0xE0 extended prefix,
 * then hands off to the shared process_scancode() pipeline.
 */
static void keyboard_isr(uint64_t vector, uint64_t error_code)
{
    (void)vector;
    (void)error_code;
    extern void lapic_eoi(void);

    uint8_t scancode = inb(KB_DATA_PORT);
    if (scancode == 0xE0) {
        e0_prefix = 1;
        /* Explicit EOI added 2026-07-23 (was missing on every path here
         * before this fix) -- see mouse.c's mouse_isr for the full note.
         * isr_dispatch also auto-EOIs the 8259 for this vector regardless,
         * so this alone was likely never the real block; lapic_eoi() is
         * unconfirmed to fix the actual symptom either. See bible-db E.7. */
        pic_eoi(1);
        lapic_eoi();
        return;
    }
    int extended = e0_prefix;
    e0_prefix = 0;
    /* Push to the scancode ring; CHAIN_KEYBOARD's resolve drains it
     * each scheduler tick. Decoding and ASCII translation happen there
     * via keyboard_inject_scancode -> process_scancode. */
    kb_sc_push(scancode, extended);
    pic_eoi(1);
    lapic_eoi();
}

void keyboard_inject_char(char c)
{
    /* Serial RX + any other ASCII source feeds into here. Treat each
     * injected character as an input event for idle tracking, and let
     * the lock screen intercept when it owns the modal. */
    idle_mark_active();
    if (lockscreen_active()) {
        lockscreen_input(c);
        return;
    }
    kb_buf_push(c);
}

void keyboard_inject_scancode(uint8_t scancode, int extended)
{
    /* USB HID boot keyboards translate HID usage codes to set-1
     * scancodes and feed them here, so the rest of the keyboard
     * subsystem (keybinds, shift/caps state, shell buffer) works
     * identically across PS/2 and USB inputs. */
    process_scancode(scancode, extended);
}

void keyboard_init(void)
{
    kb_head = 0;
    kb_tail = 0;
    shift_held = 0;
    caps_lock = 0;
    e0_prefix = 0;

    /* Initialize keyboard shortcuts before enabling interrupts */
    keybinds_init();

    idt_register(0x21, keyboard_isr);
    pic_unmask(1);  /* Unmask IRQ1 (keyboard) */
}

int keyboard_has_char(void)
{
    return kb_head != kb_tail;
}

/* Forward decl -- pulled from usb_hid.h. We avoid the include to keep
 * keyboard.c independent; the symbol is provided by usb_hid.c (or stays
 * unresolved if HID isn't linked in, but in this build it always is). */
extern void usb_hid_poll(void);

char keyboard_getc(void)
{
    /* Drain USB HID interrupt-IN reports while we wait. The PS/2 IRQ
     * pushes scancodes into kb_sc_ring; without the scheduler running
     * we drain them inline here so blocking callers (early boot paths)
     * still observe ASCII output. */
    while (kb_head == kb_tail) {
        usb_hid_poll();
        if (kb_sc_tail != kb_sc_head)
            keyboard_chain_drain();
        __asm__ volatile("pause");
    }

    char c = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}

int keyboard_try_getc(char *out)
{
    if (kb_head == kb_tail)
        return 0;
    *out = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return 1;
}

uint32_t keyboard_pending_irqs(void)
{
    return kb_pending_irqs;
}
