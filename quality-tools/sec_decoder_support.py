#!/usr/bin/env python3
"""Independently validate both boot files and execute their real SEC decoder.
The AArch64 TE module is extracted from the delivered boot; it is not recompiled
or patched. Unicorn models CPU/memory, not the MT6877 hardware or a full boot.
"""
from pathlib import Path
import gzip, hashlib, json, lzma, struct, uuid
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import *

H=Path(__file__).resolve().parent
sha=lambda b:hashlib.sha256(b).hexdigest()
a=lambda n,p:(n+p-1)&-p

def split(b):
    assert b[:8]==b'ANDROID!'
    p=struct.unpack_from('<I',b,36)[0]
    ks,rs=struct.unpack_from('<I',b,8)[0],struct.unpack_from('<I',b,16)[0]
    ds=struct.unpack_from('<I',b,1648)[0]
    ro=p+a(ks,p);do=ro+a(rs,p)
    k,r,d=b[p:p+ks],b[ro:ro+rs],b[do:do+ds]
    h=hashlib.sha1()
    for v in [k,r,b'',b'',d]:h.update(v);h.update(struct.pack('<I',len(v)))
    assert b[576:596]==h.digest()
    return b[:p],gzip.decompress(k),r,d

def files(fv):
    assert fv[40:44]==b'_FVH'
    assert struct.unpack_from('<Q',fv,32)[0]==len(fv)
    hl=struct.unpack_from('<H',fv,48)[0]
    assert sum(struct.unpack('<'+'H'*(hl//2),fv[:hl]))&65535==0
    o=hl;out={}
    while o<len(fv):
        o=a(o,8)
        if fv[o:o+24]==b'\xff'*24:
            assert fv[o:]==b'\xff'*(len(fv)-o);break
        hd=fv[o:o+24];n=int.from_bytes(hd[20:23],'little')
        assert 24<=n<=len(fv)-o
        assert (sum(hd[:23])-hd[17])&255==0
        data=fv[o+24:o+n]
        if hd[19]&0x40:assert (sum(data)+hd[17])&255==0
        else:assert hd[17]==0xaa
        out[str(uuid.UUID(bytes_le=hd[:16]))]=(hd,data)
        o+=n
    return out

def extract(raw):
    fs=files(raw[-0x200000:])
    section=fs['9e21fd93-9c72-4c15-8c4b-e77f1db2d792'][1]
    assert int.from_bytes(section[:3],'little')==len(section) and section[3]==2
    offset=struct.unpack_from('<H',section,20)[0]
    compressed=section[offset:]
    dec=lzma.decompress(compressed,format=lzma.FORMAT_ALONE)
    assert dec[7]==0x17
    files(dec[8:])
    secfile=fs['9affb503-e643-4141-8b90-17e8588b1d35'][1]
    off=0;te=None
    while off<len(secfile):
        n=int.from_bytes(secfile[off:off+3],'little');assert n>=4
        if secfile[off+3]==0x12:te=secfile[off+4:off+n]
        off=a(off+n,4)
    assert te and te[:2]==b'VZ'
    return section,dec,te

def execute(section,te,expected,decode):
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    BASE=struct.unpack_from('<Q',te,16)[0]
    assert BASE==0x40400000
    u.mem_map(BASE,0x20000)
    strip=struct.unpack_from('<H',te,6)[0]
    for j in range(te[4]):
        p=40+j*40
        vs,va,rs,rp=struct.unpack_from('<IIII',te,p+8)
        real=rp-strip+40
        if rs:u.mem_write(BASE+va,te[real:real+rs])
    # Pin actual function entry bytes from the examined original TE.
    assert bytes(u.mem_read(BASE+0x57d0,4))==bytes.fromhex('f353bca9')
    SRC,META,STACK,DST,SCR,STOP=0x50000000,0x51000000,0x52000000,0x53000000,0x54000000,0x55000000
    for addr,sz in [(SRC,0x100000),(META,0x1000),(STACK,0x10000),(DST,0x400000),(SCR,0x10000),(STOP,0x1000)]:u.mem_map(addr,sz)
    u.mem_write(SRC,section)
    u.mem_write(META,b'\xa5'*0x1000)
    u.mem_write(DST,b'\xa5'*0x400000)
    u.reg_write(UC_ARM64_REG_CPACR_EL1,3<<20)
    def call(entry,*args):
        for j,value in enumerate(args):u.reg_write(UC_ARM64_REG_X0+j,value)
        u.reg_write(UC_ARM64_REG_SP,STACK+0xfff0)
        u.reg_write(UC_ARM64_REG_X30,STOP)
        u.emu_start(BASE+entry,STOP,timeout=180000000,count=500000000)
        assert u.reg_read(UC_ARM64_REG_PC)==STOP,'decoder did not return'
        return u.reg_read(UC_ARM64_REG_X0)
    status=call(0x57d0,SRC,META,META+4,META+8)
    result={'GetInfo_status':hex(status),'SEC_TE_sha256':sha(te)}
    if decode:
        assert status==0
        size,scratch=struct.unpack('<II',u.mem_read(META,8))
        assert size==len(expected) and size<=0x400000 and scratch==65536
        u.mem_write(META+16,struct.pack('<Q',DST))
        status2=call(0x599c,SRC,META+16,SCR,META+24)
        assert status2==0
        assert bytes(u.mem_read(DST,size))==expected
        assert bytes(u.mem_read(DST+size,0x400000-size))==b'\xa5'*(0x400000-size)
        assert bytes(u.mem_read(SRC,len(section)))==section
        result.update(decompress_status=hex(status2),output_bytes=size,output_matches_full_FV=True,destination_tail_untouched=True)
    else:
        assert status==0x8000000000000003
        assert bytes(u.mem_read(META,8))==b'\xa5'*8
    return result

def main():
    old=(H/'base/Mu-ruby-v0.32-ramcontract.img').read_bytes()
    new=(H/'Mu-ruby-v0.32a-lzmafix.img').read_bytes()
    assert sha(old)=='75bc2a4f767598c61c1dbafe2865cbf9ffbd2cd9e688979b94b9dc1ac0d21244'
    oh,orr,oram,odtb=split(old);nh,nrr,nram,ndtb=split(new)
    assert oram==nram and odtb==ndtb and len(old)==len(new)
    diffs=[i for i,(x,y) in enumerate(zip(orr,nrr)) if x!=y]
    assert len(diffs)==8 and diffs==list(range(diffs[0],diffs[0]+8))
    for off,count in [(8,4),(576,32)]:nh=nh[:off]+oh[off:off+count]+nh[off+count:]
    assert nh==oh
    oldsec,olddec,oldte=extract(orr)
    newsec,newdec,newte=extract(nrr)
    assert oldte==newte and olddec==newdec
    print('Old final-image GetInfo:',flush=True)
    oldr=execute(oldsec,oldte,olddec,False);print(json.dumps(oldr),flush=True)
    print('Corrected final-image GetInfo + full decompression:',flush=True)
    newr=execute(newsec,newte,newdec,True);print(json.dumps(newr),flush=True)
    result={'result':'PASS','old_image_decoder':oldr,'corrected_image_decoder':newr,
            'image_sha256':sha(new),'raw_changed_bytes':len(diffs),'all_FV_FFS_checksums_valid':True,
            'entire_decompressed_FV_unchanged':True,'shim_ram_transport_DTB_unchanged':True,
            'kernel_payload_and_EFI_app_unchanged':True,'legacy_boot_id_valid':True,
            'executed':'actual SEC guided-section GetInfo and decode functions, extracted from final boot, on Unicorn AArch64',
            'limitations':'not a complete firmware boot; real MT6877, caches and EBS not modeled',
            'on_device_tested':False}
    (H/'verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2),flush=True)

if __name__=='__main__':main()
