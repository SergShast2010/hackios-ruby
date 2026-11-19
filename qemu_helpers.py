#!/usr/bin/env python3
"""v29 FINAL-image native cp5 test. Production is READ ONLY, never rebuilt.
UEFI service doubles are a harness model. PrepareXnu, physical handoff, original
kernel MMU-on instructions, native callback/cache-off and rendering execute as
ARM64 under QEMU TCG. A passive read-only plugin observes live regs and actual
memory translations. No Python instruction model is used as execution evidence.
Positive debugger writes CFG synchronization gate only. CRC corrupts input bytes;
alignment negative changes harness boot_args input and SCTLR.A only.
"""
import argparse
import gzip
import hashlib
import json
import lzma
import re
import socket
import struct
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

HERE = Path(__file__).resolve().parent
TEST = HERE / 'qemu-test'
from build_and_check import pe_info, unpack

CTX, STACK, PROBE, FB = 0x62008000, 0x62010000, 0x62009000, 0x7c406000
CFG, PRE_BA, SOURCE, DEST, PRELOAD = 0x40180000, 0x62002000, 0x60000000, 0x63000000, 0x66000000
STAGE_BYTES, WIDTH, HEIGHT, PITCH = 0x2000000, 1080, 2400, 4320
LINK, MAGIC, SLOT_MAGIC = 0xfffffff007000000, 0x45425350524f4245, 0x545352444c554e58
SLOT, CP5_BRANCH, TTB = 0x6345f7f0, 0x63461468, 0x40200000
BLACK, GREEN, WHITE, YELLOW = 0xff000000, 0xff70ff90, 0xffffffff, 0xffffff60
MASK = (1 << 64) - 1
CONTEXT_FIELDS = ('magic base stack ebs map_bytes desc_size map_key attempts frequency counter '
                  'sctlr_before sctlr_after identity_pages stage vbar brk_ok esr elr far vector '
                  'entry bootargs checkpoint original_word saved_x0 saved_x20 reason code '
                  'kernel_vbar sp0 sp1 x22 x23 x24 x25 x26 ttbr0 ttbr1 return_sctlr tcr mair return_x11').split()
CONTEXT_BYTES = 8 * len(CONTEXT_FIELDS)
assert CONTEXT_BYTES == 336
sha = lambda data: hashlib.sha256(data).hexdigest()
u64 = lambda data, at=0: struct.unpack_from('<Q', data, at)[0]


def map_syms(path):
    result = {}
    for line in path.read_text().splitlines():
        match = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+00000000([0-9a-f]{8})\s+\S+\s*$', line)
        if match:
            result[match[1]] = int(match[2], 16)
    return result


def fat_file(disk, name):
    bps = struct.unpack_from('<H', disk, 11)[0]
    spc, reserved, nf = disk[13], struct.unpack_from('<H', disk, 14)[0], disk[16]
    roots, fs = struct.unpack_from('<H', disk, 17)[0], struct.unpack_from('<H', disk, 22)[0]
    root = (reserved + nf * fs) * bps
    data = root + ((roots * 32 + bps - 1) // bps) * bps
    for pos in range(root, root + roots * 32, 32):
        if disk[pos:pos + 11] != name:
            continue
        cluster, size = struct.unpack_from('<H', disk, pos + 26)[0], struct.unpack_from('<I', disk, pos + 28)[0]
        result, seen = bytearray(), set()
        while cluster < 0xfff8:
            assert cluster >= 2 and cluster not in seen
            seen.add(cluster)
            off = data + (cluster - 2) * spc * bps
            result.extend(disk[off:off + spc * bps])
            cluster = struct.unpack_from('<H', disk, reserved * bps + cluster * 2)[0]
        assert len(result) >= size
        return bytes(result[:size])
    raise AssertionError('file absent from FINAL FAT: ' + repr(name))


def final_app(image):
    """Extract only the app from final FD, with no verify_efi import side effects."""
    _, _, kernel, fat, _ = unpack(image)
    # Transport has standard UEFI compression; independent host tool extracts
    # the exact final-image FAT before the existing native-XNU harness runs.
    if len(fat) == 0x600000:
        used = struct.unpack_from('<I', fat)[0] + 8
        assert 8 <= used <= len(fat)
        assert fat[used:] == bytes(len(fat) - used)
        packed_path = image.parent / 'qemu-packed.uefi'
        decoded_path = image.parent / 'qemu-decoded.img'
        packed_path.write_bytes(fat[:used])
        import EfiCompressor
        fat = EfiCompressor.UefiDecompress(fat[:used], used)
        decoded_path.write_bytes(fat)
        assert len(fat) == 0x1000000

    fd = gzip.decompress(kernel)[-0x200000:]
    guid = uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf').bytes_le
    start = 0
    while True:
        pos = fd.find(guid, start)
        assert pos >= 0, 'guided FV absent'
        size, off = int.from_bytes(fd[pos - 4:pos - 1], 'little'), struct.unpack_from('<H', fd, pos + 16)[0]
        if 24 <= off < 100 and size < len(fd) and fd[pos - 1] == 2:
            dec = lzma.decompress(fd[pos - 4 + off:pos - 4 + size], format=lzma.FORMAT_ALONE)
            break
        start = pos + 1
    base = dec.find(b'_FVH') - 40
    assert base >= 0
    fv = dec[base:base + u64(dec, base + 32)]
    pos = struct.unpack_from('<H', fv, 48)[0]
    while pos < len(fv):
        pos = (pos + 7) & ~7
        if fv[pos:pos + 24] == b'\xff' * 24:
            break
        size = int.from_bytes(fv[pos + 20:pos + 23], 'little')
        assert size >= 24
        if fv[pos:pos + 16] == uuid.UUID('81da3d6b-1d18-4702-bd0f-a9a2551d6075').bytes_le:
            assert fv[pos + 18] == 9
            sec = pos + 24
            while sec < pos + size:
                n = int.from_bytes(fv[sec:sec + 3], 'little')
                assert n >= 4
                if fv[sec + 3] == 0x10:
                    return fv[sec + 4:sec + n], fat
                sec = (sec + n + 3) & ~3
        pos += size
    raise AssertionError('FINAL FV app absent')


def relocate(app, address):
    preferred, _, image = pe_info(app)
    image = bytearray(image)
    opt = struct.unpack_from('<I', app, 0x3c)[0] + 24
    rva, size = struct.unpack_from('<II', app, opt + 112 + 5 * 8)
    pos = rva
    while pos < rva + size:
        page, n = struct.unpack_from('<II', image, pos)
        assert n >= 8
        for at in range(pos + 8, pos + n, 2):
            item = struct.unpack_from('<H', image, at)[0]
            kind, dest = item >> 12, page + (item & 0xfff)
            if kind == 10:
                struct.pack_into('<Q', image, dest, (u64(image, dest) + address - preferred) & MASK)
            else:
                assert kind == 0
        pos += n
    assert pos == rva + size
    return preferred, bytes(image)


def kernel_metadata(payload):
    assert payload[:4] == b'XNUP' and payload[8:24] == b'kernel' + b'\0' * 10
    kernel = payload[32:32 + struct.unpack_from('<I', payload, 24)[0]]
    assert kernel[:4] == b'\xcf\xfa\xed\xfe'
    ncmd, cmdbytes = struct.unpack_from('<II', kernel, 16)
    pos, segments, symbols, thread_entry = 32, [], {}, None
    symtab = None
    for _ in range(ncmd):
        cmd, size = struct.unpack_from('<II', kernel, pos)
        assert size >= 8 and pos + size <= 32 + cmdbytes
        if cmd == 0x19:
            va, mem, off, files = struct.unpack_from('<4Q', kernel, pos + 24)
            maxprot, initprot, nsect = struct.unpack_from('<3I', kernel, pos + 56)
            if mem:
                segments.append(dict(name=kernel[pos + 8:pos + 24].split(b'\0')[0].decode(), va=va,
                                     dest=va - LINK, mem=mem, off=off, files=files,
                                     maxprot=maxprot, initprot=initprot))
            assert size == 72 + 80 * nsect and off + files <= len(kernel)
        elif cmd == 2:
            symtab = struct.unpack_from('<4I', kernel, pos + 8)
        elif cmd == 5:
            flavor, count = struct.unpack_from('<II', kernel, pos + 8)
            assert flavor == 6 and count == 68
            thread_entry = u64(kernel, pos + 16 + 32 * 8)
        pos += size
    assert pos == 32 + cmdbytes and symtab and thread_entry
    so, ns, st, ss = symtab
    strings = kernel[st:st + ss]
    for i in range(ns):
        no, typ, section, desc, value = struct.unpack_from('<IBBHQ', kernel, so + i * 16)
        if no < ss:
            symbols[strings[no:strings.find(b'\0', no)].decode(errors='replace')] = value
    assert symbols['__start'] == thread_entry
    return kernel, segments, symbols, DEST + thread_entry - LINK


def bootargs(data):
    assert len(data) == 736
    revision, version = struct.unpack_from('<HH', data)
    virt, phys, mem, top = struct.unpack_from('<4Q', data, 8)
    dt, dtbytes = struct.unpack_from('<QI', data, 96)
    return dict(revision=revision, version=version, virt=virt, phys=phys, mem=mem, top=top,
                video=struct.unpack_from('<6Q', data, 40), dt=dt, dtbytes=dtbytes,
                command=data[108:716].split(b'\0')[0].decode(), flags=u64(data, 720), actual=u64(data, 728))


def independent_oracle(kernel, segments, symbols, ba):
    """Architectural 4 KiB/2 MiB indices, derived from image and boot_args.

    First mapping is identity(root table 0); second is high(root table 1).
    Code uses a 39-bit VA window: (VA & 0x3fc0000000)>>30, NOT >>30&255.
    The ORIGINAL instruction masks with 0x3fc0000000 (8 bits, not the usual
    9-bit L1 mask): high root index is 192, not 0 or 448. This describes this
    kernel's constructed tables, NOT a claim they are a valid hardware map.
    No numbers from TablesMatch are used.
    """
    first = min(segments, key=lambda s: s['va'])
    header = DEST + first['dest']
    slide = 0
    if struct.unpack_from('<I', kernel, 24)[0] & 0x80000000:
        assert struct.unpack_from('<I', kernel, 32)[0] == 0x19
        slide = header - u64(kernel, 32 + 24)
        header = (first['va'] + slide) & MASK
    adjustment = (header - ba['phys']) & MASK
    phys, virt = (ba['phys'] + adjustment) & MASK, (ba['virt'] + adjustment) & MASK
    size = (ba['mem'] - adjustment) & MASK
    tables = DEST + symbols['_bootstrap_pagetables'] - LINK
    words, mappings, cursor = [0] * 2048, [], tables + 4096
    for root_page, va, pa, label in [(0, header, header, 'identity'), (1, virt, phys, 'high')]:
        remaining = size >> 21  # Assembly truncates: no invented last block.
        initial = va
        groups = []
        while remaining:
            cursor += 4096
            root_index, leaf_index = (va & 0x3fc0000000) >> 30, (va & 0x3fe00000) >> 21
            root_word = root_page * 512 + root_index
            assert words[root_word] == 0
            words[root_word] = (cursor & 0xfffffffff000) | 3
            count = min(remaining, 512 - leaf_index)
            leaf_page = (cursor - tables) // 4096
            assert 2 <= leaf_page < 4, 'bounded kernel needs more than the reserved 16 KiB'
            for i in range(count):
                words[leaf_page * 512 + leaf_index + i] = ((pa & 0xffffffe00000) + i * 0x200000) | 0x601
            groups.append(dict(root_index=root_index, table_pa=cursor, leaf_first=leaf_index, count=count))
            remaining -= count
            va, pa = (va + count * 0x200000) & MASK, (pa + count * 0x200000) & MASK
        mapped = (size >> 21) << 21
        mappings.append(dict(label=label, input_start=initial, mapped_start=initial & ~0x1fffff,
                             mapped_end=(initial & ~0x1fffff) + mapped, groups=groups))
    return dict(tables=tables, words=words, bytes=struct.pack('<2048Q', *words), mappings=mappings,
                x22=virt, x23=phys, x24=size, x25=tables, x26=ba['flags'],
                sp0=(DEST + symbols['_intstack_top'] - LINK + ba['virt'] - ba['phys']) & MASK,
                sp1=(DEST + symbols['_excepstack_top'] - LINK + ba['virt'] - ba['phys']) & MASK)


def trace_kernel(execution):
    """in_asm is not execution: expand ONLY TBs listed by exec+nochain."""
    blocks = {}
    for chunk in execution.split('----------------'):
        insns = [(int(p, 16), int(w, 16)) for p, w in re.findall(r'^0x([0-9a-f]+):\s+([0-9a-f]{8})', chunk, re.M)]
        if insns:
            blocks[insns[0][0]] = insns
    starts = [int(p, 16) for p in re.findall(r'^Trace\s+\d+:\s+\S+\s+\[[0-9a-f]+/([0-9a-f]+)/', execution, re.M)]
    dynamic = []
    for start in starts:
        assert start in blocks, ('executed TB lacks disassembly', hex(start))
        dynamic.extend(blocks[start])
    return dynamic, starts, blocks


def glyph(value):
    return np.array([[bool((((value >> 32) if y < 6 else value) >> ((y % 6) * 5)) & (1 << x)) for x in range(5)] for y in range(12)])


def load_glyphs():
    text = (HERE / 'ebsprobe/Font.h').read_text()
    block = text[text.index('#define GLYPH_FONT'):]
    return [glyph(int(x, 16)) for x in re.findall(r'0x[0-9A-Fa-f]+', block[:block.index('}')])[:95]]


def decode_row(band, glyphs):
    result, colors = [], set()
    for column in range(43):
        cell = np.zeros((12, 5), dtype=bool)
        for y in range(12):
            for x in range(5):
                value = int(band[y * 4, 24 + column * 24 + x * 4])
                cell[y, x] = value not in (BLACK, 0)
                if cell[y, x]:
                    colors.add(value)
        if not cell.any():
            result.append(' ')
        else:
            hit = [i for i, g in enumerate(glyphs) if np.array_equal(g, cell)]
            result.append(chr(32 + hit[0]) if hit else '?')
    return ''.join(result).rstrip(), [hex(c) for c in sorted(colors)]


class QMP:
    def __init__(self, path):
        for _ in range(200):
            try:
                self.sock = socket.socket(socket.AF_UNIX)
                self.sock.connect(str(path))
                break
            except OSError:
                self.sock.close()
                time.sleep(.05)
        else:
            raise RuntimeError('no QMP socket')
        self.file = self.sock.makefile('r')
        self.file.readline()
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.sock.sendall((json.dumps(dict(execute=name, arguments=args)) + '\n').encode())
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError('QEMU disconnected')
            result = json.loads(line)
            if 'error' in result:
                raise RuntimeError(str(result))
            if 'return' in result:
                return result['return']


class RSP:
    """Minimal GDB remote client. Writes are logged; success writes CFG only."""
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(90)
        self.sock.connect(str(path))
        self.writes = []

    def command(self, command):
        body = command.encode()
        if command.startswith(('M', 'P', 'G', 'Z', 'z')):
            self.writes.append(command)
        self.sock.sendall(b'$' + body + b'#' + f'{sum(body) % 256:02x}'.encode())
        while self.sock.recv(1) != b'$':
            pass
        data = bytearray()
        while True:
            ch = self.sock.recv(1)
            if not ch:
                raise RuntimeError('GDB disconnected')
            if ch == b'#':
                break
            data.extend(ch)
        checksum = self.sock.recv(2)
        assert int(checksum, 16) == sum(data) % 256
        self.sock.sendall(b'+')
        decoded, pos = bytearray(), 0
        while pos < len(data):
            if data[pos] == ord('}'):
                pos += 1
                decoded.append(data[pos] ^ 0x20)
            elif data[pos] == ord('*'):
                pos += 1
                decoded.extend(decoded[-1:] * (data[pos] - 29))
            else:
                decoded.append(data[pos])
            pos += 1
        return decoded.decode()

    def write(self, addr, data):
        assert self.command(f'M{addr:x},{len(data):x}:{data.hex()}') == 'OK'

    def registers(self):
        self.command('qSupported:qXfer:features:read+')
        result, nextnum = {}, [0]
        wanted = {'sp', 'pc', 'cpsr', 'x0', 'SCTLR_EL1', 'VBAR_EL1', 'SP_EL0', 'SP_EL1', 'TTBR0_EL1', 'TTBR1_EL1', 'TCR_EL1', 'MAIR_EL1'}
        def xml(name):
            data, off = '', 0
            while True:
                chunk = self.command(f'qXfer:features:read:{name}:{off:x},1000')
                assert chunk and chunk[0] in 'ml', ('missing register XML', name, chunk)
                data += chunk[1:]
                off += len(chunk[1:])
                if chunk[0] == 'l':
                    return ET.fromstring(data.replace('xi:', ''))
        def visit(element):
            for child in element:
                tag = child.tag.rsplit('}', 1)[-1]
                if tag == 'include':
                    visit(xml(child.attrib['href']))
                elif tag == 'reg':
                    number = int(child.get('regnum', nextnum[0]))
                    nextnum[0] = number + 1
                    name = {'SCTLR':'SCTLR_EL1','VBAR':'VBAR_EL1','TCR':'TCR_EL1','MAIR':'MAIR_EL1'}.get(child.get('name'),child.get('name'))
                    if name in wanted:
                        value = self.command(f'p{number:x}')
                        assert re.fullmatch('[0-9a-fA-F]+', value), (name, value)
                        result[name] = int.from_bytes(bytes.fromhex(value), 'little')
                else:
                    visit(child)
        visit(xml('target.xml'))
        required = wanted - {'SP_EL1'}
        assert required <= result.keys(), ('QEMU system-register XML incomplete', required - result.keys())
        return result

    def close(self):
        self.sock.close()


def parse_audit(path):
    instructions, memory, ended, registers, clean, mmio = [], [], False, [], 0, []
    for line in path.read_text().splitlines():
        fields = line.split()
        if not fields: continue
        if fields[0] == 'I':
            item = dict(pc=int(fields[1], 16), word=int(fields[2], 16))
            item['regs'] = {k: int(v, 16) for k, v in (f.split('=') for f in fields[3:])}
            instructions.append(item)
        elif fields[0] == 'M':
            memory.append(dict(pc=int(fields[1],16), operation=fields[2], va=int(fields[3],16), pa=int(fields[4],16), size=int(fields[5]), io=int(fields[6])))
        elif fields[0] == 'IO': mmio.append(line)
        elif fields[0] == 'REGS': registers = fields[1:]
        elif fields[0] == 'CLEAN': clean = int(fields[1])
        elif fields[0] == 'END': ended = True
    return dict(instructions=instructions, memory=memory, ended=ended, registers=registers, dc_cisw_count=clean, forbidden_mmio=mmio)


def check_mmu_audit(audit, context, final_regs, load, rva, oracle):
    checks = {}
    ins = audit['instructions']
    bypc = {}
    for i in ins: bypc.setdefault(i['pc'], []).append(i)
    def regs(pc):
        entries = bypc.get(pc, [])
        return entries[0]['regs'] if len(entries) == 1 else {}
    on = regs(0x63461424)
    translated = [regs(p) for p in range(0x63461428, 0x63461460, 4)]
    off_m = regs(0x63461460)
    callback = regs(load + rva['XnuReturn'])
    terminal = regs(load + rva['CheckpointOrFault'])
    checks['actual_msr_SCTLR_x0_3454593d'] = on.get('x0') == 0x3454593d and on.get('SCTLR_EL1', -1) & 0x1005 == 0
    checks['actual_identity_instruction_callbacks_while_MCI_on'] = bool(translated) and all(r.get('SCTLR_EL1') == 0x3454593d for r in translated)
    checks['actual_TTBR_TCR_MAIR_while_M1'] = all((r.get('TTBR0_EL1'), r.get('TTBR1_EL1'), r.get('TCR_EL1'), r.get('MAIR_EL1')) == (0x631b6000, 0x631b7000, 0x22a51a251a, 0x0c08044f00bb44ff) for r in translated)
    checks['native_macro_M_off_CI_left_on'] = off_m.get('SCTLR_EL1') == callback.get('SCTLR_EL1') == context['return_sctlr'] == context['return_x11'] == 0x3454593c and callback.get('x11') == 0x3454593c
    checks['actual_context_and_final_installed_registers'] = context['ttbr0'] == final_regs['TTBR0_EL1'] == 0x631b6000 and context['ttbr1'] == final_regs['TTBR1_EL1'] == 0x631b7000 and context['tcr'] == final_regs['TCR_EL1'] == 0x22a51a251a and context['mair'] == final_regs['MAIR_EL1'] == 0x0c08044f00bb44ff
    checks['callback_MCI_off_before_first_C'] = terminal.get('SCTLR_EL1', 0x1005) & 0x1005 == 0 and terminal.get('sp') == CTX + 0x4000 and terminal.get('VBAR_EL1') == load + rva['OwnVectors']
    checks['callback_complete_clean_invalidate_executed'] = audit['dc_cisw_count'] > 0 and any(i['word'] == 0xd508751f and load+rva['XnuReturn'] <= i['pc'] < load+rva['XnuReturn']+0x19c for i in ins)
    # Runtime memory callbacks supply translated physical addresses independently
    # of the boot_args/table oracle, not a Python CP3 execution interpreter.
    identity_loads = [m for m in audit['memory'] if m['pc'] == 0x63461438]
    checks['actual_M1_identity_load_VA_equals_PA'] = len(identity_loads) == 1 and identity_loads[0]['va'] == identity_loads[0]['pa'] == SLOT + 16 and identity_loads[0]['io'] == 0
    # Architectural table walk for fetched PCs under the live TCR geometry.
    tcr = 0x22a51a251a
    t0sz, tg0 = tcr & 63, (tcr >> 14) & 3
    def walk(va):
        if tg0 != 0 or t0sz != 26 or va >= 1 << (64-t0sz): return None
        root = (va >> 30) & 511
        d1 = oracle['words'][root]
        if d1 & 3 != 3: return None
        leaf_page = ((d1 & 0x0000fffffffff000) - oracle['tables']) // 4096
        if leaf_page not in (2, 3): return None
        d2 = oracle['words'][leaf_page*512 + ((va >> 21) & 511)]
        if d2 & 3 != 1 or not d2 & (1 << 10): return None
        return (d2 & 0x0000ffffffe00000) | (va & 0x1fffff)
    checks['all_M1_executed_PCs_identity_translated_by_actual_exact_tables'] = all(walk(p) == p for p in range(0x63461428, 0x63461460, 4))
    checks['bounded_kernel_only_expected_code_regions'] = all(0x6346105c <= i['pc'] <= 0x63461468 or i['pc'] == 0x6345e4a8 or 0x63465c54 <= i['pc'] <= 0x63465c60 for i in ins if DEST <= i['pc'] < DEST + STAGE_BYTES)
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=HERE / 'Mu-ruby-v0.29-xnummu.img')
    parser.add_argument('--load', type=lambda x: int(x, 0), choices=(0x44000000, 0x46000000), default=0x44000000)
    parser.add_argument('--name')
    parser.add_argument('--duration', type=float, default=5.2)
    parser.add_argument('--interval', type=float, default=.45)
    parser.add_argument('--negative', choices=('slot', 'original', 'alignment'))
    args = parser.parse_args()
    assert 4 <= args.duration <= 6 and .1 <= args.interval <= .6
    assert args.image.is_file(), 'FINAL image not available; do not substitute template/v27'
    out = TEST / 'out' / (args.name or f'v29_{args.load:08x}' + ('_negative_' + args.negative if args.negative else ''))
    out.mkdir(parents=True, exist_ok=True)
    assert all(p.is_file() or p.is_socket() for p in out.iterdir()), 'unexpected directory in run output'
    for path in out.iterdir():
        path.unlink()
    log, checks, injected_descriptor = [], {}, None
    def say(*parts):
        line = ' '.join(map(str, parts))
        print(line, flush=True)
        log.append(line)
    # The ONLY objects ever built here are independent test harness objects.
    for command in [
        ['aarch64-linux-gnu-gcc', '-march=armv8.2-a', '-mgeneral-regs-only', '-ffreestanding', '-fno-stack-protector', '-fno-pic', '-O2', '-nostdlib', '-c', 'start.S', '-o', str(out / 'start.o')],
        ['aarch64-linux-gnu-gcc', '-march=armv8.2-a', '-mgeneral-regs-only', '-ffreestanding', '-fno-stack-protector', '-fno-pic', '-O2', '-nostdlib', '-c', 'harness.c', '-o', str(out / 'harness.o')],
        ['aarch64-linux-gnu-gcc', '-nostdlib', '-static', '-T', 'harness.ld', '-Wl,--no-warn-rwx-segments', str(out / 'start.o'), str(out / 'harness.o'), '-o', str(out / 'harness.elf')]]:
        subprocess.run(command, cwd=TEST, check=True, capture_output=True)
    glib = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True).split()
    subprocess.run(['gcc', '-shared', '-fPIC', '-O2', '-DQEMU_AUDIT_PLUGIN', str(TEST / 'harness.c'), '-o', str(out / 'audit.so')] + glib, check=True, capture_output=True)
    image = args.image.read_bytes()
    app, fat = final_app(args.image)
    assert app == (HERE / 'ebsprobe/loader.efi').read_bytes(), 'FINAL PE differs from PE paired with map'
    preferred, relocated = relocate(app, args.load)
    assert preferred == 0xa1000000
    symbols = map_syms(HERE / 'ebsprobe/loader.map')
    need = ['PrepareXnu', 'EnterPhysical', 'OwnVectors', 'OwnVectorsEnd', 'ProbeBrk', 'screen',
            'PostPhysical', 'JumpXnu', 'CheckpointOrFault', 'XnuReturn', 'Rlevel', 'Rclean_done']
    assert all(n in symbols for n in need), [n for n in need if n not in symbols]
    rva = {n: symbols[n] - preferred for n in need}
    assert not rva['OwnVectors'] & 0x7ff and rva['OwnVectorsEnd'] - rva['OwnVectors'] == 2048
    payload = fat_file(fat, b'PAYLOAD XNP')
    assert len(payload) <= 0x1000000 and payload == (HERE / 'payload.xnup').read_bytes()
    kernel, segments, ksyms, entry = kernel_metadata(payload)
    assert sha(kernel) == '9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c'
    assert entry == 0x6345e4a8 and DEST + ksyms['_loader_stub_slot'] - LINK == SLOT
    callback = args.load + rva['XnuReturn']
    original_slot = None
    for seg in segments:
        off = SLOT - DEST - seg['dest']
        if 0 <= off and off + 32 <= seg['files']:
            original_slot = kernel[seg['off'] + off:seg['off'] + off + 32]
    assert original_slot == struct.pack('<4Q', SLOT_MAGIC, 0, 0, 0)
    injected = bytearray(payload)
    crc_negative = args.negative in ('slot', 'original')
    if crc_negative:
        pa = SLOT + 16 if args.negative == 'slot' else 0x634610a4
        for seg in segments:
            off = pa - DEST - seg['dest']
            if 0 <= off < seg['files']:
                injected[32 + seg['off'] + off] ^= 1
                break
        else:
            raise AssertionError('negative target not file-backed')
    rel, preload_path, config_path = out / 'app_relocated.bin', out / 'payload_from_final_fat.xnp', out / 'config.bin'
    rel.write_bytes(relocated)
    preload_path.write_bytes(injected)
    config = [args.load, rva['EnterPhysical'], rva['screen'], rva['PrepareXnu'], len(payload), int(args.negative == 'alignment')] + [0] * 26
    config_path.write_bytes(struct.pack('<32Q', *config))
    inputs = dict(final_image_sha256=sha(image), app_sha256=sha(app), payload_sha256=sha(payload), kernel_sha256=sha(kernel), rva=rva)
    (out / 'inputs.json').write_text(json.dumps(inputs, indent=2))
    say('FINAL', args.image.name, sha(image), 'PE load', hex(args.load), 'negative', args.negative)
    say('Models: EFI file/allocation only; EBS/efi_main NOT executed. Real ARM64: PrepareXnu + handoff + XNU + return/rendering.')
    glyphs = load_glyphs()
    sock, serial, trace = out / 'qmp.sock', out / 'serial.log', out / 'execution.log'
    qemu_log = open(out / 'qemu.stderr', 'w')
    command = ['qemu-system-aarch64', '-machine', 'virt', '-cpu', 'cortex-a55', '-accel', 'tcg', '-smp', '1', '-m', '1G',
               '-display', 'none', '-monitor', 'none', '-nic', 'none', '-serial', f'file:{serial}', '-no-reboot',
               '-kernel', str(out / 'harness.elf'), '-device', f'loader,file={rel},addr={args.load:#x}',
               '-device', f'loader,file={preload_path},addr={PRELOAD:#x}', '-device', f'loader,file={config_path},addr={CFG:#x}',
               '-qmp', f'unix:{sock},server=on,wait=off', '-gdb', f'unix:{out / "gdb.sock"},server=on,wait=off',
               '-plugin', f'{out / "audit.so"},out={out / "audit.log"},callback={callback:#x},end={args.load + rva["XnuReturn"] + 0x19c:#x},checkpoint={args.load + rva["CheckpointOrFault"]:#x}',
                '-d', 'in_asm,exec,nochain', '-dfilter', f'{DEST:#x}..{DEST + STAGE_BYTES - 1:#x}', '-D', str(trace)]
    (out / 'qemu-command.json').write_text(json.dumps(command, indent=2))
    process = subprocess.Popen(command, cwd=TEST, stdout=qemu_log, stderr=subprocess.STDOUT)
    samples, debug_operations, before_regs, oracle = [], [], {}, None
    def snap(qmp, name, addr, size):
        qmp.cmd('pmemsave', val=addr, size=size, filename=str(out / name))
        data = (out / name).read_bytes()
        assert len(data) == size
        return data
    try:
        qmp = QMP(sock)
        deadline = time.monotonic() + 90
        while True:
            qmp.cmd('stop')
            state = struct.unpack('<32Q', snap(qmp, 'config_live.bin', CFG, 256))
            if state[8] in (2, 3):
                break
            qmp.cmd('cont')
            if time.monotonic() > deadline:
                raise RuntimeError('PrepareXnu did not finish within 90 seconds')
            time.sleep(.15)
        say('PrepareXnu status', hex(state[9]), 'EFI model counters', list(state[16:27]))
        prectx = snap(qmp, 'ctx_before.bin', CTX, CONTEXT_BYTES)
        prestage = snap(qmp, 'stage_before.bin', DEST, STAGE_BYTES)
        preload = snap(qmp, 'preload_before.bin', PRELOAD, len(payload))
        source = snap(qmp, 'source_before.bin', SOURCE, len(payload))
        copiedba = snap(qmp, 'bootargs_harness_copy.bin', PRE_BA, 736)
        prepared_context = dict(zip(CONTEXT_FIELDS, struct.unpack('<42Q', prectx)))
        hi = max(s['dest'] + s['mem'] for s in segments)
        dt_pa = DEST + ((hi + 0x3fff) & ~0x3fff)
        ba_pa, top_pa = dt_pa + 0x10000, dt_pa + 0x14000
        preba = snap(qmp, 'bootargs_before.bin', ba_pa, 736)
        if not crc_negative:
            assert state[8] == 2 and state[9] == 0, 'PrepareXnu failed'
            ba = bootargs(preba)
            oracle = independent_oracle(kernel, segments, ksyms, ba)
            (out / 'independent-oracle.json').write_text(json.dumps({k: v for k, v in oracle.items() if k != 'bytes'}, indent=2))
            rsp = RSP(out / 'gdb.sock')
            before_regs = rsp.registers()
            rsp.write(CFG + 80, struct.pack('<Q', 1))  # data-only synchronization gate
            debug_operations.extend(rsp.writes)
            rsp.close()
        qmp.cmd('cont')
        start, number = time.monotonic(), 0
        while time.monotonic() - start < args.duration:
            time.sleep(args.interval)
            qmp.cmd('stop')
            band = snap(qmp, f'band28_{number:02d}.bin', FB + (120 + 28 * 54) * PITCH, 54 * PITCH)
            context_bytes = snap(qmp, f'ctx_{number:02d}.bin', CTX, CONTEXT_BYTES)
            samples.append(dict(seconds=round(time.monotonic() - start, 3), heartbeat=decode_row(np.frombuffer(band, '<u4').reshape(54, WIDTH), glyphs)[0], stage=u64(context_bytes, 104)))
            qmp.cmd('cont')
            number += 1
        qmp.cmd('stop')
        finalctx = snap(qmp, 'ctx_final.bin', CTX, CONTEXT_BYTES)
        finalcfg = snap(qmp, 'config_final.bin', CFG, 256)
        finalstage = snap(qmp, 'stage_final.bin', DEST, STAGE_BYTES)
        finalpe = snap(qmp, 'image_final.bin', args.load, len(relocated))
        finalpayload = snap(qmp, 'payload_final.bin', PRELOAD, len(payload))
        finalsource = snap(qmp, 'source_final.bin', SOURCE, len(payload))
        finalba = snap(qmp, 'bootargs_final.bin', ba_pa, 736)
        probe = snap(qmp, 'probe_final.bin', PROBE, 16)
        fbraw = snap(qmp, 'fb.raw', FB, WIDTH * HEIGHT * 4)
        registers = qmp.cmd('human-monitor-command', **{'command-line': 'info registers -a'})
        (out / 'registers.txt').write_text(registers)
        rsp = RSP(out / 'gdb.sock')
        final_regs = rsp.registers()
        assert not rsp.writes
        rsp.close()
        (out / 'register-audit.json').write_text(json.dumps(dict(before=before_regs, final=final_regs), indent=2))
        (out / 'debug-operations.json').write_text(json.dumps(debug_operations, indent=2))
        qmp.cmd('quit')
    finally:
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        qemu_log.close()
    serial_text = serial.read_text(errors='replace')
    execution = trace.read_text(errors='replace')
    dynamic, tb_starts, blocks = trace_kernel(execution)
    context = dict(zip(CONTEXT_FIELDS, struct.unpack('<42Q', finalctx)))
    state = struct.unpack('<32Q', finalcfg)
    fb = np.frombuffer(fbraw, '<u4').reshape(HEIGHT, WIDTH)
    rows = {}
    for row in range(40):
        y = 120 + row * 54
        if y + 54 > HEIGHT:
            break
        text, colors = decode_row(fb[y:y + 54], glyphs)
        if text:
            rows[row] = dict(text=text, colors=colors)
    checks['no_harness_fail'] = 'FAIL' not in serial_text
    checks['EFI_model_exact_open_read_close'] = list(state[16:22]) == ([2, 1, 1, 1, 1, 1] if crc_negative else [2, 1, 1, 2, 1, 1])
    checks['EBS_NOT_called'] = state[26] == 0
    checks['PrepareXnu_actual_call_MCI_on'] = state[27] & 0x1005 == state[28] & 0x1005 == 0x1005
    checks['preload_and_source_unchanged'] = preload == finalpayload == source == finalsource == bytes(injected)
    checks['FINAL_files_unchanged'] = args.image.read_bytes() == image and (HERE / 'payload.xnup').read_bytes() == payload
    checks['OwnVectors_code_unchanged'] = finalpe[rva['OwnVectors']:rva['OwnVectorsEnd']] == relocated[rva['OwnVectors']:rva['OwnVectorsEnd']]
    peoff = struct.unpack_from('<I', app, 0x3c)[0]
    nsec, opts = struct.unpack_from('<H', app, peoff + 6)[0], struct.unpack_from('<H', app, peoff + 20)[0]
    pe_code_checks = []
    for index in range(nsec):
        at = peoff + 24 + opts + index * 40
        size, rv = struct.unpack_from('<II', app, at + 8)
        if struct.unpack_from('<I', app, at + 36)[0] & 0x20000000:
            pe_code_checks.append(finalpe[rv:rv + size] == relocated[rv:rv + size])
    checks['all_PE_executable_sections_unchanged'] = bool(pe_code_checks) and all(pe_code_checks)
    ccidx = int(re.search(r'ID_AA64MMFR2_EL1=0x([0-9A-F]+)', serial_text)[1], 16)
    freq = int(re.search(r'cntfrq=0x([0-9A-F]+)', serial_text)[1], 16)
    audit = parse_audit(out / 'audit.log')
    (out / 'audit-parsed.json').write_text(json.dumps(audit, indent=2))
    checks['passive_plugin_clean_shutdown'] = audit['ended']
    checks['no_forbidden_MMIO_any_guest_instruction'] = not audit['forbidden_mmio']
    checks['no_SMC_HVC_executed_anywhere'] = not any((i['word'] & 0xffe0001f) in (0xd4000002, 0xd4000003) for i in audit['instructions'])
    checks['CCIDX_legacy_CNTFRQ_matches'] = ((ccidx >> 20) & 15) == 0 and context['frequency'] == freq and freq != 0
    details, transitions = [], 0
    if crc_negative:
        checks['CRC_rejected_0x1a'] = state[8] == 3 and state[9] == 0x800000000000001a
        checks['source_allocated_freed_destination_never_allocated'] = list(state[22:26]) == [1, 0, 1, 0]
        checks['no_kernel_execution_or_stage_write'] = not dynamic and prestage == finalstage and not any(finalstage)
        checks['no_handoff_context'] = all(context[name] == 0 for name in CONTEXT_FIELDS[10:])
        checks['no_test_debug_writes'] = not debug_operations
    else:
        checks['source_freed_destination_retained'] = list(state[22:26]) == [1, 1, 1, 0] and state[29:31] == (0, 1)
        checks['context_magic_base_stack'] = context['magic'] == MAGIC and context['base'] == 0x62000000 and context['stack'] == STACK
        checks['native_slot_exact_before_and_after'] = prestage[SLOT - DEST:SLOT - DEST + 32] == finalstage[SLOT - DEST:SLOT - DEST + 32] == struct.pack('<4Q', SLOT_MAGIC, callback, 5, 0)
        checks['entry_native_cp5_no_instruction_patch'] = context['entry'] == entry and context['checkpoint'] == 5 and context['original_word'] == 0
        checks['stage4_native_return'] = context['stage'] == 4
        checks['MCI_off_before_kernel_and_after_return'] = context['sctlr_before'] & 0x1005 == 0x1005 and context['sctlr_after'] & 0x1005 == 0 and final_regs['SCTLR_EL1'] & 0x1005 == 0
        checks['OwnVectors_actual_restored'] = context['vbar'] == final_regs['VBAR_EL1'] == args.load + rva['OwnVectors']
        checks['own_BRK526_returned'] = context['brk_ok'] == 1
        checks['physical_stack_restored_EL1h'] = PROBE + 16 <= final_regs['sp'] < CTX + 0x4000 and final_regs['sp'] % 16 == 0 and final_regs['cpsr'] & 15 == 5
        checks['pre_handoff_harness_TTBR0'] = before_regs['TTBR0_EL1'] == TTB
        checks['RAM_probe_intact'] = probe == struct.pack('<QQ', 0x123456789abcdef0, 0xedcba9876543210f)
        checks['bootargs_unchanged_and_harness_copy_matches'] = preba == finalba == copiedba
        checks['independent_bootargs_ABI'] = (ba['revision'], ba['version'], ba['virt'], ba['phys'], ba['mem'], ba['top']) == (2, 2, 0xfffffff004000000, 0x60000000, 0x8000000, top_pa) and ba['video'] == (FB, 0, PITCH, WIDTH, HEIGHT, 32) and ba['dt'] == ba['virt'] + dt_pa - ba['phys'] and 0 < ba['dtbytes'] <= 65536 and ba['actual'] == ba['mem'] and ba['flags'] == 0 and ba['command'] == '-v debug=0x100 cpus=1' and prepared_context['bootargs'] == ba_pa
        slot_start = SLOT - DEST
        for seg in segments:
            original = kernel[seg['off']:seg['off'] + seg['files']]
            expected = bytearray(original)
            off = slot_start - seg['dest']
            if 0 <= off and off + 32 <= len(expected):
                expected[off:off + 32] = struct.pack('<4Q', SLOT_MAGIC, callback, 5, 0)
            prepared = prestage[seg['dest']:seg['dest'] + seg['files']]
            actual = finalstage[seg['dest']:seg['dest'] + seg['files']]
            # The declared XNULDRST data island is INSIDE __TEXT_EXEC/__text.
            # Never claim the entire executable segment is byte-identical.
            instruction_expected = bytearray(original)
            instruction_actual = bytearray(actual)
            if 0 <= off and off + 32 <= len(expected):
                instruction_expected[off:off + 32] = b'\0' * 32
                instruction_actual[off:off + 32] = b'\0' * 32
            details.append(dict(**seg, staged_file_exact=prepared == expected,
                                prepared_BSS_zero=not any(prestage[seg['dest'] + seg['files']:seg['dest'] + seg['mem']]),
                                executable_unchanged_except_declared_DATA_island=not (seg['initprot'] & 4) or instruction_actual == instruction_expected,
                                final_sha256=sha(actual)))
        checks['all_prepared_segments_original_except_native_DATA_slot'] = all(s['staged_file_exact'] and s['prepared_BSS_zero'] for s in details)
        checks['all_kernel_executable_INSTRUCTIONS_unchanged'] = all(s['executable_unchanged_except_declared_DATA_island'] for s in details)
        checks['actual_kernel_memory_all_RAM_no_MMIO'] = bool(audit['memory']) and all(x['io'] == 0 and DEST <= x['pa'] and x['pa'] + x['size'] <= DEST + STAGE_BYTES for x in audit['memory'])
        decoder = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
        executed = [next(decoder.disasm(struct.pack('<I', x['word']), x['pc'], count=1)) for x in audit['instructions']]
        checks['actual_no_SMC_HVC_or_platform_calls'] = bool(executed) and all(i.mnemonic not in ('smc', 'hvc', 'svc') for i in executed)
        plugin_kernel = [(x['pc'], x['word']) for x in audit['instructions'] if DEST <= x['pc'] < DEST + STAGE_BYTES]
        compared_dynamic = dynamic
        if args.negative == 'alignment':
            # QEMU exec logs name the TB before executing it. The synchronous
            # abort at 0x634610f0 prevents the remaining five translated
            # instructions in that TB from executing. The passive per-insn
            # callbacks (not the TB listing) are the execution authority.
            tb_tail = [(0x634610f4,0xf9400a97),(0x634610f8,0xf9400e98),(0x634610fc,0xb0ffeab9),(0x63461100,0xf9416a9a),(0x63461104,0xd51bd07f)]
            checks['faulting_TB_exact_five_unexecuted_tail_instructions'] = all(dynamic.count(pair)==1 and pair not in plugin_kernel for pair in tb_tail)
            compared_dynamic = [pair for pair in dynamic if pair not in tb_tail]
        checks['plugin_real_instruction_callbacks_match_QEMU_exec'] = plugin_kernel == compared_dynamic
        checks['kernel_trace_present_actual_exec_not_just_translation'] = bool(tb_starts)
        expected_stage = bytearray(prestage)
        table_off = oracle['tables'] - DEST
        expected_stage[table_off:table_off + 16384] = oracle['bytes']
        expected_tables = oracle['bytes']
        if args.negative == 'alignment':
            checks['alignment_test_only_input_perturbation'] = state[11] == 1 and state[12] == ba_pa + 1 and state[15] & 2 == 2
            checks['early_fault_native_reason1_ESR_alignment'] = context['reason'] == 1 and context['code'] == context['esr'] == 0x96000021
            checks['early_fault_ELR_FAR_x20'] = context['elr'] == 0x634610f0 and context['far'] == ba_pa + 9 and context['saved_x20'] == context['bootargs'] == ba_pa + 1
            checks['early_fault_BEFORE_cp5_table_writes'] = finalstage == prestage and not any(p == CP5_BRANCH for p, w in dynamic) and any(p == 0x6345f788 for p, w in dynamic)
            checks['no_false_table_success'] = 24 not in rows and all('Success' not in row['text'] for n, row in rows.items() if 4 <= n < 32) and rows.get(15, {}).get('text') == 'x22=0000000000000000'
        else:
            checks['native_reason2_code5_no_early_fault'] = context['reason'] == 2 and context['code'] == context['esr'] == 5
            checks['saved_x0_zero_x20_bootargs'] = context['saved_x0'] == 0 and context['saved_x20'] == context['bootargs'] == ba_pa
            checks['SP0_independent_MachO_bootargs'] = context['sp0'] == oracle['sp0'] == final_regs['SP_EL0']
            checks['SP1_independent_MachO_bootargs'] = context['sp1'] == oracle['sp1']
            for n in range(22, 27):
                checks[f'x{n}_independent_MachO_bootargs'] = context[f'x{n}'] == oracle[f'x{n}']
            checks['native_kernel_VBAR'] = context['kernel_vbar'] == 0xfffffff00745a000
            checks['all2048_table_words_exact_including_zeroes'] = finalstage[table_off:table_off + 16384] == expected_tables
            checks['only_bootstrap_tables_change_during_kernel'] = finalstage == expected_stage
            if not args.negative:
                checks['no_breakpoints_register_or_kernel_writes_by_test'] = debug_operations == [f'M{CFG + 80:x},8:0100000000000000']
                checks['CP5_return_branch_executed_not_later_kernel'] = bool(dynamic) and dynamic[-1][0] == CP5_BRANCH and all(p < 0x6346146c or 0x63465c54 <= p <= 0x63465c60 for p, w in dynamic)
                checks.update(check_mmu_audit(audit, context, final_regs, args.load, rva, oracle))
        if args.negative == 'alignment':
            wanted_rows = {4: ('XNU MMU STOPPED - send photo', YELLOW),
                           6: (f'Reason={context["reason"]:016X}', WHITE),
                           7: (f'Code={context["code"]:016X}', WHITE),
                           8: (f'XNU VBAR={context["kernel_vbar"]:016X}', WHITE)}
            checks['negative_terminal_NO_heartbeat'] = all(not x['heartbeat'].startswith('Heartbeat:') for x in samples)
            checks['negative_no_success_banner'] = not any('CHECKPOINT DONE' in row['text'] or 'tables 16 KiB: Success' in row['text'] for row in rows.values())
        else:
            wanted_rows = {4: ('XNU NATIVE CHECKPOINT 5 REACHED', GREEN),
                           5: ('XNU enabled MMU + caches', GREEN),
                           6: ('Original kernel instructions unchanged', WHITE),
                           8: (f'XNU VBAR={context["kernel_vbar"]:016X}', WHITE),
                           9: (f'XNU boot_args={ba_pa:016X}', WHITE),
                           10: ('boot_args registers: Success', GREEN),
                           11: ('Bootstrap tables 16 KiB: Success', GREEN),
                           12: ('TTBR0=00000000631B6000', WHITE),
                           13: ('TTBR1=00000000631B7000', WHITE),
                           14: ('TCR=00000022A51A251A', WHITE),
                           15: ('MAIR=0C08044F00BB44FF', WHITE),
                           16: ('Native return SCTLR=000000003454593C', WHITE),
                           17: (f'Diagnostic SCTLR={final_regs["SCTLR_EL1"]:016X}', WHITE),
                           19: ('Native callback turns MMU off', WHITE),
                           20: ('Our stub cleans caches before output', WHITE),
                           21: ('No platform drivers / UFS / userdata', WHITE),
                           24: ('XNU MMU CHECKPOINT DONE', GREEN),
                           26: ('Heartbeat runs in our return handler', WHITE)}
            heartbeat = [x['heartbeat'] for x in samples]
            transitions = sum(a != b and a.startswith('Heartbeat:') and b.startswith('Heartbeat:') for a, b in zip(heartbeat, heartbeat[1:]))
            checks['heartbeat_both_values_at_least3_transitions'] = 'Heartbeat: 0' in heartbeat and 'Heartbeat: 1' in heartbeat and transitions >= 3
        checks['framebuffer_independent_exact_rows_and_colors'] = all(rows.get(r, {}).get('text') == text and rows.get(r, {}).get('colors') == [hex(color)] for r, (text, color) in wanted_rows.items())
        checks['framebuffer_no_fault_or_BRK527_claim'] = not any('CPU EXCEPTION' in row['text'] or 'BRK527' in row['text'] for row in rows.values())
    (out / 'kernel-dynamic-path.json').write_text(json.dumps(dynamic, indent=2))
    for row, value in rows.items():
        say('FB', row, value['text'])
    for name, good in checks.items():
        say('PASS' if good else 'FAIL', name)
    hashes = dict(final_image=sha(image), app_extracted=sha(app), payload_from_FINAL_FAT=sha(payload), kernel=sha(kernel), relocated=sha(relocated), stage_before=sha(prestage), stage_after=sha(finalstage), execution_trace=sha(trace.read_bytes()))
    for label, path in [('loader_efi_file', HERE / 'ebsprobe/loader.efi'), ('loader_map', HERE / 'ebsprobe/loader.map'),
                        ('transition_S', HERE / 'ebsprobe/transition.S'), ('loader_c', HERE / 'ebsprobe/loader.c'),
                        ('xnu_prepare_h', HERE / 'ebsprobe/xnu_prepare.h'), ('harness_c', TEST / 'harness.c'),
                        ('harness_elf', out / 'harness.elf'), ('runner', Path(__file__))]:
        hashes[label] = sha(path.read_bytes())
    result = dict(load=hex(args.load), negative=args.negative, duration=args.duration, all_pass=all(checks.values()),
                  checks=checks, samples=samples, context={k: hex(v) for k, v in context.items()}, hashes=hashes,
                  rows=rows, segments=details, heartbeat_transitions=transitions, debug_operations=debug_operations,
                  injected_descriptor=injected_descriptor, dynamic_kernel_instructions=len(dynamic),
                  test_boundary=dict(models=['UEFI file/protocol/allocation C service doubles; no DXE core', 'harness identity page tables, not firmware map ownership'],
                                     real_ARM64=['PrepareXnu', 'EnterPhysical/cache maintenance instructions', 'OwnVectors BRK526', 'original XNU through SCTLR(MCI on), identity instructions, native cp5 callback', 'XnuReturn and framebuffer renderer'],
                                     instrumentation='test-only misaligned bootargs input and SCTLR.A' if args.negative == 'alignment' else 'CFG synchronization data write only' if not crc_negative else 'preloaded payload byte corruption only'),
                  not_tested=['EBS NOT executed; success text is not EBS evidence', 'efi_main RAM/FDT/map/identity ownership guards',
                              'real firmware filesystem, phone MMIO, cache/coherency hardware, platform drivers, full XNU boot'])
    Image.fromarray(np.stack([(fb >> 16) & 255, (fb >> 8) & 255, fb & 255], axis=-1).astype(np.uint8)).save(out / 'framebuffer.png')
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    say('ALL PASS' if result['all_pass'] else 'SOME CHECKS FAILED')
    (out / 'run.log').write_text('\n'.join(log) + '\n')
    assert result['all_pass'], [name for name, good in checks.items() if not good]


if __name__ == '__main__':
    main()
