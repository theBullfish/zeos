/*
 * Zeos — minimal UEFI test. Just print and halt.
 */
#include <efi.h>
#include <efilib.h>

EFI_STATUS
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    (void)image;
    InitializeLib(image, st);
    Print(L"Zeos is alive.\r\n");

    /* Spin forever */
    for (;;)
        __asm__ volatile("hlt");

    return EFI_SUCCESS;
}
