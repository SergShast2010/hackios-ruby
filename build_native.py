#!/usr/bin/env python3
from pathlib import Path
import subprocess,json,struct,hashlib,sys
H=Path(__file__).resolve().parent;P=H/'ebsprobe';B=H
sys.path.insert(0,str(B));import qemu_helpers as q
from fat_reader import fat_path
image=Path(sys.argv[1]) if len(sys.argv)>1 else H/'base/Mu-ruby-v0.38b-gicgate.img';assert q.sha(image.read_bytes())=='ccf5e56b8221b6c79d7e277ed2e78d231098b5c60084bf0f5315ed4adfd39637'
_,fat=q.final_app(image);payload=fat_path(fat,[b'PAYLOAD XNP']);k,segs,syms,entry=q.kernel_metadata(payload)
assert q.sha(k)=='9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c'
base=0xfffffff007472368;end=0xfffffff007472bf0
flags=['clang','--target=aarch64-none-elf','-ffreestanding','-fno-builtin','-fno-stack-protector','-mgeneral-regs-only','-mstrict-align','-Oz','-Wall','-Wextra','-Werror','-fno-pic','-fno-unwind-tables','-fno-asynchronous-unwind-tables']
for src in ['native_quiet.c','native_bridge.S','native_switch.S']:subprocess.run(flags+['-c',str(P/src),'-o',str(P/(src+'.o'))],check=True)
subprocess.run(['ld.lld','-T',str(H/'native.ld'),str(P/'native_bridge.S.o'),str(P/'native_quiet.c.o'),str(P/'native_switch.S.o'),'-o',str(P/'native_bridge.elf')],check=True)
subprocess.run(['aarch64-linux-gnu-objcopy','-O','binary','--only-section=.text',str(P/'native_bridge.elf'),str(P/'native_bridge.bin')],check=True)
subprocess.run(['aarch64-linux-gnu-objcopy','-O','binary','--only-section=.switch',str(P/'native_bridge.elf'),str(P/'native_switch.bin')],check=True)
blob=(P/'native_bridge.bin').read_bytes();print('native cave bytes',len(blob),'capacity',end-base);assert len(blob)<=end-base
nm=subprocess.check_output(['aarch64-linux-gnu-nm','-n',str(P/'native_bridge.elf')],text=True);symbols={l.split()[2]:int(l.split()[0],16) for l in nm.splitlines() if len(l.split())==3}
def old(va,n):
 s=next(s for s in segs if s['va']<=va and va+n<=s['va']+s['files']);return k[s['off']+va-s['va']:s['off']+va-s['va']+n]
def branch(va,target):
 delta=target-va;assert delta%4==0 and -(1<<27)<=delta<(1<<27);return struct.pack('<I',0x14000000|((delta>>2)&0x3ffffff))
assert symbols['NtPanic']==base,'ELF/file cave alignment mismatch'
assert base+len(blob)<=syms['_loader_jump'],'leave legacy jump entry separately fenceable'
switch_base=syms['_loader_jump']+4;switch_end=end
switch_blob=(P/'native_switch.bin').read_bytes()
assert switch_base==0xfffffff0074729f0 and switch_end==0xfffffff007472bf0
assert symbols['NtSwitchFence']==switch_base and len(switch_blob)<=switch_end-switch_base
original_switch_cave=old(switch_base,switch_end-switch_base)
assert q.sha(original_switch_cave)=='6a817a0498bcb501e24c578cfd41cec168a6a2c4f7a396657ffdfeec203d26d3' # exact original 512-byte tail
print('fenced legacy loader_jump tail',len(switch_blob),'capacity',switch_end-switch_base,'original SHA256',q.sha(original_switch_cave))
switch_blob+=struct.pack('<I',0x14000000)*((switch_end-switch_base-len(switch_blob))//4)
assert len(switch_blob)==switch_end-switch_base
patches=[(base,blob,'read-only quiet GIC binding + RAM-only exit cave'),(switch_base,switch_blob,'bounded switch in original fenced legacy loader_jump tail')]
def adrp_add(va,target,reg=0):
 d=(target>>12)-(va>>12);assert -(1<<20)<=d<(1<<20);u=d&0x1fffff
 return struct.pack('<II',0x90000000|((u&3)<<29)|((u>>2)<<5)|reg,0x91000000|((target&4095)<<10)|(reg<<5)|reg)
patches += [(0xfffffff007309b10,adrp_add(0xfffffff007309b10,symbols['NtSwitchSecond']),'bounded sched_startup native kernel thread continuation'),(0xfffffff007309b64,branch(0xfffffff007309b64,0xfffffff007309b98),'bounded sched_startup return before powered-core service thread'),(0xfffffff007305b00,struct.pack('<I',0x52800003),'diagnostic continuation IRQ masked; enable_interrupts=false')]
for name in ['_loader_return','_loader_jump']:patches.append((syms[name],branch(syms[name],symbols['NtPanic']),'independent legacy entry fence '+name))
for name in ['_PE_init_iokit','_hv_wdt_pet','_loader_return_timer','_loader_stop_poll']:
 if name in syms:patches.append((syms[name],branch(syms[name],symbols['NtPanic']),'independent forbidden-path fence '+name))
# Fence every SMC/HVC opcode in executable native text, even on failure paths.
for sg in segs:
 if sg['name']!='__TEXT_EXEC':continue
 for off in range(sg['off'],sg['off']+sg['files']-3,4):
  w=struct.unpack_from('<I',k,off)[0];va=sg['va']+off-sg['off']
  if w&0xffe0001f in [0xd4000002,0xd4000003]:patches.append((va,branch(va,symbols['NtPanic']),'firmware instruction fail fence'))
for name,va,target in [('native timer sysreg fix',0xfffffff007a564a8,0xfffffff007a565bc),('GIC hardware tail replacement',0xfffffff00747487c,symbols['NtGicEntry']),('first native thread entry stop',syms['_kernel_bootstrap_thread'],symbols['NtFirstThread']),('panic terminal RAM exit',syms['_panic'],symbols['NtPanic'])]:patches.append((va,branch(va,target),name))
# Original loader_jump lives inside replaced cave; all legacy return entrypoints
# are unreachable in successful path. Early vector/error gets a direct own stop.
for seg in segs:
 if seg['name']!='__TEXT_EXEC':continue
 for off in range(seg['off'],seg['off']+seg['files']-3,4):
  va=seg['va']+off-seg['off'];w=struct.unpack_from('<I',k,off)[0]
  if base<=va<end or w&0x7c000000!=0x14000000:continue
  imm=w&0x3ffffff;imm=imm-(1<<26) if imm&(1<<25) else imm
  if va+4*imm in [syms['_loader_jump'],syms['_loader_return']]:patches.append((va,branch(va,symbols['NtPanic']),'legacy return call rerouted own RAM stop'))
rows=[];hdr=['/* GENERATED by build_native.py; all writes are staged-RAM only. */','static const unsigned char nt_payload_hash[]={'+','.join('0x%02x'%b for b in hashlib.sha256(payload).digest())+'};']
for i,(va,new,name) in enumerate(patches):
 o=old(va,len(new));rows.append(dict(name=name,va=hex(va),pa=hex(0x63000000+va-0xfffffff007000000),old=o.hex(),new=new.hex()))
 for label,data in [('old',o),('new',new)]:hdr.append('static const unsigned char nt_%s_%d[]={%s};'%(label,i,','.join('0x%02x'%b for b in data)))
hdr.append('typedef struct { uint64_t pa,n;const unsigned char *old,*newbytes;} NtPatch;')
hdr.append('static const NtPatch nt_patches[]={'+','.join('{UINT64_C(%s),%d,nt_old_%d,nt_new_%d}'%(r['pa'],len(bytes.fromhex(r['new'])),i,i) for i,r in enumerate(rows))+'};')
(P/'native_patches.h').write_text('\n'.join(hdr)+'\n')
(H/'runtime_manifest.json').write_text(json.dumps(dict(input_image_sha256=q.sha(image.read_bytes()),kernel_sha256=q.sha(k),payload_sha256=q.sha(payload),bridge_symbols={n:hex(v) for n,v in symbols.items()},patches=rows),indent=2)+'\n')
