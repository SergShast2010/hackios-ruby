#!/usr/bin/env python3
import argparse
import json
import struct
import subprocess
from pathlib import Path

from fat_reader import fat_path
import qemu_helpers as q
from v41_artifacts import (ROOT, OUT, KERNEL_BASE, KERNEL_BASE_SHA, KERNEL_SHA, LINK, STAGE,
                           check_patches, check_ranges, covered_by, firmware_instruction, sha)


def branch(va, target):
    delta = target - va
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError(f'branch out of range: {va:#x} -> {target:#x}')
    return struct.pack('<I', 0x14000000 | ((delta >> 2) & 0x3ffffff))


def adrp_add(va, target):
    delta = (target >> 12) - (va >> 12)
    if not -(1 << 20) <= delta < (1 << 20):
        raise ValueError('ADRP out of range')
    imm = delta & 0x1fffff
    return struct.pack('<II', 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5),
                       0x91000000 | ((target & 4095) << 10))


def build(image, out):
    image_sha = sha(image.read_bytes())
    if image_sha != KERNEL_BASE_SHA:
        raise ValueError('native input must be the pinned v0.38b image')
    _, fat = q.final_app(image)
    payload = fat_path(fat, [b'PAYLOAD XNP'])
    kernel, segments, syms, _ = q.kernel_metadata(payload)
    if sha(kernel) != KERNEL_SHA:
        raise ValueError('unexpected Mach-O')

    out.mkdir(parents=True, exist_ok=True)
    elf = out / 'native_bridge.elf'
    flags = ['clang', '--target=aarch64-none-elf', '-ffreestanding', '-fno-builtin',
             '-fno-stack-protector', '-mgeneral-regs-only', '-ffixed-x18', '-mstrict-align',
             '-Oz', '-Wall', '-Wextra', '-Werror', '-fno-pic', '-fno-unwind-tables',
             '-fno-asynchronous-unwind-tables']
    objects = []
    for src in ('native_quiet.c', 'native_bridge.S', 'native_switch.S'):
        obj = out / (src + '.o')
        subprocess.run([*flags, '-c', ROOT / 'ebsprobe' / src, '-o', obj], check=True)
        objects.append(obj)
    subprocess.run(['ld.lld', '-T', ROOT / 'native.ld', *objects, '-o', elf], check=True)
    nm = subprocess.check_output(['aarch64-linux-gnu-nm', '-n', elf], text=True)
    symbols = {name: int(va, 16) for va, _, name in (s.split() for s in nm.splitlines() if len(s.split()) == 3)}

    def original(va, length):
        seg = next((s for s in segments if s['name'] == '__TEXT_EXEC'
                    and s['va'] <= va < va + length <= s['va'] + s['files']), None)
        if seg is None:
            raise ValueError(f'bytes outside file-backed executable text at {va:#x}')
        off = seg['off'] + va - seg['va']
        return kernel[off:off + length]

    patches = []
    occupied = []
    sections = {}
    for name in ('text', 'switch', 'extra'):
        start, end = (symbols[f'__nt_{name}_{key}'] for key in ('start', 'end'))
        limit = syms['_loader_jump'] if name == 'text' else symbols[f'__nt_{name}_limit']
        binary = out / f'native_{name}.bin'
        subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', f'--only-section=.{name}',
                        elf, binary], check=True)
        data = binary.read_bytes()
        if len(data) != end - start or not start < end <= limit or (start | end | limit) & 3:
            raise ValueError(f'invalid ELF layout for .{name}')
        if any(firmware_instruction(w) for w, in struct.iter_unpack('<I', data)):
            raise ValueError(f'unexpected SMC/HVC opcode in generated .{name}')
        if name == 'switch' and sha(original(start, limit - start)) != '6a817a0498bcb501e24c578cfd41cec168a6a2c4f7a396657ffdfeec203d26d3':
            raise ValueError('original .switch tail does not match')
        written = end if name == 'text' else limit
        if written > end:
            data += struct.pack('<I', 0x14000000) * ((written - end) // 4)
        print(f'.{name}: {end - start}/{limit - start} bytes')
        sections[name] = dict(start=hex(start), code_end=hex(end), written_end=hex(written))
        occupied.append((start, written, '.' + name))
        patches.append((start, data, '.' + name))
    check_ranges(occupied)
    if (symbols['NtPanic'] != symbols['__nt_text_start']
            or symbols['NtSwitchFence'] != syms['_loader_jump'] + 4
            or not symbols['__nt_extra_start'] <= symbols['NtPreemptArm'] < symbols['__nt_extra_end']):
        raise ValueError('unexpected native bridge entry layout')

    panic = symbols['NtPanic']
    patches += [
        (0xfffffff007309b10, adrp_add(0xfffffff007309b10, symbols['NtSwitchSecond']), 'bounded sched_startup continuation'),
        (0xfffffff007309b64, branch(0xfffffff007309b64, 0xfffffff007309b98), 'return before powered-core service thread'),
        (0xfffffff007305b00, struct.pack('<I', 0x52800003), 'bounded continuation keeps IRQ masked'),
    ]
    for name in ('_loader_return', '_loader_jump', '_PE_init_iokit', '_hv_wdt_pet'):
        va = syms[name]
        patches.append((va, branch(va, panic), f'independent entry fence {name}'))
    entries = [
        ('native timer sysreg fix', 0xfffffff007a564a8, 0xfffffff007a565bc),
        ('GIC hardware tail replacement', 0xfffffff00747487c, symbols['NtGicEntry']),
        ('native EL1t IRQ vector fence', 0xfffffff00745a080, symbols['NtPreemptIrq']),
        ('native EL1h IRQ vector fence', 0xfffffff00745a280, symbols['NtPreemptIrq']),
        ('first native thread entry stop', syms['_kernel_bootstrap_thread'], symbols['NtFirstThread']),
        ('panic terminal RAM exit', syms['_panic'], panic),
    ]
    patches.extend((va, branch(va, target), name) for name, va, target in entries)

    # Only inspect original words that survive the explicit writes, including padded caves.
    replaced = [(va, va + len(data), name) for va, data, name in patches]
    check_ranges(replaced)
    returns = {syms['_loader_jump'], syms['_loader_return']}
    for seg in segments:
        if seg['name'] != '__TEXT_EXEC':
            continue
        for off in range(seg['off'], seg['off'] + seg['files'] - 3, 4):
            word, = struct.unpack_from('<I', kernel, off)
            firmware = firmware_instruction(word)
            imm = word & 0x3ffffff
            va = seg['va'] + off - seg['off']
            legacy = (word & 0x7c000000) == 0x14000000 and va + 4 * (imm - 0x4000000 if imm & 0x2000000 else imm) in returns
            if (firmware or legacy) and not covered_by(replaced, va):
                name = 'firmware instruction fail fence' if firmware else 'legacy return call rerouted own RAM stop'
                patches.append((va, branch(va, panic), name))

    rows = [dict(name=name, va=hex(va), pa=hex(STAGE + va - LINK),
                 old=original(va, len(data)).hex(), new=data.hex()) for va, data, name in patches]
    check_patches(rows)
    manifest = dict(input_image_sha256=image_sha, kernel_sha256=KERNEL_SHA, payload_sha256=sha(payload),
                    bridge_symbols={n: hex(v) for n, v in symbols.items()}, sections=sections, patches=rows)
    header = ['/* GENERATED by build_native.py; all writes are staged-RAM only. */']

    def array(name, data):
        header.append(f'static const unsigned char {name}[]={{' + ','.join(f'0x{n:02x}' for n in data) + '};')

    array('nt_payload_hash', bytes.fromhex(manifest['payload_sha256']))
    for i, row in enumerate(rows):
        for kind in ('old', 'new'):
            array(f'nt_{kind}_{i}', bytes.fromhex(row[kind]))
    header.append('typedef struct { uint64_t pa,n;const unsigned char *old,*newbytes;} NtPatch;')
    header.append('static const NtPatch nt_patches[]={')
    for i, row in enumerate(rows):
        header.append(f' {{UINT64_C({row["pa"]}),{len(row["new"]) // 2},nt_old_{i},nt_new_{i}}},')
    header.append('};')
    (out / 'native_patches.h').write_text('\n'.join(header) + '\n', encoding='ascii')
    (out / 'runtime_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='ascii')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Build and validate v0.41 staged-RAM patches')
    parser.add_argument('--input', type=Path, default=KERNEL_BASE)
    parser.add_argument('--out', type=Path, default=OUT)
    args = parser.parse_args()
    build(args.input, args.out)
