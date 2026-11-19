#ifndef RUBY_XNU_PREPARE_H
#define RUBY_XNU_PREPARE_H
#include "ruby_stage.h"
#include "native_thread.h"
#include "native_sha256.h"
#include "native_irq_dt.h"
#include "ruby_handoff.h"
#include "CheckpointPayload.h"
#include "CpuTopology.h"
static void hexline(unsigned,const char *,uint64_t,uint32_t);
void RawLine(unsigned,const char *,uint32_t);
typedef struct { uint32_t revision,pad;efi_handle parent;efi_system_table *system;efi_handle device; } XnuLoaded;
typedef struct XnuFile XnuFile;
struct XnuFile { uint64_t revision;
 efi_status (*open)(XnuFile *,XnuFile **,const char16 *,uint64_t,uint64_t);
 efi_status (*close)(XnuFile *);void *delete_file;
 efi_status (*read)(XnuFile *,uint64_t *,void *);
};
typedef struct XnuFs {uint64_t revision;efi_status (*open_volume)(struct XnuFs *,XnuFile **);} XnuFs;
static const efi_guid xnu_loaded={0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0,0xa0,0xc9,0x69,0x72,0x3b}};
static const efi_guid xnu_fs={0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0,0xa0,0xc9,0x69,0x72,0x3b}};
static uint64_t cntkctl_before;
static uint32_t PayloadCrc(const unsigned char *p,uint64_t n){uint32_t c=UINT32_MAX;for(uint64_t i=0;i<n;i++){c^=p[i];for(unsigned j=0;j<8;j++)c=(c>>1)^(UINT32_C(0xedb88320)&(0U-(c&1)));}return ~c;}
__attribute__((noinline)) efi_status PrepareXnu(efi_handle image,efi_boot_services *bs,struct context *c) {
 XnuLoaded *li=0;XnuFs *fs=0;XnuFile *root=0,*file=0;efi_status s;uint64_t source=RUBY_STAGE_SOURCE_BASE,dest=RUBY_STAGE_DEST_BASE;
 int source_owned=0,dest_owned=0;
 s=bs->handle_protocol(image,&xnu_loaded,(void **)&li);if(EFI_ERROR(s)||!li||!li->device)return EFI_ERROR(s)?s:ERR(26);
 s=bs->handle_protocol(li->device,&xnu_fs,(void **)&fs);if(EFI_ERROR(s)||!fs||!fs->open_volume)return EFI_ERROR(s)?s:ERR(26);
 s=fs->open_volume(fs,&root);if(EFI_ERROR(s)||!root)return EFI_ERROR(s)?s:ERR(26);
 const char16 path[]={'\\','P','A','Y','L','O','A','D','.','X','N','P',0};
 s=root->open(root,&file,path,1,0);if(EFI_ERROR(s)||!file){if(!EFI_ERROR(s))s=ERR(26);goto done;}
 s=bs->allocate_pages(AllocateAddress,EfiLoaderData,RUBY_STAGE_SOURCE_MAX>>12,&source);if(EFI_ERROR(s))goto done;source_owned=1;if(source!=RUBY_STAGE_SOURCE_BASE){s=ERR(26);goto done;}
 uint64_t n=RUBY_STAGE_SOURCE_MAX;s=file->read(file,&n,(void *)(uintptr_t)source);if(EFI_ERROR(s))goto done;
 if(!NtInputGuard(c->frequency)){s=ERR(26);goto done;}
 if(n!=CHECKPOINT_PAYLOAD_BYTES||PayloadCrc((const unsigned char *)(uintptr_t)source,n)!=CHECKPOINT_PAYLOAD_CRC||!NtHash((const unsigned char *)(uintptr_t)source,n,nt_payload_hash)){s=ERR(26);goto done;}
 unsigned char extra;uint64_t one=1;s=file->read(file,&one,&extra);if(EFI_ERROR(s)||one){s=ERR(26);goto done;}
 s=file->close(file);file=0;if(EFI_ERROR(s))goto done;s=root->close(root);root=0;if(EFI_ERROR(s))goto done;
 RubyStagePlan plan;
 if(RubyStagePlanPayload((const unsigned char *)(uintptr_t)source,n,&plan)){s=ERR(33);goto done;}
 if(plan.entry_va!=UINT64_C(0xfffffff00745e4a8)||plan.entry_pa!=UINT64_C(0x6345e4a8)){s=ERR(26);goto done;}
 s=bs->allocate_pages(AllocateAddress,EfiLoaderData,RUBY_STAGE_DEST_MAX>>12,&dest);if(EFI_ERROR(s))goto done;dest_owned=1;if(dest!=RUBY_STAGE_DEST_BASE){s=ERR(26);goto done;}
 RawLine(6,"Copying and verifying XNU segments...",WHITE);
 if(RubyStageCopy((const unsigned char *)(uintptr_t)source,n,(unsigned char *)(uintptr_t)dest,dest,RUBY_STAGE_DEST_MAX,&plan)){s=ERR(7);goto done;}
 uint64_t mpidr;__asm__ volatile("mrs %0,mpidr_el1":"=r"(mpidr));
 RubyHandoffInput in={c->frequency,mpidr,FB,1080,2400,4320,0};RubyHandoffLayout l={0};
 if(RubyHandoffBuild((unsigned char *)(uintptr_t)dest,dest,RUBY_STAGE_DEST_MAX,&plan,&in,&l)){s=ERR(26);goto done;}
 /* Extra byte guard for the early assembly path and native return slot.
  * Whole-payload CRC and full copy/readback cover all other segments. */
 volatile unsigned char *staged=(void *)(uintptr_t)dest;
 for(uint64_t pa=UINT64_C(0x6345e4a8);pa<UINT64_C(0x63472c20);pa++){
  uint64_t va=RUBY_STAGE_LINK_BASE+(pa-dest);int found=0;
  for(uint64_t i=0;i<plan.segments;i++){
   uint64_t off=va-RUBY_STAGE_LINK_BASE;
   if(off>=plan.seg[i].dest_offset&&off-plan.seg[i].dest_offset<plan.seg[i].file_bytes){
    if(staged[off]!=((const volatile unsigned char *)(uintptr_t)source)[plan.kernel_offset+plan.seg[i].file_offset+off-plan.seg[i].dest_offset]){s=ERR(26);goto done;}found=1;break;
   }
  }
  if(!found){s=ERR(26);goto done;}
 }
 volatile uint64_t *slot=(void *)(uintptr_t)UINT64_C(0x6345f7f0);
 if(slot[0]!=UINT64_C(0x545352444c554e58)||slot[1]||slot[2]||slot[3]){s=ERR(26);goto done;}
 /* v36: real core bootstrap through arm_vm_init, native checkpoint17.
  * RAM-only terminal fence before console/device initialization. */
 volatile uint32_t *continuation=(void *)(uintptr_t)UINT64_C(0x63453e28);
 if(*continuation!=UINT32_C(0xd0ffea88)){s=ERR(26);goto done;}
 /* v37 deliberately does not patch this continuation. */
 if(*continuation!=UINT32_C(0xd0ffea88)){s=ERR(7);goto done;}
 /* No initialized native GIC/timebase/topology fields are seeded. */
 if(*(volatile uint64_t *)(uintptr_t)UINT64_C(0x63e8a7a0)||*(volatile uint64_t *)(uintptr_t)UINT64_C(0x63e8afb8)||*(volatile uint64_t *)(uintptr_t)UINT64_C(0x63d535b8)){s=ERR(26);goto done;}
 if(!NtPatches()){s=ERR(26);goto done;}
 __asm__ volatile("mrs %0,cntkctl_el1":"=r"(cntkctl_before));
 c->entry=plan.entry_pa;c->bootargs=l.ba_pa;c->checkpoint=0;c->original_word=0;
 slot[1]=(uint64_t)(uintptr_t)XnuReturn;slot[2]=0;slot[3]=UINT64_C(0x63000000)-RUBY_STAGE_LINK_BASE;
 __asm__ volatile("dsb sy":::"memory");
 if(slot[1]!=(uint64_t)(uintptr_t)XnuReturn||slot[2]||slot[3]!=(UINT64_C(0x63000000)-RUBY_STAGE_LINK_BASE)){s=ERR(7);goto done;}
 s=bs->free_pages(source,RUBY_STAGE_SOURCE_MAX>>12);if(EFI_ERROR(s))goto done;source_owned=0;
 RawLine(6,"XNU + native first-thread fence verified",GREEN);
 return 0; /* Keep destination exclusively owned until terminal stop. */
 done:
 if(file)file->close(file);if(root)root->close(root);
 if(dest_owned)bs->free_pages(dest,RUBY_STAGE_DEST_MAX>>12);
 if(source_owned)bs->free_pages(source,RUBY_STAGE_SOURCE_MAX>>12);
 return s;
}
#endif
