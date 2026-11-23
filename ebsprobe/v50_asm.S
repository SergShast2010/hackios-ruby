/* v0.53 AArch64 EL1 physical transition, SError-safe RAM probe, XNU handoff and return stub.
 * Identity-mapped UEFI entry -> clean+invalidate caches -> MMU off -> probe RAM -> XNU -> return with log.
 */
.data
.p2align 3
.globl v50_saved_ctx
v50_saved_ctx:
    .quad 0
v50_probe_active:
    .quad 0
v50_probe_hit:
    .quad 0
v50_probe_esr:
    .quad 0

.text
.p2align 4
.globl V50EnterPhysical
/* void V50EnterPhysical(struct v50_ctx *ctx, uint64_t stack_top) */
V50EnterPhysical:
    msr daifset, #15
    mov x19, x0
    msr spsel, #1
    mov sp, x1

    /* Save ctx pointer in RAM and clean its cache line before disabling D-cache. */
    adrp x9, v50_saved_ctx
    add  x9, x9, :lo12:v50_saved_ctx
    str  x19, [x9]
    dc   cvac, x9
    dsb  sy

    adrp x9, V50OwnVectors
    add  x9, x9, :lo12:V50OwnVectors
    msr  vbar_el1, x9
    isb

    /* Clean+invalidate all D/unified caches by set/way when FEAT_CCIDX == 0. */
    mrs  x18, csselr_el1
    dsb  sy
    mrs  x9, sctlr_el1
    bic  x9, x9, #4                 /* clear C */
    msr  sctlr_el1, x9
    isb
    dsb  sy

    mrs  x6, id_aa64mmfr2_el1
    ubfx x6, x6, #20, #4            /* CCIDX (0 or 1) */

    mrs  x10, clidr_el1
    ubfx x11, x10, #24, #3          /* LoC */
    mov  x12, #0
.Lv50_level:
    cmp  x12, x11
    b.hs .Lv50_cache_done
    add  x13, x12, x12, lsl #1
    lsr  x14, x10, x13
    and  x14, x14, #7
    cmp  x14, #2
    b.lo .Lv50_next_level
    lsl  x13, x12, #1
    msr  csselr_el1, x13
    isb
    mrs  x14, ccsidr_el1
    and  x15, x14, #7
    add  x15, x15, #4               /* log2(line bytes) */
    cbnz x6, .Lv50_ccidx1
    ubfx x5, x14, #3, #10           /* CCIDX=0: max way */
    ubfx x8, x14, #13, #15          /* CCIDX=0: max set */
    b    .Lv50_ccidx_ok
.Lv50_ccidx1:
    ubfx x5, x14, #3, #21           /* CCIDX=1: max way */
    ubfx x8, x14, #32, #24          /* CCIDX=1: max set */
.Lv50_ccidx_ok:
    clz  w17, w5
.Lv50_set:
    mov  x16, x5
.Lv50_way:
    lsl  x9, x16, x17
    lsl  x7, x8, x15
    orr  x9, x9, x7
    orr  x9, x9, x13
    dc   cisw, x9
    subs x16, x16, #1
    b.ge .Lv50_way
    subs x8, x8, #1
    b.ge .Lv50_set
    dsb  sy
.Lv50_next_level:
    add  x12, x12, #1
    b    .Lv50_level

.Lv50_cache_done:
    msr  csselr_el1, x18
    dsb  sy
    mrs  x9, sctlr_el1
    bic  x9, x9, #1                 /* M = 0 */
    bic  x9, x9, #4                 /* C = 0 */
    bic  x9, x9, #0x1000            /* I = 0 */
    msr  sctlr_el1, x9
    isb
    ic   iallu
    tlbi vmalle1
    dsb  sy
    isb

    mov  x0, x19
    b    V50BootPhysical

.p2align 4
.globl V50ProbeRam64
/* uint64_t V50ProbeRam64(uint64_t pa, uint64_t *esr_out)
 * Must be called with MMU OFF under V50OwnVectors. Tests 8-byte read + write-back
 * and catches any synchronous External Abort or asynchronous SError cleanly.
 * Returns 0 if clean, 1 if faulted (and stores ESR_EL1 in *esr_out if non-NULL).
 */
V50ProbeRam64:
    msr  daifset, #15
    adrp x2, v50_probe_active
    add  x2, x2, :lo12:v50_probe_active
    str  xzr, [x2, #8]              /* v50_probe_hit = 0 */
    str  xzr, [x2, #16]             /* v50_probe_esr = 0 */
    mov  x3, #1
    str  x3, [x2]                   /* v50_probe_active = 1 */
    dsb  sy
    isb

    ldr  x9, [x0]
    dsb  sy
    str  x9, [x0]
    dsb  sy
    isb

    /* Briefly unmask SError (DAIF.A = bit 2 of daifclr immediate #4). */
    msr  daifclr, #4
    isb
    nop
    nop
    nop
    nop
    dsb  sy
    msr  daifset, #4
    isb

    str  xzr, [x2]                  /* v50_probe_active = 0 */
    dsb  sy
    ldr  x0, [x2, #8]               /* return v50_probe_hit */
    cbz  x1, 1f
    ldr  x9, [x2, #16]
    str  x9, [x1]
1:
    ret

.p2align 4
.globl V50JumpXnu
/* void V50JumpXnu(uint64_t entry_pa, uint64_t boot_args_pa) */
V50JumpXnu:
    msr  daifset, #15
    dsb  sy
    ic   iallu
    tlbi vmalle1
    dsb  sy
    isb
    mov  x9, x0
    mov  x0, x1
    mov  x1, xzr
    mov  x2, xzr
    mov  x3, xzr
    mov  x30, xzr
    br   x9

/* Return stub invoked by XNU loader_jump() (with MMU ON, 2 MiB block mapped around this stub)
 * or by start.s early exception handler (MMU OFF).
 * ABI: x0=log_pa, x1=log_len, x2=reason, x3=code/ESR, x4=ELR, x5=FAR.
 * First 8 instructions (32 bytes) touch NO memory and turn MMU/caches off within one 64B cache line.
 */
.p2align 6
.globl V50XnuReturn
V50XnuReturn:
    msr  daifset, #15
    mrs  x14, sctlr_el1
    bic  x14, x14, #1
    bic  x14, x14, #4
    bic  x14, x14, #0x1000
    msr  sctlr_el1, x14
    dsb  sy
    isb

    /* Now running physically; reload ctx and our physical stack. */
    adrp x16, v50_saved_ctx
    add  x16, x16, :lo12:v50_saved_ctx
    ldr  x16, [x16]
    msr  spsel, #1
    ldr  x15, [x16, #0]             /* ctx->stack_top */
    mov  sp, x15
    adrp x15, V50OwnVectors
    add  x15, x15, :lo12:V50OwnVectors
    msr  vbar_el1, x15
    isb
    mov  x6, x16
    b    V50OnXnuReturn

.p2align 11
.globl V50OwnVectors
V50OwnVectors:
.macro V50SLOT n
    mov  x17, #\n
    b    V50OwnException
    .space 120
.endm
V50SLOT 0
V50SLOT 1
V50SLOT 2
V50SLOT 3
V50SLOT 4
V50SLOT 5
V50SLOT 6
V50SLOT 7
V50SLOT 8
V50SLOT 9
V50SLOT 10
V50SLOT 11
V50SLOT 12
V50SLOT 13
V50SLOT 14
V50SLOT 15

V50OwnException:
    /* Fast path for V50ProbeRam64: record fault and return cleanly with SError masked. */
    adrp x10, v50_probe_active
    add  x10, x10, :lo12:v50_probe_active
    ldr  x11, [x10]
    cbz  x11, .Lv50_real_exc

    mov  x11, #1
    str  x11, [x10, #8]             /* v50_probe_hit = 1 */
    mrs  x11, esr_el1
    str  x11, [x10, #16]            /* v50_probe_esr = esr_el1 */
    /* If synchronous (vector 0 or 4), skip the faulting ldr/str instruction. */
    and  x12, x17, #3
    cbnz x12, .Lv50_probe_serror
    mrs  x11, elr_el1
    add  x11, x11, #4
    msr  elr_el1, x11
.Lv50_probe_serror:
    /* Keep A (SError) masked on return from exception. */
    mrs  x11, spsr_el1
    orr  x11, x11, #0x100
    msr  spsr_el1, x11
    dsb  sy
    isb
    eret

.Lv50_real_exc:
    msr  daifset, #15
    mrs  x14, sctlr_el1
    bic  x14, x14, #1
    bic  x14, x14, #4
    bic  x14, x14, #0x1000
    msr  sctlr_el1, x14
    dsb  sy
    isb
    adrp x16, v50_saved_ctx
    add  x16, x16, :lo12:v50_saved_ctx
    ldr  x16, [x16]
    msr  spsel, #1
    ldr  x15, [x16, #0]
    mov  sp, x15
    mov  x0, x17
    mrs  x1, esr_el1
    mrs  x2, elr_el1
    mrs  x3, far_el1
    mov  x4, x16
    b    V50OnException

.p2align 4
.globl V50PsciCall
/* int64_t V50PsciCall(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3, uint32_t use_hvc) */
V50PsciCall:
    stp  x18, x30, [sp, #-16]!
    cbnz w4, 1f
    smc  #0
    b    2f
1:
    hvc  #0
2:
    ldp  x18, x30, [sp], #16
    ret
