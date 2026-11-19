/* First-thread-only platform backend. ORIGINAL native GIC DT/io_map prefix,
 * native handler registration, no GIC writes/ACK at all. Not IRQ-capable init. */
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
extern void NtLowReturn(void) __attribute__((noreturn));
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
 S->restored=1;SYNC();NtLowReturn();
}
void NtGicBody(void){
 u64 daif=RD(daif);__asm__ volatile("msr daifset,#15":::"memory");
 u64 d=V64(0xfffffff007e8a7a0ULL),r=V64(0xfffffff007e8afb8ULL),size=V64(0xfffffff007e8afc0ULL);
 if(S->magic!=0x394e415449564553ULL||!(daif&128)||!d||!r||size!=0x200000||RD(mpidr_el1)&0xff00ffffffULL)NtExitBody(0xe1);
 if(physical(d)!=S->d||physical(r)!=S->r||physical(r+size-1)!=S->r+size-1)NtExitBody(0xe2);
 S->mapped_d=d;S->mapped_r=r;
 if(V32(d)!=S->ctlr||(V64(r+8)>>32)!=0||((V32(d+0xffe8)>>4)&15)!=3||RD(icc_rpr_el1)!=S->rpr||RD(icc_ap1r0_el1)!=0||RD(icc_igrpen1_el1)||RD(icc_pmr_el1)!=S->pmr||!(RD(icc_sre_el1)&1))NtExitBody(0xe3);
 ((void(*)(u64,u32,u64,u64,u64))0xfffffff007463218ULL)(0,0,0,0xfffffff007474a5cULL,0);
 /* OWN diagnostic marker means bindings installed, NOT controller initialized. */
 S->gic_ready=1;SYNC();
}
