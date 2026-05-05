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

/* Boot info passed from UEFI to kernel */
static struct zeos_boot_info boot_info;

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
        if (cold_boot_login_required()) {
            kputs("[main] cold-boot login required\n");
            lockscreen_run_cold_boot_gate();
        } else {
            kputs("[main] cold-boot login disabled\n");
        }
    }

    /* CFA-native disk encryption. After the PIN gate succeeds (or
     * enrollment completed), derive the master AES-XTS-256 key from
     * the PIN, wrap it in a SOVEREIGN CFA handle, and arm the
     * crypto_transform node in CHAIN_BLOCK. From here, accesses to
     * registered encrypted regions are transparently encrypted. */
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
        /* Wipe PIN bytes from stack ASAP. */
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

    /* First-boot welcome flow. Per identity context. After identity_init
     * so we know which ctx to scope the markers to; before scheduler_run
     * so the user sees the welcome before the shell prompt. Idempotent:
     * skips on subsequent boots of the same ctx. */
    {
        extern void welcome_run_if_first_boot(void);
        extern void welcome_print_selftest_line(void);
        welcome_run_if_first_boot();
        welcome_print_selftest_line();
    }

    /* Scheduler: chain resolution as the kernel main loop. Must
     * follow chain_registry_init + mde_auto_route (done inside) so
     * the topo order is ready before the first tick. */
    {
        extern void scheduler_init(void);
        scheduler_init();
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

    /* Enter shell — initializes shell state then hands off to
     * scheduler_run() which never returns. */
    shell_run(&boot_info);

    /* Safety net: if shell_run ever returns, halt instead of
     * returning to the UEFI CRT0 stub (which is in freed memory). */
    panic("shell_run returned unexpectedly");

    /* Unreachable, but keeps the compiler happy */
    return EFI_SUCCESS;
}
