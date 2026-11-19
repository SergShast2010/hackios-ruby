/* QEMU-only native GIC binding, exit/restore and NtPreemptArm. The one-shot IRQ
 * (the only GIC/timer writer) is native_irq.c under native_irq_policy.h. */
#include <stdint.h>
#include "native_state.h"
typedef uint64_t u64;typedef uint32_t u32;
#define RD(r) ({u64 v;__asm__ volatile("mrs %0," #r:"=r"(v));v;})
#define WR(r,v) do{u64 x=(v);__asm__ volatile("msr " #r ",%0"::"r"(x):"memory");}while(0)
#define SYNC() __asm__ volatile("dsb sy\nisb":::"memory")
#define V64(a) (*(volatile u64*)(u64)(a))
#define V32(a) (*(volatile u32*)(u64)(a))
#define V8(a) (*(volatile unsigned char*)(u64)(a))
#define S ((volatile NativeState*)0xfffffff007e7d800ULL)
#define MARK(n) do{S->preempt_phase=(n);__asm__ volatile("dc cvac,%0\ndsb sy"::"r"(&S->preempt_phase):"memory");}while(0)
extern void NtLowReturn(void) __attribute__((noreturn));
extern void NtPreemptIrq(void),NtIrqEl1t(void);
extern int NtIrqOnce(void);
static u64 physical(u64 v){__asm__ volatile("at s1e1r,%0\nisb"::"r"(v):"memory");u64 p=RD(par_el1);return p&1?~0ULL:(p&0xfffffffff000ULL)|(v&4095);}
void NtExitBody(u64 reason) __attribute__((noreturn));
void NtExitBody(u64 reason){
 __asm__ volatile("msr daifset,#15":::"memory");
 if(S->magic!=0x394e415449564553ULL||S->exit_guard)for(;;)__asm__ volatile("yield");
 S->exit_guard=1;S->reason=reason;S->native_sp=({u64 v;__asm__ volatile("mov %0,sp":"=r"(v));v;});S->thread=RD(tpidr_el1);S->native_sctlr=RD(sctlr_el1);S->native_vbar=RD(vbar_el1);
 if((S->native_sctlr&0x1005)!=0x1005||(S->native_sctlr&0x80000)||(RD(tcr_el1)&0xc03f)!=26||(RD(mair_el1)&255)!=255||S->callback!=V64(0xfffffff00745f7f8ULL)||S->callback>>32||(S->callback&0x1fffff)>0x1ff800)for(;;)__asm__ volatile("yield");
 if((reason==0x39||reason==0x40)&&(!(S->entry_daif&128)||S->entry_tpidr!=S->thread||!S->entry_sp))S->reason=0xe8;
 WR(cntv_ctl_el0,2);WR(icc_igrpen1_el1,0);WR(icc_pmr_el1,0);SYNC();
 /* No GIC register changed in this backend. Validate all inherited fields. */
 if(S->mapped_d&&S->mapped_r){u64 d=S->mapped_d,r=S->mapped_r,b=r+0x10000;
  if(physical(d)!=S->d||physical(r)!=S->r)for(;;)__asm__ volatile("yield");
  if(V32(d)!=S->ctlr||V32(r+0x14)!=S->waker||V32(b+0x80)!=S->group||V32(b+0xd00)!=S->modifier||V8(b+0x41b)!=S->priority||V32(b+0x100)!=S->enable||((V32(b+0x200)^S->pending)&(1u<<27))||((V32(b+0x300)^S->active)&(1u<<27)))S->restore_error|=1;
 }
 if(RD(icc_rpr_el1)!=S->rpr||RD(icc_ap1r0_el1)!=S->ap1r0)S->restore_error|=2;
 WR(icc_bpr1_el1,S->bpr);WR(icc_ctlr_el1,S->iccctl);WR(icc_pmr_el1,S->pmr);WR(icc_igrpen1_el1,S->igrpen);WR(icc_sre_el1,S->sre);SYNC();
 WR(cntkctl_el1,S->cntkctl);WR(cntv_cval_el0,S->cval);WR(cntv_ctl_el0,S->timer_ctl);SYNC();
 if(RD(cntv_cval_el0)!=S->cval||((RD(cntv_ctl_el0)^S->timer_ctl)&3)||RD(cntkctl_el1)!=S->cntkctl||RD(icc_igrpen1_el1)!=S->igrpen||RD(icc_pmr_el1)!=S->pmr||RD(icc_sre_el1)!=S->sre||RD(icc_bpr1_el1)!=S->bpr||RD(icc_ctlr_el1)!=S->iccctl)S->restore_error|=4;
 S->restored=!S->restore_error;SYNC();NtLowReturn();
}
void NtGicBody(void){
 u64 daif=RD(daif);__asm__ volatile("msr daifset,#15":::"memory");
 u64 d=V64(0xfffffff007e8a7a0ULL),r=V64(0xfffffff007e8afb8ULL),size=V64(0xfffffff007e8afc0ULL);
 if(S->magic!=0x394e415449564553ULL||S->profile!=NT_QEMU_PROFILE||!(daif&128)||!d||!r||size!=0x200000||RD(mpidr_el1)&0xff00ffffffULL)NtExitBody(0xe1);
 if(physical(d)!=S->d||physical(r)!=S->r||physical(r+size-1)!=S->r+size-1)NtExitBody(0xe2);
 S->mapped_d=d;S->mapped_r=r;
 if(V32(d)!=S->ctlr||(V64(r+8)>>32)!=0||((V32(d+0xffe8)>>4)&15)!=3||RD(icc_rpr_el1)!=S->rpr||RD(icc_ap1r0_el1)!=0||RD(icc_igrpen1_el1)||RD(icc_pmr_el1)!=S->pmr||!(RD(icc_sre_el1)&1))NtExitBody(0xe3);
 ((void(*)(u64,u32,u64,u64,u64))0xfffffff007463218ULL)(0,0,0,0xfffffff007474a5cULL,0);
 S->gic_ready=1;SYNC();
}

__attribute__((section(".extra"),noinline)) int NtPreemptArm(void){
 const u64 vbar=RD(vbar_el1),t=vbar+0x80,h=vbar+0x280,target_t=(u64)(uintptr_t)NtIrqEl1t,target_h=(u64)(uintptr_t)NtPreemptIrq;
 MARK(1);
 if(S->profile!=NT_QEMU_PROFILE||!S->gic_ready||!S->mapped_d||!S->mapped_r||
    !(RD(daif)&128)||RD(icc_igrpen1_el1)||RD(cntfrq_el0)!=62500000||
    vbar!=UINT64_C(0xfffffff00745a000)||S->preempt_error){S->preempt_error=0x201;return 1;}
 S->vector_patch=V32(t);MARK(2);
 u32 want_t=0x14000000|((u32)((target_t-t)>>2)&0x3ffffff);
 u32 want_h=0x14000000|((u32)((target_h-h)>>2)&0x3ffffff);
 if(S->vector_patch!=want_t||V32(h)!=want_h){S->preempt_error=0x203;return 1;}
 MARK(3);
 return NtIrqOnce();
}
