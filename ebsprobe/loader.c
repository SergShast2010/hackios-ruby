/* ruby v0.38: real XNU checkpoint18, then bounded own-handler GIC/timer IRQ diagnostic. */
#include "efi.h"
#include "RubyRamPolicy.h"
#include "Font.h"
#include "IrqFdt.h"
#define RED 0xffff6060U
#define IRQ_PROBE_IMPLEMENTATION 1
#include "irq_probe.h"
static struct irq_report own_irq_report;
static RubyIrqHardware irq_hardware;
static int irq_fdt_ok;
/* Public symbol also allows component testing; normal firmware has no API
 * selecting QEMU bases. This gate validates the real LK tree before GIC MMIO. */
__attribute__((noinline)) int RubyRunIrq(void){
 irq_fdt_ok=RubyIrqFdt((const unsigned char *)(uintptr_t)UINT64_C(0x47c80000),UINT64_C(0x80000),&irq_hardware);
 if(!irq_fdt_ok)return -100;
 uint64_t hz;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(hz));
 if(irq_hardware.dt_frequency!=hz)return -101;
 return IrqProbeRun(&own_irq_report,IRQ_PROBE_RUBY_D,IRQ_PROBE_RUBY_R,IRQ_PROBE_RUBY_R_BYTES);
}
#define ERR(n) (EFI_ERROR_BIT|(n))
#define FB UINT64_C(0x7c406000)
#define BASE UINT64_C(0x62000000)
#define BYTES UINT64_C(0x10000)
#define MAP_CAP UINT64_C(0x8000)
#define GREEN UINT32_C(0xff70ff90)
#define WHITE UINT32_C(0xffffffff)
#define YELLOW UINT32_C(0xffffff60)
static const uint64_t font[]=GLYPH_FONT;
static volatile uint32_t *screen;
static volatile unsigned physical;
struct context { uint64_t magic,base,stack,ebs,map_bytes,desc_size,map_key,attempts,frequency,counter;
 uint64_t sctlr_before,sctlr_after,identity_pages,stage,vbar,brk_ok,esr,elr,far,vector;
 uint64_t entry,bootargs,checkpoint,original_word,saved_x0,saved_x20;
 uint64_t reason,code,kernel_vbar,sp0,sp1,x22,x23,x24,x25,x26,ttbr0,ttbr1;
 uint64_t return_sctlr,tcr,mair,return_x11; };
_Static_assert(offsetof(struct context,sctlr_before)==80,"assembly context ABI");
_Static_assert(offsetof(struct context,brk_ok)==120,"assembly BRK ABI");
_Static_assert(offsetof(struct context,vector)==152,"assembly vector ABI");
_Static_assert(offsetof(struct context,reason)==208,"native reason ABI");
_Static_assert(offsetof(struct context,sp0)==232,"native stack ABI");
_Static_assert(offsetof(struct context,x22)==248,"native register ABI");
_Static_assert(offsetof(struct context,ttbr1)==296,"native TTBR ABI");
_Static_assert(offsetof(struct context,return_sctlr)==304,"MMU return ABI");
_Static_assert(sizeof(struct context)==336,"context size");
extern unsigned char __ImageBase[];
extern unsigned char OwnVectors[],OwnVectorsEnd[];
extern void EnterPhysical(struct context *,uint64_t) __attribute__((noreturn));
extern void ProbeException(void);
extern void XnuReturn(void);
extern void JumpXnu(struct context *) __attribute__((noreturn));
#include "xnu_prepare.h"
__attribute__((noinline,noreturn)) void NativeTerminalDone(void){for(;;)__asm__ volatile("yield");}
#include "VmCheck.h"
#include "RamContract.h"
static int IdentityRange(uint64_t base,uint64_t bytes,uint64_t *count,int writable) {
 if(!bytes||base>UINT64_MAX-bytes)return 0;
 uint64_t end=base+bytes;
 for(uint64_t a=base&~UINT64_C(4095);a<end;a+=4096){
  uint64_t par; __asm__ volatile("at s1e1r,%1\nisb\nmrs %0,par_el1":"=r"(par):"r"(a):"memory");
  if((par&1)||(par&UINT64_C(0x0000fffffffff000))!=a)return 0;
  if(writable){__asm__ volatile("at s1e1w,%1\nisb\nmrs %0,par_el1":"=r"(par):"r"(a):"memory");
  if((par&1)||(par&UINT64_C(0x0000fffffffff000))!=a)return 0;
  }
  (*count)++;
 }
 return 1;
}
static uint32_t read32(const unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t OwnImageBytes(void){
 const unsigned char *p=__ImageBase;
 if(p[0]!='M'||p[1]!='Z')return 0;
 uint32_t off=read32(p+0x3c);
 if(off<64||off>1024||read32(p+off)!=0x4550)return 0;
 uint64_t n=read32(p+off+24+56);
 return n>=4096&&n<=0x40000?n:0;
}
static efi_guid gop_guid={0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
_Static_assert(offsetof(efi_boot_services,exit_boot_services)==232,"EBS ABI");
_Static_assert(sizeof(efi_memory_descriptor)==40,"map ABI");
_Static_assert(sizeof(struct context)<0x1000,"context bound");
void *memset(void *p,int c,size_t n) { unsigned char *q=p;while(n--)*q++=(unsigned char)c;return p; }
void *memcpy(void *dst,const void *src,size_t n){unsigned char *d=dst;const unsigned char *s=src;while(n--)*d++=*s++;return dst;}
static void flush(uint64_t a,uint64_t n) {
 uint64_t ctr;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));
 uint64_t line=UINT64_C(4)<<((ctr>>16)&15);
 for(uint64_t p=a&~(line-1);p<a+n;p+=line)__asm__ volatile("dc cvac,%0"::"r"(p):"memory");
 __asm__ volatile("dsb sy":::"memory");
}
__attribute__((noinline)) void RawLine(unsigned row,const char *s,uint32_t color) {
 if(!screen||row>=40)return;
 unsigned y0=120+row*54;
 for(unsigned y=y0;y<y0+54;y++)for(unsigned x=0;x<1080;x++)screen[y*1080+x]=0xff000000;
 for(unsigned c=0;s[c]&&c<43;c++) {
  unsigned ch=(unsigned char)s[c];if(ch<32||ch>=127)continue;uint64_t g=font[ch-32];
  for(unsigned gy=0;gy<12;gy++) {
   uint32_t bits=(gy<6)?(uint32_t)(g>>32):(uint32_t)g;bits>>=(gy%6)*5;
   for(unsigned gx=0;gx<5;gx++)if(bits&(1U<<gx))for(unsigned sy=0;sy<4;sy++)for(unsigned sx=0;sx<4;sx++)
    screen[(y0+gy*4+sy)*1080+24+c*24+gx*4+sx]=color;
  }
 }
 if(!physical)flush(FB+(uint64_t)y0*4320,54*4320);
 else __asm__ volatile("dsb sy":::"memory");
}
static void hexline(unsigned row,const char *label,uint64_t value,uint32_t color) {
 char s[44];unsigned n=0;while(label[n]&&n<24){s[n]=label[n];n++;}
 for(int sh=60;sh>=0&&n<43;sh-=4)s[n++]="0123456789ABCDEF"[(value>>sh)&15];s[n]=0;RawLine(row,s,color);
}
__attribute__((noinline,noreturn)) void RawStop(struct context *c) {
 __asm__ volatile("msr daifset,#15":::"memory");
 RawLine(24,"EXIT STOPPED - photograph this screen",YELLOW);
 hexline(25,"EFI status=",c->ebs,YELLOW);hexline(26,"Attempts=",c->attempts,WHITE);
 RawLine(28,"No return to UEFI after first EBS call",WHITE);
 for(;;)__asm__ volatile("yield");
}
__attribute__((noinline,noreturn)) void RawFault(struct context *c) {
 RawLine(24,"CPU EXCEPTION - photograph this screen",YELLOW);
 hexline(25,"ESR=",c->esr,YELLOW);hexline(26,"ELR=",c->elr,WHITE);
 hexline(27,"FAR=",c->far,WHITE);hexline(28,"Vector=",c->vector,WHITE);
 for(;;)__asm__ volatile("yield");
}
__attribute__((noinline,noreturn)) void NativeSnapshotStopped(void){
 __asm__ volatile("nop":::"memory");for(;;)__asm__ volatile("yield");
}
__attribute__((noinline,noreturn)) void PostPhysical(struct context *c) {
 physical=1;
 uint64_t sp;__asm__ volatile("mov %0,sp":"=r"(sp));
 RawLine(13,"ExitBootServices: Success",GREEN);
 RawLine(14,"Physical EL1 / own vectors installed",GREEN);
 hexline(16,"New stack SP=",sp,WHITE);
 hexline(17,"SCTLR before=",c->sctlr_before,WHITE);
 hexline(18,"SCTLR after=",c->sctlr_after,WHITE);
 hexline(19,"Identity pages=",c->identity_pages,WHITE);
 if(c->sctlr_after&UINT64_C(0x1005)){
  RawLine(24,"CPU STOPPED: M/C/I not clear",YELLOW);for(;;)__asm__ volatile("yield");
 }
 RawLine(20,"MMU / data / instruction cache: OFF",GREEN);
 ProbeException();
 if(!c->brk_ok){RawLine(24,"CPU STOPPED: no own exception",YELLOW);for(;;)__asm__ volatile("yield");}
 RawLine(21,"Own BRK exception + return: Success",GREEN);
 volatile uint64_t *probe=(void *)(uintptr_t)(BASE+0x9000);
 if(probe[0]!=UINT64_C(0x123456789abcdef0)||probe[1]!=~UINT64_C(0x123456789abcdef0)){
  RawLine(24,"CPU STOPPED: RAM pattern changed",YELLOW);for(;;)__asm__ volatile("yield");
 }
 RawLine(23,"RAM pattern after cache-off: Success",GREEN);
 RawLine(22,"Storage absent; native checkpoint 11",WHITE);
 RawLine(24,"Checking inherited IRQ snapshot...",YELLOW);
 hexline(25,"XNU entry PA=",c->entry,WHITE);
 hexline(26,"boot_args PA=",c->bootargs,WHITE);
 __asm__ volatile("dsb sy\nisb":::"memory");
 if(!NtSnapshot()){
  for(unsigned row=4;row<40;row++)RawLine(row,"",WHITE);
   RawLine(0,"ruby v0.41 - QEMU snapshot",YELLOW);
  RawLine(4,"NATIVE SNAPSHOT REJECTED - send photo",RED);
  hexline(5,"Failure mask=",nt_snapshot.failed,WHITE);
  hexline(6,"Input guard=",nt_snapshot.input,WHITE);
  hexline(7,"CNTFRQ=",nt_snapshot.frequency,WHITE);
  hexline(8,"MPIDR=",nt_snapshot.mpidr,WHITE);
  hexline(9,"DAIF=",nt_snapshot.daif,WHITE);
  hexline(10,"ICC RPR=",nt_snapshot.rpr,WHITE);
  hexline(11,"ICC IGRPEN1=",nt_snapshot.igrpen,WHITE);
  hexline(12,"ICC PMR=",nt_snapshot.pmr,WHITE);
  hexline(13,"ICC SRE=",nt_snapshot.sre,WHITE);
  hexline(14,"GICD CTLR=",nt_snapshot.ctlr,WHITE);
  hexline(15,"GICR WAKER=",nt_snapshot.waker,WHITE);
  hexline(16,"GICR TYPER=",nt_snapshot.typer,WHITE);
  hexline(17,"GICD PIDR2=",nt_snapshot.pidr,WHITE);
  hexline(18,"PPI enable=",nt_snapshot.enable,WHITE);
  hexline(19,"PPI pending=",nt_snapshot.pending,WHITE);
  hexline(20,"PPI active=",nt_snapshot.active,WHITE);
  hexline(21,"PPI ICFGR1=",nt_snapshot.config,WHITE);
  hexline(22,"ICC AP1R0=",nt_snapshot.ap1r0,WHITE);
  hexline(23,"CNTV CTL=",nt_snapshot.timer,WHITE);
  RawLine(26,"No XNU entry; no IRQ ACK or GIC writes",WHITE);
  NativeSnapshotStopped();
 }
 RawLine(24,"Calling real XNU entry now...",YELLOW);
 flush(UINT64_C(0x63e7d000),4096);
 c->stage=3;JumpXnu(c);
}
/* Validate map lengths/types without allocation; preserve its exact map key. */
static int map_valid(const unsigned char *map,uint64_t n,uint64_t ds,uint32_t dv,int require_free) {
 int fb=0;
 if(!n||n>MAP_CAP||ds<sizeof(efi_memory_descriptor)||ds>4096||n%ds||dv!=1)return 0;
 for(uint64_t i=0;i<n;i+=ds){
  const efi_memory_descriptor *d=(const void *)(map+i);uint64_t b=d->phys_start,z=d->pages;
  if(!z||(b&4095)||z>(UINT64_MAX>>12))return 0;z<<=12;if(b>UINT64_MAX-z)return 0;
  if(b<=FB&&b+z>=FB+1080ULL*2400*4&&d->type!=EfiConventionalMemory)fb=1;
 }
 return fb&&RamOwnedMap(map,n,ds,RUBY_XNU_BASE,RUBY_XNU_BYTES)&&
  RamOwnedMap(map,n,ds,BASE,BYTES)&&
  RamArgsOwned((const RubyBootArgs *)(uintptr_t)UINT64_C(0x642f4000))&&
  RamOutside(BASE,BYTES,RUBY_XNU_BASE,RUBY_XNU_BYTES)&&
  RamOutside((uint64_t)(uintptr_t)__ImageBase,OwnImageBytes(),RUBY_XNU_BASE,RUBY_XNU_BYTES)&&
  RamOutside(UINT64_C(0x66000000),UINT64_C(0x01000000),RUBY_XNU_BASE,RUBY_XNU_BYTES);
}
__attribute__((noinline)) efi_status efi_main(efi_handle image,efi_system_table *st) {
 efi_gop *gop=0;efi_boot_services *bs;efi_status s;uint64_t el;
 if(NtTestProfile!=NT_QEMU_PROFILE)return ERR(26);
 screen=0;physical=0;if(!st||!st->bs)return ERR(2);bs=st->bs;
 s=bs->locate_protocol(&gop_guid,0,(void **)&gop);
 if(EFI_ERROR(s)||!gop||!gop->mode||!gop->mode->info)return ERR(14);
 efi_gop_mode *m=gop->mode;efi_gop_mode_info *info=m->info;
 if(m->fb_base!=FB||m->fb_size<1080ULL*2400*4||info->width!=1080||info->height!=2400||info->pixels_per_scanline!=1080)return ERR(26);
 screen=(volatile uint32_t *)(uintptr_t)FB;
 RawLine(0,"ruby v0.41 - QEMU only",GREEN);
 RawLine(1,"No UFS / USB / disk writes",YELLOW);
 __asm__ volatile("mrs %0,CurrentEL":"=r"(el));if(el!=4){RawLine(4,"Requires current EL1",YELLOW);return ERR(3);}
 struct ruby_ram_policy_report policy;
 s=RubyValidateRamFdt((const unsigned char *)(uintptr_t)RUBY_LIVE_FDT_BASE,RUBY_LIVE_FDT_LIMIT,&policy);
 if(s){RawLine(4,"Live LK RAM guard denied",YELLOW);return ERR(26);}
 RawLine(4,"EL1 and live LK RAM guard: Success",GREEN);
 uint64_t addr=BASE; s=bs->allocate_pages(AllocateAddress,EfiLoaderData,BYTES>>12,&addr);
 if(EFI_ERROR(s)){RawLine(5,"Claim scratch RAM: failed",YELLOW);return s;}
 if(addr!=BASE){bs->free_pages(addr,BYTES>>12);return ERR(26);}
 struct context *c=(void *)(uintptr_t)(BASE+0x8000);memset(c,0,sizeof(*c));
 c->magic=UINT64_C(0x45425350524f4245);c->base=BASE;c->stack=BASE+BYTES;c->ebs=ERR(19);
 __asm__ volatile("mrs %0,cntfrq_el0":"=r"(c->frequency));
 if(!c->frequency||c->frequency>UINT64_C(10000000000)){bs->free_pages(BASE,BYTES>>12);return ERR(26);}
 uint64_t ccidx,sctlr;__asm__ volatile("mrs %0,id_aa64mmfr2_el1":"=r"(ccidx));
 __asm__ volatile("mrs %0,sctlr_el1":"=r"(sctlr));
 uint64_t image_bytes=OwnImageBytes();
 if(((ccidx>>20)&15)||!(sctlr&1)||(sctlr&(UINT64_C(3)<<24))||!image_bytes||
   ((uint64_t)(uintptr_t)OwnVectors&2047)||(uint64_t)(OwnVectorsEnd-OwnVectors)!=2048||
   !IdentityRange((uint64_t)(uintptr_t)__ImageBase,image_bytes,&c->identity_pages,0)||
   !IdentityRange(BASE,BYTES,&c->identity_pages,1)||
   !IdentityRange(FB,1080ULL*2400*4,&c->identity_pages,1)){
  RawLine(6,"CPU/identity guard denied; no EBS",YELLOW);bs->free_pages(BASE,BYTES>>12);return ERR(26);
 }
 volatile uint64_t *probe=(void *)(uintptr_t)(BASE+0x9000);
 probe[0]=UINT64_C(0x123456789abcdef0);probe[1]=~UINT64_C(0x123456789abcdef0);
 RawLine(5,"Own stack and identity pages: checked",GREEN);
 s=PrepareXnu(image,bs,c);
 if(EFI_ERROR(s)){hexline(24,"XNU prepare status=",s,YELLOW);bs->free_pages(BASE,BYTES>>12);return s;}
 RawLine(7,"Next: EBS, MMU off, real XNU entry",YELLOW);
 RawLine(8,"Final screen should stay; no reboot",WHITE);
 if(!IdentityRange(RUBY_STAGE_DEST_BASE,RUBY_STAGE_DEST_MAX,&c->identity_pages,1)){
  RawLine(24,"Staged RAM identity rejected; no EBS",YELLOW);bs->free_pages(RUBY_STAGE_DEST_BASE,RUBY_STAGE_DEST_MAX>>12);bs->free_pages(BASE,BYTES>>12);return ERR(26);
 }
 RawLine(10,"Preparing final memory map...",WHITE);
 uint64_t bytes=MAP_CAP,key=0,ds=0;uint32_t dv=0;
 s=bs->get_memory_map(&bytes,(void *)(uintptr_t)BASE,&key,&ds,&dv);
 if(EFI_ERROR(s)||!map_valid((const void *)(uintptr_t)BASE,bytes,ds,dv,0)){
  RawLine(12,"Final memory map rejected; no EBS",YELLOW);bs->free_pages(BASE,BYTES>>12);return EFI_ERROR(s)?s:ERR(26);
 }
 c->map_bytes=bytes;c->desc_size=ds;c->map_key=key;
 RawLine(11,"Calling ExitBootServices...",YELLOW);
 /* Raw output above makes no UEFI calls or allocations. */
 for(unsigned attempt=0;attempt<2;attempt++) {
  c->attempts=attempt+1;
  s=bs->exit_boot_services(image,key);c->ebs=s;
  if(s==0){__asm__ volatile("mrs %0,cntpct_el0":"=r"(c->counter));EnterPhysical(c,c->stack);}
  if(s!=ERR(2)||attempt!=0)RawStop(c);
  /* After first EBS attempt only GetMemoryMap and ExitBootServices are allowed. */
  bytes=MAP_CAP;ds=0;dv=0;s=bs->get_memory_map(&bytes,(void *)(uintptr_t)BASE,&key,&ds,&dv);
  if(EFI_ERROR(s)){c->ebs=s;RawStop(c);}
  if(!map_valid((const void *)(uintptr_t)BASE,bytes,ds,dv,0)){c->ebs=ERR(26);RawStop(c);}
  c->map_bytes=bytes;c->desc_size=ds;c->map_key=key;
 }
 RawStop(c);
}

/* Readback oracle covers all 2048 bootstrap PTE words, including unused zeroes.
 * XNU has only constructed them; hardware TTBR/TCR and MMU are not installed. */
static int TablesMatch(void){
 const volatile uint64_t *t=(const void *)(uintptr_t)UINT64_C(0x631b6000);
 for(unsigned i=0;i<2048;i++){
  uint64_t want=0;
  if(i==1)want=UINT64_C(0x631b8003);
  if(i==512+192)want=UINT64_C(0x631b9003);
  if(i>=1024+280&&i<1024+280+15)want=UINT64_C(0x63000000)+(uint64_t)(i-1024-280)*UINT64_C(0x200000)+UINT64_C(0x601);
  if(i>=1536+56&&i<1536+56+15)want=UINT64_C(0x63000000)+(uint64_t)(i-1536-56)*UINT64_C(0x200000)+UINT64_C(0x601);
  if(t[i]!=want)return 0;
 }
 return 1;
}
__attribute__((noinline,noreturn)) void CheckpointOrFault(struct context *c) {
 physical=1;
 {
 volatile uint32_t *out=(void *)(uintptr_t)screen;for(uint64_t i=0;i<UINT64_C(1080)*2400;i++)out[i]=UINT32_C(0xff000000);
 NativeState*s=nt_state;uint64_t sc=NTR(sctlr_el1),vb=NTR(vbar_el1);
  int ok=s->magic==UINT64_C(0x394e415449564553)&&s->reason==0x40&&s->switch_pass&&s->phase==6&&s->preempt_count==1&&s->preempt_iar==27&&s->preempt_phase==7&&s->preempt_sched==3&&!s->preempt_error&&!s->irq_policy&&s->boot_after==s->boot_before&&s->second&&s->second!=s->boot_before&&s->gic_ready&&s->restored&&!s->restore_error&&!(sc&0x1005)&&vb==(uint64_t)(uintptr_t)OwnVectors;
  RawLine(0,"ruby v0.41 - QEMU only",ok?GREEN:RED);
  RawLine(4,ok?"ONE TIMER IRQ: PASS":s->irq_policy?"ONE TIMER IRQ: REFUSED":"ONE TIMER IRQ: FAILED",ok?GREEN:RED);
 hexline(6,"Native reason=",s->reason,WHITE);hexline(7,"Kernel thread=",s->thread,WHITE);hexline(8,"Native stack=",s->native_sp,WHITE);hexline(9,"GIC bound=",s->gic_ready,WHITE);hexline(10,"Hardware restored=",s->restored,WHITE);hexline(11,"Restore error=",s->restore_error,WHITE);hexline(12,"Return SCTLR=",sc,WHITE);hexline(13,"Kernel SCTLR=",s->native_sctlr,WHITE);
 hexline(14,"Inherited RPR=",s->rpr,WHITE);hexline(15,"Inherited PMR=",s->pmr,WHITE);
 hexline(17,"Preempt IRQ IAR=",s->preempt_iar,WHITE);hexline(18,"Preempt ELR=",s->preempt_elr,WHITE);hexline(19,"Preempt count=",s->preempt_count,WHITE);hexline(20,"Preempt phase=",s->preempt_phase,WHITE);hexline(21,"Preempt error=",s->preempt_error,WHITE);hexline(22,"IRQ policy=",s->irq_policy,WHITE);hexline(23,"Scheduler proof=",s->preempt_sched,WHITE);
 hexline(23,"Boot before=",s->boot_before,WHITE);hexline(24,"Second thread=",s->second,WHITE);hexline(25,"Second stack=",s->second_sp,WHITE);hexline(26,"Boot resumed=",s->boot_after,WHITE);hexline(27,"Switch pass=",s->switch_pass,WHITE);hexline(28,"Phase=",s->phase,WHITE);
  RawLine(30,"One PPI 27 handled; no scheduler, no preemption",WHITE);
  RawLine(32,"Do not flash this image",WHITE);
  if(!ok&&s->reason==0xef){
   hexline(34,"Panic LR=",s->panic_lr,WHITE);hexline(35,"Last ESR (may be stale)=",s->last_esr,WHITE);
   hexline(36,"Last ELR (may be stale)=",s->last_elr,WHITE);
  }
 NativeTerminalDone();
 }
 uint64_t sctlr,vbar,el;__asm__ volatile("mrs %0,sctlr_el1":"=r"(sctlr));__asm__ volatile("mrs %0,vbar_el1":"=r"(vbar));__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
 if(c->stage!=4)RawFault(c);
 int args_match=RhSame((const void *)(uintptr_t)UINT64_C(0x631b4530),(const void *)(uintptr_t)c->bootargs,736);
 int contract=RamArgsOwned((const RubyBootArgs *)(uintptr_t)c->bootargs)&&
  RamArgsOwned((const RubyBootArgs *)(uintptr_t)UINT64_C(0x631b4530));
 int dt_match=0;
 { uint64_t mpidr;__asm__ volatile("mrs %0,mpidr_el1":"=r"(mpidr));
   RubyHandoffInput in={c->frequency,mpidr,FB,1080,2400,1080,0};
   unsigned char expected[RUBY_HOFF_DT_CAP/32];uint64_t dt_n=0;
   const RubyBootArgs *a=(const void *)(uintptr_t)c->bootargs;
   dt_match=!RhMakeDt(expected,sizeof(expected),&in,&dt_n)&&dt_n==a->dt_bytes&&VmConsumedSeed(expected,dt_n)&&
     RhSame(expected,(const void *)(uintptr_t)UINT64_C(0x642e4000),dt_n);
 }
 const volatile unsigned char *cpu=(const void *)(uintptr_t)UINT64_C(0x63b42720);
 const volatile unsigned char *pe=(const void *)(uintptr_t)UINT64_C(0x63252860);
 const RubyBootArgs *ba=(const void *)(uintptr_t)c->bootargs;
 int cpu_match=*(const volatile uint32_t *)(cpu+0x48)==UINT32_C(0x80000000)&&
  *(const volatile uint64_t *)(cpu+0x108)==UINT64_MAX&&
  *(const volatile uint32_t *)(cpu+0x1d4)==0&&
  *(const volatile uint32_t *)(cpu+0x288)==0;
 int pe_match=*(const volatile uint32_t *)pe==1&&
  *(const volatile uint64_t *)(pe+0xa0)==UINT64_C(0xfffffff0071b4530)&&
  *(const volatile uint64_t *)(pe+0x98)==ba->dt&&
  *(const volatile uint64_t *)(pe+0xa8)==ba->dt_bytes&&
  *(const volatile uint64_t *)(pe+8)==ba->video.base&&
  *(const volatile uint32_t *)(uintptr_t)UINT64_C(0x6325284c)==1;
 int device_bases_zero=*(const volatile uint64_t *)(uintptr_t)UINT64_C(0x63e8a7a0)==0&&
  *(const volatile uint64_t *)(uintptr_t)UINT64_C(0x63f230c8)==0&&
  *(const volatile uint64_t *)(uintptr_t)UINT64_C(0x63f230f0)==0;
 uint64_t cntkctl,tpidr,tpidrro,mpidr;
 __asm__ volatile("mrs %0,cntkctl_el1":"=r"(cntkctl));
 __asm__ volatile("mrs %0,tpidr_el0":"=r"(tpidr));
 __asm__ volatile("mrs %0,tpidrro_el0":"=r"(tpidrro));
 __asm__ volatile("mrs %0,mpidr_el1":"=r"(mpidr));
 int topology=CtTopology((const void *)(uintptr_t)UINT64_C(0x631ca5a0),
  (const void *)(uintptr_t)UINT64_C(0x631ca198),(const void *)(uintptr_t)UINT64_C(0x631ca558))&&
  !(mpidr&UINT64_C(0xff00ffffff))&&!tpidr&&!tpidrro;
 uint32_t boot_cpu_number=Ct32((const void *)(uintptr_t)UINT64_C(0x631ac764));
  int boot_cpu_ok=boot_cpu_number==0;
  unsigned event_shift=CtEventShift(c->frequency);
 int event_ok=event_shift<16&&Ct32((const void *)(uintptr_t)UINT64_C(0x631ca5f8))==event_shift&&
  Ct64((const void *)(uintptr_t)UINT64_C(0x63d535b8))==c->frequency&&
  cntkctl==(cntkctl_before|((uint64_t)event_shift<<4)|UINT64_C(15));
 const volatile unsigned char *ram=(const void *)(uintptr_t)VM_RAM_BASE;
 uint64_t root=VmWord(ram,UINT64_C(0x631b43f0));
 uint64_t avail=VmWord(ram,UINT64_C(0x631b5130));
 VmSummary vm={0};uint64_t text_pa=0,text_desc=0;
 int vm_ok=VmSpan(root,4096)&&root>=ba->top&&!(root&4095)&&
   VmScan(ram,root,&vm)&&VmTranslate(ram,root,UINT64_C(0xfffffff007453994),&text_pa,&text_desc)&&
   text_pa==UINT64_C(0x63453994)&&(text_desc&128)&&!(text_desc&(UINT64_C(1)<<53))&&
   VmWord(ram,UINT64_C(0x631b5038))==1&&avail>=ba->top&&avail<VM_RAM_END&&!(avail&4095)&&
   VmWord(ram,UINT64_C(0x631ca130))==avail&&VmWord(ram,UINT64_C(0x631ca000))==VM_RAM_END&&
   VmWord(ram,UINT64_C(0x631b5100))==VM_RAM_BASE&&VmWord(ram,UINT64_C(0x631b5120))==RUBY_HOFF_MEM&&
   VmWord(ram,UINT64_C(0x63b94880))==RUBY_HOFF_MEM&&VmWord(ram,UINT64_C(0x63b94888))==RUBY_HOFF_MEM;
 uint64_t timer_now;__asm__ volatile("mrs %0,cntpct_el0":"=r"(timer_now));
  int timer_live=timer_now>c->counter;
  int continuation_ok=*(const volatile uint32_t *)(uintptr_t)UINT64_C(0x63453e28)==UINT32_C(0xd0ffea88);
  int ok=c->reason==2&&c->code==18&&vm_ok&&args_match&&contract&&dt_match&&cpu_match&&pe_match&&device_bases_zero&&topology&&boot_cpu_ok&&event_ok&&timer_live&&continuation_ok&&
  !(sctlr&UINT64_C(0x1005))&&vbar==(uint64_t)(uintptr_t)OwnVectors&&el==4&&
  c->kernel_vbar==UINT64_C(0xfffffff00745a000)&&
  c->ttbr0==UINT64_C(0x63e7d000)&&c->ttbr1==root&&
  c->tcr==UINT64_C(0x22a51a251a)&&c->mair==UINT64_C(0x0c08044f00bb44ff)&&
  (c->return_sctlr&UINT64_C(0x1005))==UINT64_C(0x1005)&&TablesMatch();
 for(unsigned row=4;row<32;row++)RawLine(row,"",WHITE);
 if(!ok){
  RawLine(4,"XNU C INIT STOPPED - send photo",YELLOW);
  hexline(6,"Reason=",c->reason,WHITE);hexline(7,"Code=",c->code,WHITE);
  hexline(8,"XNU VBAR=",c->kernel_vbar,WHITE);hexline(9,"TTBR0=",c->ttbr0,WHITE);hexline(10,"TTBR1=",c->ttbr1,WHITE);
  hexline(11,"TCR=",c->tcr,WHITE);hexline(12,"MAIR=",c->mair,WHITE);hexline(13,"Return SCTLR=",c->return_sctlr,WHITE);
  hexline(14,"x20=",c->saved_x20,WHITE);hexline(15,"x22=",c->x22,WHITE);hexline(16,"x23=",c->x23,WHITE);
  if(c->reason==2){hexline(17,"VM check=",vm_ok,WHITE);hexline(18,"DT check=",dt_match,WHITE);hexline(19,"VM walker error=",vm.error,WHITE);}
  else{hexline(17,"ESR=",c->esr,WHITE);hexline(18,"ELR=",c->elr,WHITE);hexline(19,"FAR=",c->far,WHITE);}
  hexline(20,"CPU data match=",cpu_match,WHITE);hexline(21,"PE_state match=",pe_match,WHITE);
  hexline(22,"PE bootArgs=",*(const volatile uint64_t *)(pe+0xa0),WHITE);
  for(;;)__asm__ volatile("yield");
 }
 RawLine(0,"ruby v0.38b - GIC gate + timer IRQ",GREEN);
 RawLine(4,"XNU NATIVE CHECKPOINT 18 REACHED",GREEN);
 RawLine(5,"CPU + processor bootstrap: Success",GREEN);
 RawLine(6,"Owned RAM contract: Success",GREEN);
 RawLine(8,"arm_vm_init + pmap_bootstrap: Success",GREEN);
 RawLine(9,"Permanent kernel mappings: Success",GREEN);
 RawLine(10,"Kernel boot_args + consumed DT: Success",GREEN);
 RawLine(11,"Kernel text read-only/executable: Success",GREEN);
 RawLine(12,"All mapped PA inside owned RAM",GREEN);
 RawLine(13,"FAT / firmware / scratch excluded",GREEN);
 RawLine(14,"One CPU; no scheduler running",GREEN);
 hexline(15,"Kernel TTBR1 PA=",root,WHITE);
 hexline(16,"Available RAM starts=",avail,WHITE);
 hexline(17,"VM table pages=",vm.tables,WHITE);
 hexline(18,"VM leaf mappings=",vm.leaves,WHITE);
 hexline(19,"Architectural CNTFRQ=",c->frequency,WHITE);
 RawLine(21,"Post-VM native continuation: Success",WHITE);
 RawLine(22,"Architectural timer still advancing: Success",WHITE);
 RawLine(23,"GIC Group 1 gate + owned timer IRQ",WHITE);
 int irq_status=RubyRunIrq();
 RawLine(23,irq_fdt_ok?"Live FDT GIC/timer: Success":"Live FDT GIC/timer: REJECTED",irq_fdt_ok?GREEN:RED);
 int irq_ok=irq_status==0&&own_irq_report.stage==IRQ_STAGE_DONE&&own_irq_report.irq_count==3&&own_irq_report.restore_error==0;
 RawLine(24,irq_ok?"GIC TIMER IRQ CHECKPOINT DONE":"GIC/TIMER PROBE STOPPED",irq_ok?GREEN:RED);
 hexline(25,"Timer IRQ count=",own_irq_report.irq_count,WHITE);
 hexline(26,"Probe stage=",own_irq_report.stage,WHITE);
 hexline(27,"Failure stage=",own_irq_report.failure_stage,WHITE);
  hexline(28,"Probe status=",(uint64_t)(int64_t)irq_status,WHITE);
 hexline(30,"GICD CTLR before=",own_irq_report.d_ctlr,WHITE);
 hexline(31,"GICD CTLR after=",own_irq_report.d_ctlr_after,WHITE);
  hexline(32,"ICC SRE=",own_irq_report.sre,WHITE);
 hexline(33,"ICC IGRPEN1=",own_irq_report.igrpen1,WHITE);
 hexline(34,"ICC PMR=",own_irq_report.pmr_before,WHITE);
 hexline(35,"GICR WAKER=",own_irq_report.r_waker,WHITE);
 hexline(36,"ICC RPR=",own_irq_report.rpr,WHITE);
  hexline(37,"Restoration error=",own_irq_report.restore_error,WHITE);
  hexline(38,"Probe ESR=",own_irq_report.esr,WHITE);
 RawLine(40,"Own IRQ handler; NOT XNU scheduler",WHITE);
 RawLine(41,"No UFS / storage / device drivers",WHITE);
 uint64_t start,now;__asm__ volatile("mrs %0,cntpct_el0":"=r"(start));uint64_t last=UINT64_MAX;
 for(;;){__asm__ volatile("mrs %0,cntpct_el0":"=r"(now));uint64_t n=(now-start)/c->frequency;if(n!=last){RawLine(28,(n&1)?"Heartbeat: 1":"Heartbeat: 0",GREEN);last=n;}__asm__ volatile("yield");}
}
