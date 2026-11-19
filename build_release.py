#!/usr/bin/env python3
from pathlib import Path
import sys,subprocess,json,hashlib,struct,zlib,lzma
H=Path(__file__).resolve().parent;sys.path.insert(0,str(H));import package_image as p
from fat_reader import fat_path
BASE=Path(sys.argv[1]) if len(sys.argv)>1 else H/'base/Mu-ruby-v0.38b-gicgate.img'
base=BASE.read_bytes();assert p.sha(base)=='ccf5e56b8221b6c79d7e277ed2e78d231098b5c60084bf0f5315ed4adfd39637'
subprocess.run([sys.executable,str(H/'build_native.py'),str(BASE)],check=True);subprocess.run([sys.executable,str(H/'build_app.py')],check=True)
import gzip,EfiCompressor
_,_,kernel,oldpack,_=p.split_boot(base);fd=gzip.decompress(kernel)[-p.FD_SIZE:];outer={g:blob for g,_,blob in p.fv_files(fd)};zf=outer[p.LZMA_GUID];sp,sn,_=p.sections(zf)[0];sec=zf[sp:sp+sn];decoded=lzma.decompress(sec[p.u16(sec,20):],format=lzma.FORMAT_ALONE);inner={g:blob for g,_,blob in p.fv_files(decoded[8:])};bds=p.pe_of_ffs(inner[p.BDS_GUID])
p.BASE_SHA256=p.sha(base);p.BDS_SHA256=p.sha(bds);p.OLD_PACK_CRC=zlib.crc32(oldpack);p.OLD_USED=p.u32(oldpack,0)+8
oldfat=bytes(EfiCompressor.UefiDecompress(oldpack[:p.OLD_USED],p.OLD_USED));p.OLD_FAT_CRC=zlib.crc32(oldfat)
_,sects=p.pe_offsets(bds);p.MOV_PATCHES=tuple((rva,struct.unpack_from('<I',bds,next(raw+rva-va for va,size,raw in sects if va<=rva<va+size))[0],which,shift) for rva,_,which,shift in p.MOV_PATCHES)
app=(H/'ebsprobe/loader.efi').read_bytes();image,report,_=p.build(base,app);pack=p.split_boot(image)[3];used=p.u32(pack,0)+8;fat=bytes(EfiCompressor.UefiDecompress(pack[:used],used))
assert fat_path(fat,[b'PAYLOAD XNP'])==fat_path(oldfat,[b'PAYLOAD XNP'])
assert fat_path(fat,[b'EFI        ',b'BOOT       ',b'BOOTAA64EFI'])==app
out=H/'Mu-ruby-v0.40-native-switch.img';out.write_bytes(image)
report.update(image_sha256=p.sha(image),app_sha256=p.sha(app),base_image_sha256=p.sha(base),payload_sha256=p.sha(fat_path(fat,[b'PAYLOAD XNP'])))
(H/'build.json').write_text(json.dumps(report,indent=2)+'\n');print('BUILT',out,'SHA256',p.sha(image))
