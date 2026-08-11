// Zeos aarch64 arch entry. Land at EL1 (drop from EL2 if we booted there),
// install the exception vectors, stack, zero BSS, into C.
.section ".text.boot"
.global _start
_start:
    // FIRST INSTRUCTION: firmware hands us the device tree in x0 (Linux/UEFI
    // aarch64 boot protocol). It describes where THIS board's hardware actually
    // is. Everything below clobbers x0, so stash it before touching anything —
    // Zeos discovers hardware, it never assumes a memory map.
    adrp    x1, g_dtb_ptr
    add     x1, x1, :lo12:g_dtb_ptr
    str     x0, [x1]

    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbnz    x0, park                 // M1/M2: primary core only

    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    at_el1

    // --- we are at EL2: configure and eret down to EL1h ---
    mov     x0, #(1 << 31)           // HCR_EL2.RW = 1 -> EL1 is AArch64
    msr     hcr_el2, x0
    msr     sctlr_el1, xzr           // EL1 MMU/caches off
    mov     x0, #0x3c5               // SPSR: D,A,I,F=1, mode=EL1h
    msr     spsr_el2, x0
    adr     x0, at_el1
    msr     elr_el2, x0
    ldr     x0, =_stack_top
    msr     sp_el1, x0
    eret

at_el1:
    ldr     x0, =_stack_top
    mov     sp, x0
    adr     x0, vectors
    msr     vbar_el1, x0
    isb

    mov     x0, #(3 << 20)           // CPACR_EL1.FPEN=0b11 -> FP/SIMD usable at EL1
    msr     cpacr_el1, x0
    isb

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:  cmp     x1, x2
    b.ge    2f
    str     xzr, [x1], #8
    b       1b
2:
    bl      kmain_aarch64
hang:
    wfe
    b       hang
park:
    wfe
    b       park

// Secondary cores enter here via PSCI CPU_ON (MMU off, own stack).
.global secondary_entry
secondary_entry:
    ldr     x0, =_sec_stack_top
    mov     sp, x0
    bl      kmain_secondary
1:  wfe
    b       1b
