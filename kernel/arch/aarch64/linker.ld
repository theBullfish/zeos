/* Zeos aarch64 M0 — QEMU virt image layout. RAM base 0x40000000; load at +512K. */
ENTRY(_start)
SECTIONS
{
    . = 0x40080000;
    .text : { KEEP(*(.text.boot)) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;

    . = ALIGN(16);
    . = . + 0x10000;      /* 64 KiB primary boot stack */
    _stack_top = .;

    . = . + 0x4000;       /* 16 KiB secondary stack */
    _sec_stack_top = .;
}
