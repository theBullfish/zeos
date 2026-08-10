/*
 * Zeos Kernel — UEFI Bootstrap
 *
 * This is step 1: enter via UEFI, grab the framebuffer via GOP,
 * grab the memory map, exit boot services, and land on bare metal
 * in long mode with a working display.
 *
 * UEFI on x86_64 already gives us long mode. We don't need to set it up.
 * What we DO need:
 *   1. GOP framebuffer pointer + resolution
 *   2. UEFI memory map (so we know what RAM is ours)
 *   3. ACPI RSDP pointer (for later hardware discovery)
 *   4. Exit boot services (we own the machine now)
 *
 * After ExitBootServices, UEFI is gone. No firmware calls. We're alone
 * with a framebuffer, a memory map, and a CPU in long mode.
 */

#include <efi.h>
#include <efilib.h>

#include "fb.h"
#include "theme.h"
#include "zeos_boot.h"
#include "gdt.h"
#include "idt.h"
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "panic.h"
#include "keyboard.h"
#include "mouse.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "timer.h"
#include "signal.h"
#include "serial.h"
#include "kprint.h"
#include "splash.h"
#include "shell.h"
#include "usb_xhci.h"
#include "usb_hid.h"
#include "usb_cdc.h"
#include "usb_msc.h"
#include "block.h"
#include "fat32.h"
#include "net_rtl8188eu.h"
#include "lockscreen.h"
#include "firstboot.h"
#include "access.h"
#include "wm.h"
#include "font.h"

/* Boot info passed from UEFI to kernel */
static struct zeos_boot_info boot_info;

/* Content-draw callbacks so the two boot windows aren't empty chrome.
 * wm calls these with the chrome-stripped content rect. */

/* Terminal window: render the LIVE console ring (the real Z+ shell session),
 * not a static mockup. kprint tees kputc/kputs into term_console_*; we draw the
 * tail that fits, oldest→newest, so typed input + command output appear here. */
static void boot_term_draw_content(int id, int x, int y, int w, int h)
{
    (void)id; (void)w;
    int lh = 16;
    font_draw(x + 12, y + 4, "Zeos Terminal - Z+ shell (live)", FONT_UI, 12, 0xFF8FB6C9);
    int top = y + 4 + lh + 4;
    int rows_fit = (h - (top - y) - 6) / lh;
    if (rows_fit < 1) rows_fit = 1;
    if (rows_fit > TERM_CONSOLE_ROWS) rows_fit = TERM_CONSOLE_ROWS;
    int cur = term_console_cur_row();
    for (int i = 0; i < rows_fit; i++) {
        int ring = ((cur - (rows_fit - 1) + i) % TERM_CONSOLE_ROWS + TERM_CONSOLE_ROWS)
                   % TERM_CONSOLE_ROWS;
        const char *line = term_console_row(ring);
        if (line && line[0])
            font_draw(x + 12, top + i * lh, line, FONT_UI, 12, 0xFFDDE6EC);
    }
}

static void boot_files_draw_content(int id, int x, int y, int w, int h)
{
    (void)id; (void)w; (void)h;
    int lh = 24, ty = y + 12;
    static const char *rows[] = {
        "programs/", "editor/", "notes/", "chat/", "time/", "settings.cfg"
    };
    font_draw(x + 14, ty, "VAULT  /", FONT_UI, 13, 0xFF8FB6C9);
    for (int i = 0; i < 6; i++)
        font_draw(x + 22, ty + lh*(i + 1), rows[i], FONT_UI, 14, 0xFFDDE6EC);
}

/*
 * Find the ACPI RSDP in UEFI configuration tables.
 * Try ACPI 2.0 first (XSDT), fall back to 1.0 (RSDT).
 */
static void *find_rsdp(EFI_SYSTEM_TABLE *st)
{
    /* ACPI 2.0 GUID */
    EFI_GUID acpi20 = { 0x8868e871, 0xe4f1, 0x11d3,
        { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
    /* ACPI 1.0 GUID */
    EFI_GUID acpi10 = { 0xeb9d2d30, 0x2d88, 0x11d3,
        { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };

    void *rsdp = NULL;
    UINTN i;

    for (i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &st->ConfigurationTable[i];
        if (CompareGuid(&t->VendorGuid, &acpi20) == 0) {
            return t->VendorTable;
        }
        if (CompareGuid(&t->VendorGuid, &acpi10) == 0) {
            rsdp = t->VendorTable;
        }
    }
    return rsdp;
}

/*
 * EDID protocols — UEFI 2.x §11.9. Either Active (current monitor) or
 * Discovered (read from EDID block, may differ from active mode) is OK.
 * Both have an identical layout: { UINT32 SizeOfEdid; UINT8 *Edid; }.
 */
typedef struct {
    UINT32  SizeOfEdid;
    UINT8  *Edid;
} ZEOS_EFI_EDID_PROTOCOL;

#define ZEOS_EFI_EDID_ACTIVE_GUID \
    { 0xbd8c1056, 0x9f36, 0x44ec, \
      { 0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86 } }
#define ZEOS_EFI_EDID_DISCOVERED_GUID \
    { 0x1c0c34f6, 0xd380, 0x41fa, \
      { 0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa } }

/*
 * Parse a 128-byte EDID block 0. Returns 0 on success, -1 on bad checksum
 * or invalid header. Fills mfr (3 chars + NUL), product_id, and the
 * preferred-timing native_w/native_h/native_hz. Refresh is computed from
 * the detailed timing descriptor (DTD #1 at offset 54).
 */
static int parse_edid(const UINT8 *e, struct zeos_display_info *d)
{
    /* Header: 00 FF FF FF FF FF FF 00 */
    static const UINT8 hdr[8] = {0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0};
    for (int i = 0; i < 8; i++)
        if (e[i] != hdr[i]) return -1;

    /* Checksum: sum of all 128 bytes mod 256 == 0 */
    UINT32 sum = 0;
    for (int i = 0; i < 128; i++) sum += e[i];
    if ((sum & 0xff) != 0) return -1;

    /* Manufacturer ID: 2 big-endian bytes at offset 8, 5-bit per letter */
    UINT16 mid = (UINT16)((e[8] << 8) | e[9]);
    d->mfr[0] = (char)('@' + ((mid >> 10) & 0x1f));
    d->mfr[1] = (char)('@' + ((mid >>  5) & 0x1f));
    d->mfr[2] = (char)('@' + ( mid        & 0x1f));
    d->mfr[3] = '\0';

    d->product_id = (UINT16)(e[10] | (e[11] << 8));

    /* Preferred Detailed Timing Descriptor at offset 54 */
    const UINT8 *dtd = e + 54;
    UINT32 hactive = dtd[2] | ((UINT32)(dtd[4] & 0xF0) << 4);
    UINT32 hblank  = dtd[3] | ((UINT32)(dtd[4] & 0x0F) << 8);
    UINT32 vactive = dtd[5] | ((UINT32)(dtd[7] & 0xF0) << 4);
    UINT32 vblank  = dtd[6] | ((UINT32)(dtd[7] & 0x0F) << 8);
    UINT32 pclk_10khz = (UINT32)dtd[0] | ((UINT32)dtd[1] << 8); /* in 10 kHz */

    d->native_w = hactive;
    d->native_h = vactive;
    d->native_hz = 0;
    UINT32 htotal = hactive + hblank;
    UINT32 vtotal = vactive + vblank;
    if (htotal && vtotal && pclk_10khz) {
        /* refresh = pclk / (htotal * vtotal); pclk in Hz = pclk_10khz * 10000 */
        UINT64 hz = (UINT64)pclk_10khz * 10000ULL / ((UINT64)htotal * vtotal);
        d->native_hz = (UINT32)hz;
    }

    /* Copy the raw block */
    for (int i = 0; i < 128; i++) d->edid[i] = e[i];
    d->edid_valid = 1;
    return 0;
}

/*
 * Try to locate an EDID block and parse it. Tries Active first, then
 * Discovered. On any failure leaves edid_valid == 0.
 */
static void try_read_edid(EFI_SYSTEM_TABLE *st, struct zeos_display_info *d)
{
    EFI_GUID active = ZEOS_EFI_EDID_ACTIVE_GUID;
    EFI_GUID disc   = ZEOS_EFI_EDID_DISCOVERED_GUID;
    ZEOS_EFI_EDID_PROTOCOL *e = NULL;
    EFI_STATUS s;

    d->edid_valid = 0;
    d->mfr[0] = '\0';

    s = uefi_call_wrapper(st->BootServices->LocateProtocol, 3,
                          &active, NULL, (void **)&e);
    if (EFI_ERROR(s) || !e || !e->Edid || e->SizeOfEdid < 128) {
        e = NULL;
        s = uefi_call_wrapper(st->BootServices->LocateProtocol, 3,
                              &disc, NULL, (void **)&e);
    }
    if (EFI_ERROR(s) || !e || !e->Edid || e->SizeOfEdid < 128) {
        Print(L"EDID not exposed\r\n");
        return;
    }

    if (parse_edid(e->Edid, d) != 0) {
        Print(L"EDID present but invalid\r\n");
        return;
    }

    Print(L"Display: %c%c%c %04x, native %dx%d @ %d Hz\r\n",
          d->mfr[0], d->mfr[1], d->mfr[2], d->product_id,
          d->native_w, d->native_h, d->native_hz);
}

/*
 * Score a candidate mode. Higher = better.
 * Native (matches EDID) wins, then a fixed preference ladder, then area.
 */
static UINT64 mode_score(UINT32 w, UINT32 h,
                         const struct zeos_display_info *d)
{
    if (d->edid_valid && w == d->native_w && h == d->native_h)
        return (UINT64)1 << 60;

    struct { UINT32 w, h; UINT64 bonus; } pref[] = {
        {1920, 1080, (UINT64)1 << 50},
        {1600,  900, (UINT64)1 << 49},
        {1366,  768, (UINT64)1 << 48},
        {1280,  800, (UINT64)1 << 47},
        {1024,  768, (UINT64)1 << 46},
    };
    for (UINTN i = 0; i < sizeof(pref)/sizeof(pref[0]); i++)
        if (w == pref[i].w && h == pref[i].h)
            return pref[i].bonus + (UINT64)w * h;

    /* Largest available — only the area term, well below the ladder */
    return (UINT64)w * h;
}

static int pixel_format_usable(UINT32 fmt)
{
    return fmt == PixelRedGreenBlueReserved8BitPerColor ||
           fmt == PixelBlueGreenRedReserved8BitPerColor;
}

/*
 * Acquire GOP, enumerate modes, query EDID, pick the best mode, SetMode,
 * then publish the resulting framebuffer to fb.
 */
static EFI_STATUS init_gop(EFI_SYSTEM_TABLE *st,
                           struct zeos_framebuffer *fb,
                           struct zeos_display_info *d)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status;

    status = uefi_call_wrapper(st->BootServices->LocateProtocol, 3,
                               &gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(status) || !gop)
        return status;

    /* Read EDID before mode selection so native timing influences scoring. */
    try_read_edid(st, d);

    UINT32 max_mode = gop->Mode->MaxMode;
    UINT32 best_mode = gop->Mode->Mode;
    UINT64 best_score = 0;
    int best_found = 0;

    for (UINT32 m = 0; m < max_mode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN info_sz = 0;
        EFI_STATUS qs = uefi_call_wrapper(gop->QueryMode, 4,
                                          gop, m, &info_sz, &info);
        if (EFI_ERROR(qs) || !info)
            continue;

        if (!pixel_format_usable(info->PixelFormat))
            continue;

        UINT64 sc = mode_score(info->HorizontalResolution,
                               info->VerticalResolution, d);
        if (!best_found || sc > best_score) {
            best_score = sc;
            best_mode = m;
            best_found = 1;
        }
    }

    if (best_found && best_mode != gop->Mode->Mode) {
        EFI_STATUS ss = uefi_call_wrapper(gop->SetMode, 2, gop, best_mode);
        if (EFI_ERROR(ss)) {
            Print(L"GOP SetMode(%d) failed (%r); keeping current mode\r\n",
                  best_mode, ss);
        }
    }

    fb->base       = (uint32_t *)(UINTN)gop->Mode->FrameBufferBase;
    fb->size       = gop->Mode->FrameBufferSize;
    fb->width      = gop->Mode->Info->HorizontalResolution;
    fb->height     = gop->Mode->Info->VerticalResolution;
    fb->pitch      = gop->Mode->Info->PixelsPerScanLine;
    fb->pixel_format = gop->Mode->Info->PixelFormat;

    return EFI_SUCCESS;
}

/*
 * Get the UEFI memory map.
 * Must be called immediately before ExitBootServices — the map key
 * changes on any allocation, so no allocations between this and exit.
 */
static EFI_STATUS get_memory_map(EFI_SYSTEM_TABLE *st,
                                  struct zeos_memory_map *mm)
{
    EFI_STATUS status;
    UINTN map_size = 0;
    UINTN map_key;
    UINTN desc_size;
    UINT32 desc_ver;
    void *map = NULL;

    /* First call: get required buffer size */
    status = uefi_call_wrapper(st->BootServices->GetMemoryMap, 5,
                               &map_size, NULL, &map_key, &desc_size, &desc_ver);

    /* Add slack — firmware may add entries between calls */
    map_size += 4096;

    status = uefi_call_wrapper(st->BootServices->AllocatePool, 3,
                               EfiLoaderData, map_size, &map);
    if (EFI_ERROR(status))
        return status;

    status = uefi_call_wrapper(st->BootServices->GetMemoryMap, 5,
                               &map_size, map, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status))
        return status;

    mm->entries    = map;
    mm->size       = map_size;
    mm->desc_size  = desc_size;
    mm->desc_ver   = desc_ver;
    mm->map_key    = map_key;

    return EFI_SUCCESS;
}

/*
 * UEFI entry point.
 */
/*
 * NOTE: efi_main must NOT be EFIAPI (ms_abi) — the GNU-EFI CRT0
 * stub calls us with SysV ABI (rdi, rsi) regardless of ms_abi settings.
 */
EFI_STATUS
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_STATUS status;

    InitializeLib(image, st);

    /* Banner on UEFI console (pre-GOP, for serial/debug) */
    Print(L"Zeos kernel bootstrap\r\n");
    Print(L"UEFI firmware: %s\r\n", st->FirmwareVendor);

    /* Step 1: Framebuffer */
    Print(L"Acquiring GOP framebuffer... ");
    status = init_gop(st, &boot_info.fb, &boot_info.display);
    if (EFI_ERROR(status)) {
        Print(L"FAILED (status %r)\r\n", status);
        return status;
    }
    Print(L"%dx%d pitch=%d\r\n",
          boot_info.fb.width, boot_info.fb.height, boot_info.fb.pitch);

    /* Step 2: ACPI */
    Print(L"Locating ACPI RSDP... ");
    boot_info.rsdp = find_rsdp(st);
    Print(L"%s at %p\r\n", boot_info.rsdp ? L"found" : L"NOT FOUND",
          boot_info.rsdp);

    /* Disable UEFI watchdog timer — firmware sets a 5-minute watchdog
     * that resets the machine if the OS doesn't disable it. */
    uefi_call_wrapper(st->BootServices->SetWatchdogTimer, 4, 0, 0, 0, NULL);

    /* Step 3: Memory map + exit boot services */
    Print(L"Getting memory map and exiting boot services...\r\n");

    status = get_memory_map(st, &boot_info.mmap);
    if (EFI_ERROR(status)) {
        Print(L"GetMemoryMap FAILED (status %r)\r\n", status);
        return status;
    }

    status = uefi_call_wrapper(st->BootServices->ExitBootServices, 2,
                               image, boot_info.mmap.map_key);
    if (EFI_ERROR(status)) {
        /*
         * Memory map may have changed. Re-fetch and retry once.
         * This is the standard UEFI pattern.
         */
        status = get_memory_map(st, &boot_info.mmap);
        if (EFI_ERROR(status))
            return status;
        status = uefi_call_wrapper(st->BootServices->ExitBootServices, 2,
                                   image, boot_info.mmap.map_key);
        if (EFI_ERROR(status))
            return status;
    }

    /*
     * ========================================================
     * BOOT SERVICES ARE GONE. WE OWN THE MACHINE.
     * No Print(), no AllocatePool(), no UEFI calls.
     * We have: framebuffer, memory map, RSDP, long mode, CPU.
     * ========================================================
     */

    /* Initialize framebuffer console */
    fb_init(&boot_info.fb);
    fb_clear(COLOR_SURFACE);  /* Design system background */

    /* Initialize serial console — debug output channel */
    serial_init();
    kprint_init();

    /* Activate the TTF font system (Inter/JBMono + F.4 Noto fallback). Was
     * never called -> all text had been falling back to the 8x16 boot bitmap.
     * InitFont is parse-only (no heap needed); glyph cache mallocs lazily
     * later once the heap is up. */
    { extern int font_init(void); font_init(); }

    /* Engage boot splash on the GOP framebuffer. From here through
     * chain_registry_init the user sees a wordmark + progress bar
     * instead of the kernel debug stream. Serial logs continue. */
    splash_init();
    splash_progress("boot services released", 4);

    kputs("================================================\n");
    kputs("  Zeos\n");
    kputs("  The first operating system with proprioception.\n");
    kputs("================================================\n\n");

    kputs("Boot services released. We own the machine.\n\n");

    /* Print framebuffer info */
    kputs("Framebuffer: ");
    kput_dec(boot_info.fb.width);
    kputs("x");
    kput_dec(boot_info.fb.height);
    kputs(" pitch=");
    kput_dec(boot_info.fb.pitch);
    kputs("\n");

    /* Print ACPI status */
    kputs("ACPI RSDP:   ");
    if (boot_info.rsdp) {
        kputs("found at 0x");
        kput_hex((uint64_t)(UINTN)boot_info.rsdp);
    } else {
        kputs("not found");
    }
    kputs("\n");

    /* Count usable memory from the UEFI map */
    uint64_t total_usable = 0;
    uint64_t total_entries = 0;
    uint8_t *entry = (uint8_t *)boot_info.mmap.entries;
    uint8_t *end = entry + boot_info.mmap.size;

    while (entry < end) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)entry;
        total_entries++;
        if (desc->Type == EfiConventionalMemory ||
            desc->Type == EfiBootServicesCode ||
            desc->Type == EfiBootServicesData) {
            total_usable += desc->NumberOfPages * 4096;
        }
        entry += boot_info.mmap.desc_size;
    }

    kputs("Memory map:  ");
    kput_dec(total_entries);
    kputs(" entries, ");
    kput_dec(total_usable / (1024 * 1024));
    kputs(" MB usable\n\n");

    /* Read TSC — first timing measurement */
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | lo;

    kputs("TSC:         0x");
    kput_hex(tsc);
    kputs("\n");
    kputs("             Zixel embryo — first timing delta on bare metal.\n\n");

    kputs("Zeos is alive.\n\n");

    /* Initialize physical memory manager */
    splash_progress("PMM", 8);
    kputs("Initializing PMM... ");
    pmm_init(&boot_info.mmap, &boot_info.fb);
    if (pmm_free_pages() == 0)
        panic("PMM init failed — no free memory");
    kput_dec(pmm_free_pages() * 4 / 1024);
    kputs(" MB free / ");
    kput_dec(pmm_total_pages() * 4 / 1024);
    kputs(" MB total.\n");

    /* Initialize virtual memory */
    splash_progress("VMM", 16);
    kputs("Setting up VMM... ");
    vmm_init();
    kputs("done (4GB identity + higher-half).\n");

    /* B.7 root-cause fix: the identity map above marked the whole low 4GB
     * write-back cached, which re-caches the framebuffer -> compositor writes sit
     * in CPU cache and the display misses them (the "windows vanish" scream).
     * Remap the FB range uncached so writes reach VRAM directly. */
    {
        uint64_t fb_phys = fb_phys_base();
        uint64_t fb_sz   = (uint64_t)fb_pitch_pixels() * fb_height() * 4;
        if (fb_phys && fb_sz) {
            vmm_set_range_wc(fb_phys, fb_sz);
            kputs("VMM: framebuffer mapped write-combining (B.7 fix).\n");
        }
    }

    /* Initialize kernel heap */
    splash_progress("heap", 24);
    kputs("Initializing heap... ");
    heap_init(512);  /* 2 MB initial heap (TLS handshake peak ~50 KB) */
    if (heap_total_bytes() == 0)
        panic("Heap init failed — cannot allocate initial pages");
    kput_dec(heap_total_bytes() / 1024);
    kputs(" KB.\n");

    /*
     * ── GDT + TSS ──────────────────────────────────────────
     * Replace UEFI's GDT (which is in freed memory) with our own.
     * Must happen BEFORE IDT init — IDT gates reference GDT selectors.
     * The TSS provides IST stacks for safe exception handling.
     */
    splash_progress("GDT+IDT", 32);
    kputs("Loading GDT+TSS... ");
    gdt_init();
    kputs("done.\n");

    /* Initialize interrupts (IDT with exception handlers + IRQ stubs) */
    kputs("Setting up IDT... ");
    idt_init();
    kputs("done (vectors 0x00-0x14 + 0x20-0x2F).\n");

    /* Initialize PCI (uses I/O ports, safe after IDT is up) */
    splash_progress("PCI scan", 40);
    kputs("Scanning PCI bus... ");
    pci_init(boot_info.rsdp);
    int pci_count = pci_enumerate();
    kput_dec(pci_count);
    kputs(" devices found.\n");

    /*
     * ── ACPI / LAPIC / IOAPIC ──────────────────────────────────────
     * Parse the MADT from the RSDP UEFI handed us, bring up the BSP's
     * Local APIC (mapped uncached, SVR enabled, every LVT masked),
     * calibrate the APIC bus frequency against PIT channel 2, and map
     * each IOAPIC. The APIC timer is left disarmed; arming it for
     * scheduler preemption is a separate change.
     *
     * Without lapic_init() running before the first MSI-X interrupt,
     * subsequent interrupts on the same vector queue at the LAPIC ISR
     * because nobody acknowledges them. msix_dispatch() now writes
     * LAPIC EOI at the tail of every dispatch.
     */
    splash_progress("ACPI / LAPIC / IOAPIC", 48);
    kputs("ACPI MADT... ");
    if (acpi_init(boot_info.rsdp) == 0) {
        kputs("ok ("); kput_dec((uint64_t)acpi_madt()->lapic_count);
        kputs(" CPU, "); kput_dec((uint64_t)acpi_madt()->ioapic_count);
        kputs(" IOAPIC).\n");
    } else {
        kputs("not found (defaults).\n");
    }

    /* AML interpreter — catalog every Name/Method across DSDT+SSDTs.
     * Must run before battery/brightness/power-buttons init so those
     * modules can call aml_evaluate() for Method-form _BST/_BCM/_LID.
     * Honest scope: ~50 of ~200 opcodes; methods that hit unimplemented
     * opcodes report "skipped" in the AML selftest line. */
    {
        extern int aml_init(void);
        int n = aml_init();
        kputs("AML catalog... ");
        kput_dec((uint64_t)n);
        kputs(" symbols.\n");

        /* ACPI Embedded Controller — discover via PNP0C09 in the AML
         * namespace. EmbeddedControl OperationRegions are routed through
         * the EC byte protocol once initialized. Required for
         * _BST/_BIF on every laptop with battery data behind the EC. */
        extern int ec_discover_and_init(void);
        extern void ec_print_selftest_line(void);
        (void)ec_discover_and_init();
        ec_print_selftest_line();
    }
    kputs("LAPIC init... ");
    lapic_init();
    lapic_timer_calibrate();
    kputs("id="); kput_dec(lapic_id());
    kputs(", "); kput_dec(lapic_ticks_per_us());
    kputs(" ticks/us.\n");
    kputs("IOAPIC init... ");
    if (ioapic_init() == 0) {
        kput_dec(ioapic_count()); kputs(" mapped.\n");
    } else {
        kputs("none.\n");
    }

    /* Initialize xHCI (USB 3.x) host controller. Polling-based; safe
     * even before timer/IDT IRQs are wired. Enumerates root-hub ports
     * and reads the device descriptor of any attached device. */
    splash_progress("USB xHCI", 56);
    if (xhci_init() == 0) {
        kputs("USB xHCI: ready (");
        kput_dec(xhci_device_count());
        kputs(" device(s))\n");

        /* Walk every hub reachable from the root and address each
         * downstream device. Must run before HID/CDC/MSC bind so they
         * see hub-attached devices when they iterate xhci devices. */
        {
            extern int usb_hub_init(void);
            usb_hub_init();
        }

        /* Probe enumerated USB devices for an RTL8188EU WiFi dongle.
         * Detection-only at this stage: see net_rtl8188eu.c for the
         * honest accounting of what's missing for a working link. */
        rtl8188eu_probe();

        /* Bind USB HID boot devices (mouse + keyboard). Issues
         * SET_CONFIGURATION/SET_PROTOCOL/SET_IDLE on each, configures
         * the interrupt-IN endpoint, and arms it for polling. Reports
         * are dispatched to the same input pipeline the PS/2 drivers
         * feed (keyboard_inject_scancode, mouse_inject). */
        usb_hid_init();

        /* USB UVC video class — webcams. Walks every enumerated
         * device, attaches class 0x0E (Video), parses VC + VS
         * descriptors, runs Probe/Commit for MJPEG 640x480@30, and
         * picks the iso-IN alt setting. Streaming is not started
         * automatically; `camera preview <n>` or
         * `camera capture <n> <path>` arms it. */
        {
            extern int usb_uvc_init(void);
            usb_uvc_init();
        }

        /* USB CDC ACM class driver — Arduino, ESP32, Pi Pico, etc.
         * Walks every enumerated xHCI device and binds CDC-ACM serial
         * endpoints. Safe even with zero serial devices present. */
        usb_cdc_init();

        /* USB Mass Storage class driver (BBB / SCSI). Brings up the
         * first thumb drive found and prints sector count. */
        usb_msc_init();

        /* USB Bluetooth HCI controller. Detects E0/01/01 class triple,
         * sets up EP1 IN (events) + EP2 IN/OUT (ACL data), then runs
         * Reset / Read Local Version / Read BD_ADDR. Foundation for
         * BT keyboards / mice / audio (OS_LITTLE_THINGS #16). */
        {
            extern int bt_usb_init(void);
            extern int bt_hci_init(void);
            extern int bt_l2cap_init(void);
            if (bt_usb_init() == 0) {
                (void)bt_hci_init();
            }
            /* L2CAP layers on top of HCI ACL — initialize unconditionally
             * so the signaling channel is registered even when no
             * controller is bound (chain still appears in the graph). */
            (void)bt_l2cap_init();
        }
    } else {
        kputs("USB xHCI: not available\n");
    }

    /* Block device dispatcher — picks NVMe, AHCI, or USB-MSC. */
    splash_progress("block + audio + FAT32", 64);
    block_init();

    /* Intel HD Audio -- minimum-viable controller + one output stream.
     * Walks PCI class 0x04 sub 0x03, brings up CORB/RIRB, locates an
     * analog output path on the first responding codec, programs SD0.
     * Failure is non-fatal: the OS just stays silent. */
    {
        extern int hda_init(void);
        if (hda_init() == 0) kputs("Audio: HDA ready\n");
        else                 kputs("Audio: HDA not available\n");
    }

    /* Try to auto-mount a FAT32 volume on the active block device.
     * USB sticks usually have no partition table (whole-disk FAT32);
     * NVMe/AHCI installs use GPT with the ESP as partition 1. The
     * automount tries whole-disk first, then walks GPT entries until
     * one mounts. Failure is non-fatal — `fat-mount` works manually. */
    if (fat32_automount() == 0) {
        kputs("FAT32: volume mounted (read-only)\n");
    } else {
        kputs("FAT32: no volume auto-mounted\n");
    }

    /* Initialize keyboard */
    splash_progress("input devices", 72);
    kputs("Initializing keyboard... ");
    keyboard_init();
    kputs("done.\n");

    /* Initialize PS/2 mouse */
    kputs("Initializing mouse... ");
    mouse_init();
    kputs("done.\n");

    /* A.8 fix (2026-07-25): route the legacy keyboard (IRQ1) and mouse (IRQ12)
     * interrupts through the IOAPIC to their existing IDT vectors, instead of
     * the 8259 -> ExtINT/LINT0 virtual-wire path. That path was delivering each
     * legacy IRQ EXACTLY ONCE then latching forever (measured: 8259 IRR bit
     * stuck=1, ISR=0, the CPU never re-vectors -- the A.8/E.1/E.7 single-fire
     * bug). On q35 with the LAPIC enabled the IOAPIC is the correct delivery
     * path; ioapic_init() had initialized it but left every redirection entry
     * masked with nothing routed. Route these two lines and mask them on the
     * 8259 so only the IOAPIC delivers (no double-delivery). PIT/IRQ0 stays on
     * ExtINT -- it delivers fine and the scheduler depends on it. Must run
     * after keyboard_init/mouse_init (they pic_unmask their lines) and before
     * sti. The ISRs already call lapic_eoi(), the correct ack for IOAPIC
     * edge-triggered delivery. */
    if (ioapic_count() > 0) {
        uint8_t apic = (uint8_t)lapic_id();
        ioapic_set_irq(1,  0x21, apic);   /* keyboard IRQ1  -> vector 0x21 */
        ioapic_set_irq(12, 0x2C, apic);   /* mouse    IRQ12 -> vector 0x2C */
        pic_mask(1);
        pic_mask(12);
        kputs("IOAPIC: routed IRQ1(kbd)+IRQ12(mouse) -> LAPIC vec, masked on 8259\n");
    } else {
        kputs("IOAPIC: not present -- keyboard/mouse remain on 8259 ExtINT (A.8 unfixed)\n");
    }

    /* Wire COM1 RX IRQ (IRQ4 / vector 0x24). The serial UART itself
     * was configured in serial_init() earlier; this hooks the ISR
     * that drains incoming bytes into the CHAIN_SERIAL_IN ring. */
    serial_irq_init();

    /* Enable interrupts */
    __asm__ volatile("sti");

    /* Initialize timer (1000 Hz PIT + TSC calibration) */
    splash_progress("timer calibration", 80);
    kputs("Calibrating timer... ");
    timer_init(1000);
    kputs("TSC freq: ");
    kput_dec(timer_tsc_freq() / 1000000);
    kputs(" MHz.\n");

    /* SMP: enumerate APs from MADT, place AP trampoline, INIT-SIPI-SIPI.
     * BSP-safe: if smp_init returns 1 (single core or AP failure) the
     * system continues on the BSP. APs reach a 64-bit alive-loop;
     * concurrent chain resolution is staged but not enabled (see
     * smp.c top-of-file scope note). */
    {
        extern int  smp_init(void);
        extern void smp_print_selftest_line(void);
        smp_init();
        smp_print_selftest_line();
    }

    /* Wall clock: read CMOS RTC and bind to TSC for fractional seconds.
     * If CMOS is absent (some hypervisors) we'll print a TSC-only line
     * here, then persistence_init() will try to load /time/last-known.bin
     * and upgrade us to TOD_SRC_PERSISTED. */
    {
        extern void tod_init(void);
        extern void tod_print_selftest_line(void);
        tod_init();
        tod_print_selftest_line();
    }

    /* Initialize signal chain engine */
    splash_progress("signals + VAULT", 88);
    kputs("Signal chain engine... ");
    sig_init();
    kputs("ready.\n\n");

    /* Bring up VAULT BEFORE chain_registry_init so the persistence
     * layer can replay the masq_journal + buffer the chain registry
     * snapshot in time for chain_registry_init to apply it to the
     * freshly-built chains. block_init() above already enumerated
     * the persistent NVMe drive that backs vault_ram. */
    {
        extern void shell_vault_init(void);
        extern void persistence_init(void);
        shell_vault_init();
        persistence_init();

        /* Accessibility config: set sane defaults + load persisted prefs. Was
         * NEVER called, so the whole a11y config sat zero-initialized
         * (anim_speed=0) and no consumer could honor it. Must run before the
         * compositor/anim loop reads it. */
        extern void access_init(void);
        access_init();
    }

    /* Wire all subsystems into the chain/MDE graph (CPU, memory, GPU,
     * NIC discovery + system chains: compositor, panel, dock, desktop,
     * shell, browser, inspector, palette, AND audio -- the first hardware
     * driver in the native paradigm. See docs/PARADIGM_CONVERSION.md. */
    splash_progress("chain registry", 95);
    {
        extern int chain_registry_init(void);
        chain_registry_init();
    }

    /* Splash dismissed once the chain graph exists. From here the
     * scheduler-driven shell takes over the framebuffer. */
    splash_dismiss();

    /* The GUI now owns the framebuffer (lockscreen -> welcome -> desktop,
     * all drawing via fb_* directly). Keep kprint serial-only so late-boot
     * diagnostics (net/DHCP/scheduler/etc.) don't bleed over the UI. */
    kprint_set_splash_mode(1);

    /* With the registry built, replay the persisted snapshot onto the
     * live chains (B3 priors, vault_version, watchdog/backoff
     * tunables). No-op on first boot. */
    {
        extern int persistence_apply_snapshot(void);
        persistence_apply_snapshot();
    }

    /* (Partition activation deferred until after scheduler_init() so
     * the BSP-only init paths below run single-threaded. Activation
     * happens immediately before scheduler_run() takes over.) */

    /* Restore master audio volume + mute from VAULT (/audio/volume).
     * Default 60/unmuted on first boot. Pushes verb 0x300 to the DAC
     * and pin output amps once loaded. */
    {
        extern int hda_audio_restore_from_vault(void);
        (void)hda_audio_restore_from_vault();
    }

    /* Display brightness — ACPI _BCL pattern-scan for supported levels
     * and _BCM detection. Real laptops have a backlight; QEMU
     * virtio-gpu doesn't (no _BCL package). Restore the last user
     * preference from /display/brightness if persisted. */
    {
        extern void brightness_init(void);
        extern int  brightness_restore_from_vault(void);
        brightness_init();
        (void)brightness_restore_from_vault();
    }

    /* Z+ runtime registry — backs `chain ... { ... }` blocks declared
     * in user Z+ programs with persistent kernel chain entries. Idempotent. */
    {
        extern void zp_runtime_init(void);
        zp_runtime_init();
    }

    /* ZIR ingestion selftest — proves the front-end -> ZIR -> kernel path
     * executes in the live kernel (not just the host unit test). Loads a
     * minimal emit->gate->print ZIR document through zir_run() and reports on
     * serial. HONEST: reaches the sig_chain engine, not chain_t (no MasQ/B3
     * yet — see kernel/boot/zplus_zir.h). */
    {
        extern int zir_run(const char *json);
        static const char zir_selftest[] =
            "{ \"zir\": 1, \"source\": \"boot-selftest\","
            " \"chains\": [ { \"id\": 0, \"name\": \"main\", \"masq\": \"reference\", \"parent\": -1, \"nodes\": [0,1,2] } ],"
            " \"nodes\": ["
            "  { \"id\": 0, \"kind\": \"source\", \"verb\": \"emit\", \"name\": \"emit_0\", \"emit\": {\"int\": 7} },"
            "  { \"id\": 1, \"kind\": \"gate\", \"verb\": \"gate\", \"name\": \"gate_1\", \"gate\": {\"op\": \"gt\", \"rhs\": {\"int\": 3}} },"
            "  { \"id\": 2, \"kind\": \"processor\", \"verb\": \"print\", \"name\": \"print_2\" } ],"
            " \"edges\": [ { \"kind\": \"flow\", \"from\": 0, \"to\": 1 }, { \"kind\": \"flow\", \"from\": 1, \"to\": 2 } ] }";
        int fired = zir_run(zir_selftest);
        if (fired >= 0) {
            kputs("ZIR: selftest loaded+ran (emit->gate->print), ");
            kput_dec((uint64_t)fired);
            kputs(" nodes fired\n");
        } else {
            kputs("ZIR: selftest FAILED to load\n");
        }
    }

    /* Editor session restore: re-open files that were open at last
     * shutdown and restore cursor positions. Reads /editor/open_files
     * from VAULT. No-op if VAULT entry doesn't exist (first boot or
     * editor was never used). */
    {
        extern void editor_persist_restore(void);
        editor_persist_restore();
    }

    /* Unified settings registry — every config knob in the system
     * exposes a getter/setter pair through this surface so the shell
     * `settings` command can list and toggle everything. Persistence
     * still lives in each source module. Idempotent. */
    {
        extern void settings_register_all(void);
        extern void settings_print_selftest_line(void);
        settings_register_all();
        settings_print_selftest_line();
    }

    /* Calendar / clock app selftest: alarms armed, events, world clocks. */
    {
        extern void cal_print_selftest_line(void);
        cal_print_selftest_line();
    }

    /* File manager selftest: chains-over-fs_event count + events tracked. */
    {
        extern void file_mgr_print_selftest_line(void);
        extern void file_mgr_persist_restore(void);
        file_mgr_persist_restore();
        file_mgr_print_selftest_line();
    }

    /* Activity Monitor selftest: CHAIN_SYSTEM_STATE + anomaly subscriber. */
    {
        extern void activity_print_selftest_line(void);
        extern void activity_persist_restore(void);
        activity_persist_restore();
        activity_print_selftest_line();
    }

    /* Firewall selftest: enabled, rule count, conntrack, drops/min. */
    {
        extern void firewall_print_selftest_line(void);
        firewall_print_selftest_line();
    }

    /* USB UVC cameras selftest: count + formats, or "not present". */
    {
        extern void usb_uvc_print_selftest_line(void);
        usb_uvc_print_selftest_line();
    }

    /* Replacement-polish selftest lines (kv/web/build/notes/chat). */
    {
        extern void kv_polish_print_selftest_line(void);
        extern void web_polish_print_selftest_line(void);
        extern void build_polish_print_selftest_line(void);
        extern void notes_polish_print_selftest_line(void);
        extern void chat_polish_print_selftest_line(void);
        kv_polish_print_selftest_line();
        web_polish_print_selftest_line();
        build_polish_print_selftest_line();
        notes_polish_print_selftest_line();
        chat_polish_print_selftest_line();
    }

    /* Cold-boot login gate. After splash dismiss + chain registry +
     * persistence replay, but before the scheduler starts. Shows the
     * same PIN overlay used by idle-lock; on first ever boot
     * (no /lock/pin) the gate runs an enrollment flow first. The
     * setting /lock/cold-boot-required defaults to 1 (require PIN) and
     * can be toggled with the `cold-boot-login` shell command or the
     * lock.cold_boot_required setting. The shell pump never sees the
     * bytes typed at the gate -- keyboard.c routes them straight to
     * lockscreen_input() while the overlay is active. */
    {
#if defined(ZEOS_SMP_TEST_BYPASS_LOCKSCREEN) || defined(ZEOS_DIAG_SKIP_PIN_GATE)
        kputs("[main] diag build: cold-boot lockscreen bypassed\n");
#else
        if (cold_boot_login_required()) {
            kputs("[main] cold-boot login required\n");
            lockscreen_run_cold_boot_gate();
        } else {
            kputs("[main] cold-boot login disabled\n");
        }
#endif
    }

    /* Disk encryption. After the PIN gate succeeds (or enrollment completed),
     * derive the master AES-XTS-256 key from the PIN and arm the
     * crypto_transform node in CHAIN_BLOCK; from here accesses to registered
     * encrypted regions are transparently encrypted.
     *
     * R.2 scope note (2026-07-25): the CFA-native protection is on the DERIVED
     * KEY -- crypto_disk_init() wraps the master key in a SOVEREIGN CFA handle
     * (see crypto_disk.c). The raw PIN itself necessarily transits a plain
     * stack buffer here: a KDF needs the plaintext PIN bytes in memory, so this
     * frame is NOT CFA-backed. The buffer is volatile-wiped immediately after
     * key derivation (below). A guard-paged/CFA-transient PIN path would be a
     * large change with marginal payoff on bare metal (no swap, immediate wipe),
     * so it is deliberately not done -- the honest boundary is: key = CFA/SOVEREIGN,
     * PIN input = transient plaintext, zeroed on the spot. */
    {
        extern void lockscreen_init(void);
        extern int  lockscreen_pin_copy(char *out, int max);
        extern void crypto_disk_init(const char *pin, int pin_len);
        extern void crypto_disk_print_selftest_line(void);
        char pin_buf[24];
        int n = lockscreen_pin_copy(pin_buf, sizeof(pin_buf));
        if (n > 0) {
            crypto_disk_init(pin_buf, n);
        } else {
            kputs("[main] crypto_disk: no PIN, encryption inactive\n");
        }
        /* Wipe PIN bytes from stack ASAP (R.2: PIN is transient plaintext). */
        for (uint32_t _i = 0; _i < sizeof(pin_buf); _i++)
            ((volatile char *)pin_buf)[_i] = 0;
        crypto_disk_print_selftest_line();
    }

    /* CFA identity contexts. Loads contexts from VAULT or migrates the
     * legacy single-PIN boot into context 1 "owner". After this point
     * every newly created chain is tagged with the active context and
     * cross-context perception of INTERNAL/SOVEREIGN is denied. */
    {
        extern void identity_init(void);
        extern void identity_print_selftest_line(void);
        identity_init();
        identity_print_selftest_line();
    }

    /* First-run flow. After identity_init (so ctx is known) and before
     * scheduler_run (so the user is onboarded before the shell prompt).
     *
     * N.1/N.3 (2026-07-25): reconciled the two competing first-run flows to
     * ONE. The canonical flow is firstboot.c's 5-screen wizard
     * (welcome -> persona -> controls -> appearance -> done): it is the
     * complete, structured onboarding that configures all four onboarding
     * settings and persists a completion flag, vs welcome.c's single persona
     * modal (now retired from the boot path). firstboot_run() returns the
     * chosen config; the caller applies it here. Idempotent via
     * firstboot_should_run() (persisted "system/firstboot_complete").
     *
     * To revert the reconciliation decision, restore welcome_run_if_first_boot()
     * in place of this block -- it is a single-call swap. */
    {
#if defined(ZEOS_SMP_TEST_BYPASS_LOCKSCREEN) && !defined(ZEOS_DIAG_N1)
        kputs("[main] SMP-test build: first-run flow bypassed\n");
#else
        if (firstboot_should_run()) {
            struct firstboot_config cfg = firstboot_run();

            /* Apply chosen config to the live system. */
            extern void shell_set_persona(int);
            shell_set_persona((int)cfg.persona);   /* 0=Zeros 1=DereZ 2=Full == PERSONA_* */

            wm_set_controls_side(cfg.controls_side ? WM_CONTROLS_RIGHT
                                                   : WM_CONTROLS_LEFT);

            /* cfg.theme 0=light 1=dark 2=auto -> SCHEME_* (enum order differs) */
            {
                color_scheme_t sch = (cfg.theme == 0) ? SCHEME_LIGHT
                                   : (cfg.theme == 2) ? SCHEME_AUTO
                                                      : SCHEME_DARK;
                access_set_scheme(sch);
            }
            access_set_density((density_mode_t)cfg.density);  /* 0/1/2 == DENSITY_* */

            kputs("[N1] first-run applied: persona=");   kput_dec((uint64_t)cfg.persona);
            kputs(" controls=");                          kput_dec((uint64_t)cfg.controls_side);
            kputs(" theme=");                             kput_dec((uint64_t)cfg.theme);
            kputs(" density=");                           kput_dec((uint64_t)cfg.density);
            kputs("\n");
        } else {
            kputs("[main] first-run: already complete, skipped\n");
        }
#endif
    }

    /* Scheduler: chain resolution as the kernel main loop. Must
     * follow chain_registry_init + mde_auto_route (done inside) so
     * the topo order is ready before the first tick. */
    {
        extern void scheduler_init(void);
        scheduler_init();

#ifdef ZEOS_DIAG_R1
        { extern void vault_tier_selftest(void); vault_tier_selftest(); }
#endif

#ifdef ZEOS_DIAG_M4
        /* M.4 sensory-mode consumer selftest: accent muting/boost + border/
         * decorative-anim queries. Passive (no state mutation persists). */
        { extern void access_m4_selftest(void); access_m4_selftest(); }
#endif

#ifdef ZEOS_DIAG_M5
        /* M.5 touch-target selftest: hit_control catch-zone widened to 44px.
         * Passive (restores controls_side; drives a synthetic surface). */
        { extern void wm_m5_selftest(void); wm_m5_selftest(); }
#endif

#ifdef ZEOS_DIAG_D5
        /* D.5 selftest: panel height follows density (48/40/32) live.
         * Restores original density. */
        { extern void access_d5_selftest(void); access_d5_selftest(); }
#endif

#ifdef ZEOS_DIAG_E4
        /* E.4 selftest: cursor_confirm flashes + reverts. */
        { extern void cursor_e4_selftest(void); cursor_e4_selftest(); }
#endif

#ifdef ZEOS_DIAG_J3
        /* J.3 selftest: palette enumerates every registered setting. */
        { extern void palette_j3_selftest(void); palette_j3_selftest(); }
#endif

#ifdef ZEOS_DIAG_L5
        /* L.5 selftest: context menu spring open + deferred-teardown close. */
        { extern void context_menu_l5_selftest(void); context_menu_l5_selftest(); }
#endif

#ifdef ZEOS_DIAG_L1
        /* L.1 selftest: spring engine (Euler converge, 64 concurrent, retarget). */
        { extern void anim_l1_selftest(void); anim_l1_selftest(); }
#endif

#ifdef ZEOS_DIAG_L2
        /* L.2 selftest: spring presets produce distinct name-appropriate physics. */
        { extern void anim_l2_selftest(void); anim_l2_selftest(); }
#endif

#ifdef ZEOS_DIAG_E3
        /* E.3 selftest: cursor click feedback (scale pulse / ripple / burst). */
        { extern void cursor_e3_selftest(void); cursor_e3_selftest(); }
#endif


#ifdef ZEOS_DIAG_J2
        /* J.2 selftest: settings GUI mutates the one real access config. */
        { extern void settings_j2_selftest(void); settings_j2_selftest(); }
#endif

#ifdef ZEOS_DIAG_F4
        /* F.4 selftest: Inter->Noto glyph fallback chain. */
        { extern void font_f4_selftest(void); font_f4_selftest(); }
#endif

#ifdef ZEOS_DIAG_C4
        /* C.4 selftest: resize edge/corner hit-detection (8px band). */
        { extern void wm_c4_selftest(void); wm_c4_selftest(); }
#endif


#ifdef ZEOS_DIAG_A4_PREEMPT_SELFTEST
        /* A.4 selftest: prove LAPIC-timer preemption rescues a hung
         * chain_resolve. GATED behind a diagnostic define (fleet-review
         * finding #4, 2026-07-25): this is active STRESS scaffolding, not a
         * passive status print -- it registers a chain that spins for(;;)pause,
         * forces a LAPIC watchdog kill, costs ~5ms, and permanently leaves
         * scheduler_preempt_kills()>=1. It must NOT run in the production
         * binary. Build with -DZEOS_DIAG_A4_PREEMPT_SELFTEST to observe it, or
         * run the `preempt-test` shell command interactively. NOTE: A.4 is
         * PARTIAL, not VERIFIED -- this proves the mechanism in isolation only;
         * real-build steady-state survival is open (see A.9). */
        extern void cmd_preempt_test(const char *args);
        cmd_preempt_test(0);
#endif
    }

    /* Now that all single-threaded init is done, release APs from
     * their pre-partition spin. From here APs walk the chain registry
     * and resolve chains where (id % cpu_count) matches their cpu
     * index. BSP's scheduler_run skips AP-owned chains automatically. */
    {
        extern void smp_partition_activate(void);
        extern void smp_print_selftest_line(void);
        extern uint32_t smp_cpu_tps(int cpu_idx);
        extern int smp_cpus_online(void);
        extern void smp_refresh_tps_sample_pub(void);

        smp_partition_activate();
        /* Prime the rolling TPS sample, settle 250ms so APs accumulate
         * ticks, refresh the sample again, then print. Without the
         * priming step the first refresh has zero history and the line
         * always reports 0 tps. */
        smp_refresh_tps_sample_pub();
        timer_wait_ms(250);
        smp_print_selftest_line();

        /* TLB shootdown selftest line — ISR registered in idt_init,
         * BSP-side broadcast wired into vmm_map_range / vmm_unmap. */
        extern void tlb_print_selftest_line(void);
        tlb_print_selftest_line();
    }

    /* ── Graphical desktop shell ──────────────────────────────────
     * The compositor/desktop/dock/panel render nodes are registered by
     * chain_registry_init() (above) and tick under scheduler_run(), but their
     * STATE was never initialized -- which is why boot landed on a bare
     * surface. Initialize it here so Zeos boots into a populated desktop:
     * wallpaper + top panel (with clock) + dock. compositor_init_ex(...,0)
     * skips the registry re-init (already done) so existing chains survive. */
    {
        extern int  compositor_init_ex(int, int, int);
        extern void desktop_init(uint32_t, int);
        extern void dock_init(int);
        extern int  dock_pin(const char *, int);
        extern void dock_show(void);
        extern void dock_force_open(void);
        extern void panel_set_persona(int);
        extern uint32_t fb_width(void);
        extern uint32_t fb_height(void);

        compositor_init_ex((int)fb_width(), (int)fb_height(), 0);
        /* PREREQUISITE: without cursor_init(), g_cursor.scale is 0 and the
         * cursor renders at size 0 (invisible). anim_init() readies the spring
         * pool. Both must run before the live loop draws anything. */
        { extern void cursor_init(void); extern void anim_init(void);
          cursor_init(); anim_init(); }
        desktop_init(0xFF0E1626, 1);          /* deep-blue wallpaper */
        dock_init(0);                          /* 0 = always visible */
        dock_pin("Files",      -1);
        dock_pin("Editor",     -1);
        dock_pin("Terminal",   -1);
        dock_pin("Settings",   -1);
        dock_pin("Calculator", -1);
        dock_show();
        dock_force_open();                     /* settle slide-in for a static paint */
        panel_set_persona(2);                  /* PERSONA_FULL */

        /* Open a couple of app windows so the desktop reads as a desktop:
         * real WM chrome (title bar + close X) + client area. */
        extern int  wm_create_surface(const char *, int, int, int, int, int,
                                      void (*)(int, int, int, int, int));
        extern void wm_focus_surface(int);
        extern void wm_force_visible(int);
        int w_files = wm_create_surface("Files",     -1, 180, 200, 640, 460, boot_files_draw_content);
        int w_term  = wm_create_surface("Terminal",  -1, 760, 320, 780, 480, boot_term_draw_content);
        wm_force_visible(w_files);
        wm_force_visible(w_term);
        wm_focus_surface(w_term);

#ifdef ZEOS_DIAG_D3
        /* D.3: force distinct signal states so the panel center-pill color-by-state
         * (pill_color: LIVE->accent, PAUSED->dim, ERROR->danger) is observable in a
         * single screendump. LIVE(green) is the already-observed default. Nothing in
         * the tick path rewrites s->signal, so the forced states persist. */
        {
            extern chain_surface_t *wm_get_surface(int id);
            chain_surface_t *sf = wm_get_surface(w_files);
            chain_surface_t *st = wm_get_surface(w_term);
            if (sf) sf->signal = SIGNAL_PAUSED;
            if (st) st->signal = SIGNAL_ERROR;
            kputs("[D3] forced Files=PAUSED Terminal=ERROR for pill color observe\n");
        }
#endif

#ifdef ZEOS_DIAG_E9
        { extern void keyboard_e9_selftest(void); keyboard_e9_selftest(); }
#endif

#ifdef ZEOS_DIAG_C12
        { extern void wm_c12_selftest(void); wm_c12_selftest(); }
#endif
#ifdef ZEOS_DIAG_C14
        { extern void wm_c14_selftest(void); wm_c14_selftest(); }
#endif

#ifdef ZEOS_DIAG_C10
        /* C.10 selftest: modal sheet slide-from-titlebar. */
        { extern void sheet_c10_selftest(void); sheet_c10_selftest(); }
#endif

#ifdef ZEOS_DIAG_C11
        /* C.11 selftest: non-modal popover. */
        { extern void popover_c11_selftest(void); popover_c11_selftest(); }
#endif

#ifdef ZEOS_DIAG_C13
        /* C.13 selftest: same-chain magnetic adjacency. */
        { extern void wm_c13_selftest(void); wm_c13_selftest(); }
#endif

#ifdef ZEOS_DIAG_D11
        /* D.11 selftest: drag icon -> drop over window feeds the chain. */
        { extern void desktop_d11_selftest(void); desktop_d11_selftest(); }
#endif

#ifdef ZEOS_DIAG_J4
        /* J.4 selftest: Settings-for-this opens Settings on the element page. */
        { extern void settings_j4_selftest(void); settings_j4_selftest(); }
#endif

#ifdef ZEOS_DIAG_L6
        /* L.6 selftest: spring scroll physics (momentum + rubber-band). */
        { extern void anim_l6_selftest(void); anim_l6_selftest(); }
#endif

#ifdef ZEOS_DIAG_M8
        /* M.8 selftest: CVD color transform. */
        { extern void access_m8_selftest(void); access_m8_selftest(); }
#endif

#ifdef ZEOS_DIAG_B8
        /* B.8 selftest: material vibrancy ladder. */
        { extern void fb_b8_selftest(void); fb_b8_selftest(); }
#endif

#ifdef ZEOS_DIAG_G5
        /* G.5 selftest: per-persona default dock launcher sets. */
        { extern void dock_g5_selftest(void); dock_g5_selftest(); }
#endif

#ifdef ZEOS_DIAG_D6
        /* D.6 selftest: panel auto-hide reveal/hide. */
        { extern void panel_d6_selftest(void); panel_d6_selftest(); }
#endif

#ifdef ZEOS_DIAG_K3
        /* K.3 selftest: sigviz live pulse animation. */
        { extern void sigviz_k3_selftest(void); sigviz_k3_selftest(); }
#endif

#ifdef ZEOS_DIAG_E2
        /* E.2 selftest: 22 cursor sprites + hotspot table. */
        { extern void cursor_e2_selftest(void); cursor_e2_selftest(); }
#endif

#ifdef ZEOS_DIAG_J1
        /* J.1 selftest: Settings app VAULT persist round-trip. */
        { extern void settings_j1_selftest(void); settings_j1_selftest(); }
#endif

#ifdef ZEOS_DIAG_O2
        { extern void hal_o2_selftest(void); hal_o2_selftest(); }
#endif

#ifdef ZEOS_DIAG_D13
        { extern void dock_d13_selftest(void); dock_d13_selftest(); dock_apply_density(); }
#endif

#ifdef ZEOS_DIAG_A7
        { extern void crypto_disk_a7_selftest(void); crypto_disk_a7_selftest(); }
#endif

#ifdef ZEOS_DIAG_L3
        { extern void compositor_l3_selftest(void); compositor_l3_selftest(); }
#endif

#ifdef ZEOS_DIAG_P3
        { extern void zp_p3_selftest(void); zp_p3_selftest(); }
#endif

#ifdef ZEOS_DIAG_P2
        /* P.2 selftest: Z+ REPL core runs real programs, rejects garbage. */
        { extern void zp_p2_selftest(void); zp_p2_selftest(); }
#endif

#ifdef ZEOS_DIAG_M7
        /* M.7 selftest: Focus Mode suppresses non-critical notifications. */
        { extern void notify_m7_selftest(void); notify_m7_selftest(); }
#endif

#ifdef ZEOS_DIAG_G4
        /* G.4 selftest: dark/light scheme switching yields distinct palettes. */
        { extern int theme_g4_selftest(void); (void)theme_g4_selftest(); }
#endif

#ifdef ZEOS_DIAG_G12
        /* G.1+G.2: persona accent/dim tokens + prompt/cursor colorway switch. */
        { extern void persona_g12_selftest(void); persona_g12_selftest(); }
#endif

#ifdef ZEOS_DIAG_G3
        /* G.3 selftest: persona crossfade spring color lerp. */
        { extern void persona_g3_selftest(void); persona_g3_selftest(); }
#endif

#ifdef ZEOS_DIAG_L4
        /* L.4 selftest: surface open/close spring (dock-slide proven by D.12). */
        { extern void wm_l4_selftest(void); wm_l4_selftest(); }
#endif

#ifdef ZEOS_DIAG_D12
        /* D.12 selftest: run AFTER dock pin + window creation so the dock is
         * populated (pinned + running). Restores dock_show() at the end. */
        { extern void dock_d12_selftest(void); dock_d12_selftest();
          dock_force_open(); }
#endif

#ifdef ZEOS_DIAG_C9
        /* C.9: force controls side LEFT (AFTER wm_init, which defaults RIGHT) so
         * a screenshot shows the window control buttons on the left. */
        wm_set_controls_side(WM_CONTROLS_LEFT);
        kputs("[C9] controls_side="); kput_dec((uint64_t)wm_get_controls_side()); kputs("\n");
#endif

        /* Paint the initial desktop immediately (direct-to-framebuffer), in
         * case the render chains haven't been resolved yet this early. */
        extern void desktop_draw(void);
        extern void panel_update(void); extern void panel_draw(void);
        extern void dock_update(void);  extern void dock_draw(void);
        extern void wm_draw_all(void);
        desktop_draw();
        panel_update(); panel_draw();
        dock_update();  dock_draw();
        wm_draw_all();
        kputs("[main] graphical desktop shell up (wallpaper + panel + dock + windows)\n");
    }

    /* Enter shell — initializes shell state then hands off to
     * scheduler_run() which never returns. */
    shell_run(&boot_info);

    /* Safety net: if shell_run ever returns, halt instead of
     * returning to the UEFI CRT0 stub (which is in freed memory). */
    panic("shell_run returned unexpectedly");

    /* Unreachable, but keeps the compiler happy */
    return EFI_SUCCESS;
}
