/* v0.41a: one virtual-timer PPI 27 into an own EL1 handler, QEMU profile only.
 * Contract and allowed writes: native_irq_policy.h. Placement (caves are small):
 * handler in .extra, restore/poll in .irq (_hv_wdt_pet body), arm in .irqarm (_PE_init_iokit body). */
#include <stdint.h>
#include "native_state.h"
#include "native_irq_policy.h"
typedef uint64_t u64;typedef uint32_t u32;
#define RD(r) ({u64 v;__asm__ volatile("mrs %0," #r:"=r"(v));v;})
#define WR(r,v) do{u64 x=(v);__asm__ volatile("msr " #r ",%0"::"r"(x):"memory");}while(0)
#define SYNC() __asm__ volatile("dsb sy\nisb":::"memory")
#define V32(a) (*(volatile u32*)(u64)(a))
#define V8(a) (*(volatile unsigned char*)(u64)(a))
#define S ((volatile NativeState*)0xfffffff007e7d800ULL)
#define MARK(n) do{S->preempt_phase=(n);__asm__ volatile("dc cvac,%0\ndsb sy"::"r"(&S->preempt_phase):"memory");}while(0)
#define IRQ __attribute__((section(".irq"),noinline))
#define IRQ_HANDLER __attribute__((section(".extra"),noinline))
#define IRQ_ARM __attribute__((section(".irqarm"),noinline))
#define OWN(bit) (S->irq_mod|=(bit))
#define FAIL(code) do{S->preempt_error=(code);goto out;}while(0)
typedef struct {u64 x[33];} NtIrqFrame;
extern void NtIrqWindow(u64 deadline);

IRQ static int NtWait32(u64 a,u32 mask,u32 want){
 for(u32 i=0;i<NT_IRQ_POLL_LIMIT;i++)if((V32(a)&mask)==want)return 1;
 return 0;
}
/* Called once from the EL1 IRQ frame. Returns with the interrupted context's IRQ masked. */
IRQ_HANDLER void NtPreemptBody(NtIrqFrame *f){
 u64 iar=RD(icc_iar1_el1);
 WR(cntv_ctl_el0,2);SYNC(); /* deassert the owned level source before completion */
 S->preempt_iar=iar;S->preempt_elr=f->x[31];S->preempt_spsr=f->x[32];
 if((iar&0xffffff)<1020){WR(icc_eoir1_el1,iar);SYNC();}
 if(iar==NT_IRQ_ID&&!S->preempt_count){S->preempt_count=1;MARK(5);}
 else S->preempt_error=NT_IRQ_E_UNEXPECTED;
 f->x[32]|=128;
}
IRQ static void NtIrqRestore(u64 d,u64 r){
 u64 b=r+NT_SGI_FRAME,m=S->irq_mod;int bad=0;
 WR(cntv_ctl_el0,2);SYNC();
 if(m&NT_IRQ_OWN_ENABLE){V32(b+NT_SGI_ICENABLER0)=NT_IRQ_BIT;SYNC();bad|=!NtWait32(r+NT_GICR_CTLR,8,0);}
 if(V32(b+NT_SGI_ISPENDR0)&NT_IRQ_BIT){V32(b+NT_SGI_ICPENDR0)=NT_IRQ_BIT;SYNC();}
 if(m&NT_IRQ_OWN_CPU){WR(icc_igrpen1_el1,S->igrpen);WR(icc_pmr_el1,S->pmr);SYNC();}
 if(m&NT_IRQ_OWN_DGATE){V32(d+NT_GICD_CTLR)=(u32)S->ctlr;SYNC();bad|=!NtWait32(d+NT_GICD_CTLR,0x80000000u,0);}
 if(m&NT_IRQ_OWN_PRIORITY)V8(b+NT_SGI_IPRIORITY27)=(unsigned char)S->priority;
 if(m&NT_IRQ_OWN_GROUP)V32(b+NT_SGI_IGROUPR0)=(u32)S->group;
 SYNC();
 if(m&NT_IRQ_OWN_WAKER){V32(r+NT_GICR_WAKER)=(u32)S->waker;SYNC();bad|=!NtWait32(r+NT_GICR_WAKER,4,(u32)S->waker&4);}
 if(bad||V32(d)!=S->ctlr||V32(r+NT_GICR_WAKER)!=S->waker||V32(b+NT_SGI_IGROUPR0)!=S->group||
    V8(b+NT_SGI_IPRIORITY27)!=S->priority||RD(icc_rpr_el1)!=S->rpr||
    ((V32(b+NT_SGI_ISENABLER0)|V32(b+NT_SGI_ISPENDR0)|V32(b+NT_SGI_ISACTIVER0))&NT_IRQ_BIT))S->restore_error|=8;
 /* Hand the timer back last, after PPI 27 is disabled again. */
 if(m&NT_IRQ_OWN_TIMER){WR(cntv_cval_el0,S->irq_timer_cval);WR(cntv_ctl_el0,S->irq_timer_ctl&3);SYNC();
  if(RD(cntv_cval_el0)!=S->irq_timer_cval||(RD(cntv_ctl_el0)&3)!=(S->irq_timer_ctl&3))S->restore_error|=16;}
}
/* 0: prepared (one IRQ handled, or refused by policy with nothing written). */
IRQ_ARM int NtIrqOnce(void){
 u64 d=S->mapped_d,r=S->mapped_r,b=r+NT_SGI_FRAME;
 NtIrqView v={.qemu=S->profile==NT_QEMU_PROFILE,.daif=RD(daif),.rpr=RD(icc_rpr_el1),.ap1r0=RD(icc_ap1r0_el1),
  .igrpen=RD(icc_igrpen1_el1),.iccctl=RD(icc_ctlr_el1),.timer=RD(cntv_ctl_el0),.frequency=RD(cntfrq_el0),
  .ctlr=V32(d+NT_GICD_CTLR),.waker=V32(r+NT_GICR_WAKER),.modifier=V32(b+NT_SGI_IGRPMODR0),
  .enable=V32(b+NT_SGI_ISENABLER0),.pending=V32(b+NT_SGI_ISPENDR0),.active=V32(b+NT_SGI_ISACTIVER0),
  .config=V32(b+NT_SGI_ICFGR1)};
 if((S->irq_policy=NtIrqPolicy(&v))){S->preempt_error=NT_IRQ_E_REFUSED;return 0;}
 S->gic_touched=1;
 S->irq_timer_ctl=v.timer;S->irq_timer_cval=RD(cntv_cval_el0);
 OWN(NT_IRQ_OWN_TIMER);WR(cntv_ctl_el0,2);SYNC();
 if(!NtWait32(b+NT_SGI_ISPENDR0,NT_IRQ_BIT,0))FAIL(NT_IRQ_E_PENDING);
 if(v.waker&2){OWN(NT_IRQ_OWN_WAKER);V32(r+NT_GICR_WAKER)=v.waker&~2u;SYNC();
  if(!NtWait32(r+NT_GICR_WAKER,4,0))FAIL(NT_IRQ_E_WAKER);}
 if(!(V32(b+NT_SGI_IGROUPR0)&NT_IRQ_BIT)){OWN(NT_IRQ_OWN_GROUP);V32(b+NT_SGI_IGROUPR0)=V32(b+NT_SGI_IGROUPR0)|NT_IRQ_BIT;}
 OWN(NT_IRQ_OWN_PRIORITY);V8(b+NT_SGI_IPRIORITY27)=NT_IRQ_PRIORITY;SYNC();
 if(!(V32(b+NT_SGI_IGROUPR0)&NT_IRQ_BIT)||V8(b+NT_SGI_IPRIORITY27)!=NT_IRQ_PRIORITY)FAIL(NT_IRQ_E_READBACK);
 OWN(NT_IRQ_OWN_DGATE);V32(d+NT_GICD_CTLR)=v.ctlr|2u;SYNC();
 if(!NtWait32(d+NT_GICD_CTLR,0x80000000u,0)||!(V32(d+NT_GICD_CTLR)&2))FAIL(NT_IRQ_E_DGATE);
 OWN(NT_IRQ_OWN_CPU);WR(icc_pmr_el1,0xff);WR(icc_igrpen1_el1,1);SYNC();
 if(RD(icc_igrpen1_el1)!=1)FAIL(NT_IRQ_E_CPU); /* PMR=0xff is checked by the IRQ arriving */
 OWN(NT_IRQ_OWN_ENABLE);V32(b+NT_SGI_ISENABLER0)=NT_IRQ_BIT;SYNC();
 if(!NtWait32(r+NT_GICR_CTLR,8,0)||!(V32(b+NT_SGI_ISENABLER0)&NT_IRQ_BIT))FAIL(NT_IRQ_E_ENABLE);
 MARK(4);
 WR(cntv_cval_el0,RD(cntvct_el0)+v.frequency/1000);WR(cntv_ctl_el0,1);SYNC();
 NtIrqWindow(RD(cntpct_el0)+v.frequency/5);
 if(!S->preempt_error&&S->preempt_count!=1)S->preempt_error=NT_IRQ_E_TIMEOUT;
 if(!S->preempt_error)MARK(6);
out:
 NtIrqRestore(d,r);
 if(S->preempt_error)return 1;
 MARK(7);return 0;
}
