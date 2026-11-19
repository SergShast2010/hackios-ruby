.section .text.start,"ax"
.global NtGicEntry,NtFirstThread,NtPanic,NtLowReturn,NtExitBody
NtPanic:
 msr daifset,#15
 mov x0,#0xef
 b NtExitBody
NtGicEntry:
 bl NtGicBody
 ldr x16,=0xfffffff0074749ec
 br x16
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
