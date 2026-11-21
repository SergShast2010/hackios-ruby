#ifndef RUBY_RAM_POLICY_H
#define RUBY_RAM_POLICY_H
/* Shared, allocation-free policy. Input bytes are untrusted; no physical writes.
 * LK mblock layout is explicitly limited to the captured ruby v2 format.
 * Unknown versions/layouts or missing live evidence leave the window closed. */
#define RUBY_RAM_WINDOW_BASE 0x60000000ULL
#define RUBY_RAM_WINDOW_SIZE 0x08000000ULL
#define RUBY_LIVE_FDT_BASE 0x47C80000ULL
#define RUBY_LIVE_FDT_LIMIT 0x00080000ULL
struct ruby_ram_policy_report {
 unsigned long long code,fdt_bytes,mblock_count,reserved_count,free_base,free_size;
};
static unsigned int RubyRamBe32(const unsigned char *p) {
 return ((unsigned int)p[0]<<24)|((unsigned int)p[1]<<16)|((unsigned int)p[2]<<8)|p[3];
}
static unsigned long long RubyRamBe64(const unsigned char *p) {
 return ((unsigned long long)RubyRamBe32(p)<<32)|RubyRamBe32(p+4);
}
static unsigned int RubyRamLe32(const unsigned char *p) {
 return (unsigned int)p[0]|((unsigned int)p[1]<<8)|((unsigned int)p[2]<<16)|((unsigned int)p[3]<<24);
}
static unsigned long long RubyRamLe64(const unsigned char *p) {
 return (unsigned long long)RubyRamLe32(p)|((unsigned long long)RubyRamLe32(p+4)<<32);
}
static int RubyRamEq(const unsigned char *p,unsigned long long n,const char *s) {
 unsigned long long i=0;while(s[i]){if(i>=n||p[i]!=(unsigned char)s[i])return 0;i++;}return i==n;
}
static int RubyRamMemoryName(const unsigned char *p,unsigned long long n) {
 return RubyRamEq(p,n,"memory")||(n>7&&p[0]=='m'&&p[1]=='e'&&p[2]=='m'&&p[3]=='o'&&p[4]=='r'&&p[5]=='y'&&p[6]=='@');
}
static int RubyRamRange(unsigned long long a,unsigned long long n) {
 return n && a<=~0ULL-n;
}
static int RubyRamHits(unsigned long long a,unsigned long long n) {
 return RubyRamRange(a,n)&&a<RUBY_RAM_WINDOW_BASE+RUBY_RAM_WINDOW_SIZE&&RUBY_RAM_WINDOW_BASE<a+n;
}
static unsigned long long RubyValidateRamFdt(const unsigned char *f,unsigned long long cap,struct ruby_ram_policy_report *r) {
 unsigned long long total,so,ss,to,ts,ro,o,end,name,n,token,depth=0,ac[32]={0},sc[32]={0};
 unsigned int kinds[32]={0}; /* root=1, memory=2, reserved-memory=3, reserved child=4 */
 unsigned long long mem_seen=0,mb_seen=0,root_seen=0,rm_seen=0,mem_ok=0,free_ok=0;
 unsigned long long failure=1;
 if(!r)return 1;
 r->code=1;r->fdt_bytes=r->mblock_count=r->reserved_count=r->free_base=r->free_size=0;
 if(!f||cap<40||RubyRamBe32(f)!=0xD00DFEEDU)goto fail;
 total=RubyRamBe32(f+4);so=RubyRamBe32(f+8);to=RubyRamBe32(f+12);ro=RubyRamBe32(f+16);
 ts=RubyRamBe32(f+32);ss=RubyRamBe32(f+36);r->fdt_bytes=total;
 failure=2;
 if(total<40||total>cap||total>RUBY_LIVE_FDT_LIMIT||RubyRamBe32(f+20)!=17||RubyRamBe32(f+24)>17||
    so<40||to<40||ro<40||(so&3)||(ro&7)||ss<4||!ts||so>total||ss>total-so||to>total||ts>total-to||
    (so<to+ts&&to<so+ss)||ro>=so)goto fail;
 /* Reservation table must terminate before the structure block. */
 o=ro;failure=3;
 for(;;){unsigned long long a,z;if(o>so||so-o<16)goto fail;a=RubyRamBe64(f+o);z=RubyRamBe64(f+o+8);o+=16;
  if(!a&&!z)break;if(!RubyRamRange(a,z)||RubyRamHits(a,z))goto fail;}
 o=so;end=so+ss;failure=4;
 while(o<end){
  if(end-o<4)goto fail;token=RubyRamBe32(f+o);o+=4;
  if(token==1){
   if(depth>=32)goto fail;name=o;while(o<end&&f[o])o++;if(o==end)goto fail;n=o-name;o=(o+4)&~3ULL;if(o>end)goto fail;
   kinds[depth]=0;ac[depth]=2;sc[depth]=2;
   if(depth==0){if(root_seen++||n)goto fail;kinds[depth]=1;}
   else if(depth==1&&RubyRamMemoryName(f+name,n)){kinds[depth]=2;if(mem_seen++)goto fail;}
   else if(depth==1&&RubyRamEq(f+name,n,"reserved-memory")){kinds[depth]=3;if(rm_seen++)goto fail;}
   else if(depth==2&&kinds[depth-1]==3)kinds[depth]=4;
   depth++;
  }else if(token==2){if(!depth)goto fail;depth--;
  }else if(token==3){
   unsigned long long len,no,pn,pl,k;if(!depth||end-o<8)goto fail;len=RubyRamBe32(f+o);no=RubyRamBe32(f+o+4);o+=8;
   if(len>end-o||no>=ts)goto fail;pn=to+no;pl=0;while(pl<ts-no&&f[pn+pl])pl++;if(pl==ts-no)goto fail;
   k=kinds[depth-1];
   if((k==1||k==3)&&(RubyRamEq(f+pn,pl,"#address-cells")||RubyRamEq(f+pn,pl,"#size-cells"))){
    if(len!=4||RubyRamBe32(f+o)!=2)goto fail;
    if(RubyRamEq(f+pn,pl,"#address-cells"))ac[depth-1]=2;else sc[depth-1]=2;
   }
   if((k==2||k==4)&&RubyRamEq(f+pn,pl,"reg")){
    unsigned long long i;if(!len||(len%16)||ac[depth-2]!=2||sc[depth-2]!=2)goto fail;
    for(i=0;i<len;i+=16){unsigned long long a=RubyRamBe64(f+o+i),z=RubyRamBe64(f+o+i+8);
     if(!RubyRamRange(a,z))goto fail;
     if(k==2){if(a<=RUBY_RAM_WINDOW_BASE&&a+z>=RUBY_RAM_WINDOW_BASE+RUBY_RAM_WINDOW_SIZE)mem_ok=1;}
     else if(RubyRamHits(a,z)){failure=5;goto fail;}}
   }
   if(k==2&&RubyRamEq(f+pn,pl,"mblock_info")){
    unsigned long long i,j,count,rc,b=8+128*24,entries=b+16;
    if(mb_seen++||len!=22552){failure=6;goto fail;}count=RubyRamLe32(f+o);r->mblock_count=count;
    if(!count||count>128||RubyRamLe32(f+o+4)){failure=6;goto fail;}
    if(RubyRamLe32(f+o+b)!=0x99999999U||RubyRamLe32(f+o+b+4)!=2||RubyRamLe32(f+o+b+12)){failure=6;goto fail;}
    rc=RubyRamLe32(f+o+b+8);r->reserved_count=rc;if(!rc||rc>128){failure=6;goto fail;}
    for(i=0;i<count;i++){
     const unsigned char *p=f+o+8+i*24;unsigned long long a=RubyRamLe64(p),z=RubyRamLe64(p+8);
     if(!z)continue;
     if(!RubyRamRange(a,z)||(a&4095)||(z&4095)||RubyRamLe32(p+16)>1||RubyRamLe32(p+20)){failure=7;goto fail;}
     for(j=0;j<i;j++){const unsigned char *q=f+o+8+j*24;unsigned long long c=RubyRamLe64(q),d=RubyRamLe64(q+8);
      if(d&&a<c+d&&c<a+z){failure=7;goto fail;}}
     if(a<=RUBY_RAM_WINDOW_BASE&&a+z>=RUBY_RAM_WINDOW_BASE+RUBY_RAM_WINDOW_SIZE){free_ok=1;r->free_base=a;r->free_size=z;}
    }
    for(i=0;i<rc;i++){
     const unsigned char *p=f+o+entries+i*152;unsigned long long a=RubyRamLe64(p),z=RubyRamLe64(p+8);
     if(!RubyRamRange(a,z)){failure=8;goto fail;}if(RubyRamHits(a,z)){failure=9;goto fail;}
    }
   }
   o=(o+len+3)&~3ULL;if(o>end)goto fail;
  }else if(token==4){}else if(token==9){
   if(depth||!root_seen||!mem_seen||!rm_seen||!mb_seen||!mem_ok||!free_ok){failure=10;goto fail;}
   r->code=0;return 0;
  }else goto fail;
 }
 fail:r->code=failure;return failure;
}
#endif
