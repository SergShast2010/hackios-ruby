#!/usr/bin/env python3
"""Validate exact image, original kernel, byte-checked RAM patches and passive audits.
The audit log is a selective instruction/system-operation log, NOT a full trace.
All native memory callbacks still observe external MMIO; all system/firmware
instructions and independent fences are selected by the observer.
"""
from pathlib import Path
import json,sys,struct,gzip,hashlib,subprocess,shlex,tempfile
from collections import Counter
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
H=Path(__file__).resolve().parent;sys.path.insert(0,str(H))
import qemu_helpers as q
from fat_reader import fat_path
image=H/'Mu-ruby-v0.40-native-switch.img';app,fat=q.final_app(image)
payload=fat_path(fat,[b'PAYLOAD XNP']);kernel,segs,syms,_=q.kernel_metadata(payload)
manifest=json.loads((H/'runtime_manifest.json').read_text());build=json.loads((H/'build.json').read_text())
sha=lambda p:hashlib.sha256(p).hexdigest()
assert sha(image.read_bytes())==build['image_sha256']
assert sha(kernel)==manifest['kernel_sha256']=='9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c'
assert sha(payload)==manifest['payload_sha256']==build['payload_sha256']
patches=[]
for p in manifest['patches']:
 a=int(p['va'],16);o=bytes.fromhex(p['old']);n=bytes.fromhex(p['new']);assert len(o)==len(n)
 off=next(s['off']+a-s['va'] for s in segs if s['va']<=a and a+len(o)<=s['va']+s['files'])
 assert kernel[off:off+len(o)]==o,p['name']
 assert int(p['pa'],16)==0x63000000+a-0xfffffff007000000
 patches.append((a,a+len(n),p['name']))
for a,b,n in patches:
 assert all(d<=a or b<=c or (a,b,n)==(c,d,m) for c,d,m in patches),(hex(a),n)
critical_ranges={}
for name in ['_thread_invoke','_machine_switch_context','_Switch_context','_kernel_thread_create','_kernel_thread_start_priority','_idle_thread_create','_thread_block_reason','_assert_wait']:
 a=syms[name];b=min(v for v in syms.values() if v>a)
 assert not any(c<b and a<d for c,d,n in patches),(name,'native routine range patched')
 critical_ranges[name]=[hex(a),hex(b)]
assert all(not any(c<=syms[name]<d for c,d,n in patches) for name in ['_sched_init','_task_init','_thread_init','_load_context','_machine_load_context'])
D=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
results=[]
for folder in sorted((H/'tests').iterdir()):
 if not (folder/'result.json').exists():continue
 r=json.loads((folder/'result.json').read_text());assert r['pass'],str(folder)
 assert r['image_sha256']==build['image_sha256']
 assert r['kernel_sha256']==sha(kernel) and r['payload_sha256']==sha(payload)
 assert r['observer_source_sha256']==sha((H/'qemu-test/harness.c').read_bytes())
 assert r['observer_sha256']==sha((folder/'audit.so').read_bytes())
 assert all(c.startswith(('Z1,','z1,')) or c=='M40180050,8:0100000000000000' for c in r['debug_writes'])
 data_writes=[c for c in r['debug_writes'] if c.startswith('M')]
 expected=[] if r.get('negative_phone_guard_rejected') else ['M40180050,8:0100000000000000']
 assert data_writes==expected
 errors=[];counts=Counter();sysops=Counter();ops=[]
 with gzip.open(folder/'audit.log.gz','rt') as log:
  for line in log:
   p=line.split()
   if not p:continue
   if p[0]=='I':
    counts['audited_instruction_records']+=1;pc=int(p[1],16);w=int(p[2],16)
    regs={k.lower():int(v,16) for item in p[3:] if '=' in item for k,v in [item.split('=',1)]}
    if 'cpsr' not in regs:
     counts['instruction_records_without_register_snapshot']+=1
     if (w&0xff000000)==0xd5000000:errors.append(('missing CPSR on selected system instruction',line.strip()))
    elif not regs['cpsr']&128:errors.append(('IRQ observed unmasked',line.strip()))
    if w&0xffe0001f in (0xd4000002,0xd4000003):errors.append(('SMC/HVC',line.strip()))
    if w&0xfffff0ff==0xd50340ff and ((w>>8)&15)&2:errors.append(('DAIFClr IRQ',line.strip()))
    for a,b,n in patches:
     if n.startswith('independent ') and pc==a:errors.append(('independent fence entered',n,line.strip()))
    for ins in D.disasm(struct.pack('<I',w),pc):
     if ins.mnemonic in ('msr','mrs'):
      operands=ins.op_str.split(',');sysops[ins.mnemonic+' '+operands[0 if ins.mnemonic=='msr' else -1].strip()]+=1
     if ins.mnemonic=='msr' and ',' in ins.op_str:
      target,source=[s.strip().lower() for s in ins.op_str.split(',',1)]
      if target in ('daif','spsr_el1','icc_igrpen1_el1'):
       val=0 if source=='xzr' else regs.get(source)
       if val is None:errors.append(('missing source register',ins.op_str,line.strip()))
       elif target=='icc_igrpen1_el1' and val!=0:errors.append(('Group1 opened',line.strip()))
       elif target in ('daif','spsr_el1') and not val&128:errors.append(('IRQ unmask through saved flags',line.strip()))
   elif p[0]=='M':
    counts['native_memory_callback_records']+=1;op=p[2];pa=int(p[4],16);n=int(p[5]);gic=(0x8000000<=pa and pa+n<=0x8010000) or (0x80a0000<=pa and pa+n<=0x82a0000)
    if gic:
     counts['gic_'+('writes' if op=='W' else 'reads')]+=1
     if op=='W':errors.append(('GIC MMIO write',line.strip()))
    else:
     base=int(r['cfg'][0]);allowed=(0x63000000<=pa and pa+n<=0x65000000 or 0x62000000<=pa and pa+n<=0x62010000 or base<=pa and pa+n<=base+0x200000 or 0x7c406000<=pa and pa+n<=0x7c406000+1080*2400*4)
     if not allowed:errors.append(('outside owned RAM',line.strip()))
   elif p[0]=='IO':
    counts['external_mmio_records']+=1;pa=int(p[3],16)
    if not (0x8000000<=pa<0x8010000 or 0x80a0000<=pa<0x82a0000):errors.append(('external MMIO outside GIC fixture',line.strip()))
 positive=bool(r.get('native_state',{}).get('switch_pass')=='0x1')
 if positive:
  assert counts['audited_instruction_records']>100 and counts['gic_reads']>0
  assert r['native_state']['restored']=='0x1' and r['native_state']['restore_error']=='0x0'
  assert r['native_state']['reason']=='0x40'
 assert not errors,(folder,errors[:20])
 proof={'pass':True,'test':folder.name,'positive_roundtrip':positive,'image_sha256':r['image_sha256'],'counts':dict(counts),'system_operations':dict(sysops),'errors':errors,'observer_fixture_release_only':True,'limitations':'QEMU fixture only; selected instruction audit is not a full trace; no phone proof or preemption claim'}
 (folder/'audit-summary.json').write_text(json.dumps(proof,indent=2)+'\n');results.append(proof)
# Rebuild actual observer bytes from delivered source and exact compile arguments.
with tempfile.TemporaryDirectory(prefix='observer-rebuild-',dir=H) as temp:
 out=Path(temp)/'audit.so';cmd=['cc','-shared','-fPIC','-O2','-DQEMU_AUDIT_PLUGIN',str(H/'qemu-test/harness.c'),'-o',str(out)]+shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True))
 subprocess.run(cmd,check=True);observer_hash=sha(out.read_bytes())
 assert all(json.loads((H/'tests'/r['test']/'result.json').read_text())['observer_sha256']==observer_hash for r in results)
summary={'pass':True,'image_sha256':build['image_sha256'],'app_sha256':sha(app),'payload_sha256':sha(payload),'original_kernel_sha256':sha(kernel),'runtime_patch_count':len(patches),'all_patch_old_bytes_match_original':True,'native_switch_routine_entries_unpatched':True,'native_switch_routine_ranges_unpatched':critical_ranges,'observer_rebuild_identical':True,'observer_sha256':observer_hash,'observer_source_sha256':sha((H/'qemu-test/harness.c').read_bytes()),'tests':results,'limitations':'No phone execution. Cooperative bounded diagnostic only, IRQ masked/Group1 closed, not preemption, no IOKit/PSCI/storage bring-up.'}
(H/'verification').mkdir(exist_ok=True);(H/'verification/final-proof.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
