/* Bounded diagnostic after the independently fenced legacy loader_jump entry.
 * All threads/queues/stacks/context switches belong to native XNU.
 * No EFI scheduler, no IRQ enabling, no direct thread-field writes.
 */
.section .switch,"ax"
.global NtSwitchFence,NtSwitchBoot,NtSwitchSecond,NtSwitchResumed
NtSwitchFence:
 b .Lfailure
NtSwitchBoot:
 ldr x19,=0xfffffff007e7d800
 mov x8,#0x40
 str x8,[x19,#344]
 mrs x8,tpidr_el1
 mov x9,sp
 stp x8,x9,[x19,#352]
 mov x8,#1
 str x8,[x19,#440]
 bl XnuCurrentProcessor
 str x0,[x19,#368]
 bl XnuIdleThreadCreate
 cbnz w0,.Lfailure
 ldr x8,[x19,#368]
 ldr x8,[x8,#0x18]
 cbz x8,.Lfailure
 str x8,[x19,#376]
 mov x8,#2
 str x8,[x19,#440]
 bl XnuSchedStartup
NtSwitchResumed:
 mrs x8,tpidr_el1
 mov x9,sp
 stp x8,x9,[x19,#416]
 mrs x10,daif
 str x10,[x19,#464]
 tbz x10,#7,.Lfailure
 ldp x10,x11,[x19,#352]
 cmp x8,x10
 b.ne .Lfailure
 cmp x9,x11
 b.ne .Lfailure
 ldr x12,[x19,#384]
 cbz x12,.Lfailure
 cmp x12,x8
 b.eq .Lfailure
 ldr x12,[x19,#392]
 cbz x12,.Lfailure
 cmp x12,x9
 b.eq .Lfailure
 ldr x12,[x19,#440]
 cmp x12,#4
 b.ne .Lfailure
 mov x8,#5
 str x8,[x19,#440]
 mov x8,#1
 str x8,[x19,#432]
 mov x0,#0x40
 b NtExitBody
NtSwitchSecond:
 ldr x19,=0xfffffff007e7d800
 mrs x8,tpidr_el1
 mov x9,sp
 stp x8,x9,[x19,#384]
 mrs x10,daif
 str x10,[x19,#400]
 tbz x10,#7,.Lfailure
 ldr x11,[x19,#352]
 cmp x8,x11
 b.eq .Lfailure
 ldr x11,[x19,#360]
 cmp x9,x11
 b.eq .Lfailure
 mrs x10,sctlr_el1
 mrs x11,vbar_el1
 stp x10,x11,[x19,#448]
 mov x8,#3
 str x8,[x19,#440]
 add x0,x19,#344
 mov w1,#0
 bl XnuAssertWait
 str x0,[x19,#408]
 cmn w0,#1
 b.ne .Lfailure
 mov x8,#4
 str x8,[x19,#440]
 mov x0,#0
 mov x1,#0
 mov w2,#0
 bl XnuThreadBlockReason
 b .Lfailure
.Lfailure:
 b NtPanic
