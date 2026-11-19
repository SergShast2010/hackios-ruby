#ifndef NATIVE_STATE_H
#define NATIVE_STATE_H
#include <stdint.h>
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
} NativeState;
_Static_assert(sizeof(NativeState)==472,"native diagnostic ABI");
_Static_assert(sizeof(NativeState)<=2048,"reserved NativeState half-page bound");
#endif
