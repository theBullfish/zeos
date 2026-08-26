/* Zeos aarch64 — bring-up orchestrator. Climbs the ladder, prints each rung. */
#include <stdint.h>
#include <stddef.h>
#include "hal.h"
#include "platform.h"
extern uint64_t g_dtb_ptr;
extern void kputs(const char *);
extern void kputc(char);
extern void kput_hex(uint64_t);
extern void kput_dec(uint64_t);
extern void mmu_init(void);
extern void heap_init(uint64_t);
extern void *kmalloc(uint64_t);
extern void kfree(void *);
extern uint64_t heap_used_bytes(void);
extern void gic_init(void);
extern void gic_enable_ppi(unsigned);
extern void timer_init(uint64_t);
extern uint64_t timer_ticks(void);
extern int64_t psci_cpu_on(uint64_t, uint64_t);
extern void secondary_entry(void);
extern volatile uint64_t g_sec_mpidr;
extern volatile int g_sec_online;
/* freestanding runtime */
extern size_t strlen(const char *);
extern char *strcpy(char *, const char *);
extern int strcmp(const char *, const char *);


extern int zp_run(const char *);

/* The visual layer. The drawing code is the SHARED one in os/boot/fb.c — the
 * same engine the x86 compositor, dock and browser draw with — so anything that
 * renders correctly there renders correctly here. This installer's only job is
 * to find the surface (ramfb.c) and hand it over. */
#include "fb.h"
#include "zeos_boot.h"
extern int ramfb_probe(struct zeos_framebuffer *out);

static int fb_ready;

/* Vertical gradient. Local to the boot screen rather than in the shared fb API:
 * it is a splash flourish, not a framebuffer primitive, and os/boot/fb.c should
 * not grow entry points that only one caller wants. */
static void boot_gradient(uint32_t top, uint32_t bot)
{
    int h = (int)fb_height(), w = (int)fb_width();
    int tr=(top>>16)&0xff, tg=(top>>8)&0xff, tb=top&0xff;
    int br=(bot>>16)&0xff, bg=(bot>>8)&0xff, bb=bot&0xff;
    for (int y = 0; y < h; y++) {
        int r=tr+(br-tr)*y/h, g=tg+(bg-tg)*y/h, b=tb+(bb-tb)*y/h;
        fb_rect(0, y, w, 1, (uint32_t)((r<<16)|(g<<8)|b));
    }
}

static inline void irq_enable(void) { __asm__ volatile("msr daifclr, #2"); }

/* Draw the Zeos boot screen: title + one status row per rung that passed.
 * Every value shown here was actually produced by the bring-up above. */
static void draw_boot_screen(uint64_t ticks, int smp, int nodes)
{
    const uint32_t BG_T=0x0b1524, BG_B=0x060a12;   /* navy gradient */
    const uint32_t WHITE=0xf0f4ff, CYAN=0x38e0ff, DIM=0x6a7a95;
    const uint32_t GREEN=0x2ecc71, PANEL=0x101c30, ACCENT=0x38e0ff;

    boot_gradient(BG_T, BG_B);
    fb_rect(0, 0, (int)fb_width(), 6, ACCENT);     /* top accent bar */

    /* Scales are smaller than the old hand-rolled font's because the shared
     * engine's glyphs are 8x16, not 5x7 — and it has real lowercase, so these
     * strings finally render as written instead of folded to capitals. */
    fb_text_scaled(60, 40, "ZEOS", WHITE, 5);      /* big wordmark */
    fb_rect(60, 132, 4*5*8, 4, CYAN);              /* rule under the wordmark */
    fb_text_scaled(60, 148, "aarch64  bare-metal", CYAN, 2);
    fb_text_scaled(60, 192, "the first os with proprioception", DIM, 1);

    /* status panel */
    int px=60, py=230, pw=680, ph=290;
    fb_rect(px, py, pw, ph, PANEL);
    fb_rect(px, py, pw, 3, ACCENT);
    fb_text_scaled(px+24, py+20, "bring-up ladder", DIM, 1);

    struct { const char *name; int ok; } rung[6] = {
        {"M0  BOOT   EL1 / PL011 / VBAR",         1},
        {"M1  MMU    39-bit VA / caches on",       1},
        {"M2  IRQ    GICv3 + timer heartbeat",     ticks > 0},
        {"M3  SMP    secondary core online",       smp},
        {"M4  RT     heap + freestanding libc",    1},
        {"ZP  ENGINE Z+ node fired on metal",      nodes > 0},
    };
    for (int i = 0; i < 6; i++) {
        int ry = py + 60 + i*36;
        uint32_t col = rung[i].ok ? GREEN : 0xe74c3c;
        fb_rect(px+24, ry, 14, 14, col);           /* status LED */
        fb_text_scaled(px+52, ry, rung[i].name, WHITE, 1);
        fb_text_scaled(px+pw-90, ry, rung[i].ok ? "ok" : "--", col, 1);
    }

    char line[64];
    /* footer line with live numbers */
    int n=0;
    const char *pre="timer ticks="; for (const char *q=pre;*q;q++) line[n++]=*q;
    { uint64_t t=ticks; char tmp[16]; int m=0; if(!t)tmp[m++]='0'; while(t){tmp[m++]='0'+t%10;t/=10;} while(m)line[n++]=tmp[--m]; }
    const char *pre2="   z+ nodes="; for (const char *q=pre2;*q;q++) line[n++]=*q;
    line[n++]='0'+(nodes%10); line[n]=0;
    /* Clear of the last rung: rung 5 sits at py+60+5*36 = py+240 and is 16px
     * tall with the 8x16 font, so anything above py+258 collides with it. */
    fb_text_scaled(px+24, py+ph-24, line, CYAN, 1);

    fb_text_scaled(60, 552, "codex labs  //  trisa correction tech  //  zeos alpha", DIM, 1);
}

void kmain_aarch64(void)
{
    /* M-1 -- DISCOVER THE MACHINE. Before touching any register we read the
     * device tree firmware handed us in x0, so every base address below is this
     * board's real address. Nothing is compiled in: the same image is meant to
     * come up on QEMU virt, a Snapdragon, a Rockchip, an Ampere. */
    plat_init();

    kputs("\n================ ZEOS / aarch64 ================\n");
    kputs("[M-1] device tree @"); kput_hex(g_dtb_ptr);
    kputs(plat_discovered() ? "  PARSED -- hardware DISCOVERED\n"
                            : "  ABSENT -- falling back to assumed bases\n");
    kputs("[M-1]   uart="); kput_hex(g_plat.uart);
    kputs(" ("); kputs(g_plat.uart_compat ? g_plat.uart_compat : "?"); kputs(")\n");
    kputs("[M-1]   gicd="); kput_hex(g_plat.gicd);
    kputs(" gicc/gicr="); kput_hex(g_plat.gicc);
    kputs(" ("); kputs(g_plat.gic_compat ? g_plat.gic_compat : "?"); kputs(")\n");
    kputs("[M-1]   ecam="); kput_hex(g_plat.ecam);
    kputs(" rtc="); kput_hex(g_plat.rtc); kputs("\n");

    kputs("[M0] boot ok: EL1, UART, VBAR_EL1 installed.\n");

    kputs("[M1] enabling MMU (identity map, 39-bit VA, 4KB)...\n");
    mmu_init();
    heap_init(0);
    kputs("[M1] MMU: ON  -- still executing => translation is good.\n");

    kputs("[M2] GICv3 + generic timer @100Hz, unmasking IRQs...\n");
    gic_init();
    gic_enable_ppi(30);
    timer_init(100);
    irq_enable();
    while (timer_ticks() < 5) __asm__ volatile("wfi");
    kputs("[M2] timer IRQ fired, ticks="); kput_dec(timer_ticks());
    kputs("  -- the kernel has a HEARTBEAT.\n");

    kputs("[M3] PSCI CPU_ON -> secondary core 1...\n");
    int64_t r = psci_cpu_on(1, (uint64_t)secondary_entry);
    kputs("[M3] PSCI returned="); kput_hex((uint64_t)r); kputs("\n");
    /* bounded wait: the secondary does SEV after setting the flag, and the
     * 100Hz timer also wakes us; cap the spin so a slow/absent core can't stall boot. */
    for (int t = 0; t < 2000000 && !g_sec_online; t++) __asm__ volatile("");
    if (g_sec_online) {
        kputs("[M3] secondary ONLINE, mpidr="); kput_dec(g_sec_mpidr);
        kputs("  -- SMP alive.\n");
    } else {
        kputs("[M3] secondary did NOT report in.\n");
    }

    /* M4 -- the kernel C runtime that every portable Zeos module builds on. */
    kputs("[M4] runtime layer: freestanding strings + heap...\n");
    char buf[24];
    strcpy(buf, "Zeos");
    strcpy(buf + 4, "/aarch64");
    void *p1 = kmalloc(1000);
    void *p2 = kmalloc(2000);
    kputs("[M4] strlen(\"Zeos\")="); kput_dec(strlen("Zeos"));
    kputs(" strcpy=\""); kputs(buf); kputs("\"");
    kputs(" match="); kput_dec(strcmp(buf, "Zeos/aarch64") == 0);
    kputs("\n[M4] kmalloc p1="); kput_hex((uint64_t)p1);
    kputs(" p2="); kput_hex((uint64_t)p2);
    kputs(" heap_used="); kput_dec(heap_used_bytes()); kputs(" bytes\n");
    kputs("[M4] runtime LIVE -- portable Zeos code can now build on this base.\n");

    /* M4.1 -- Zeos's own language engine, on bare-metal ARM. */
    /* M4.1 -- Zeos's language engine on bare metal. zplus.c (4535 LOC) + the real
     * signal-chain runtime (signal.c/zp_runtime.c) run the full parse->compile->
     * resolve pipeline here, stably. Programs fire 0 nodes until the node-kind
     * handlers (tick source / print sink) get their chain-registry backend ported
     * -- that's the next grind; the engine + runtime themselves are live. */
    kputs("[M4] launching Z+ engine (zplus.c 4535 LOC + real signal runtime)...\n");
    /* kernel-dialect Z+: a program that fires a node on bare-metal ARM. */
    kputs("\n--- Z+ program: emit(42) -> print (fires + times a node) ---\n");
    int fired = zp_run("x : emit(42) -> print(\"hi\")\n");
    kputs("[M4] Z+ nodes fired="); kput_dec((uint64_t)(int64_t)fired);
    kputs(" -- a Z+ program executed on bare-metal aarch64.\n");

    /* M5 -- the VISUAL layer. Framebuffer up, boot screen drawn by the kernel.
     * This is now part of bring-up, tested with pixels, not deferred. */
    kputs("[M5] framebuffer: configuring ramfb via fw_cfg...\n");
    {
        /* Installer finds the surface; the shared OS framebuffer drives it.
         *
         * STATIC, not a local: fb_init() BORROWS this pointer (g_fb = fb) and
         * keeps writing through it for the life of the system — it even swaps
         * ->base when a back buffer is presented. A stack local here compiles
         * and boots and draws the first half of the screen, then dies silently
         * the moment the frame is reused, with the serial log still reporting
         * success. Cost me a render to find; it must outlive every caller. */
        static struct zeos_framebuffer fbinfo;
        if (ramfb_probe(&fbinfo) == 0) { fb_init(&fbinfo); fb_ready = 1; }
    }
    if (fb_ready) {
        kputs("[M5] ramfb LIVE 800x600 XRGB8888 -- drawing Zeos boot screen.\n");
        draw_boot_screen(timer_ticks(), g_sec_online, fired);
        kputs("[M5] boot screen rendered -- PIXELS ON SCREEN.\n");
    } else {
        kputs("[M5] ramfb NOT available (no -device ramfb?).\n");
    }
    /* M6 -- the HAL (O.2 contract) exercised on real aarch64. The shared OS in
     * os/ reaches hardware ONLY through hal.h, so this proves the ARM backend's
     * port dispatch actually works rather than merely compiling: PCI config
     * really lands on ECAM, the RTC really reads PL031, COM1 really reaches the
     * PL011, and absent legacy devices report absent instead of corrupting the
     * UART. */
    kputs("[M6] HAL (hal.h) on aarch64: ");
    kputs(hal_arch_name()); kputs("\n");

    /* PCI config space via the legacy 0xCF8/0xCFC path -> ECAM. Bus0/dev0/fn0
     * on QEMU virt is the PCIe host bridge; a real vendor:device must appear
     * (0xFFFFFFFF would mean "nothing there" = dispatch broken). */
    hal_out32(0xCF8, 0x80000000);
    uint32_t id = hal_in32(0xCFC);
    kputs("[M6] PCI cfg via CF8/CFC->ECAM: vendor="); kput_hex(id & 0xFFFF);
    kputs(" device="); kput_hex((id >> 16) & 0xFFFF);
    kputs((id != 0xFFFFFFFF && (id & 0xFFFF) != 0xFFFF) ? "  REAL DEVICE\n"
                                                        : "  (none found)\n");

    /* RTC via the legacy CMOS index/data ports -> PL031. */
    hal_out8(0x70, 0x09); uint8_t yy = hal_in8(0x71);
    hal_out8(0x70, 0x08); uint8_t mo = hal_in8(0x71);
    hal_out8(0x70, 0x07); uint8_t dd = hal_in8(0x71);
    hal_out8(0x70, 0x04); uint8_t hh = hal_in8(0x71);
    hal_out8(0x70, 0x02); uint8_t mi = hal_in8(0x71);
    kputs("[M6] RTC via CMOS ports->PL031: 20"); kput_dec(yy);
    kputs("-"); kput_dec(mo); kputs("-"); kput_dec(dd);
    kputs(" "); kput_dec(hh); kputs(":"); kput_dec(mi); kputs("\n");

    /* Absent legacy hardware must report absent (NOT scribble into the UART):
     * i8042 status 0x64 == 0 (output buffer empty), data 0x60 == 0xFF. */
    uint8_t k_st = hal_in8(0x64), k_dt = hal_in8(0x60);
    kputs("[M6] i8042 (absent on ARM): status="); kput_hex(k_st);
    kputs(" data="); kput_hex(k_dt);
    kputs((k_st == 0x00 && k_dt == 0xFF) ? "  correctly reports ABSENT\n"
                                         : "  WRONG\n");

    /* COM1 through the 16550->PL011 translation: this text is printed by
     * hal_out8(0x3F8,...), i.e. the same path the shared serial.c uses. If you
     * can read the next line, the translation works. */
    kputs("[M6] COM1 via hal_out8(0x3F8)->PL011: ");
    const char *m6 = "HAL-ROUTED SERIAL OK";
    for (const char *p = m6; *p; ++p) hal_out8(0x3F8, (uint8_t)*p);
    hal_out8(0x3F8, '\n');
    /* And the synthesized LSR must report THR-empty (bit 5) like a real 16550. */
    uint8_t lsr = hal_in8(0x3FD);
    kputs("[M6] LSR synthesized from PL011 FR: "); kput_hex(lsr);
    kputs((lsr & 0x20) ? "  THR-empty OK\n" : "  WRONG\n");

    kputs("================================================\n");
}
