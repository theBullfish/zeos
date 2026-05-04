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

    uint8_t scancode = inb(KB_DATA_PORT);
    if (scancode == 0xE0) {
        e0_prefix = 1;
        return;
    }
    int extended = e0_prefix;
    e0_prefix = 0;
    /* Push to the scancode ring; CHAIN_KEYBOARD's resolve drains it
     * each scheduler tick. Decoding and ASCII translation happen there
     * via keyboard_inject_scancode -> process_scancode. */
    kb_sc_push(scancode, extended);
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
