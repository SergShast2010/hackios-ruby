#ifndef RUBY_IRQ_FDT_H
#define RUBY_IRQ_FDT_H
/* Strict read-only allowlist for the observed ruby interrupt/timer DT contract.
 * Unknown layouts are rejected before any GIC MMIO; no storage or firmware calls. */
#include <stdint.h>
#include <stddef.h>
typedef struct { uint64_t gicd,gicr,gicr_bytes,dt_frequency; uint32_t code; } RubyIrqHardware;
static uint32_t IfBe32(const unsigned char *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t IfBe64(const unsigned char *p){return ((uint64_t)IfBe32(p)<<32)|IfBe32(p+4);}
static int IfEq(const unsigned char *p,uint64_t n,const char *s){uint64_t i=0;while(s[i]){if(i>=n||p[i]!=(unsigned char)s[i])return 0;i++;}return i==n;}
static int IfCompat(const unsigned char *p,uint64_t n,const char *s){uint64_t at=0;while(at<n){uint64_t e=at;while(e<n&&p[e])e++;if(e==n)return 0;if(IfEq(p+at,e-at,s))return 1;at=e+1;}return 0;}
__attribute__((noinline)) int RubyIrqFdt(const unsigned char *f,uint64_t cap,RubyIrqHardware *r){
 uint64_t n,so,ss,to,ts,o,end,depth=0;uint32_t kind[32]={0},seen[32]={0};
 uint64_t d=0,rs=0,rb=0,freq=0;uint32_t ph=0,ip=0,gics=0,timers=0,roots=0,rootcells=0;
 if(!r)return 0;r->gicd=r->gicr=r->gicr_bytes=r->dt_frequency=0;r->code=1;
 if(!f||cap<40||IfBe32(f)!=0xd00dfeed)return 0;
 n=IfBe32(f+4);so=IfBe32(f+8);to=IfBe32(f+12);ts=IfBe32(f+32);ss=IfBe32(f+36);
 if(n<40||n>cap||n>0x80000||IfBe32(f+20)!=17||IfBe32(f+24)>17||so<40||to<40||(so&3)||so>n||ss>n-so||to>n||ts>n-to||!ts||ss<4||(so<to+ts&&to<so+ss))return 0;
 o=so;end=so+ss;
 while(o<end){
  if(end-o<4)return 0;uint32_t t=IfBe32(f+o);o+=4;
  if(t==1){
   if(depth>=32)return 0;uint64_t s=o;while(o<end&&f[o])o++;if(o==end)return 0;
   uint64_t len=o-s;o=(o+4)&~UINT64_C(3);if(o>end)return 0;kind[depth]=seen[depth]=0;
   if(depth==0){if(len||roots++)return 0;kind[depth]=1;}
   if(depth==1&&IfEq(f+s,len,"interrupt-controller")){kind[depth]=2;if(gics++)return 0;}
   if(depth==1&&IfEq(f+s,len,"timer")){kind[depth]=3;if(timers++)return 0;}
   depth++;
  }else if(t==2){
   if(!depth)return 0;depth--;if(kind[depth]==2&&(seen[depth]&63)!=63)return 0;
   if(kind[depth]==3&&(seen[depth]&15)!=15)return 0;
  }else if(t==3){
   if(!depth||end-o<8)return 0;uint64_t len=IfBe32(f+o),no=IfBe32(f+o+4);o+=8;
   if(len>end-o||no>=ts)return 0;uint64_t pl=0;while(pl<ts-no&&f[to+no+pl])pl++;if(pl==ts-no)return 0;
   const unsigned char *p=f+o,*name=f+to+no;uint32_t k=kind[depth-1],bit=0;
   if(k==1&&(IfEq(name,pl,"#address-cells")||IfEq(name,pl,"#size-cells"))){
    unsigned b=IfEq(name,pl,"#address-cells")?1:2;if(len!=4||IfBe32(p)!=2||(rootcells&b))return 0;rootcells|=b;
   }
   if((k==2||k==3)&&IfEq(name,pl,"status")){
    if(!((len==5&&IfEq(p,4,"okay")&&p[4]==0)||(len==3&&IfEq(p,2,"ok")&&p[2]==0)))return 0;
   }
   if((k==2||k==3)&&IfEq(name,pl,"compatible")){
    bit=1;if(!IfCompat(p,len,k==2?"arm,gic-v3":"arm,armv8-timer"))return 0;
   }
   if(k==2&&IfEq(name,pl,"reg")){
    bit=2;if(rootcells!=3||len!=32||IfBe64(p)!=0x0c000000||IfBe64(p+8)!=0x40000||IfBe64(p+16)!=0x0c040000||IfBe64(p+24)!=0x200000)return 0;
    d=IfBe64(p);rs=IfBe64(p+16);rb=IfBe64(p+24);
   }
   if(k==2&&IfEq(name,pl,"phandle")){bit=4;if(len!=4||!(ph=IfBe32(p)))return 0;}
   if(k==2&&IfEq(name,pl,"#interrupt-cells")){bit=8;if(len!=4||IfBe32(p)!=3)return 0;}
   if(k==2&&IfEq(name,pl,"interrupt-controller")){bit=16;if(len!=0)return 0;}
   if(k==2&&IfEq(name,pl,"#redistributor-regions")){bit=32;if(len!=4||IfBe32(p)!=1)return 0;}
   if(k==2&&IfEq(name,pl,"redistributor-stride")&&(len!=8||IfBe64(p)!=0x20000))return 0;
   if(k==3&&IfEq(name,pl,"interrupt-parent")){bit=2;if(len!=4||!(ip=IfBe32(p)))return 0;}
   if(k==3&&IfEq(name,pl,"interrupts")){
    const uint32_t ids[4]={13,14,11,10};bit=4;if(len!=48)return 0;
    for(unsigned i=0;i<4;i++)if(IfBe32(p+i*12)!=1||IfBe32(p+i*12+4)!=ids[i]||IfBe32(p+i*12+8)!=4)return 0;
   }
   if(k==3&&IfEq(name,pl,"clock-frequency")){bit=8;if(len!=4||!(freq=IfBe32(p))||freq>1000000000)return 0;}
   if(bit){if(seen[depth-1]&bit)return 0;seen[depth-1]|=bit;}
   o=(o+len+3)&~UINT64_C(3);if(o>end)return 0;
  }else if(t==4){}else if(t==9){
   if(depth||roots!=1||gics!=1||timers!=1||rootcells!=3||!ph||ph!=ip)return 0;
   r->gicd=d;r->gicr=rs;r->gicr_bytes=rb;r->dt_frequency=freq;r->code=0;return 1;
  }else return 0;
 }
 return 0;
}
#endif
