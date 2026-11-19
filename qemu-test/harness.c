#ifdef QEMU_AUDIT_PLUGIN
/* Host-only passive QEMU 10 plugin; built from this TEST source, never guest
 * production. Minimal API v4 declarations checked against QEMU v10.0 header.
 * No writes, register hooks, guest code patches or breakpoints in this observer. */
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
typedef uint64_t qemu_plugin_id_t;
typedef struct { const char *target_name; struct {int min,cur;} version; bool system_emulation; union {struct{int smp_vcpus,max_vcpus;} system;}; } qemu_info_t;
struct qemu_plugin_tb; struct qemu_plugin_insn; struct qemu_plugin_register; struct qemu_plugin_hwaddr;
typedef struct {struct qemu_plugin_register *handle;const char *name,*feature;} qemu_plugin_reg_descriptor;
typedef uint32_t qemu_plugin_meminfo_t;
typedef void (*vcb)(unsigned,void*);
typedef void (*mcb)(unsigned,qemu_plugin_meminfo_t,uint64_t,void*);
extern GArray *qemu_plugin_get_registers(void);
extern int qemu_plugin_read_register(struct qemu_plugin_register*,GByteArray*);
extern void qemu_plugin_register_vcpu_init_cb(qemu_plugin_id_t,void(*)(qemu_plugin_id_t,unsigned));
extern void qemu_plugin_register_vcpu_tb_trans_cb(qemu_plugin_id_t,void(*)(qemu_plugin_id_t,struct qemu_plugin_tb*));
extern void qemu_plugin_register_atexit_cb(qemu_plugin_id_t,void(*)(qemu_plugin_id_t,void*),void*);
extern size_t qemu_plugin_tb_n_insns(const struct qemu_plugin_tb*);
extern struct qemu_plugin_insn *qemu_plugin_tb_get_insn(const struct qemu_plugin_tb*,size_t);
extern uint64_t qemu_plugin_insn_vaddr(const struct qemu_plugin_insn*);
extern size_t qemu_plugin_insn_data(const struct qemu_plugin_insn*,void*,size_t);
extern void qemu_plugin_register_vcpu_insn_exec_cb(struct qemu_plugin_insn*,vcb,int,void*);
extern void qemu_plugin_register_vcpu_mem_cb(struct qemu_plugin_insn*,mcb,int,int,void*);
extern struct qemu_plugin_hwaddr *qemu_plugin_get_hwaddr(qemu_plugin_meminfo_t,uint64_t);
extern bool qemu_plugin_hwaddr_is_io(const struct qemu_plugin_hwaddr*);
extern uint64_t qemu_plugin_hwaddr_phys_addr(const struct qemu_plugin_hwaddr*);
extern unsigned qemu_plugin_mem_size_shift(qemu_plugin_meminfo_t);
extern bool qemu_plugin_mem_is_store(qemu_plugin_meminfo_t);
int qemu_plugin_version=4;
static FILE *audit;
static uint64_t callback,return_end,checkpoint,clean_count,fence_count;
static GByteArray *buf;
static struct qemu_plugin_register *handles[64];
static const char *names[]={"x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","x18","x19","x20","x21","x22","x23","x24","x25","x26","x27","x28","x29","x30","sp","SCTLR_EL1","TTBR0_EL1","TTBR1_EL1","TCR_EL1","MAIR_EL1","VBAR_EL1","TPIDR_EL1","CPSR"};
struct inst {uint64_t pc;uint32_t word;int kernel,selected;};
static int equal_name(const char*a,const char*b){return !g_ascii_strcasecmp(a,b)||(!strcmp(b,"SCTLR_EL1")&&!strcmp(a,"SCTLR"))||(!strcmp(b,"VBAR_EL1")&&!strcmp(a,"VBAR"));}
static void init(qemu_plugin_id_t id,unsigned cpu){
 (void)id;(void)cpu;buf=g_byte_array_new();GArray*a=qemu_plugin_get_registers();
 for(unsigned i=0;i<a->len;i++){qemu_plugin_reg_descriptor*d=&g_array_index(a,qemu_plugin_reg_descriptor,i);for(unsigned n=0;n<G_N_ELEMENTS(names);n++)if(equal_name(d->name,names[n]))handles[n]=d->handle;}
 fprintf(audit,"REGS");for(unsigned n=0;n<G_N_ELEMENTS(names);n++)if(handles[n])fprintf(audit," %s",names[n]);fputc('\n',audit);fflush(audit);g_array_free(a,TRUE);
}
static void execute(unsigned cpu,void*user){
 (void)cpu;struct inst*i=user;
 if(i->pc==0xfffffff007453e28ULL&&i->word==0x14000000U&&++fence_count>2)return;
 if(i->pc>=callback+0xcc&&i->pc<callback+0x140){if((i->word&0xffffffe0U)==0xd5087e40U)clean_count++;return;}
 if(!i->selected&&!(i->pc>=0xfffffff007472368ULL&&i->pc<0xfffffff007472f98ULL))return;
 fprintf(audit,"I %016"PRIx64" %08x",i->pc,i->word);
 if(i->selected){for(unsigned n=0;n<G_N_ELEMENTS(names);n++)if(handles[n]){g_byte_array_set_size(buf,0);if(qemu_plugin_read_register(handles[n],buf)>0&&buf->len<=8){uint64_t v=0;for(unsigned j=0;j<buf->len;j++)v|=(uint64_t)buf->data[j]<<(j*8);fprintf(audit," %s=%016"PRIx64,names[n],v);}}}
 fputc('\n',audit);
}
static void memory(unsigned cpu,qemu_plugin_meminfo_t info,uint64_t va,void*user){
 (void)cpu;struct inst*i=user;struct qemu_plugin_hwaddr*h=qemu_plugin_get_hwaddr(info,va);
 uint64_t pa=h?qemu_plugin_hwaddr_phys_addr(h):UINT64_MAX;unsigned sz=1U<<qemu_plugin_mem_size_shift(info);
 if((pa>=0x63000000&&pa+sz<=0x65000000)||(pa>=0x62000000&&pa+sz<=0x62010000)||(pa>=0x44000000&&pa+sz<=0x44200000)||(pa>=0x7c406000&&pa+sz<=0x7cdec000))return;
 fprintf(audit,"M %016"PRIx64" %c %016"PRIx64" %016"PRIx64" %u %d\n",i->pc,qemu_plugin_mem_is_store(info)?'W':'R',va,pa,sz,h?qemu_plugin_hwaddr_is_io(h):-1);
}
static void global_memory(unsigned cpu,qemu_plugin_meminfo_t info,uint64_t va,void*user){
 (void)cpu;struct qemu_plugin_hwaddr*h=qemu_plugin_get_hwaddr(info,va);
 if(h&&qemu_plugin_hwaddr_is_io(h)&&qemu_plugin_hwaddr_phys_addr(h)!=0x09000000&&qemu_plugin_hwaddr_phys_addr(h)!=0x09000018)
 fprintf(audit,"IO %016"PRIx64" %016"PRIx64" %016"PRIx64"\n",(uint64_t)(uintptr_t)user,va,qemu_plugin_hwaddr_phys_addr(h));
}
static void translate(qemu_plugin_id_t id,struct qemu_plugin_tb*tb){
 (void)id;for(size_t n=0;n<qemu_plugin_tb_n_insns(tb);n++){
 struct qemu_plugin_insn*in=qemu_plugin_tb_get_insn(tb,n);uint64_t pc=qemu_plugin_insn_vaddr(in);uint32_t word=0;qemu_plugin_insn_data(in,&word,4);
 qemu_plugin_register_vcpu_mem_cb(in,global_memory,0,3,(void*)(uintptr_t)pc);
 int k=(pc>=0x63000000ULL&&pc<0x65000000ULL)||(pc>=0xfffffff007000000ULL&&pc<0xfffffff009000000ULL);
 int r=pc>=callback&&pc<return_end;
 int forbidden=(word&0xffe0001fU)==0xd4000003U||(word&0xffe0001fU)==0xd4000002U||((word&0xfffff0ffU)==0xd50340ffU&&((word>>8)&2));
 if(!k&&!r&&pc!=checkpoint&&!forbidden)continue;
 struct inst*i=g_new0(struct inst,1);i->pc=pc;i->word=word;i->kernel=k;
 i->selected=(k&&(word&0xff000000U)==0xd5000000U)||(k&&(pc>=0x63461324&&pc<=0x63461468))||(k&&pc>=0x63465c54&&pc<=0x63465c60)||r||pc==checkpoint||forbidden;
 qemu_plugin_register_vcpu_insn_exec_cb(in,execute,i->selected?1:0,i);
 if(k)qemu_plugin_register_vcpu_mem_cb(in,memory,0,3,i);
 }
}
static void finish(qemu_plugin_id_t id,void*user){(void)id;(void)user;fprintf(audit,"FENCE_COUNT %"PRIu64"\nCLEAN %"PRIu64"\nEND\n",fence_count,clean_count);fclose(audit);}
int qemu_plugin_install(qemu_plugin_id_t id,const qemu_info_t*info,int argc,char**argv){
 if(!info->system_emulation||strcmp(info->target_name,"aarch64")||argc!=4)return -1;
 audit=fopen(strchr(argv[0],'=')+1,"w");if(!audit)return -1;setvbuf(audit,NULL,_IOFBF,65536);
 callback=strtoull(strchr(argv[1],'=')+1,0,0);return_end=strtoull(strchr(argv[2],'=')+1,0,0);checkpoint=strtoull(strchr(argv[3],'=')+1,0,0);
 qemu_plugin_register_vcpu_init_cb(id,init);qemu_plugin_register_vcpu_tb_trans_cb(id,translate);qemu_plugin_register_atexit_cb(id,finish,NULL);return 0;
}

#else
/* Independent bare EL1 harness. Runs the unmodified, relocated FINAL PE's
 * PrepareXnu with real AAPCS64 EFI C service mocks, then EnterPhysical.
 * EBS is never called. No PE/kernel instruction or system-register hooks. */
#include <stdint.h>
#include <stddef.h>
#include "../ebsprobe/efi.h"
#define CFG 0x40180000UL
#define TTB 0x40200000UL
#define CTX 0x62008000UL
#define STACK 0x62010000UL
#define PROBE 0x62009000UL
#define PRE_BA 0x62002000UL
#define FB 0x7c406000UL
#define UART 0x09000000UL
#define PRELOAD 0x66000000UL
#define SOURCE 0x60000000UL
#define DEST 0x63000000UL
#define ERR(n) (0x8000000000000000ULL|(n))
typedef uint64_t u64;
struct context { u64 magic,base,stack,ebs,map_bytes,desc_size,map_key,attempts,frequency,counter;
 u64 sctlr_before,sctlr_after,identity_pages,stage,vbar,brk_ok,esr,elr,far,vector;
 u64 entry,bootargs,checkpoint,original_word,saved_x0,saved_x20;
 u64 reason,code,kernel_vbar,sp0,sp1,x22,x23,x24,x25,x26,ttbr0,ttbr1,return_sctlr,tcr,mair,return_x11; };
typedef struct { uint32_t revision,pad;efi_handle parent;efi_system_table *system;efi_handle device; } XnuLoaded;
typedef struct XnuFile XnuFile;
struct XnuFile {u64 revision;efi_status (*open)(XnuFile*,XnuFile**,const char16*,u64,u64);efi_status (*close)(XnuFile*);void *delete_file;efi_status (*read)(XnuFile*,u64*,void*);};
typedef struct XnuFs {u64 revision;efi_status (*open_volume)(struct XnuFs*,XnuFile**);} XnuFs;
_Static_assert(offsetof(struct context,entry)==160,"context ABI");
_Static_assert(offsetof(struct context,saved_x0)==192,"fault x0 ABI");
_Static_assert(sizeof(struct context)==336,"v29 context size");
_Static_assert(offsetof(struct context,reason)==208,"native return reason ABI");
_Static_assert(offsetof(struct context,sp0)==232,"native SP_EL0 ABI");
_Static_assert(offsetof(struct context,x22)==248,"native x22 ABI");
_Static_assert(offsetof(struct context,ttbr1)==296,"native TTBR1 ABI");
_Static_assert(offsetof(struct context,return_sctlr)==304,"cp5 SCTLR ABI");
_Static_assert(offsetof(struct context,return_x11)==328,"cp5 native x11 ABI");
_Static_assert(offsetof(efi_boot_services,handle_protocol)==152,"EFI handle_protocol ABI");
static volatile u64 * const cfg=(volatile u64*)CFG;
static XnuFile root,file;
static XnuFs fs;
static XnuLoaded loaded;
static efi_boot_services bs;
static u64 pos,source_owned,dest_owned;
static inline void wr64(u64 a,u64 v){*(volatile u64*)a=v;}
static void putc_(char c){*(volatile unsigned*)UART=(unsigned)c;}
static void puts_(const char*s){while(*s)putc_(*s++);}
static void hex(u64 v){puts_("0x");for(int s=60;s>=0;s-=4)putc_("0123456789ABCDEF"[(v>>s)&15]);}
static void line(const char*s,u64 v){puts_(s);hex(v);puts_("\n");}
static void copy(void*d,const void*s,u64 n){volatile unsigned char*a=d;const unsigned char*b=s;while(n--)*a++=*b++;}
static int guid_is(const efi_guid*g,uint32_t a,uint16_t b,uint16_t c,const unsigned char*d){if(g->a!=a||g->b!=b||g->c!=c)return 0;for(unsigned i=0;i<8;i++)if(g->d[i]!=d[i])return 0;return 1;}
static efi_status handle(efi_handle h,const efi_guid*g,void**p){
 static const unsigned char ld[8]={0x8e,0x3f,0,0xa0,0xc9,0x69,0x72,0x3b},fd[8]={0x8e,0x39,0,0xa0,0xc9,0x69,0x72,0x3b};
 cfg[16]++;
 if(h==(void*)0x12345000&&guid_is(g,0x5b1b31a1,0x9562,0x11d2,ld)){*p=&loaded;return 0;}
 if(h==(void*)0x12346000&&guid_is(g,0x964e5b22,0x6459,0x11d2,fd)){*p=&fs;return 0;}
 return ERR(14);
}
static efi_status open_volume(XnuFs*self,XnuFile**p){if(self!=&fs)return ERR(2);cfg[17]++;*p=&root;return 0;}
static efi_status open_file(XnuFile*self,XnuFile**p,const char16*path,u64 mode,u64 attr){
 static const char expect[]="\\PAYLOAD.XNP";if(self!=&root||mode!=1||attr)return ERR(2);
 unsigned i=0;for(;expect[i];i++)if(path[i]!=(unsigned char)expect[i])return ERR(14);if(path[i])return ERR(14);
 cfg[18]++;*p=&file;pos=0;return 0;
}
static efi_status close_file(XnuFile*self){if(self==&file)cfg[20]++;else if(self==&root)cfg[21]++;else return ERR(2);return 0;}
static efi_status read_file(XnuFile*self,u64*n,void*buf){
 if(self!=&file)return ERR(2);cfg[19]++;u64 left=cfg[4]-pos;if(*n<left)left=*n;
 if(pos==0&&(u64)buf!=SOURCE)return ERR(2);copy(buf,(void*)(PRELOAD+pos),left);pos+=left;*n=left;return 0;
}
static efi_status allocate(int type,int mem,u64 pages,u64*addr){
 if(type!=AllocateAddress||mem!=EfiLoaderData)return ERR(2);
 if(*addr==SOURCE&&pages==0x1000&&!source_owned){source_owned=1;cfg[22]++;return 0;}
 if(*addr==DEST&&pages==0x2000&&!dest_owned){dest_owned=1;cfg[23]++;return 0;}
 return ERR(9);
}
static efi_status free_(u64 addr,u64 pages){
 if(addr==SOURCE&&pages==0x1000&&source_owned){source_owned=0;cfg[24]++;return 0;}
 if(addr==DEST&&pages==0x2000&&dest_owned){dest_owned=0;cfg[25]++;return 0;}
 return ERR(2);
}
static efi_status forbidden_ebs(efi_handle h,u64 key){(void)h;(void)key;cfg[26]++;puts_("FAIL unexpected EBS\n");return ERR(3);}
static void mmu_on(void){
 wr64(TTB,0x401);wr64(TTB+8,0x40000705);wr64(TTB+16,0);wr64(TTB+24,0);
 u64 mair=0xff00,tcr=32ULL|(1ULL<<8)|(1ULL<<10)|(3ULL<<12)|(1ULL<<23)|(2ULL<<32),s;
 __asm__ volatile("dsb ish\n msr mair_el1,%0\n msr tcr_el1,%1\n msr ttbr0_el1,%2\n isb"::"r"(mair),"r"(tcr),"r"((u64)TTB):"memory");
 __asm__ volatile("tlbi vmalle1\n dsb sy\n isb\n ic iallu\n dsb sy\n isb\n mrs %0,sctlr_el1":"=r"(s));s|=0x1005;
 __asm__ volatile("msr sctlr_el1,%0\n isb"::"r"(s):"memory");
}
void harness_main(void){
 u64 el,ccidx,s,freq,cnt;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));line("harness CurrentEL=",el);
 if(el!=4){puts_("FAIL not EL1\n");for(;;);}
 __asm__ volatile("mrs %0,id_aa64mmfr2_el1":"=r"(ccidx));line("ID_AA64MMFR2_EL1=",ccidx);
 mmu_on();__asm__ volatile("mrs %0,sctlr_el1":"=r"(s));line("SCTLR_EL1 after harness MMU on=",s);
 __asm__ volatile("mrs %0,cntfrq_el0":"=r"(freq));__asm__ volatile("mrs %0,cntpct_el0":"=r"(cnt));line("cntfrq=",freq);
 for(unsigned i=0;i<sizeof(struct context);i+=8)wr64(CTX+i,0);
 struct context*c=(void*)CTX;c->magic=0x45425350524f4245ULL;c->base=0x62000000;c->stack=STACK;c->ebs=ERR(19);c->frequency=freq;c->counter=cnt;
 wr64(PROBE,0x123456789abcdef0ULL);wr64(PROBE+8,~0x123456789abcdef0ULL);wr64(cfg[0]+cfg[2],FB);
 loaded.device=(void*)0x12346000;fs.open_volume=open_volume;root.open=open_file;root.close=close_file;file.close=close_file;file.read=read_file;
 bs.allocate_pages=allocate;bs.free_pages=free_;bs.handle_protocol=handle;bs.exit_boot_services=forbidden_ebs;
 /* QEMU-only inherited closed GIC fixture; injection precedes Prepare/native. */
 __asm__ volatile("msr daifset,#15\nmsr icc_sre_el1,%0\nisb\nmsr icc_igrpen1_el1,xzr\nmsr icc_pmr_el1,xzr"::"r"((u64)1):"memory");
 *(volatile unsigned*)0x08000000=0x50;
 /* Fixture-only: emulate inherited priorities BEFORE the final PE runs. */
 if(cfg[63]==1)__asm__ volatile("msr icc_ap0r0_el1,%0"::"r"((u64)1):"memory");
 if(cfg[63]==2)__asm__ volatile("msr icc_ap1r0_el1,%0"::"r"((u64)1):"memory");
 if(cfg[63]==3||cfg[63]==6)__asm__ volatile("msr icc_pmr_el1,%0"::"r"((u64)0xf0):"memory");
 if(cfg[63]==5)__asm__ volatile("msr icc_pmr_el1,%0"::"r"((u64)0x80):"memory");
 if(cfg[63]==6)__asm__ volatile("msr icc_ap0r0_el1,%0"::"r"((u64)1):"memory");
 if(cfg[63]==4)__asm__ volatile("msr icc_igrpen1_el1,%0"::"r"((u64)1):"memory");
 __asm__ volatile("dsb sy\nisb":::"memory");
 if(cfg[6])wr64(cfg[0]+cfg[6],0x51454d5546495854ULL);
 cfg[27]=s;cfg[8]=1;puts_("calling final PE PrepareXnu\n");
 efi_status result=((efi_status(*)(efi_handle,efi_boot_services*,struct context*))(cfg[0]+cfg[3]))((void*)0x12345000,&bs,c);
 cfg[9]=result;line("PrepareXnu returned=",result);__asm__ volatile("mrs %0,sctlr_el1":"=r"(s));cfg[28]=s;line("SCTLR_EL1 after PrepareXnu=",s);
 cfg[29]=source_owned;cfg[30]=dest_owned;
 if(result){cfg[8]=3;puts_("PrepareXnu rejected; kernel NOT entered\n");for(;;)__asm__ volatile("yield");}
 copy((void*)PRE_BA,(void*)c->bootargs,736);cfg[8]=2;
 puts_("PrepareXnu success; waiting for host snapshots (not EBS)\n");
 while(!cfg[10])__asm__ volatile("yield");
 /* Test-only unexpected early fault: no production text modification.
  * SCTLR.A makes XNU's first ldr from boot_args+8 fault on the +1 input.
  * The native early vector returns reason=1 through the original slot. */
 if(cfg[5]){if(cfg[5]!=1){puts_("FAIL unknown test mode\n");for(;;);}
  cfg[11]=1;c->bootargs+=1;cfg[12]=c->bootargs;
  __asm__ volatile("mrs %0,sctlr_el1":"=r"(s));s|=2;
  __asm__ volatile("msr sctlr_el1,%0\n isb"::"r"(s):"memory");cfg[15]=s;
 }
 __asm__ volatile("mrs %0,cntpct_el0":"=r"(c->counter));
 puts_("jumping to EnterPhysical (EBS NOT called)\n");
 ((void(*)(void*,u64))(cfg[0]+cfg[1]))(c,STACK);
 puts_("FAIL returned from EnterPhysical\n");for(;;);
}

#endif
