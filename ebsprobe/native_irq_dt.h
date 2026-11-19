#ifndef A71_NATIVE_IRQ_DT_H
#define A71_NATIVE_IRQ_DT_H
/* Apple ADT, NOT an FDT. Standalone, allocation-free host/target encoder.
 * Production _gic_v3_irq_cpu_init adds ranges[1] to BOTH reg addresses.
 * Generic timer uses system registers: deliberately NO invented MMIO reg.
 * This header only encodes a subtree. It never accesses hardware or RAM policy.
 * Caller appends this as one ROOT child and updates DT size/root child count.
 * This is a contract prototype, NOT authorization to open ruby's IRQ gates.
 */
#include <stdint.h>
#include <stddef.h>
typedef struct { uint64_t d, ds, r, rs, frequency; } A71IrqDt;
typedef struct { unsigned char *p; size_t cap,n; unsigned stack[4],depth; int bad; } A71Adt;
static inline void a71_u32(unsigned char*p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static inline void a71_u64(unsigned char*p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(unsigned char)(v>>(8*i));}
static inline uint32_t a71_get32(const unsigned char*p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static inline size_t a71_len(const char*s){size_t n=0;while(s[n])++n;return n;}
static inline int a71_space(A71Adt*w,size_t n){if(w->bad||n>w->cap-w->n){w->bad=1;return 0;}return 1;}
static inline void a71_prop(A71Adt*w,const char*k,const void*v,size_t n){size_t z=a71_len(k),pad=(n+3)&~(size_t)3;if(!w->depth||z>=32||n>4096||!a71_space(w,36+pad)){w->bad=1;return;}unsigned char*p=w->p+w->n;for(size_t i=0;i<36+pad;i++)p[i]=0;for(size_t i=0;i<z;i++)p[i]=(unsigned char)k[i];a71_u32(p+32,(uint32_t)n);for(size_t i=0;i<n;i++)p[36+i]=((const unsigned char*)v)[i];w->n+=36+pad;p=w->p+w->stack[w->depth-1];a71_u32(p,a71_get32(p)+1);}
static inline void a71_str(A71Adt*w,const char*k,const char*v){a71_prop(w,k,v,a71_len(v)+1);}
static inline void a71_words(A71Adt*w,const char*k,const uint64_t*v,size_t n){unsigned char b[32];if(n>4){w->bad=1;return;}for(size_t i=0;i<n;i++)a71_u64(b+8*i,v[i]);a71_prop(w,k,b,n*8);}
static inline void a71_word(A71Adt*w,const char*k,uint32_t v){unsigned char b[4];a71_u32(b,v);a71_prop(w,k,b,4);}
static inline void a71_begin(A71Adt*w,const char*name){if(w->depth==4||!a71_space(w,8)){w->bad=1;return;}if(w->depth){unsigned char*p=w->p+w->stack[w->depth-1]+4;a71_u32(p,a71_get32(p)+1);}w->stack[w->depth++]=(unsigned)w->n;for(unsigned i=0;i<8;i++)w->p[w->n++]=0;a71_str(w,"name",name);}
static inline void a71_end(A71Adt*w){if(!w->depth)w->bad=1;else --w->depth;}
static inline int A71MakeIrqDt(unsigned char*out,size_t cap,const A71IrqDt*c,size_t*bytes){
 if(!out||!c||!bytes||!c->d||!c->ds||c->r<c->d||c->rs<0x20000||!c->frequency||c->r>UINT64_MAX-c->rs||c->d>UINT64_MAX-c->ds||c->r-c->d>UINT64_MAX-c->rs)return 1;
 A71Adt w={0};w.p=out;w.cap=cap;
 a71_begin(&w,"arm-io");a71_str(&w,"device_type","generic");a71_word(&w,"#address-cells",2);a71_word(&w,"#size-cells",2);
 uint64_t ranges[3]={0,c->d,c->r-c->d+c->rs};a71_words(&w,"ranges",ranges,3);
 a71_begin(&w,"gic");a71_str(&w,"compatible","arm,gic-v3");a71_str(&w,"interrupt-controller","master");a71_word(&w,"phandle",1);
 uint64_t reg[4]={0,c->ds,c->r-c->d,c->rs};a71_words(&w,"reg",reg,4);a71_end(&w);
 a71_begin(&w,"timer");a71_str(&w,"compatible","arm,armv8-timer");a71_str(&w,"device_type","timer");a71_word(&w,"interrupt-parent",1);a71_word(&w,"interrupts",27);a71_words(&w,"clock-frequency",&c->frequency,1);a71_end(&w);a71_end(&w);
 if(w.bad||w.depth)return 2;*bytes=w.n;return 0;
}
#endif
