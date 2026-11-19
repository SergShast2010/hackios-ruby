#ifndef RUBY_VM_CHECK_H
#define RUBY_VM_CHECK_H
#include <stdint.h>
#include <stddef.h>
#define VM_RAM_BASE UINT64_C(0x63000000)
#define VM_RAM_END UINT64_C(0x65000000)
#define VM_PA_MASK UINT64_C(0x0000fffffffff000)
typedef struct {uint64_t tables,leaves;unsigned error;} VmSummary;
static inline int VmSpan(uint64_t p,uint64_t n){return p>=VM_RAM_BASE&&p<=VM_RAM_END&&n<=VM_RAM_END-p;}
static inline uint64_t VmWord(const volatile unsigned char *ram,uint64_t p){uint64_t x=0;for(unsigned j=0;j<8;j++)x|=(uint64_t)ram[p-VM_RAM_BASE+j]<<(8*j);return x;}
static inline int VmWalk(const volatile unsigned char *ram,uint64_t p,unsigned level,VmSummary *s){
 if(!VmSpan(p,4096)||(p&4095)||level<1||level>3||++s->tables>512){s->error=1;return 0;}
 unsigned shift=39-9*level;uint64_t bytes=UINT64_C(1)<<shift;
 for(unsigned i=0;i<(level==1?256:512);i++){
  uint64_t d=VmWord(ram,p+i*8);if(!(d&1))continue;
  if(level<3&&(d&3)==3){if(!VmWalk(ram,d&VM_PA_MASK,level+1,s))return 0;continue;}
  if((level==3&&(d&3)!=3)||(level<3&&(d&3)!=1)||!(d&UINT64_C(0x400))||(d&UINT64_C(0x1c))){s->error=2;return 0;}
  uint64_t pa=(d&VM_PA_MASK)&~(bytes-1);
  if(!VmSpan(pa,bytes)||(d&VM_PA_MASK)!=(pa)||(++s->leaves)>65536){s->error=3;return 0;}
 }
 return 1;
}
static inline int VmScan(const volatile unsigned char *ram,uint64_t root,VmSummary *s){s->tables=0;s->leaves=0;s->error=0;return VmWalk(ram,root,1,s)&&s->tables>1&&s->leaves>0;}
static inline int VmTranslate(const volatile unsigned char *ram,uint64_t root,uint64_t va,uint64_t *pa,uint64_t *desc){
 uint64_t p=root;
 for(unsigned level=1;level<=3;level++){
  if(!VmSpan(p,4096)||(p&4095))return 0;
  unsigned shift=39-9*level;uint64_t d=VmWord(ram,p+((va>>shift)&(level==1?255:511))*8);if(!(d&1))return 0;
  if(level<3&&(d&3)==3){p=d&VM_PA_MASK;continue;}
  if((level==3&&(d&3)!=3)||(level<3&&(d&3)!=1))return 0;
  uint64_t mask=(UINT64_C(1)<<shift)-1;*pa=((d&VM_PA_MASK)&~mask)|(va&mask);*desc=d;return VmSpan(*pa,1);
 }
 return 0;
}
/* The native XNU bootseed path intentionally zeroes chosen/random-seed.
 * Modify ONLY the local expected DT, never the kernel-owned original. */
static inline int VmConsumedSeed(unsigned char *dt,uint64_t n){
 static const unsigned char name[12]={'r','a','n','d','o','m','-','s','e','e','d',0};unsigned count=0;
 for(uint64_t i=0;i+100<=n;i++){
  unsigned equal=1;for(unsigned j=0;j<12;j++)if(dt[i+j]!=name[j])equal=0;
  if(!equal)continue;
  for(unsigned j=12;j<32;j++)if(dt[i+j])equal=0;
  if(!equal||dt[i+32]!=64||dt[i+33]||dt[i+34]||dt[i+35])continue;
  for(unsigned j=0;j<64;j++)dt[i+36+j]=0;count++;
 }
 return count==1;
}
#endif
