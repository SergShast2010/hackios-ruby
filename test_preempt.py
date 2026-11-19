#!/usr/bin/env python3
"""Observe the exact v0.41 QEMU-only preparation image. This is not an IRQ pass."""
import argparse
import gzip
import json
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
from tempfile import TemporaryDirectory, mkdtemp
import time

from fat_reader import fat_path
from probe_support import RSP
import qemu_helpers as q
from v41_artifacts import (IMAGE, KERNEL_SHA, LINK, OUT, ROOT, STAGE, STAGE_BYTES,
                           check_patches, firmware_instruction, sha)

H = ROOT
NAMES = ('magic callback profile d r ctlr waker group modifier priority enable pending active '
         'sre igrpen pmr bpr iccctl timer_ctl cval cntkctl mapped_d mapped_r gic_touched gic_ready reason '
         'restored restore_error thread native_sp native_sctlr native_vbar entry_sp entry_tpidr entry_daif '
         'exit_guard stage entry_x0 entry_lr ap1r0 entry_spsel entry_pc rpr switch_magic boot_before '
         'boot_sp_before processor idle_thread second second_sp second_daif second_wait_result boot_after '
         'boot_sp_after switch_pass phase second_sctlr second_vbar boot_daif_after vector_patch preempt_iar '
         'preempt_elr preempt_spsr preempt_count preempt_phase preempt_error panic_lr last_esr last_far last_elr').split()
STATE = struct.Struct('<70Q')
RELEASE = 'M40180050,8:0100000000000000'


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_artifacts(image):
    directory = image.parent
    release = json.loads((directory / 'build.json').read_text())
    manifest_path, map_path = (directory / name for name in ('runtime_manifest.json', 'loader.map'))
    manifest = json.loads(manifest_path.read_text())
    require(release['qemu_only'] and release['stage'] == 'prepare-only', 'not the v0.41 preparation image')
    for key, path in (('image_sha256', image), ('runtime_manifest_sha256', manifest_path),
                      ('loader_map_sha256', map_path)):
        require(release[key] == sha(path.read_bytes()), f'{path.name} does not match build.json')
    check_patches(manifest['patches'])
    app, fat = q.final_app(image)
    require(sha(app) == release['app_sha256'] and app == fat_path(fat, [b'EFI        ', b'BOOT       ', b'BOOTAA64EFI']),
            'the two embedded EFI copies must match this build')
    payload = fat_path(fat, [b'PAYLOAD XNP'])
    kernel, segments, syms, _ = q.kernel_metadata(payload)
    require(sha(payload) == manifest['payload_sha256'] == release['payload_sha256'] and
            sha(kernel) == KERNEL_SHA == manifest['kernel_sha256'], 'wrong payload/kernel')
    for row in manifest['patches']:
        va, old = int(row['va'], 16), bytes.fromhex(row['old'])
        seg = next((seg for seg in segments if seg['name'] == '__TEXT_EXEC' and
                    seg['va'] <= va < va + len(old) <= seg['va'] + seg['files']), None)
        require(seg is not None, f'non-executable patch {row["name"]}')
        off = va - seg['va'] + seg['off']
        require(kernel[off:off + len(old)] == old, f'wrong old bytes {row["name"]}')
    return release, manifest, q.map_syms(map_path), app, payload, syms


def check_observations(events, state, inherited):
    names = [event['name'] for event in events]
    unexpected = {'_panic', 'NtPanic', '_PE_init_iokit', 'NtPreemptIrq', 'NtPreemptBody', 'UNKNOWN'}
    require(not unexpected.intersection(names), names)
    if inherited in (2, 4):
        require(names == ['APP_POST_PHYSICAL', 'APP_SNAPSHOT_STOP'] and not state['magic'], names)
        return
    expected = ['_sched_init', '_task_init', '_thread_init', '_load_context', '_machine_load_context',
                '_kernel_bootstrap_thread', 'NtSwitchBoot', '_idle_thread_create', '_sched_startup',
                '_kernel_thread_start_priority', 'NtSwitchSecond', 'NtSwitchResumed', 'NtPreemptArm',
                'NtSwitchPrepared', 'APP_LOW_RETURN', 'APP_TERMINAL']
    observed = iter(names)
    require(all(name in observed for name in expected) and names[-1] == 'APP_TERMINAL', names)
    wanted = dict(gic_ready=1, reason=0x40, restored=1, restore_error=0, gic_touched=0, switch_pass=1,
                  phase=6, preempt_phase=3, preempt_count=0, preempt_iar=0, preempt_elr=0, preempt_spsr=0,
                  preempt_error=0, panic_lr=0, last_esr=0, last_far=0, last_elr=0)
    require(all(state[key] == value for key, value in wanted.items()), state)
    boot, second = state['boot_before'], state['second']
    require(boot and second and boot == state['boot_after'] == state['entry_tpidr'] == state['thread'] != second,
            'thread identity did not round trip')
    require(state['boot_sp_before'] == state['boot_sp_after'] != state['second_sp'], 'stack did not round trip')
    for name, target in (('_thread_invoke', 'x1'), ('_machine_switch_context', 'x2'), ('_Switch_context', 'x2')):
        seq = [event['regs'] for event in events if event['name'] == name]
        require([(r['x0'], r[target], r['TPIDR_EL1']) for r in seq] == [(boot, second, boot), (second, boot, second)],
                f'{name}: not two native cooperative switches')
    for name in ('NtSwitchSecond', 'NtSwitchResumed'):
        event = next(event for event in events if event['name'] == name)
        saved = event['other_thread_saved_context']
        require(saved['words'][11] == 0xfffffff007468450 and saved['words'][12] and saved['task'] == event['self_task'],
                (name, saved))
    require(state['second_wait_result'] & 0xffffffff == 0xffffffff and all(e['regs']['cpsr'] & 128 for e in events),
            'expected second thread waiting and IRQ masked throughout')
    require(not events[-1]['regs']['SCTLR_EL1'] & 0x1005, 'expected MMU and caches off on terminal return')
    require(state['igrpen'] == 0 and state['pmr'] == {0: 0, 1: 0, 3: 0xf0, 5: 0x80, 6: 0xf0}[inherited] and
            state['rpr'] == (0 if inherited in (1, 6) else 255), 'wrong inherited fixture')


def audit(log, arm, expect_prepared):
    reached = set()
    gic_reads = group_writes = 0
    completed = False
    with log.open(encoding='ascii') as stream:
        for line in stream:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == 'END':
                completed = True
            if parts[0] == 'M':
                pa = int(parts[4], 16)
                if 0x08000000 <= pa < 0x08010000 or 0x080a0000 <= pa < 0x082a0000:
                    require(parts[2] != 'W', line)
                    gic_reads += 1
            if parts[0] != 'I':
                continue
            pc, word = (int(part, 16) for part in parts[1:3])
            if not (LINK <= pc < LINK + STAGE_BYTES or STAGE <= pc < STAGE + STAGE_BYTES):
                continue
            if arm <= pc <= arm + 8:
                reached.add(pc)
            op = word & 0xffffffe0
            # Use opcodes: QEMU's GDB XML does not expose these GIC sysregs.
            require(not firmware_instruction(word) and op not in (0xd538cc00, 0xd518cc20, 0xd518cb20), line)
            require(not (word & 0xfffff0ff == 0xd50340ff and word & 0x200), line)
            if word >> 24 == 0xd5:
                regs = dict(item.split('=', 1) for item in parts[3:])
                require(int(regs['CPSR'], 16) & 128, line)
                if op in (0xd518cce0, 0xd51b4220, 0xd5184000):  # MSR ICC_IGRPEN1 / DAIF / SPSR
                    rt = word & 31
                    value = 0 if rt == 31 else int(regs[f'x{rt}'], 16)
                    if op == 0xd518cce0:
                        require(value == 0, line)
                        group_writes += 1
                    else:
                        require(value & 128, line)
    require(completed, 'truncated passive audit')
    if expect_prepared:
        require({arm, arm + 4, arm + 8} <= reached, 'trace did not execute the first three arm instructions')
        require(gic_reads and group_writes, 'native GIC binding/exit not observed')
    return {'native_gic_reads': gic_reads, 'native_gic_writes': 0, 'native_group1_closed_writes': group_writes,
            'native_irq_unmask_ack_eoi_dir_observed': False,
            'arm_entry_words_observed': sorted(hex(a) for a in reached)}


def run(args):
    release, manifest, symbols, app, payload, syms = read_artifacts(args.image.resolve())
    ns = {k: int(v, 16) for k, v in manifest['bridge_symbols'].items()}
    base = args.base
    out = args.results.resolve()
    out.mkdir(parents=True, exist_ok=True)
    run_dir = Path(mkdtemp(prefix=f'{base:x}-inherited-{args.inherited}-', dir=out))
    preferred, loaded = q.relocate(app, base)
    rv = lambda name: symbols[name] - preferred
    (run_dir / 'app.bin').write_bytes(loaded)
    (run_dir / 'payload.bin').write_bytes(payload)
    flags = ['clang', '--target=aarch64-linux-gnu', '-fuse-ld=lld', '-nostdlib', '-ffreestanding',
             '-fno-builtin', '-fno-stack-protector', '-mgeneral-regs-only', '-O1', '-Wall', '-Wextra', '-Werror']
    subprocess.run([*flags, '-I', H / 'ebsprobe', '-Wl,-T,' + str(H / 'qemu-test/harness.ld'), '-Wl,--build-id=none',
                    H / 'qemu-test/start.S', H / 'qemu-test/harness.c', '-o', run_dir / 'harness.elf'], check=True)
    glib = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
    subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-DQEMU_AUDIT_PLUGIN', H / 'qemu-test/harness.c',
                    '-o', run_dir / 'audit.so', *glib], check=True)
    cfg = [0] * 64
    cfg[:5] = [base, rv('EnterPhysical'), rv('screen'), rv('PrepareXnu'), len(payload)]
    cfg[6] = 0 if args.negative_phone_guard else rv('NtTestProfile')
    cfg[63] = args.inherited
    (run_dir / 'config.bin').write_bytes(struct.pack('<64Q', *cfg))

    with TemporaryDirectory(prefix='r41-') as socket_dir:
        sock, qmp_sock = (Path(socket_dir) / name for name in ('gdb.sock', 'qmp.sock'))
        cmd = ['qemu-system-aarch64', '-M', 'virt,gic-version=3,virtualization=off,secure=off', '-cpu', 'cortex-a55',
               '-smp', '1', '-m', '1024M', '-accel', 'tcg,thread=single', '-display', 'none', '-serial', 'stdio',
               '-monitor', 'none', '-nic', 'none', '-kernel', str(run_dir / 'harness.elf'),
               '-device', f'loader,file={run_dir}/app.bin,addr={base:#x}',
               '-device', f'loader,file={run_dir}/payload.bin,addr=0x66000000',
               '-device', f'loader,file={run_dir}/config.bin,addr=0x40180000',
               '-gdb', f'unix:{sock},server=on,wait=off', '-qmp', f'unix:{qmp_sock},server=on,wait=off',
               '-plugin', f'{run_dir}/audit.so,log={run_dir}/audit.log,callback={base+rv("XnuReturn"):#x},'
                          f'end={base+rv("XnuReturn")+0x1b0:#x},checkpoint={base+rv("NativeTerminalDone"):#x}']
        report = {key: release[key] for key in ('image_sha256', 'app_sha256', 'payload_sha256', 'kernel_sha256',
                                                'runtime_manifest_sha256', 'stage', 'qemu_only')}
        report.update(command=cmd, events=[], negative_phone_guard=args.negative_phone_guard,
                      observer_sha256=sha((run_dir / 'audit.so').read_bytes()),
                      observer_source_sha256=sha((H / 'qemu-test/harness.c').read_bytes()))
        report['pass'] = False
        r = qm = None
        with (run_dir / 'uart.log').open('wb') as uart, (run_dir / 'qemu.log').open('wb') as qlog:
            proc = subprocess.Popen(cmd, stdout=uart, stderr=qlog)
            try:
                qm = q.QMP(str(qmp_sock))
                deadline = time.monotonic() + 30
                while True:
                    qm.cmd('stop')
                    qm.cmd('pmemsave', val=0x40180000, size=512, filename=str(run_dir / 'cfg-live.bin'))
                    words = struct.unpack('<64Q', (run_dir / 'cfg-live.bin').read_bytes())
                    if words[8] in (2, 3):
                        break
                    require(time.monotonic() < deadline, 'timeout waiting for PrepareXnu')
                    qm.cmd('cont')
                    time.sleep(.03)
                r = RSP(str(sock))
                report['cfg'], report['regs_pre'] = words, r.registers()
                if args.negative_phone_guard:
                    require(words[8:10] == (3, 0x800000000000001a) and words[29:31] == (0, 0),
                            'fixture-less PrepareXnu must refuse without claiming source or destination')
                    require(not r.writes, 'observer must not release failed preparation')
                    report['negative_phone_guard_rejected'] = True
                else:
                    require(words[8:10] == (2, 0), 'fixture preparation failed')
                    staged_path = run_dir / 'staged-patches.bin'
                    first = min(int(row['pa'], 16) for row in manifest['patches'])
                    end = max(int(row['pa'], 16) + len(bytes.fromhex(row['new'])) for row in manifest['patches'])
                    qm.cmd('pmemsave', val=first, size=end-first, filename=str(staged_path))
                    staged = staged_path.read_bytes()
                    for row in manifest['patches']:
                        offset, expected = int(row['pa'], 16) - first, bytes.fromhex(row['new'])
                        require(staged[offset:offset+len(expected)] == expected, f'staged bytes differ: {row["name"]}')
                    staged_path.unlink()
                    report['staged_patches_verified'] = len(manifest['patches'])
                    track(r, args, report, syms, ns, base, rv)
                    qm.cmd('pmemsave', val=STAGE + 0xe7d800, size=STATE.size, filename=str(run_dir / 'native-state.bin'))
                    state = dict(zip(NAMES, STATE.unpack((run_dir / 'native-state.bin').read_bytes()), strict=True))
                    report['native_state'] = {k: hex(v) for k, v in state.items()}
                    if args.screenshot:
                        draw(qm, run_dir)
                    check_observations(report['events'], state, args.inherited)
                    if args.inherited not in (2, 4):
                        vector = next(row for row in manifest['patches'] if int(row['va'], 16) == 0xfffffff00745a080)
                        require(state['vector_patch'] == int.from_bytes(bytes.fromhex(vector['new']), 'little'),
                                'live EL1t vector differs from the manifest')
                    writes = [c for c in r.writes if not c.startswith(('Z1,', 'z1,'))]
                    require(writes == [RELEASE], f'unexpected debugger writes: {writes}')
                    report['observer_fixture_release_only'] = True
            except Exception as error:
                report['error'] = f'{type(error).__name__}: {error}'
            finally:
                if r:
                    report['debug_writes'] = r.writes
                    r.sock.close()
                if qm:
                    try:
                        qm.cmd('quit')
                    except (OSError, RuntimeError):
                        pass
                    qm.file.close()
                    qm.sock.close()
                try:
                    proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()

    log = run_dir / 'audit.log'
    if 'error' not in report:
        try:
            report['audit'] = audit(log, ns['NtPreemptArm'], not args.negative_phone_guard and args.inherited not in (2, 4))
            report['pass'] = True
        except Exception as error:
            report['error'] = f'{type(error).__name__}: {error}'
    if log.exists():
        with log.open('rb') as src, gzip.open(run_dir / 'audit.log.gz', 'wb', compresslevel=6) as dst:
            shutil.copyfileobj(src, dst)
        log.unlink()
    for name in ('staged-patches.bin', 'app.bin', 'payload.bin'):
        (run_dir / name).unlink(missing_ok=True)
    (run_dir / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='ascii')
    print('PASS' if report['pass'] else 'FAIL', run_dir, report.get('error', ''))
    return report['pass']


def track(r, args, report, syms, ns, base, rv):
    points = {syms[n]: n for n in ('_sched_init', '_task_init', '_thread_init', '_load_context', '_machine_load_context',
                                   '_kernel_bootstrap_thread', '_panic', '_PE_init_iokit', '_idle_thread_create',
                                   '_sched_startup', '_kernel_thread_start_priority', '_thread_invoke',
                                   '_machine_switch_context', '_Switch_context')}
    points.update({ns[n]: n for n in ('NtSwitchBoot', 'NtSwitchSecond', 'NtSwitchResumed', 'NtPreemptArm',
                                     'NtSwitchPrepared', 'NtPanic', 'NtPreemptIrq', 'NtPreemptBody')})
    points.update({base + rv(n): label for n, label in (('NativeTerminalDone', 'APP_TERMINAL'),
                   ('NativeSnapshotStopped', 'APP_SNAPSHOT_STOP'), ('XnuReturn', 'APP_LOW_RETURN'),
                   ('PostPhysical', 'APP_POST_PHYSICAL'))})
    for address in points:
        require(r.command(f'Z1,{address:x},4') == 'OK', f'cannot observe {address:#x}')
    r.write(0x40180050, struct.pack('<Q', 1))
    deadline = time.monotonic() + args.seconds
    while len(report['events']) < 160:
        remaining = deadline - time.monotonic()
        require(remaining > 0, 'timeout waiting for guest terminal')
        r.command('c', timeout=remaining)
        regs = r.registers()
        pc = regs['pc']
        name = points.get(pc, 'UNKNOWN')
        event = {'name': name, 'regs': regs}
        if name in ('NtSwitchSecond', 'NtSwitchResumed'):
            previous = 'NtSwitchBoot' if name == 'NtSwitchSecond' else 'NtSwitchSecond'
            other = next(e['regs']['TPIDR_EL1'] for e in report['events'] if e['name'] == previous)
            context, = struct.unpack('<Q', r.read(other + 0x158, 8))
            event['other_thread_saved_context'] = {'thread': hex(other), 'context': hex(context),
                                                  'words': struct.unpack('<14Q', r.read(context, 112)),
                                                  'task': hex(struct.unpack('<Q', r.read(other + 0x428, 8))[0])}
            event['self_task'] = hex(struct.unpack('<Q', r.read(regs['TPIDR_EL1'] + 0x428, 8))[0])
        report['events'].append(event)
        print(name, hex(pc), 'tp', hex(regs['TPIDR_EL1']), 'sp', hex(regs['sp']), flush=True)
        if name in ('APP_TERMINAL', 'APP_SNAPSHOT_STOP', 'UNKNOWN', '_panic', 'NtPanic', 'NtPreemptIrq'):
            return
        require(r.command(f'z1,{pc:x},4') == 'OK', f'cannot remove breakpoint {pc:#x}')
        r.command('s', timeout=max(0.01, deadline - time.monotonic()))
        require(r.command(f'Z1,{pc:x},4') == 'OK', f'cannot restore breakpoint {pc:#x}')
    raise AssertionError('too many stops without terminal')


def draw(qm, run_dir):
    from PIL import Image
    frame = run_dir / 'framebuffer.bin'
    qm.cmd('pmemsave', val=0x7c406000, size=1080 * 2400 * 4, filename=str(frame))
    image = Image.frombytes('RGBA', (1080, 2400), frame.read_bytes(), 'raw', 'BGRA').convert('RGB')
    image.resize((540, 1200)).save(run_dir / 'screen-preview.png')
    frame.unlink()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=OUT / IMAGE)
    parser.add_argument('--results', type=Path, default=OUT / 'tests')
    parser.add_argument('--inherited', type=int, choices=range(7), default=3)
    parser.add_argument('--base', type=lambda n: int(n, 0), default=0x44000000)
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--negative-phone-guard', action='store_true')
    parser.add_argument('--screenshot', action='store_true')
    args = parser.parse_args()
    raise SystemExit(0 if run(args) else 1)
