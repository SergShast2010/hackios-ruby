#ifndef RUBY_CPU_TOPOLOGY_H
#define RUBY_CPU_TOPOLOGY_H
#include <stdint.h>
/* Exact ABI of the pinned HackiOS kernel, not a description of all MT6877 CPUs.
 * Only the running CPU, affinity zero, is exposed by this experiment's ADT.
 * Callers pass fixed, owned buffers; kernel pointers are compared, not followed. */
static inline uint32_t Ct32(const volatile unsigned char *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static inline uint64_t Ct64(const volatile unsigned char *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static inline unsigned CtEventShift(uint64_t frequency){
 if(frequency<UINT64_C(1000000)||frequency>UINT64_C(1000000000))return 64;
 uint64_t ticks=frequency/UINT64_C(1000000);unsigned shift=0;
 while((UINT64_C(1)<<(shift+1))<ticks)shift++;
 return shift;
}
static inline int CtTopology(const volatile unsigned char *info,const volatile unsigned char *cpu,const volatile unsigned char *cluster){
 if(!info||!cpu||!cluster)return 0;
 if(Ct32(info+4)!=1||Ct32(info+8)!=0||Ct32(info+12)!=1||Ct32(info+16)!=0)return 0;
 if(Ct64(info+24)!=UINT64_C(0xfffffff0071ca198)||Ct64(info+32)!=UINT64_C(0xfffffff0071ca558)||
    Ct64(info+40)!=UINT64_C(0xfffffff0071ca198)||Ct64(info+48)!=UINT64_C(0xfffffff0071ca558))return 0;
 /* cpu_id, phys_id, cluster_id, generic cluster-type, optional registers/cache
  * descriptions all default to zero in our minimal one-node device tree. */
 for(unsigned i=0;i<120;i++)if(cpu[i])return 0;
 if(Ct32(cluster)!=0||Ct32(cluster+4)!=0||Ct32(cluster+8)!=1||Ct64(cluster+16)!=1)return 0;
 for(unsigned i=24;i<72;i++)if(cluster[i])return 0;
 return 1;
}
#endif
