/*
 * Zeos — PS/2 keyboard driver
 *
 * Minimal scan code set 1 translation.
 * Handles shift, caps lock, and basic ASCII.
 */

#include "keyboard.h"
#include "idt.h"
#include "io.h"

#define KB_DATA_PORT    0x60
#define KB_STATUS_PORT  0x64
#define KB_BUF_SIZE     256

/* Circular buffer */
static char kb_buf[KB_BUF_SIZE];
static volatile uint32_t kb_head;
static volatile uint32_t kb_tail;

/* Modifier state */
static int shift_held;
static int caps_lock;

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
 * Keyboard IRQ handler (IRQ1 = vector 0x21)
 */
static void keyboard_isr(uint64_t vector, uint64_t error_code)
{
    (void)vector;
    (void)error_code;

    uint8_t scancode = inb(KB_DATA_PORT);

    /* Handle modifier keys */
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

    if (c)
        kb_buf_push(c);
}

void keyboard_init(void)
{
    kb_head = 0;
    kb_tail = 0;
    shift_held = 0;
    caps_lock = 0;

    idt_register(0x21, keyboard_isr);
    pic_unmask(1);  /* Unmask IRQ1 (keyboard) */
}

int keyboard_has_char(void)
{
    return kb_head != kb_tail;
}

char keyboard_getc(void)
{
    /* Busy-wait for a character */
    while (kb_head == kb_tail)
        __asm__ volatile("hlt");  /* Sleep until interrupt */

    char c = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}
