#ifndef RUBY_RAM_CONTRACT_H
#define RUBY_RAM_CONTRACT_H
/* v32: a deliberately small, single owned XNU bank. This is not a general
 * firmware memory exclusion implementation. No allocator may claim outside it. */
#define RUBY_XNU_BASE UINT64_C(0x63000000)
#define RUBY_XNU_BYTES UINT64_C(0x02000000)
static int RamRange(uint64_t a,uint64_t n){return n && a<=UINT64_MAX-n;}
static int RamInside(uint64_t a,uint64_t n,uint64_t b,uint64_t z){
 return RamRange(a,n)&&RamRange(b,z)&&a>=b&&a-b<=z&&n<=z-(a-b);
}
static int RamOutside(uint64_t a,uint64_t n,uint64_t b,uint64_t z){
 return RamRange(a,n)&&RamRange(b,z)&&(a+n<=b||b+z<=a);
}
/* Check exact, unique EfiLoaderData coverage, accepting split descriptors and
 * rejecting holes, overlaps and even a single conventional/reserved page. */
static int RamOwnedMap(const unsigned char *map,uint64_t n,uint64_t stride,uint64_t base,uint64_t size){
 if(!map||!n||n>0x8000||stride<sizeof(efi_memory_descriptor)||stride>4096||n%stride||!RamRange(base,size))return 0;
 uint64_t end=base+size,at=base;
 while(at<end){
  unsigned hits=0;uint64_t next=at;
  for(uint64_t i=0;i<n;i+=stride){
   const efi_memory_descriptor *d=(const void *)(map+i);
   if(!d->pages||d->pages>(UINT64_MAX>>12)||(d->phys_start&4095)||!RamRange(d->phys_start,d->pages<<12))return 0;
   uint64_t a=d->phys_start,b=a+(d->pages<<12);
   if(a<=at&&at<b){if(++hits!=1||(d->type!=EfiLoaderData||(d->attribute&(UINT64_C(1)<<63))))return 0;next=b<end?b:end;}
   /* A second range starting within this span is caught on the next pass by
    * truncating the cursor step, even when its first page is not at `at`. */
  }
  if(hits!=1||next<=at)return 0;
  for(uint64_t i=0;i<n;i+=stride){const efi_memory_descriptor *d=(const void *)(map+i);if(d->phys_start>at&&d->phys_start<next)next=d->phys_start;}
  at=next;
 }
 return 1;
}
static int RamArgsOwned(const RubyBootArgs *a){
 return a&&a->revision==2&&a->version==2&&a->phys_base==RUBY_XNU_BASE&&a->virt_base==RUBY_STAGE_LINK_BASE&&
 a->mem_size==RUBY_XNU_BYTES&&a->mem_actual==RUBY_XNU_BYTES&&
 RamInside(a->top-1,1,RUBY_XNU_BASE,RUBY_XNU_BYTES)&&a->top>=UINT64_C(0x642f8000)&&
 a->dt==UINT64_C(0xfffffff0082e4000)&&a->dt_bytes>0&&a->dt_bytes<=RUBY_HOFF_DT_CAP;
}
#endif
