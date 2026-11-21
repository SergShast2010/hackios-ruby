/* AArch64 EL1 physical handoff probe. No firmware calls, SMC/HVC or disk IO. */
.text
.p2align 4
.globl EnterPhysical
EnterPhysical:
    msr daifset, #15
    mov x19, x0
    msr spsel, #1
    mov sp, x1
    adrp x9, OwnVectors
    add x9, x9, :lo12:OwnVectors
    msr vbar_el1, x9
    isb
    str x9, [x19, #112]
    mrs x9, sctlr_el1
    str x9, [x19, #80]
    mov x9, #1
    str x9, [x19, #104]
    /* Preserve CSSELR; clean+invalidate every data/unified cache by set/way.
       Target must report legacy CCSIDR (FEAT_CCIDX==0), checked before EBS.
       No stack or memory writes until SCTLR.C is clear. */
    mrs x18, csselr_el1
    dsb sy
    mrs x9, sctlr_el1
    bic x9, x9, #4
    msr sctlr_el1, x9
    isb
    dsb sy
    mrs x10, clidr_el1
    ubfx x11, x10, #24, #3
    mov x12, #0
Llevel:
    cmp x12, x11
    b.hs Lclean_done
    add x13, x12, x12, lsl #1
    lsr x14, x10, x13
    and x14, x14, #7
    cmp x14, #2
    b.lo Lnext_level
    lsl x13, x12, #1
    msr csselr_el1, x13
    isb
    mrs x14, ccsidr_el1
    and x15, x14, #7
    add x15, x15, #4
    ubfx x16, x14, #3, #10
    clz w17, w16
    ubfx x8, x14, #13, #15
Lset:
    ubfx x16, x14, #3, #10
Lway:
    lsl x9, x16, x17
    lsl x7, x8, x15
    orr x9, x9, x7
    orr x9, x9, x13
    dc cisw, x9
    subs x16, x16, #1
    b.ge Lway
    subs x8, x8, #1
    b.ge Lset
    dsb sy
Lnext_level:
    add x12, x12, #1
    b Llevel
Lclean_done:
    msr csselr_el1, x18
    dsb sy
    mrs x9, sctlr_el1
    bic x9, x9, #1
    bic x9, x9, #4
    bic x9, x9, #0x1000
    msr sctlr_el1, x9
    isb
    ic iallu
    tlbi vmalle1
    dsb sy
    isb
    mrs x9, sctlr_el1
    str x9, [x19, #88]
    mov x9, #2
    str x9, [x19, #104]
    mov x0, x19
    b PostPhysical

/* Each EL1 vector slot is 128 bytes; the table is exactly 2048-byte aligned. */
.p2align 11
.globl OwnVectors
OwnVectors:
.macro SLOT n
    mov x17, #\n
    b OwnException
    .space 120
.endm
SLOT 0
SLOT 1
SLOT 2
SLOT 3
SLOT 4
SLOT 5
SLOT 6
SLOT 7
SLOT 8
SLOT 9
SLOT 10
SLOT 11
SLOT 12
SLOT 13
SLOT 14
SLOT 15
.globl OwnVectorsEnd
OwnVectorsEnd:
OwnException:
    movz x16, #0x8000
    movk x16, #0x6200, lsl #16
    str x0, [x16, #192]
    str x20, [x16, #200]
    mrs x9, esr_el1
    mrs x10, elr_el1
    mrs x11, far_el1
    str x9, [x16, #128]
    str x10, [x16, #136]
    str x11, [x16, #144]
    str x17, [x16, #152]
    /* Resume only our one deliberate BRK in current EL using SP_EL1. */
    cmp x17, #4
    b.ne Lfault
    movz x12, #0x526
    movk x12, #0xf200, lsl #16
    cmp x9, x12
    b.ne Lfault
    adrp x12, ProbeBrk
    add x12, x12, :lo12:ProbeBrk
    cmp x10, x12
    b.ne Lfault
    ldr x12, [x16, #104]
    cmp x12, #2
    b.ne Lfault
    ldr x12, [x16, #120]
    cbnz x12, Lfault
    mov x12, #1
    str x12, [x16, #120]
    add x10, x10, #4
    msr elr_el1, x10
    eret
Lfault:
    msr daifset, #15
    msr spsel, #1
    add x12, x16, #0x4000
    mov sp, x12
    mov x0, x16
    b RawFault

.globl ProbeException
ProbeException:
.globl ProbeBrk
ProbeBrk:
    brk #0x526
    ret

/* Terminal transfer to the real Mach-O entry. Only x0 is boot ABI input. */
.globl JumpXnu
JumpXnu:
    ldr x9, [x0, #160]
    ldr x0, [x0, #168]
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    /* common_start returns to the C entry with high-VA boot_args. */
    movz x30, #0x3994
    movk x30, #0x0745, lsl #16
    movk x30, #0xfff0, lsl #32
    movk x30, #0xffff, lsl #48
    br x9

/* Native HackiOS return ABI: x2=reason, x3=checkpoint or ESR.
 * SP from XNU is virtual while MMU is off: NO stack use until replaced.
 * Keep unexpected exceptions diagnosable as well as checkpoint 10. */
.globl XnuReturn
XnuReturn:
    msr daifset, #15
    /* C loader_jump maps only the return code block in TTBR0.
       Turn M off before any context/stack data access. No memory here. */
    mrs x14, sctlr_el1
    bic x15, x14, #1
    msr sctlr_el1, x15
    isb
    movz x16, #0x8000
    movk x16, #0x6200, lsl #16
    str x0, [x16, #192]
    str x20, [x16, #200]
    str x2, [x16, #208]
    str x3, [x16, #216]
    str x3, [x16, #128]
    str x4, [x16, #136]
    str x5, [x16, #144]
    mrs x9, vbar_el1
    str x9, [x16, #224]
    mrs x9, spsel
    cbz x9, Lsp0
    mov x9, sp
    str x9, [x16, #240]
    mrs x9, sp_el0
    str x9, [x16, #232]
    b Lsaved_sp
Lsp0:
    mov x9, sp
    str x9, [x16, #232]
    msr spsel, #1
    mov x9, sp
    str x9, [x16, #240]
Lsaved_sp:
    str x22, [x16, #248]
    str x23, [x16, #256]
    str x24, [x16, #264]
    str x25, [x16, #272]
    str x26, [x16, #280]
    mrs x9, ttbr0_el1
    str x9, [x16, #288]
    mrs x9, ttbr1_el1
    str x9, [x16, #296]
    str x11, [x16, #328]
    str x14, [x16, #304]
    mrs x9, tcr_el1
    str x9, [x16, #312]
    mrs x9, mair_el1
    str x9, [x16, #320]
    /* The return stub cleared M above, but left C/I on. No C or stack
       access until C-off + complete stackless cache clean/invalidate. */
    mov x19, x16
    mrs x18, csselr_el1
    dsb sy
    mrs x9, sctlr_el1
    bic x9, x9, #4
    msr sctlr_el1, x9
    isb
    dsb sy
    mrs x10, clidr_el1
    ubfx x11, x10, #24, #3
    mov x12, #0
Rlevel:
    cmp x12, x11
    b.hs Rclean_done
    add x13, x12, x12, lsl #1
    lsr x14, x10, x13
    and x14, x14, #7
    cmp x14, #2
    b.lo Rnext_level
    lsl x13, x12, #1
    msr csselr_el1, x13
    isb
    mrs x14, ccsidr_el1
    and x15, x14, #7
    add x15, x15, #4
    ubfx x16, x14, #3, #10
    clz w17, w16
    ubfx x8, x14, #13, #15
Rset:
    ubfx x16, x14, #3, #10
Rway:
    lsl x9, x16, x17
    lsl x7, x8, x15
    orr x9, x9, x7
    orr x9, x9, x13
    dc cisw, x9
    subs x16, x16, #1
    b.ge Rway
    subs x8, x8, #1
    b.ge Rset
    dsb sy
Rnext_level:
    add x12, x12, #1
    b Rlevel
Rclean_done:
    msr csselr_el1, x18
    dsb sy
    mrs x9, sctlr_el1
    bic x9, x9, #1
    bic x9, x9, #4
    bic x9, x9, #0x1000
    msr sctlr_el1, x9
    isb
    ic iallu
    tlbi vmalle1
    dsb sy
    isb
    mov x16, x19
    add x9, x16, #0x4000
    mov sp, x9
    adrp x9, OwnVectors
    add x9, x9, :lo12:OwnVectors
    msr vbar_el1, x9
    isb
    mov x9, #4
    str x9, [x16, #104]
    mov x0, x16
    b CheckpointOrFault

