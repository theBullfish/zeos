; Zeos AP bring-up trampoline.
;
; Loaded by the BSP at physical 0x8000. APs receive a SIPI with vector
; 0x08, so they begin executing in real mode at CS:IP = 0x0800:0x0000
; -> linear 0x8000.
;
; Stages:
;   1. Real-mode (16-bit): cli, zero segment regs, lgdt, set CR0.PE,
;      far-jump into 32-bit protected mode.
;   2. Protected mode (32-bit): load 32-bit data segs, set CR4.PAE,
;      load CR3 from parameter slot, set EFER.LME, enable CR0.PG|PE,
;      far-jump into 64-bit long mode.
;   3. Long mode (64-bit): load 64-bit data segs, load RDI=cpu_idx,
;      RSP=stack_top, jump to ap_main.
;
; Diagnostic checkpoints: at each transition we write a single magic
; byte to physical address 0x9000+lapic_id*4 ... but we don't have the
; LAPIC ID at this stage cheaply. Instead we write four bytes at
; param-slot OFF_DIAG indicating which stage was reached. The BSP zeros
; this slot before each SIPI and reads it back to know how far the AP
; advanced.
;
; Parameter slots (patched by BSP, all at fixed page offsets):
;   OFF_DIAG     = 0x0FD0  uint32_t  diagnostic stage (1=real,2=prot,3=long)
;   OFF_CPU_IDX  = 0x0FD8  uint64_t  cpu index argument for ap_main
;   OFF_PML4     = 0x0FE0  uint64_t  PML4 phys to load into CR3
;   OFF_ENTRY    = 0x0FE8  uint64_t  ap_main function pointer
;   OFF_STACKTOP = 0x0FF0  uint64_t  per-CPU stack top (virt)
;
; All labels are page-relative; the BSP copies the assembled blob to
; 0x8000 and computes addresses as 0x8000 + offset.

BITS 16
ORG 0x8000

%define DIAG_PHYS    0x8FD0
%define CPU_IDX_PHYS 0x8FD8
%define PML4_PHYS    0x8FE0
%define ENTRY_PHYS   0x8FE8
%define STACK_PHYS   0x8FF0

ap_start:
    cli
    cld

    ; Zero segment registers. After SIPI, CS=0x0800 but DS/ES/SS are
    ; undefined per Intel SDM. Set them to zero so absolute addresses
    ; like [0x8060] resolve to linear 0x8060.
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Diagnostic: stage 1 reached.
    mov dword [DIAG_PHYS], 0x11111111

    ; Load the 32-bit GDT.
    lgdt [gdt_ptr32]

    ; Enable PE.
    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    ; Far jump to 32-bit code. Selector 0x08 = 32-bit code.
    jmp dword 0x08:prot32

BITS 32
ALIGN 16
prot32:
    ; Load 32-bit data segments. Selector 0x10 = 32-bit data.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Diagnostic: stage 2 reached.
    mov dword [DIAG_PHYS], 0x22222222

    ; Set CR4.PAE | CR4.PSE, and CR4.OSFXSR | CR4.OSXMMEXCPT.
    ;
    ; The SSE bits are NOT optional. An AP comes out of reset with CR4=0, so
    ; unlike the BSP (which inherits OSFXSR from UEFI) it has SSE disabled. The
    ; kernel is compiled for the x86-64 ABI with no -mno-sse, so GCC emits SSE
    ; freely: chain_resolve() -- the very function an AP runs -- contains 14 SSE
    ; instructions and mde.o contains 99. Without these bits the first one an AP
    ; executes takes #UD, and with no per-AP TSS that becomes a silent triple
    ; fault. Done here in asm rather than in C so there is NO window between
    ; entering long mode and enabling SSE.
    mov eax, cr4
    or  eax, (1 << 5) | (1 << 4) | (1 << 9) | (1 << 10)
    mov cr4, eax

    ; CR0: clear EM (bit 2, no x87 emulation), set MP (bit 1). Required
    ; alongside OSFXSR for SSE to be usable.
    mov eax, cr0
    and eax, ~(1 << 2)
    or  eax, (1 << 1)
    mov cr0, eax

    ; Load CR3 from parameter slot.
    mov eax, [PML4_PHYS]
    mov cr3, eax

    ; Set EFER.LME (and EFER.NXE, harmless if not used).
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    ; Enable paging — atomically set PE | PG (PE is already on, this
    ; is the canonical PG-set step). Also keep WP cleared for kernel.
    mov eax, cr0
    or  eax, 0x80000001
    mov cr0, eax

    ; Far jump into 64-bit code. Selector 0x18 = 64-bit code.
    jmp 0x18:long64

BITS 64
ALIGN 16
long64:
    ; Load 64-bit data segments. Selector 0x20 = 64-bit data.
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Initialise the x87/SSE unit before any FP state is touched.
    fninit

    ; Diagnostic: stage 3 reached.
    mov dword [DIAG_PHYS], 0x33333333

    ; Load per-CPU args.
    mov rdi, [CPU_IDX_PHYS]
    mov rsp, [STACK_PHYS]
    mov rax, [ENTRY_PHYS]

    ; Tail-call ap_main(cpu_idx). RDI carries cpu_idx (SysV ABI arg0).
    ; Push a NULL return address so any accidental ret traps cleanly.
    xor rbp, rbp
    push 0
    jmp rax

    ; Should never return; if it does, halt.
.hang:
    cli
    hlt
    jmp .hang

; ─── GDT ────────────────────────────────────────────────────────────
; Five entries: null, 32-bit code, 32-bit data, 64-bit code, 64-bit data.
ALIGN 8
gdt_base:
    dq 0x0000000000000000  ; 0x00 null
    dq 0x00CF9A000000FFFF  ; 0x08 32-bit code: base=0, limit=4GB, G=1, D=1, type=0x9A
    dq 0x00CF92000000FFFF  ; 0x10 32-bit data: base=0, limit=4GB, G=1, D=1, type=0x92
    dq 0x00AF9A000000FFFF  ; 0x18 64-bit code: L=1, D=0, type=0x9A
    dq 0x00AF92000000FFFF  ; 0x20 64-bit data: type=0x92
gdt_end:

ALIGN 8
gdt_ptr32:
    dw gdt_end - gdt_base - 1
    dd gdt_base

; Pad up to the parameter area at offset 0xFD0.
times (0xFD0 - ($ - $$)) db 0x90

; Parameter slots — last 0x30 bytes of the page.
diag_slot:    dd 0   ; +0xFD0 — uint32_t diag stage
              dd 0   ; +0xFD4 — pad
cpu_idx_slot: dq 0   ; +0xFD8
pml4_slot:    dq 0   ; +0xFE0
entry_slot:   dq 0   ; +0xFE8
stack_slot:   dq 0   ; +0xFF0
              dq 0   ; +0xFF8 — pad to fill page
