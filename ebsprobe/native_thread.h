#ifndef NATIVE_THREAD_H
#define NATIVE_THREAD_H
#include "native_state.h"
void *memset(void *,int,size_t);
#include "native_patches.h"
#include "native_snapshot_policy.h"
NtSnapshotReport nt_snapshot;
/* Only the host fixture writes this before Prepare. Normal phone always zero. */
volatile uint64_t NtTestProfile=0;
#define NTR(r) ({uint64_t v;__asm__ volatile("mrs %0," #r:"=r"(v));v;})
#define NTV32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define NTV64(a) (*(volatile uint64_t *)(uintptr_t)(a))
static NativeState *const nt_state=(void *)(uintptr_t)UINT64_C(0x63e7d800);
static int NtInputGuard(uint64_t frequency){
 if(NTR(mpidr_el1)&UINT64_C(0xff00ffffff))return 0;
 if(NtTestProfile==UINT64_C(0x51454d5546495854))return frequency==62500000;
 if(NtTestProfile||frequency!=13000000)return 0;
 RubyIrqHardware h;
 return RubyIrqFdt((void *)(uintptr_t)UINT64_C(0x47c80000),0x80000,&h)&&h.dt_frequency==frequency;
}
static int NtPatches(void){
 for(unsigned i=0;i<sizeof(nt_patches)/sizeof(nt_patches[0]);i++){const NtPatch*p=&nt_patches[i];const volatile unsigned char*a=(void*)(uintptr_t)p->pa;for(uint64_t j=0;j<p->n;j++)if(a[j]!=p->old[j])return 0;}
 for(unsigned i=0;i<sizeof(nt_patches)/sizeof(nt_patches[0]);i++){const NtPatch*p=&nt_patches[i];volatile unsigned char*a=(void*)(uintptr_t)p->pa;for(uint64_t j=0;j<p->n;j++)a[j]=p->newbytes[j];for(uint64_t j=0;j<p->n;j++)if(a[j]!=p->newbytes[j])return 0;}return 1;
}
static int NtSnapshot(void){
 uint64_t d=NtTestProfile?0x08000000:0x0c000000,r=NtTestProfile?0x080a0000:0x0c040000,b=r+0x10000;
 NtSnapshotReport*v=&nt_snapshot;memset(v,0,sizeof(*v));
 v->frequency=NTR(cntfrq_el0);v->mpidr=NTR(mpidr_el1);v->daif=NTR(daif);
 v->input=NtInputGuard(v->frequency);
 if(!v->input){v->failed=1;return 0;}
 v->sre=NTR(icc_sre_el1);
 if(!(v->sre&1)){v->failed=32;return 0;}
 v->rpr=NTR(icc_rpr_el1);v->igrpen=NTR(icc_igrpen1_el1);v->pmr=NTR(icc_pmr_el1);
 v->ctlr=NTV32(d);v->waker=NTV32(r+0x14);v->typer=NTV64(r+8);v->pidr=NTV32(d+0xffe8);
 v->enable=NTV32(b+0x100);v->pending=NTV32(b+0x200);v->active=NTV32(b+0x300);v->config=NTV32(b+0xc04);
 v->ap1r0=NTR(icc_ap1r0_el1);v->timer=NTR(cntv_ctl_el0);
 v->failed=NtSnapshotPolicy(v);if(v->failed)return 0;
 uint64_t ctlr=v->ctlr,w=v->waker;
 NativeState*s=nt_state;memset(s,0,sizeof(*s));s->callback=(uint64_t)(uintptr_t)XnuReturn;s->profile=NtTestProfile;s->d=d;s->r=r;s->ctlr=ctlr;s->waker=w;s->group=NTV32(b+0x80);s->modifier=NTV32(b+0xd00);s->priority=*(volatile unsigned char*)(uintptr_t)(b+0x41b);s->enable=NTV32(b+0x100);s->pending=NTV32(b+0x200);s->active=NTV32(b+0x300);
 s->rpr=v->rpr;s->ap1r0=v->ap1r0;
 /* Recheck the guarded baseline; never acknowledge/drop inherited IRQs. */
 if(NTR(icc_rpr_el1)!=s->rpr||NTR(icc_ap1r0_el1)!=s->ap1r0||NTR(icc_pmr_el1)!=v->pmr){v->failed=UINT64_C(1)<<16;return 0;}
 s->sre=NTR(icc_sre_el1);s->igrpen=NTR(icc_igrpen1_el1);s->pmr=NTR(icc_pmr_el1);s->bpr=NTR(icc_bpr1_el1);s->iccctl=NTR(icc_ctlr_el1);s->timer_ctl=NTR(cntv_ctl_el0);s->cval=NTR(cntv_cval_el0);s->cntkctl=NTR(cntkctl_el1);s->magic=UINT64_C(0x394e415449564553);__asm__ volatile("dsb sy":::"memory");return 1;
}
#endif
