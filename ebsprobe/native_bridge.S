.section .text.start,"ax"
.global NtGicEntry,NtFirstThread,NtPanic,NtLowReturn,NtExitBody
NtPanic:
 msr daifset,#15
 mrs x1,esr_el1
 mrs x2,far_el1
 mrs x3,elr_el1
 ldr x5,=0xfffffff007e7d800
 str x30,[x5,#528]
 str x1,[x5,#536]
 str x2,[x5,#544]
 str x3,[x5,#552]
 mov x0,#0xef
 b NtExitBody
NtGicEntry:
 bl NtGicBody
 ldr x16,=0xfffffff0074749ec
 br x16
.section .extra,"ax"
 /* EL1t IRQ vector: the interrupted native thread runs on SP_EL0 and SP_EL1 is
  * not owned by this checkpoint, so the frame goes on the interrupted stack. */
.global NtIrqEl1t,NtPreemptIrq
NtIrqEl1t:
  msr spsel,#0
 /* EL1h vector enters here, on the SP that was already in use. */
NtPreemptIrq:
  sub sp,sp,#272
  stp x0,x1,[sp,#0]
  stp x2,x3,[sp,#16]
  stp x4,x5,[sp,#32]
  stp x6,x7,[sp,#48]
  stp x8,x9,[sp,#64]
  stp x10,x11,[sp,#80]
  stp x12,x13,[sp,#96]
  stp x14,x15,[sp,#112]
  stp x16,x17,[sp,#128]
  stp x18,x19,[sp,#144]
  stp x20,x21,[sp,#160]
  stp x22,x23,[sp,#176]
  stp x24,x25,[sp,#192]
  stp x26,x27,[sp,#208]
  stp x28,x29,[sp,#224]
  str x30,[sp,#240]
  mrs x0,elr_el1
  mrs x1,spsr_el1
  stp x0,x1,[sp,#248]
  str xzr,[sp,#264]
  mov x0,sp
  bl NtPreemptBody
  ldp x0,x1,[sp,#248]
  msr elr_el1,x0
  msr spsr_el1,x1
  ldp x2,x3,[sp,#16]
  ldp x4,x5,[sp,#32]
  ldp x6,x7,[sp,#48]
  ldp x8,x9,[sp,#64]
  ldp x10,x11,[sp,#80]
  ldp x12,x13,[sp,#96]
  ldp x14,x15,[sp,#112]
  ldp x16,x17,[sp,#128]
  ldp x18,x19,[sp,#144]
  ldp x20,x21,[sp,#160]
  ldp x22,x23,[sp,#176]
  ldp x24,x25,[sp,#192]
  ldp x26,x27,[sp,#208]
  ldp x28,x29,[sp,#224]
  ldr x30,[sp,#240]
  ldp x0,x1,[sp,#0]
  add sp,sp,#272
  eret
.section .text,"ax"
NtFirstThread:
 ldr x16,=0xfffffff007e7d800
 mov x17,sp
 str x17,[x16,#256]
 mrs x17,tpidr_el1
 str x17,[x16,#264]
 mrs x17,daif
 str x17,[x16,#272]
 str x0,[x16,#296]
 str x30,[x16,#304]
 mrs x17,spsel
 str x17,[x16,#320]
 ldr x17,=0xfffffff007312d74
 str x17,[x16,#328]
 msr daifset,#15
 b NtSwitchBoot
NtLowReturn:
 /* Own native loader_l1/l2 RAM is private; no vendor return routine called.
  * TCR TG0=4KiB, T0SZ=26 checked by Prepare/exit before use. TTBR1 unchanged. */
 ldr x10,=0xfffffff007e7d000
 ldr x11,=0xfffffff007e7c000
 ldr x12,=0xfffffff007e7d800
 ldr x16,[x12,#8]
 lsr x13,x16,#30
 ldr x14,=0x63e7c003
 str x14,[x10,x13,lsl #3]
 ubfx x13,x16,#21,#9
 lsr x14,x16,#21
 lsl x14,x14,#21
 mov x15,#0x601
 orr x14,x14,x15
 str x14,[x11,x13,lsl #3]
 /* Clean the two translation-table cache lines with native M/C still on. */
 dc cvac,x10
 lsr x13,x16,#30
 add x13,x10,x13,lsl #3
 dc cvac,x13
 ubfx x13,x16,#21,#9
 add x13,x11,x13,lsl #3
 dc cvac,x13
 dsb sy
 mrs x14,tcr_el1
 bic x14,x14,#0x80
 msr tcr_el1,x14
 ldr x14,=0x63e7d000
 msr ttbr0_el1,x14
 isb
 tlbi vmalle1
 dsb sy
 isb
 mov x2,#0x40
 ldr x3,[x12,#200]
 br x16
.section .extra,"ax"
.global NtIrqWindow,NtIrqWindowEnd
/* x0 = CNTPCT deadline. The only instructions that may run with PSTATE.I clear.
 * Leaves when the handler counted one IRQ (NativeState.preempt_count, +504) or at the deadline. */
NtIrqWindow:
 msr daifclr,#2
 movz x1,#0xd800
 movk x1,#0x07e7,lsl #16
 movk x1,#0xfff0,lsl #32
 movk x1,#0xffff,lsl #48
1:
 ldr x2,[x1,#504]
 cbnz x2,2f
 mrs x3,cntpct_el0
 cmp x3,x0
 b.lo 1b
2:
 msr daifset,#2
 isb
NtIrqWindowEnd:
 ret
