/* SPDX-License-Identifier: MIT
 * Same source for AArch64 ELF and Windows ARM64 COFF. No SEH unwinding,
 * platform-register x18 is untouched by wrapper and fully saved by vectors.
 */
.text
.p2align 4
.globl IrqProbeRun
#ifndef _WIN32
.type IrqProbeRun, %function
#endif
IrqProbeRun:
    /* 16-byte aligned and preserve every integer callee-save on both ABIs. */
    stp x19, x20, [sp, #-96]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mov x29, sp
    mov x4, sp
    adr x5, .Lfault_return
    bl IrqProbeRunCore
    b .Lrun_return
.Lfault_return:
    /* Exception vector restored our entry SP, not an abandoned C frame. */
    bl IrqProbeFaultCleanup
.Lrun_return:
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp x19, x20, [sp], #96
    ret
#ifndef _WIN32
.size IrqProbeRun, .-IrqProbeRun
#endif

/* 16 x 128 bytes, architectural VBAR alignment is 2048 bytes. */
.p2align 11
.globl IrqProbeVectors
#ifndef _WIN32
.type IrqProbeVectors, %object
#endif
IrqProbeVectors:
.macro VEC target
    b \target
    .space 124
.endm
    VEC .Lfault
    VEC .Lirq
    VEC .Lfault
    VEC .Lfault
    VEC .Lfault
    VEC .Lirq
    VEC .Lfault
    VEC .Lfault
    VEC .Lfault
    VEC .Lirq
    VEC .Lfault
    VEC .Lfault
    VEC .Lfault
    VEC .Lirq
    VEC .Lfault
    VEC .Lfault
#ifndef _WIN32
.size IrqProbeVectors, .-IrqProbeVectors
#endif

.macro SAVE_FRAME
    sub sp, sp, #272
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x0, elr_el1
    mrs x1, spsr_el1
    stp x0, x1, [sp, #248]
    str xzr, [sp, #264]
    mov x0, sp
.endm
.Lirq:
    SAVE_FRAME
    bl IrqProbeHandleIrq
    b .Lrestore_frame
.Lfault:
    SAVE_FRAME
    bl IrqProbeHandleFault
    cbz x0, .Lno_context
    /* Return to wrapper recovery PC in EL1h with all DAIF masked. There is
     * no longjmp through a compiler frame and no dependency on x18. */
    ldp x1, x2, [sp, #248]
    msr elr_el1, x1
    msr spsr_el1, x2
    mov sp, x0
    eret
.Lno_context:
    /* Impossible within the supported API lifecycle. Keep DAIF masked. */
    msr daifset, #15
    b .Lno_context
.Lrestore_frame:
    ldp x0, x1, [sp, #248]
    msr elr_el1, x0
    msr spsr_el1, x1
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    ldp x0, x1, [sp, #0]
    add sp, sp, #272
    eret
#ifndef _WIN32
.section .note.GNU-stack,"",%progbits
#endif
