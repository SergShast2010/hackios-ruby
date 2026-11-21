#ifndef NATIVE_SNAPSHOT_POLICY_H
#define NATIVE_SNAPSHOT_POLICY_H
#include <stdint.h>
/* Quiet first-thread diagnostic ONLY. RPR=0 is NOT classified as idle.
 * No IAR/EOI/DIR access, IRQ unmask, or Group1 enable is permitted on this path.
 * Preserve the observed baseline, including a hidden secure running priority.
 * A later interrupt-capable scheduler must use a different ownership contract. */
typedef struct {
 uint64_t failed,input,frequency,mpidr,daif,rpr,igrpen,pmr,sre;
 uint64_t ctlr,waker,typer,pidr,enable,pending,active,config,ap1r0,timer;
} NtSnapshotReport;
static uint64_t NtSnapshotPolicy(const NtSnapshotReport *v) {
 uint64_t f=0,bit=UINT64_C(1)<<27;
 if(!v->input)f|=1;
 if(!(v->daif&128))f|=2;
 if(v->rpr!=0&&v->rpr!=255)f|=4;
 if(v->igrpen)f|=8;
 /* PMR is an inherited 8-bit threshold, not the IRQ enable gate.
  * DAIF.I and both Group1 gates remain closed on this quiet path. */
 if(v->pmr & ~UINT64_C(0xff))f|=16;
 if(!(v->sre&1))f|=32;
 if(v->ctlr!=0x10&&v->ctlr!=0x50)f|=64;
 if((v->waker&6)!=0&&(v->waker&6)!=6)f|=128;
 if(v->typer>>32)f|=256;
 if(((v->pidr>>4)&15)!=3)f|=512;
 if(v->enable&bit)f|=1024;
 if(v->pending&bit)f|=2048;
 if(v->active&bit)f|=4096;
 if((v->config>>22)&3)f|=8192;
 if(v->ap1r0)f|=16384;
 if((v->timer&3)==1)f|=32768;
 return f;
}
#endif
