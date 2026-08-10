// aarch64 exception vectors. We run at EL1h (SP_ELx), so live exceptions land
// in the "Current EL with SP_ELx" group (slots 4..7). Slot 5 = IRQ -> returning
// handler (timer tick). Everything else = fatal reporter (visible faults).
.macro VEC label
.align 7
    b   \label
.endm

.section ".text"
.align 11
.global vectors
vectors:
    VEC exc_report      // 0 Curr EL SP0 sync
    VEC exc_report      // 1 irq
    VEC exc_report      // 2 fiq
    VEC exc_report      // 3 serror
    VEC exc_report      // 4 Curr EL SPx sync
    VEC irq_entry       // 5 irq   <-- timer
    VEC exc_report      // 6 fiq
    VEC exc_report      // 7 serror
    VEC exc_report      // 8 Lower EL A64 sync
    VEC exc_report      // 9 irq
    VEC exc_report      // 10 fiq
    VEC exc_report      // 11 serror
    VEC exc_report      // 12 Lower EL A32 sync
    VEC exc_report      // 13 irq
    VEC exc_report      // 14 fiq
    VEC exc_report      // 15 serror

// Fatal: report and halt.
exc_report:
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    bl      exc_dispatch
1:  wfe
    b       1b

// Returning IRQ handler: save the full GPR frame, dispatch, restore, eret.
irq_entry:
    sub     sp, sp, #256
    stp     x0,  x1,  [sp, #16*0]
    stp     x2,  x3,  [sp, #16*1]
    stp     x4,  x5,  [sp, #16*2]
    stp     x6,  x7,  [sp, #16*3]
    stp     x8,  x9,  [sp, #16*4]
    stp     x10, x11, [sp, #16*5]
    stp     x12, x13, [sp, #16*6]
    stp     x14, x15, [sp, #16*7]
    stp     x16, x17, [sp, #16*8]
    stp     x18, x19, [sp, #16*9]
    stp     x20, x21, [sp, #16*10]
    stp     x22, x23, [sp, #16*11]
    stp     x24, x25, [sp, #16*12]
    stp     x26, x27, [sp, #16*13]
    stp     x28, x29, [sp, #16*14]
    stp     x30, xzr, [sp, #16*15]
    bl      irq_handler
    ldp     x0,  x1,  [sp, #16*0]
    ldp     x2,  x3,  [sp, #16*1]
    ldp     x4,  x5,  [sp, #16*2]
    ldp     x6,  x7,  [sp, #16*3]
    ldp     x8,  x9,  [sp, #16*4]
    ldp     x10, x11, [sp, #16*5]
    ldp     x12, x13, [sp, #16*6]
    ldp     x14, x15, [sp, #16*7]
    ldp     x16, x17, [sp, #16*8]
    ldp     x18, x19, [sp, #16*9]
    ldp     x20, x21, [sp, #16*10]
    ldp     x22, x23, [sp, #16*11]
    ldp     x24, x25, [sp, #16*12]
    ldp     x26, x27, [sp, #16*13]
    ldp     x28, x29, [sp, #16*14]
    ldp     x30, xzr, [sp, #16*15]
    add     sp, sp, #256
    eret
