#!/usr/bin/env python3
"""Test exact final v40 image. The only guest data write is fixture-release=1.
Hardware breakpoints/single-steps observe actual execution; no PC/register injection.
"""
from pathlib import Path
import sys,struct,json,subprocess,time,shlex,gzip,shutil,argparse
H=Path(__file__).resolve().parent;sys.path.insert(0,str(H))
import qemu_helpers as q
from fat_reader import fat_path
from probe_support import RSP
p=argparse.ArgumentParser();p.add_argument('--inherited',type=int,default=3,choices=range(7));p.add_argument('--base',type=lambda v:int(v,0),default=0x44000000);p.add_argument('--seconds',type=int,default=60);p.add_argument('--negative-phone-guard',action='store_true');args=p.parse_args()
class ExpectedGuardStop(Exception):pass
O=H/'tests'/(f'{args.base:x}-inherited-{args.inherited}'+('-phone-guard' if args.negative_phone_guard else ''));O.mkdir(parents=True,exist_ok=True)
image=H/'Mu-ruby-v0.40-native-switch.img';app,fat=q.final_app(image)
assert app==(H/'ebsprobe/loader.efi').read_bytes()
payload=fat_path(fat,[b'PAYLOAD XNP']);kernel,segs,syms,_=q.kernel_metadata(payload)
assert q.sha(kernel)=='9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c'
ab=args.base;_,loaded=q.relocate(app,ab);m=q.map_syms(H/'ebsprobe/loader.map');rv=lambda n:m[n]-0xa1000000
(O/'app.bin').write_bytes(loaded);(O/'payload.bin').write_bytes(payload)
subprocess.run(['clang','--target=aarch64-linux-gnu','-fuse-ld=lld','-nostdlib','-ffreestanding','-fno-builtin','-fno-stack-protector','-mgeneral-regs-only','-O1','-Wall','-Wextra','-Werror','-I',str(H/'ebsprobe'),'-Wl,-T,'+str(H/'qemu-test/harness.ld'),'-Wl,--build-id=none',str(H/'qemu-test/start.S'),str(H/'qemu-test/harness.c'),'-o',str(O/'harness.elf')],check=True)
subprocess.run(['cc','-shared','-fPIC','-O2','-DQEMU_AUDIT_PLUGIN',str(H/'qemu-test/harness.c'),'-o',str(O/'audit.so')]+shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True)),check=True)
cfg=[0]*64;cfg[:5]=[ab,rv('EnterPhysical'),rv('screen'),rv('PrepareXnu'),len(payload)];cfg[6]=0 if args.negative_phone_guard else rv('NtTestProfile');cfg[63]=args.inherited;(O/'config.bin').write_bytes(struct.pack('<64Q',*cfg))
sock=O/'gdb.sock';sock.unlink(missing_ok=True);(O/'qmp.sock').unlink(missing_ok=True)
uart=(O/'uart.log').open('wb');qlog=(O/'qemu.log').open('wb')
cmd=['qemu-system-aarch64','-M','virt,gic-version=3,virtualization=off,secure=off','-cpu','cortex-a55','-smp','1','-m','1024M','-accel','tcg,thread=single','-display','none','-serial','stdio','-monitor','none','-nic','none','-kernel',str(O/'harness.elf'),'-device',f'loader,file={O}/app.bin,addr={ab:#x}','-device',f'loader,file={O}/payload.bin,addr=0x66000000','-device',f'loader,file={O}/config.bin,addr=0x40180000','-gdb','unix:'+str(sock)+',server=on,wait=off','-qmp','unix:'+str(O/'qmp.sock')+',server=on,wait=off','-plugin',str(O/'audit.so')+',log='+str(O/'audit.log')+',callback='+hex(ab+rv('XnuReturn'))+',end='+hex(ab+rv('XnuReturn')+0x1b0)+',checkpoint='+hex(ab+rv('NativeTerminalDone'))]
report={'command':cmd,'image_sha256':q.sha(image.read_bytes()),'app_sha256':q.sha(app),'payload_sha256':q.sha(payload),'kernel_sha256':q.sha(kernel),'observer_sha256':q.sha((O/'audit.so').read_bytes()),'observer_source_sha256':q.sha((H/'qemu-test/harness.c').read_bytes()),'events':[],'pass':False}
proc=subprocess.Popen(cmd,stdout=uart,stderr=qlog);r=None;qm=None
try:
 qm=q.QMP(str(O/'qmp.sock'));deadline=time.monotonic()+30
 while True:
  qm.cmd('stop');qm.cmd('pmemsave',val=0x40180000,size=512,filename=str(O/'cfg-live.bin'));st=struct.unpack('<64Q',(O/'cfg-live.bin').read_bytes())
  if st[8] in (2,3):break
  assert time.monotonic()<deadline,st
  qm.cmd('cont');time.sleep(.03)
 r=RSP(str(sock));report['cfg']=st;report['regs_pre']=r.registers()
 if args.negative_phone_guard:
  assert st[8]==3 and st[9]!=0
  report['negative_phone_guard_rejected']=True;report['pass']=True
  raise ExpectedGuardStop()
 assert st[8]==2 and st[9]==0
 points={syms[n]:n for n in ['_gic_v3_irq_cpu_init','_sched_init','_task_init','_thread_init','_rtclock_init','_load_context','_machine_load_context','_kernel_bootstrap_thread','_panic','_PE_init_iokit','_idle_thread_create','_sched_startup','_kernel_thread_start_priority','_thread_block_reason','_thread_invoke','_machine_switch_context','_Switch_context','_thread_continue','_thread_dispatch','_assert_wait']}
 ns={k:int(v,16) for k,v in json.loads((H/'runtime_manifest.json').read_text())['bridge_symbols'].items()}
 for n in ['NtSwitchBoot','NtSwitchSecond','NtSwitchResumed']:points[ns[n]]=n
 for n,label in [('NativeTerminalDone','APP_TERMINAL'),('NativeSnapshotStopped','APP_SNAPSHOT_STOP'),('XnuReturn','APP_LOW_RETURN'),('PostPhysical','APP_POST_PHYSICAL')]:points[ab+rv(n)]=label
 for addr in points:r.command(f'Z1,{addr:x},4')
 r.write(0x40180050,struct.pack('<Q',1));start=time.monotonic()
 while time.monotonic()-start<args.seconds and len(report['events'])<160:
  r.command('c');regs=r.registers();pc=regs['pc'];name=points.get(pc,'UNKNOWN')
  ev={'name':name,'regs':regs}
  if name in ('NtSwitchSecond','NtSwitchResumed'):
   prior='NtSwitchBoot' if name=='NtSwitchSecond' else 'NtSwitchSecond'
   other=next(e['regs']['TPIDR_EL1'] for e in report['events'] if e['name']==prior)
   context=struct.unpack('<Q',r.read(other+0x158,8))[0]
   ev['other_thread_saved_context']={'thread':hex(other),'context':hex(context),'words':list(struct.unpack('<14Q',r.read(context,112))),'task':hex(struct.unpack('<Q',r.read(other+0x428,8))[0])}
   ev['self_task']=hex(struct.unpack('<Q',r.read(regs['TPIDR_EL1']+0x428,8))[0])
  report['events'].append(ev);print(name,hex(pc),'tp',hex(regs.get('TPIDR_EL1',0)),'sp',hex(regs.get('sp',0)),flush=True)
  if name in ('APP_TERMINAL','APP_SNAPSHOT_STOP','UNKNOWN','_panic'):break
  r.command(f'z1,{pc:x},4');r.command('s');r.command(f'Z1,{pc:x},4')
 qm.cmd('pmemsave',val=0x63e7d800,size=472,filename=str(O/'native-state.bin'))
 raw=(O/'native-state.bin').read_bytes();vals=struct.unpack('<59Q',raw)
 names=['magic','callback','profile','d','r','ctlr','waker','group','modifier','priority','enable','pending','active','sre','igrpen','pmr','bpr','iccctl','timer_ctl','cval','cntkctl','mapped_d','mapped_r','gic_touched','gic_ready','reason','restored','restore_error','thread','native_sp','native_sctlr','native_vbar','entry_sp','entry_tpidr','entry_daif','exit_guard','stage','entry_x0','entry_lr','ap1r0','entry_spsel','entry_pc','rpr','switch_magic','boot_before','boot_sp_before','processor','idle_thread','second','second_sp','second_daif','second_wait_result','boot_after','boot_sp_after','switch_pass','phase','second_sctlr','second_vbar','boot_daif_after']
 state=dict(zip(names,vals));report['native_state']={k:hex(v) for k,v in state.items()}
 events=[e['name'] for e in report['events']]
 qm.cmd('pmemsave',val=0x7c406000,size=1080*2400*4,filename=str(O/'framebuffer.bin'))
 from PIL import Image
 im=Image.frombytes('RGBA',(1080,2400),(O/'framebuffer.bin').read_bytes(),'raw','BGRA').convert('RGB');im.save(O/'screen.png');im.resize((540,1200)).save(O/'screen-preview.png');(O/'framebuffer.bin').unlink()
 assert '_panic' not in events and '_PE_init_iokit' not in events,events
 if args.inherited in (2,4):
  assert events[-1]=='APP_SNAPSHOT_STOP',events
 else:
  assert events[-1]=='APP_TERMINAL',events
  for n in ['_sched_init','_task_init','_thread_init','_load_context','_machine_load_context','_kernel_bootstrap_thread','_idle_thread_create','_sched_startup','_kernel_thread_start_priority','NtSwitchSecond','NtSwitchResumed','APP_LOW_RETURN']:assert n in events,n
  for n in ['_thread_invoke','_machine_switch_context','_Switch_context']:assert events.count(n)>=2,(n,events.count(n))
  assert vals[24:28]==(1,0x40,1,0),vals[24:28]
  assert state['switch_pass']==1 and state['phase']==5
  for name in ['_thread_invoke','_machine_switch_context','_Switch_context']:
   seq=[e['regs'] for e in report['events'] if e['name']==name];assert len(seq)==2
   target='x1' if name=='_thread_invoke' else 'x2'
   assert [(e['x0'],e[target]) for e in seq]==[(state['boot_before'],state['second']),(state['second'],state['boot_before'])]
   assert [e['TPIDR_EL1'] for e in seq]==[state['boot_before'],state['second']]
  for name in ('NtSwitchSecond','NtSwitchResumed'):
   ev=next(e for e in report['events'] if e['name']==name);saved=ev['other_thread_saved_context'];words=saved['words']
   assert words[11]==0xfffffff007468450 and words[12]!=0,(name,saved)
   assert saved['task']==ev['self_task']
  assert state['second_wait_result']&0xffffffff==0xffffffff
  assert state['boot_before']==state['boot_after']==state['entry_tpidr']==state['thread']!=state['second']
  assert state['boot_sp_before']==state['boot_sp_after']!=state['second_sp']
  assert state['second_daif']&128 and state['boot_daif_after']&128
  assert all(e['regs']['cpsr']&128 for e in report['events'])
  assert report['events'][-1]['regs']['SCTLR_EL1']&0x1005==0
  assert state['pmr']=={0:0,1:0,3:0xf0,5:0x80,6:0xf0}[args.inherited]
  assert state['rpr']==(0 if args.inherited in (1,6) else 255)
 assert all(c.startswith(('Z1,','z1,')) or c=='M40180050,8:0100000000000000' for c in r.writes),r.writes
 assert sum(c=='M40180050,8:0100000000000000' for c in r.writes)==1
 report['observer_fixture_release_only']=True
 report['pass']=True
except ExpectedGuardStop:
 pass
finally:
 if r:report['debug_writes']=r.writes
 if qm:
  try:qm.cmd('quit')
  except Exception:pass
 try:proc.wait(timeout=8)
 except subprocess.TimeoutExpired:proc.kill();proc.wait()
 uart.close();qlog.close()
 if (O/'audit.log').exists():
  with (O/'audit.log').open('rb') as src,gzip.open(O/'audit.log.gz','wb',compresslevel=6) as dst:shutil.copyfileobj(src,dst)
  (O/'audit.log').unlink()
 for n in ['payload.bin','app.bin']:(O/n).unlink(missing_ok=True)
 (O/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS' if report['pass'] else 'FAIL',O)
