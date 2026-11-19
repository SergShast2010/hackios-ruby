#ifndef NATIVE_STATE_H
#define NATIVE_STATE_H
#include <stdint.h>
#include <stddef.h>
#define NT_QEMU_PROFILE UINT64_C(0x51454d5546495854)
typedef struct {
 uint64_t magic,callback,profile,d,r,ctlr,waker,group,modifier,priority,enable,pending,active;
 uint64_t sre,igrpen,pmr,bpr,iccctl,timer_ctl,cval,cntkctl;
 uint64_t mapped_d,mapped_r,gic_touched,gic_ready,reason,restored,restore_error;
 uint64_t thread,native_sp,native_sctlr,native_vbar;
 uint64_t entry_sp,entry_tpidr,entry_daif,exit_guard,stage,entry_x0,entry_lr,ap1r0,entry_spsel,entry_pc;
 uint64_t rpr;
 uint64_t switch_magic,boot_before,boot_sp_before,processor,idle_thread;
 uint64_t second,second_sp,second_daif,second_wait_result,boot_after,boot_sp_after;
 uint64_t switch_pass,phase,second_sctlr,second_vbar,boot_daif_after;
 /* Appended fields: existing assembly offsets above remain unchanged. */
 uint64_t vector_patch,preempt_iar,preempt_elr,preempt_spsr;
 uint64_t preempt_count,preempt_phase,preempt_error;
 /* NtPanic is a branch target, not an exception vector. These can be stale. */
 uint64_t panic_lr,last_esr,last_far,last_elr;
 /* v0.41a one-shot IRQ contract (native_irq_policy.h). */
 uint64_t irq_policy,irq_mod,irq_timer_ctl,irq_timer_cval;
 /* Stage 2: scheduler round-trip marker, written by IRQ/second native thread. */
 uint64_t preempt_sched;
} NativeState;
_Static_assert(offsetof(NativeState,panic_lr)==528 && offsetof(NativeState,last_esr)==536 &&
               offsetof(NativeState,last_far)==544 && offsetof(NativeState,last_elr)==552,
               "native panic ABI");
_Static_assert(offsetof(NativeState,preempt_count)==504,"NtIrqWindow polls preempt_count at +504");
_Static_assert(sizeof(NativeState)==600,"native diagnostic ABI");
_Static_assert(sizeof(NativeState)<=2048,"reserved NativeState half-page bound");
#endif
