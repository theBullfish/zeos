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
#include "zeos_boot.h"
#include "idt.h"
#include "keyboard.h"
#include "pci.h"
#include "shell.h"

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
 * Acquire the GOP framebuffer.
 */
static EFI_STATUS init_gop(EFI_SYSTEM_TABLE *st, struct zeos_framebuffer *fb)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status;

    status = uefi_call_wrapper(st->BootServices->LocateProtocol, 3,
                               &gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(status) || !gop)
        return status;

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
    status = init_gop(st, &boot_info.fb);
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
    fb_clear(0x001A1A1A);  /* Dark warm gray — not pure black */

    fb_puts("================================================\n");
    fb_puts("  Zeos\n");
    fb_puts("  The first operating system with proprioception.\n");
    fb_puts("================================================\n\n");

    fb_puts("Boot services released. We own the machine.\n\n");

    /* Print framebuffer info */
    fb_puts("Framebuffer: ");
    fb_put_dec(boot_info.fb.width);
    fb_puts("x");
    fb_put_dec(boot_info.fb.height);
    fb_puts(" pitch=");
    fb_put_dec(boot_info.fb.pitch);
    fb_puts("\n");

    /* Print ACPI status */
    fb_puts("ACPI RSDP:   ");
    if (boot_info.rsdp) {
        fb_puts("found at 0x");
        fb_put_hex((uint64_t)(UINTN)boot_info.rsdp);
    } else {
        fb_puts("not found");
    }
    fb_puts("\n");

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

    fb_puts("Memory map:  ");
    fb_put_dec(total_entries);
    fb_puts(" entries, ");
    fb_put_dec(total_usable / (1024 * 1024));
    fb_puts(" MB usable\n\n");

    /* Read TSC — first timing measurement */
    uint64_t tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc));
    /* rdtsc returns low 32 in eax, high 32 in edx on x86_64 */
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    tsc = ((uint64_t)hi << 32) | lo;

    fb_puts("TSC:         0x");
    fb_put_hex(tsc);
    fb_puts("\n");
    fb_puts("             Zixel embryo — first timing delta on bare metal.\n\n");

    fb_puts("Zeos is alive.\n\n");

    /* Initialize interrupts first — needed before any I/O that might trigger IRQs */
    fb_puts("Setting up IDT... ");
    idt_init();
    fb_puts("done.\n");

    /* Initialize PCI (uses I/O ports, safe after IDT is up) */
    fb_puts("Scanning PCI bus... ");
    pci_init(boot_info.rsdp);
    int pci_count = pci_enumerate();
    fb_put_dec(pci_count);
    fb_puts(" devices found (legacy I/O).\n");

    /* Initialize keyboard */
    fb_puts("Initializing keyboard... ");
    keyboard_init();
    fb_puts("done.\n");

    /* Enable interrupts */
    __asm__ volatile("sti");
    fb_puts("Interrupts enabled.\n\n");

    /* Enter shell — never returns */
    shell_run(&boot_info);

    return EFI_SUCCESS;
}
