/*
 * Zeos — Serial console (16550 UART)
 *
 * COM1 at 0x3F8, 115200 baud, 8N1.
 *
 * TX: polling, used by kprint as the early debug channel.
 *
 * RX: interrupt-driven via IRQ4 (vector 0x24). The ISR drains the
 *     UART RX FIFO into a small lock-free ring; CHAIN_SERIAL_IN's
 *     resolve drains that ring into ASCII input_event records each
 *     scheduler tick. A polling fallback also runs on every drain so
 *     we still capture bytes if the IRQ never wires up (e.g. some
 *     QEMU stdio configurations don't raise the COM1 IRQ on incoming
 *     bytes — the LSR.DR bit is the source of truth either way).
 */

#include "serial.h"
#include "io.h"
#include "idt.h"
#include "idle.h"

#define COM1 0x3F8

/* ── RX ring ──────────────────────────────────────────────────────
 * Power-of-two sized so head/tail wrap with a mask. Single producer
 * (the IRQ handler / poll drain) and single consumer (the chain
 * resolve), so a plain volatile pair is sufficient.
 */
#define SERIAL_RX_RING 64
static volatile uint8_t  s_rx_ring[SERIAL_RX_RING];
static volatile uint32_t s_rx_head;     /* writer */
static volatile uint32_t s_rx_tail;     /* reader */
static volatile uint32_t s_rx_dropped;  /* overflow counter */

static inline void rx_push(uint8_t b)
{
    uint32_t next = (s_rx_head + 1u) & (SERIAL_RX_RING - 1u);
    if (next == s_rx_tail) {
        s_rx_dropped++;
        return;
    }
    s_rx_ring[s_rx_head] = b;
    s_rx_head = next;
    /* UART RX is also an input source -- mark active so a serial
     * console session keeps the screen awake. The decode-into-ASCII
     * path further down ALSO calls idle_mark_active() via
     * keyboard_inject_char, but bumping here ensures non-printable
     * control bytes (which the ASCII decoder drops) still count. */
    idle_mark_active();
}

/* Drain whatever is sitting in the UART RX FIFO into the ring. Safe
 * to call from IRQ context or from the chain resolve. */
static void serial_rx_drain_fifo(void)
{
    /* LSR bit 0 = Data Ready. Read up to 16 bytes per call to bound
     * IRQ latency, more than enough for 16-byte FIFO depth. */
    for (int i = 0; i < 16; i++) {
        if (!(inb(COM1 + 5) & 0x01)) return;
        uint8_t b = (uint8_t)inb(COM1 + 0);
        rx_push(b);
    }
}

static volatile uint32_t s_isr_count;
static void serial_isr(uint64_t vector, uint64_t error_code)
{
    (void)vector;
    (void)error_code;
    s_isr_count++;
    (void)inb(COM1 + 2);
    serial_rx_drain_fifo();
}
uint32_t serial_isr_count_get(void) { return s_isr_count; }

void serial_init(void)
{
    s_rx_head = 0;
    s_rx_tail = 0;
    s_rx_dropped = 0;

    outb(COM1 + 1, 0x00);  /* IER: no IRQs during config */
    outb(COM1 + 3, 0x80);  /* LCR: DLAB=1 */
    outb(COM1 + 0, 0x01);  /* DLL: divisor lo = 1 (115200) */
    outb(COM1 + 1, 0x00);  /* DLM: divisor hi = 0 */
    outb(COM1 + 3, 0x03);  /* LCR: 8N1, DLAB=0 */
    outb(COM1 + 2, 0x01);  /* FCR: enable FIFO only (no clear yet) */
    outb(COM1 + 2, 0x07);  /* FCR: enable FIFO + clear RX/TX, 1B trig */
    outb(COM1 + 4, 0x0F);  /* MCR: DTR|RTS|OUT1|OUT2.
                              OUT2 routes UART IRQ to the PIC. */

    /* Drain anything left in the RX FIFO from the loopback test or
     * from firmware-side console handoff. Some QEMU configurations
     * stop forwarding stdin until the FIFO has been read at least
     * once. */
    for (int i = 0; i < 16; i++) {
        if (!(inb(COM1 + 5) & 0x01)) break;
        (void)inb(COM1 + 0);
    }

    /* Enable RX-data-available interrupt (bit 0 of IER). RX timeout
     * (bit 2) keeps us from sitting on a partial FIFO — the timeout
     * fires after ~4 char-times of idle. */
    outb(COM1 + 1, 0x05);
}

/* IRQ4 (COM1) wiring is separate from serial_init() because the IDT
 * isn't initialized yet at the time main.c calls serial_init(). The
 * caller (main.c) hooks this in after idt_init(). */
void serial_irq_init(void)
{
    /* COM1 = IRQ4 = vector 0x24 (PIC remap base 0x20 + 4). */
    idt_register(0x24, serial_isr);
    pic_unmask(4);
}

static int serial_tx_ready(void)
{
    return inb(COM1 + 5) & 0x20;  /* THR empty */
}

void serial_putc(char c)
{
    /* Wait for transmit buffer to be empty */
    while (!serial_tx_ready())
        ;
    outb(COM1, c);

    /* Send \r before \n for terminal compatibility */
    if (c == '\n') {
        while (!serial_tx_ready())
            ;
        outb(COM1, '\r');
    }
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

void serial_put_hex(uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    int i;
    for (i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xf];
        val >>= 4;
    }
    buf[16] = '\0';
    char *p = buf;
    while (*p == '0' && p < buf + 15)
        p++;
    serial_puts(p);
}

/* Returns 1 + char if a byte is waiting on the UART, 0 otherwise.
 * Reads from the UART directly (legacy callers); does NOT touch the
 * IRQ ring. New code uses serial_rx_pending / serial_rx_pop instead. */
int serial_try_getc(char *out)
{
    if (!(inb(COM1 + 5) & 0x01)) return 0;  /* RX data ready? */
    *out = (char)inb(COM1 + 0);
    return 1;
}

/* ── Chain RX API ────────────────────────────────────────────────
 * CHAIN_SERIAL_IN's resolve calls these. */

int serial_rx_pending(void)
{
    /* Top up the ring from the FIFO before reporting. This makes the
     * polling fallback work on systems where IRQ4 never fires (some
     * QEMU stdio configurations, certain UEFI handoffs, etc.). The
     * ISR drains the same FIFO when an interrupt does fire. */
    serial_rx_drain_fifo();
    return (int)((s_rx_head - s_rx_tail) & (SERIAL_RX_RING - 1u));
}

int serial_rx_pop(uint8_t *out)
{
    if (s_rx_tail == s_rx_head) return 0;
    *out = s_rx_ring[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1u) & (SERIAL_RX_RING - 1u);
    return 1;
}

uint32_t serial_rx_dropped(void) { return s_rx_dropped; }

void serial_put_dec(uint64_t val)
{
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (val == 0) {
        serial_putc('0');
        return;
    }
    while (val > 0) {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    }
    serial_puts(&buf[i]);
}
