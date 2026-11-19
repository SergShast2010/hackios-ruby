#ifndef RUBY_HANDOFF_H
#define RUBY_HANDOFF_H
/* v25: build/verify-only candidate boot_args + minimal Apple-format DT.
 * Never executes a payload or MMIO, exits firmware, or claims kernel readiness.
 * Sizes/offsets match XNU xnu-8796.121.2 pexpert/pexpert/arm64/boot.h.
 * deviceTreeP is a kernel VA, as used directly in ARM PE_init_platform.
 * Native callers may use a host pointer with an explicit physical base.
 * The destination must already be exclusively owned EfiLoaderData RAM.
 * On structural/layout rejection no destination bytes change; a readback error
 * can leave metadata written. Only [dt_pa,top_pa) is written, not the kernel.
 */
#include <stdint.h>
#include <stddef.h>
#define RUBY_HOFF_PHYS UINT64_C(0x63000000)
#define RUBY_HOFF_VIRT UINT64_C(0xfffffff007000000)
#define RUBY_HOFF_MEM UINT64_C(0x02000000)
#define RUBY_HOFF_DT_CAP UINT64_C(0x10000)
#define RUBY_HOFF_PAGE UINT64_C(0x4000)
typedef struct { uint64_t base,display,row_bytes,width,height,depth; } RubyBootVideo;
typedef struct {
 uint16_t revision,version;uint64_t virt_base,phys_base,mem_size,top;
 RubyBootVideo video;uint32_t machine_type;uint64_t dt;uint32_t dt_bytes;
 char command[608];uint64_t flags,mem_actual;
} RubyBootArgs;
_Static_assert(sizeof(RubyBootVideo)==48,"XNU video ABI");
_Static_assert(sizeof(RubyBootArgs)==736,"XNU boot_args ABI");
_Static_assert(offsetof(RubyBootArgs,virt_base)==8,"XNU virtBase offset");
_Static_assert(offsetof(RubyBootArgs,video)==40,"XNU video offset");
_Static_assert(offsetof(RubyBootArgs,dt)==96,"XNU deviceTreeP offset");
_Static_assert(offsetof(RubyBootArgs,command)==108,"XNU command offset");
_Static_assert(offsetof(RubyBootArgs,flags)==720,"XNU flags offset");
_Static_assert(offsetof(RubyBootArgs,mem_actual)==728,"XNU memActual offset");
typedef struct { uint64_t frequency,mpidr,fb;uint32_t width,height,stride,reserved; } RubyHandoffInput;
typedef struct { uint64_t dt_pa,ba_pa,top_pa,dt_bytes,ba_bytes; } RubyHandoffLayout;
typedef struct { unsigned char *p;uint64_t cap,n;uint32_t nodes[8];unsigned depth;int error; } RubyAdtWriter;
static inline int RhSame(const volatile void *a,const volatile void *b,uint64_t n){const volatile unsigned char *x=a,*y=b;for(uint64_t i=0;i<n;i++)if(x[i]!=y[i])return 0;return 1;}
static inline void RhZero(void *p,uint64_t n){volatile unsigned char *q=p;while(n--)*q++=0;}
static inline void RhCopy(void *a,const void *b,uint64_t n){volatile unsigned char *x=a;const unsigned char *y=b;while(n--)*x++=*y++;}
static inline uint32_t RhRead32(const volatile unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline void RhWrite32(unsigned char *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static inline unsigned RhLength(const volatile char *s,unsigned max){unsigned n=0;while(n<max&&s[n])n++;return n;}
static inline int RhReserve(RubyAdtWriter *w,uint64_t n){if(w->error||n>w->cap-w->n){w->error=1;return 0;}return 1;}
static inline void RhProp(RubyAdtWriter *w,const char *name,const void *data,uint32_t n){
 unsigned z=RhLength(name,32);uint64_t pad=((uint64_t)n+3)&~UINT64_C(3);
 if(!w->depth||!z||z==32||n>4096||!data||!RhReserve(w,36+pad)){w->error=1;return;}
 unsigned char *p=w->p+w->n;RhZero(p,36+pad);RhCopy(p,name,z);RhWrite32(p+32,n);RhCopy(p+36,data,n);w->n+=36+pad;
 p=w->p+w->nodes[w->depth-1];RhWrite32(p,RhRead32(p)+1);
}
static inline void RhString(RubyAdtWriter *w,const char *k,const char *v){unsigned n=RhLength(v,128);if(n==128){w->error=1;return;}RhProp(w,k,v,n+1);}
static inline void RhU32(RubyAdtWriter *w,const char *k,uint32_t v){RhProp(w,k,&v,4);}
static inline void RhU64(RubyAdtWriter *w,const char *k,uint64_t v){RhProp(w,k,&v,8);}
static inline void RhBegin(RubyAdtWriter *w,const char *name){
 if(w->depth==8||!RhReserve(w,8)){w->error=1;return;}
 if(w->depth){unsigned char *p=w->p+w->nodes[w->depth-1]+4;RhWrite32(p,RhRead32(p)+1);}
 w->nodes[w->depth++]=(uint32_t)w->n;RhZero(w->p+w->n,8);w->n+=8;RhString(w,"name",name);
}
static inline void RhEnd(RubyAdtWriter *w){if(!w->depth){w->error=1;return;}w->depth--;}
/* Canonical candidate DT: no iPhone identity, unknown CPU clock, GIC, UART,
 * interrupt/timer driver nodes, random-seed or invented security state.
 * Only the boot CPU is described; PSCI is a method string, never invoked.
 */
static inline int RhMakeDt(unsigned char *p,uint64_t cap,const RubyHandoffInput *in,uint64_t *n){
 RubyAdtWriter w={0};w.p=p;w.cap=cap;
 RhBegin(&w,"device-tree");RhString(&w,"model","ruby-mt6877");RhString(&w,"compatible","mediatek,mt6877");RhU32(&w,"#address-cells",2);RhU32(&w,"#size-cells",2);
 RhBegin(&w,"chosen");RhU64(&w,"dram-base",RUBY_HOFF_PHYS);RhU64(&w,"dram-size",RUBY_HOFF_MEM);RhU32(&w,"debug-enabled",1);RhString(&w,"bootargs","-v debug=0x100 cpus=1");
 #include "TestBootSeed.h"
 RhProp(&w,"random-seed",RubyTestBootSeed,sizeof(RubyTestBootSeed));
 RhBegin(&w,"memory-map");RhEnd(&w);RhEnd(&w);
 RhBegin(&w,"memory");RhString(&w,"device_type","memory");uint64_t reg[2]={RUBY_HOFF_PHYS,RUBY_HOFF_MEM};RhProp(&w,"reg",reg,sizeof(reg));RhEnd(&w);
 RhBegin(&w,"cpus");RhU32(&w,"#address-cells",2);RhU32(&w,"#size-cells",0);
 RhBegin(&w,"cpu0");RhString(&w,"device_type","cpu");RhString(&w,"state","running");RhU32(&w,"cpu-id",0);RhU64(&w,"reg",in->mpidr&UINT64_C(0xff00ffffff));RhU64(&w,"timebase-frequency",in->frequency);RhEnd(&w);RhEnd(&w);
 RhBegin(&w,"defaults");RhEnd(&w);
 RhBegin(&w,"options");RhEnd(&w);
 A71IrqDt nc={NtTestProfile?0x08000000:0x0c000000,0x10000,NtTestProfile?0x080a0000:0x0c040000,0x200000,in->frequency};size_t added=0;
 if(A71MakeIrqDt(w.p+w.n,(size_t)(w.cap-w.n),&nc,&added))return 5;
 w.n+=added;RhWrite32(w.p+w.nodes[0]+4,RhRead32(w.p+w.nodes[0]+4)+1);RhEnd(&w);
 if(w.error||w.depth||!w.n)return 5;*n=w.n;return 0;
}
/* Independent structural reader: bounded depth/count, exact final extent,
 * nonempty property names, correct value padding and exactly one node name.
 */
static inline int RhReadNode(const volatile unsigned char *p,uint64_t n,uint64_t *at,unsigned depth){
 if(depth>8||*at>n||n-*at<8)return 6;uint32_t np=RhRead32(p+*at),nc=RhRead32(p+*at+4);*at+=8;
 if(!np||np>64||nc>16)return 6;unsigned names=0;
 for(uint32_t i=0;i<np;i++){
  if(*at>n||n-*at<36)return 6;const volatile unsigned char *q=p+*at;unsigned z=0;while(z<32&&q[z])z++;if(!z||z==32)return 6;
  for(unsigned j=z;j<32;j++)if(q[j])return 6;
  uint32_t b=RhRead32(q+32);uint64_t pad=((uint64_t)b+3)&~UINT64_C(3);if(pad>n-*at-36)return 6;
  for(uint64_t j=b;j<pad;j++)if(q[36+j])return 6;
  if(z==4&&RhSame(q,"name",4)){if(!b||b>128||q[36+b-1]||RhLength((const volatile char *)q+36,b)!=b-1)return 6;names++;}
  *at+=36+pad;
 }
 if(names!=1)return 6;for(uint32_t i=0;i<nc;i++){int s=RhReadNode(p,n,at,depth+1);if(s)return s;}return 0;
}
static inline int RubyHandoffAdt(const volatile unsigned char *p,uint64_t n){uint64_t at=0;if(!p||!n||n>RUBY_HOFF_DT_CAP)return 6;int s=RhReadNode(p,n,&at,0);return s?s:(at==n?0:6);}
static inline int RubyHandoffPlan(const RubyStagePlan *plan,const RubyHandoffInput *in,uint64_t base,uint64_t cap,RubyHandoffLayout *l){
 if(!plan||!in||!l||!in->frequency||in->frequency>UINT64_C(10000000000)||in->reserved||in->fb!=UINT64_C(0x7c406000)||in->width!=1080||in->height!=2400||in->stride!=4320)return 1;
 if(base!=RUBY_STAGE_DEST_BASE||cap!=RUBY_STAGE_DEST_MAX||plan->vm_lo<RUBY_STAGE_LINK_BASE||plan->vm_hi<=plan->vm_lo||plan->vm_hi-RUBY_STAGE_LINK_BASE>cap||plan->entry_va<plan->vm_lo||plan->entry_va>=plan->vm_hi||plan->entry_pa!=base+plan->entry_va-RUBY_STAGE_LINK_BASE||!plan->segments||plan->segments>32)return 2;
 uint64_t hi=plan->vm_hi-RUBY_STAGE_LINK_BASE;
 for(uint64_t i=0;i<plan->segments;i++){uint64_t o=plan->seg[i].dest_offset,b=plan->seg[i].mem_bytes;if(!b||o>hi||b>hi-o)return 2;}
 RubyHandoffLayout q={0};q.dt_pa=base+((hi+RUBY_HOFF_PAGE-1)&~(RUBY_HOFF_PAGE-1));q.ba_pa=q.dt_pa+RUBY_HOFF_DT_CAP;q.top_pa=q.ba_pa+RUBY_HOFF_PAGE;q.ba_bytes=sizeof(RubyBootArgs);
 if(q.top_pa>base+cap||q.top_pa>RUBY_HOFF_PHYS+RUBY_HOFF_MEM||q.dt_pa<base+hi)return 3;*l=q;return 0;
}
static inline void RhArgs(RubyBootArgs *a,const RubyHandoffLayout *l,const RubyHandoffInput *in){
 RhZero(a,sizeof(*a));a->revision=2;a->version=2;a->virt_base=RUBY_HOFF_VIRT;a->phys_base=RUBY_HOFF_PHYS;a->mem_size=RUBY_HOFF_MEM;a->mem_actual=RUBY_HOFF_MEM;a->top=l->top_pa;
 a->video.base=in->fb;a->video.row_bytes=in->stride;a->video.width=in->width;a->video.height=in->height;a->video.depth=32;
 a->dt=RUBY_HOFF_VIRT+(l->dt_pa-RUBY_HOFF_PHYS);a->dt_bytes=(uint32_t)l->dt_bytes;RhCopy(a->command,"-v debug=0x100 cpus=1",sizeof("-v debug=0x100 cpus=1"));
}
static inline int RubyHandoffVerify(const unsigned char *dest,uint64_t base,uint64_t cap,const RubyStagePlan *plan,const RubyHandoffInput *in,const RubyHandoffLayout *provided){
 RubyHandoffLayout l;int s=RubyHandoffPlan(plan,in,base,cap,&l);if(s||!dest||!provided||!RubyStagePointer(dest,cap))return s?s:1;
 if(provided->dt_pa!=l.dt_pa||provided->ba_pa!=l.ba_pa||provided->top_pa!=l.top_pa||provided->ba_bytes!=l.ba_bytes)return 4;
 const volatile unsigned char *dt=dest+(l.dt_pa-base);if(!provided->dt_bytes||provided->dt_bytes>4096)return 4;
 if((s=RubyHandoffAdt(dt,provided->dt_bytes))!=0)return s;
 unsigned char expected[2048];uint64_t n=0;s=RhMakeDt(expected,sizeof(expected),in,&n);if(s||n!=provided->dt_bytes||!RhSame(expected,dt,n))return 7;
 l.dt_bytes=n;RubyBootArgs a;RhArgs(&a,&l,in);if(!RhSame(&a,dest+(l.ba_pa-base),sizeof(a)))return 8;
 for(uint64_t i=n;i<RUBY_HOFF_DT_CAP;i++)if(dt[i])return 9;
 for(uint64_t i=sizeof(a);i<RUBY_HOFF_PAGE;i++)if(((const volatile unsigned char *)dest)[l.ba_pa-base+i])return 9;
 return 0;
}
static inline int RubyHandoffBuild(unsigned char *dest,uint64_t base,uint64_t cap,const RubyStagePlan *plan,const RubyHandoffInput *in,RubyHandoffLayout *out){
 RubyHandoffLayout l;int s=RubyHandoffPlan(plan,in,base,cap,&l);if(s||!dest||!out||!RubyStagePointer(dest,cap))return s?s:1;
 /* Reject aliases to output before storing. Plans/inputs are caller-owned and
  * must not live in the metadata region that this call is about to replace. */
 uint64_t d=(uint64_t)(uintptr_t)(dest+(l.dt_pa-base)),len=l.top_pa-l.dt_pa;
 if(RubyStageOverlaps((uint64_t)(uintptr_t)dest,cap,(uint64_t)(uintptr_t)out,sizeof(*out)))return 1;
 if(RubyStageOverlaps(d,len,(uint64_t)(uintptr_t)plan,sizeof(*plan))||RubyStageOverlaps(d,len,(uint64_t)(uintptr_t)in,sizeof(*in))||RubyStageOverlaps(d,len,(uint64_t)(uintptr_t)out,sizeof(*out)))return 1;
 unsigned char *dt=dest+(l.dt_pa-base);RhZero(dt,len);s=RhMakeDt(dt,RUBY_HOFF_DT_CAP,in,&l.dt_bytes);if(s)return s;
 RubyBootArgs a;RhArgs(&a,&l,in);RhCopy(dest+(l.ba_pa-base),&a,sizeof(a));__asm__ volatile("" ::: "memory");s=RubyHandoffVerify(dest,base,cap,plan,in,&l);if(s)return s;*out=l;return 0;
}
#endif
