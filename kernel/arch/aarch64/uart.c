/* Zeos aarch64 — console over PL011 (QEMU virt UART0 @ 0x0900_0000).
 * Exposes the real kprint.h API so portable modules link unchanged. */
#include <stdint.h>

#define UART0_BASE   0x09000000UL
#define UART_DR      (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART_FR      (*(volatile uint32_t *)(UART0_BASE + 0x18))
#define UART_FR_TXFF (1u << 5)

void serial_putc(char c)
{ while (UART_FR & UART_FR_TXFF) { } UART_DR = (uint32_t)(unsigned char)c; }

void kputc(char c)
{ if (c == '\n') serial_putc('\r'); serial_putc(c); }

void kputs(const char *s) { for (; *s; ++s) kputc(*s); }

void kput_hex(uint64_t v)
{
    kputs("0x");
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((v >> i) & 0xF);
        serial_putc(d < 10 ? ('0' + d) : ('a' + d - 10));
    }
}

void kput_dec(uint64_t v)
{
    char tmp[21]; int i = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v) { tmp[i++] = '0' + (int)(v % 10); v /= 10; }
    while (i--) serial_putc(tmp[i]);
}

/* kprint.h surface stubs (no framebuffer yet on the ARM port). */
void kprint_init(void) { }
void kprint_set_splash_mode(int on) { (void)on; }
void kprint_log_prefix(void) { }
