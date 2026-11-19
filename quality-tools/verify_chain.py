#!/usr/bin/env python3
"""Final-boot BDS -> real UefiDecompress -> decoded FAT -> LoadImage/StartImage.

No Mu checkout, maps, build products or imports of older verifiers are needed.
Requires Python 3, unicorn and capstone. All executable BDS bytes come from each
boot's final FV. Only EFI-service/firmware lookup and RubyDiagLine are doubles.
Real FileDevicePath, RAM-disk functions, integrity branches, PrintLib, and the
Tiano decoder execute unmodified. StartImage records the handle's resolved
payload and returns EFI_UNSUPPORTED; the application is deliberately not run.

Run: python verify_chain.py [--old-only] [--scenario NAME] [--image PATH]
Outputs are chain-* only; never writes production images or build files.
"""
import argparse
import gzip
import hashlib
import json
import lzma
import struct
import time
import traceback
import uuid
import zlib
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import (Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE,
                     UC_HOOK_MEM_WRITE, UC_PROT_READ, UC_PROT_WRITE, UC_PROT_ALL)
from unicorn.arm64_const import *

H = Path(__file__).resolve().parent
SHA_APP = 'b49ab2075591d7f654195e6975e02ef2f2b1294d1f8864c73de2bf6992169d5c'
SHA_BDS = '65f4b3fd7d42c344668ccde41b3bb342d6d5b1061a4a25832bcfb3bdfac623e1'
SHA_FDT = '921c063316fcf72b2c014b3d40962e019510c905f43214984941df78663a2692'
BDS_GUID = '6d33944a-ec75-4855-a54d-809c75241f6c'
APP_GUID = '81da3d6b-1d18-4702-bd0f-a9a2551d6075'
GUID = lambda s: uuid.UUID(s).bytes_le
APPG = GUID(APP_GUID)
BLKG = GUID('964e5b21-6459-11d2-8e39-00a0c969723b')
DPG = GUID('09576e91-6d3f-11d2-8e39-00a0c969723b')
SFSG = GUID('964e5b22-6459-11d2-8e39-00a0c969723b')
INFOG = GUID('09576e92-6d3f-11d2-8e39-00a0c969723b')
LIG = GUID('5b1b31a1-9562-11d2-8e3f-00a0c969723b')
GOPG = GUID('9042a9de-23dc-4a38-96fb-7aded080516a')
EFI_PATH = '\\EFI\\BOOT\\BOOTAA64.EFI'
ALL = (1 << 64) - 1
E = lambda n: (1 << 63) | n
UNSUP, DEVICE, OUTRES, COMPROMISED, DENIED = E(3), E(7), E(9), E(33), E(15)
LOAD, HEAP, FB = 0x50000000, 0x70000000, 0x7c406000
PACK_BASE, PACK_BYTES, FAT_BASE, FAT_BYTES = 0x51100000, 0x600000, 0x66000000, 0x1000000
FDT_BASE, FDT_LIMIT, STOP = 0x47c80000, 0x80000, 0x40001000
u16 = lambda b, o: struct.unpack_from('<H', b, o)[0]
u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]
u64 = lambda b, o: struct.unpack_from('<Q', b, o)[0]
align = lambda n, a: (n + a - 1) & -a
sha = lambda b: hashlib.sha256(b).hexdigest()


def boot_parts(path):
    b = path.read_bytes()
    assert b[:8] == b'ANDROID!' and u32(b, 40) == 2
    ks, rs, ss, page, ds = u32(b, 8), u32(b, 16), u32(b, 24), u32(b, 36), u32(b, 1648)
    assert page == 2048 and ss == 0 and u32(b, 1632) == 0
    ro = page + align(ks, page)
    do = ro + align(rs, page)
    k, pack, dtb = b[page:page+ks], b[ro:ro+rs], b[do:do+ds]
    h = hashlib.sha1()
    for v in [k, pack, b'', b'', dtb]:
        h.update(v); h.update(struct.pack('<I', len(v)))
    assert b[576:596] == h.digest(), 'Android legacy boot ID'
    assert len(pack) == PACK_BYTES and u32(b, 20) == PACK_BASE
    return dict(path=str(path), image_sha256=sha(b), kernel_raw=gzip.decompress(k),
                pack=pack, dtb=dtb, header=b[:page])


def fv_files(fv):
    assert fv[40:44] == b'_FVH' and u64(fv, 32) == len(fv)
    hl = u16(fv, 48)
    assert sum(struct.unpack('<'+'H'*(hl//2), fv[:hl])) & 65535 == 0
    out, o = {}, hl
    while o < len(fv):
        o = align(o, 8)
        if fv[o:o+24] == b'\xff'*24:
            assert fv[o:] == b'\xff'*(len(fv)-o)
            break
        hd = fv[o:o+24]; n = int.from_bytes(hd[20:23], 'little')
        assert 24 <= n <= len(fv)-o and (sum(hd[:23])-hd[17]) & 255 == 0
        data = fv[o+24:o+n]
        if hd[19] & 0x40:
            assert (sum(data)+hd[17]) & 255 == 0
        else:
            assert hd[17] == 0xaa
        key = str(uuid.UUID(bytes_le=hd[:16]))
        assert key not in out
        out[key] = dict(type=hd[18], body=data)
        o += n
    return out


def sections(data):
    o, out = 0, {}
    while o < len(data):
        assert o+4 <= len(data)
        n, t = int.from_bytes(data[o:o+3], 'little'), data[o+3]
        assert 4 <= n <= len(data)-o
        out.setdefault(t, []).append(data[o+4:o+n])
        o = align(o+n, 4)
    return out


def final_fv(parts):
    outer = fv_files(parts['kernel_raw'][-0x200000:])
    sec = outer['9e21fd93-9c72-4c15-8c4b-e77f1db2d792']['body']
    assert sec[3] == 2 and sec[4:20] == GUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf')
    n, off = int.from_bytes(sec[:3], 'little'), u16(sec, 20)
    assert n == len(sec) and 24 <= off < n
    dec = lzma.decompress(sec[off:n], format=lzma.FORMAT_ALONE)
    assert dec[7] == 0x17
    fs = fv_files(dec[8:])
    parts['fv_sha256'] = sha(dec[8:])
    parts['bds'] = sections(fs[BDS_GUID]['body'])[0x10][0]
    parts['app'] = sections(fs[APP_GUID]['body'])[0x10][0]
    parts['app_ffs_type'] = fs[APP_GUID]['type']
    assert parts['app_ffs_type'] == 9 and sha(parts['app']) == SHA_APP
    return parts


def pe_layout(b):
    pe = u32(b, 0x3c); op = pe+24
    assert b[pe:pe+4] == b'PE\0\0' and u16(b, pe+4) == 0xaa64 and u16(b, op) == 0x20b
    mem = bytearray(u32(b, op+56)); hs = u32(b, op+60)
    mem[:hs] = b[:hs]
    sect = {}
    for j in range(u16(b, pe+6)):
        o = op+u16(b, pe+20)+j*40
        name = b[o:o+8].rstrip(b'\0').decode()
        vs, va, rs, rp = struct.unpack_from('<4I', b, o+8)
        mem[va:va+rs] = b[rp:rp+rs]
        sect[name] = dict(rva=va, virtual_size=vs, raw=rp, raw_size=rs)
    return dict(mem=mem, sections=sect, base=u64(b, op+24), entry=u32(b, op+16), op=op)


def relocate(b, address):
    p = pe_layout(b); mem = p['mem']
    r, n = struct.unpack_from('<II', b, p['op']+112+5*8); pos = r
    while pos < r+n:
        page, size = struct.unpack_from('<II', mem, pos)
        assert size >= 8 and pos+size <= r+n
        for o in range(pos+8, pos+size, 2):
            e = u16(mem, o); kind, dst = e >> 12, page+(e & 4095)
            if kind == 10:
                struct.pack_into('<Q', mem, dst, (u64(mem, dst)+address-p['base']) & ALL)
            else:
                assert kind == 0
        pos += size
    return bytes(mem)


def reconstruct_symbols(base_bds):
    """Capstone + string crossrefs + PE exception table, never external maps."""
    assert sha(base_bds) == SHA_BDS
    p = pe_layout(base_bds); mem = p['mem']; text = p['sections']['.text']; pdata = p['sections']['.pdata']
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM); cs.detail = True; cs.skipdata = True
    ins = list(cs.disasm(bytes(mem[text['rva']:text['rva']+text['virtual_size']]), text['rva']))
    ix = {i.address: k for k, i in enumerate(ins)}
    funcs = sorted(u32(mem, o) for o in range(pdata['rva'], pdata['rva']+pdata['virtual_size'], 8))
    evidence = {}
    def ref(data):
        pos = mem.find(data)
        assert pos >= 0 and mem.find(data, pos+1) < 0, repr(data)
        hits = []
        for i, n in zip(ins, ins[1:]):
            if i.mnemonic == 'adrp' and n.mnemonic == 'add' and len(n.operands) == 3:
                if n.operands[2].type == 2 and n.operands[1].reg == i.operands[0].reg:
                    if i.operands[1].imm+n.operands[2].imm == pos:
                        hits.append(i.address)
        assert hits, repr(data)
        evidence[data.hex()] = dict(data_rva=hex(pos), xrefs=[hex(x) for x in hits])
        return hits
    def next_calls(a, count):
        return [i.operands[0].imm for i in ins[ix[a]:] if i.mnemonic == 'bl'][:count]
    head = ref(b'ruby v0.30a - checked RAM transport\0')[0]
    launch = max(f for f in funcs if f <= head)
    diag = next_calls(head, 1)[0]
    probe = ref(b'Probe PE from firmware\0')[0]
    status = next_calls(probe, 1)[0]
    calls = [i for i in ins[ix[launch]:ix[probe]] if i.mnemonic == 'bl']
    section = calls[-1].operands[0].imm
    claim = ref(b'Claim decoded FAT RAM\0')[0]
    getinfo = next_calls(claim, 2)[1]
    getinfo_call = next(i.address for i in ins[ix[claim]:] if i.mnemonic == 'bl' and i.operands[0].imm == getinfo)
    decompress = next_calls(getinfo_call+4, 3)[2]
    pathrefs = [a for a in ref((EFI_PATH+'\0').encode('utf-16le')) if launch <= a < status]
    assert len(pathrefs) == 2
    filepath = next_calls(pathrefs[-1], 1)[0]
    # Store targets reconstructed from the real entry's initialization instructions.
    globals_ = {}
    entry_ins = ins[ix[p['entry']]:ix[p['entry']]+40]
    for k, i in enumerate(entry_ins):
        if i.mnemonic == 'str' and i.operands[0].type == 1:
            operand = i.operands[1]
            if operand.type != 3:
                continue
            prev = next((q for q in reversed(entry_ins[:k]) if q.mnemonic == 'adrp' and q.operands[0].reg == operand.mem.base), None)
            if prev is None:
                continue
            addr = prev.operands[1].imm+operand.mem.disp
            reg = i.reg_name(i.operands[0].reg)
            if reg == 'x0': globals_['gImageHandle'] = addr
            elif reg == 'x19': globals_['gST'] = addr
            elif reg == 'x8' and 'gBS' not in globals_: globals_['gBS'] = addr
    syms = dict(RubyEfiLaunch=launch, RubyDiagLine=diag, RubyDiagStatus=status,
                GetSectionFromAnyFv=section, UefiDecompressGetInfo=getinfo,
                UefiDecompress=decompress, FileDevicePath=filepath, **globals_)
    assert syms == dict(RubyEfiLaunch=0x14e68, RubyDiagLine=0x14364, RubyDiagStatus=0x16184,
                        GetSectionFromAnyFv=0xe4dc, UefiDecompressGetInfo=0x178e8,
                        UefiDecompress=0x179a8, FileDevicePath=0x99fc,
                        gImageHandle=0x624a8, gST=0x62480, gBS=0x624c8), syms
    # Constant materializations may have intervening non-writing instructions.
    known = {0xc7b6d073:'PACK_CRC', 0xba837732:'FAT_CRC', 0x5e8d5a:'PACK_USED_MINUS_8'}
    active, pairs = {}, []
    def regkey(i, reg): return i.reg_name(reg).replace('w', 'x', 1)
    for i in ins:
        if i.id == 0: active.clear(); continue
        if i.mnemonic == 'mov' and len(i.operands) == 2 and i.operands[1].type == 2:
            active[regkey(i, i.operands[0].reg)] = (i.operands[1].imm, i.address)
            continue
        if i.mnemonic == 'movk' and i.operands[1].type == 2:
            key = regkey(i, i.operands[0].reg)
            if key in active:
                val, start = active[key]; shift = i.operands[1].shift.value
                val = (val & ~(0xffff << shift)) | (i.operands[1].imm << shift)
                if val in known:
                    pairs.append(dict(kind=known[val], sites=[start, i.address], value=val))
                active[key] = (val, start)
            continue
        for reg in i.regs_access()[1]:
            active.pop(regkey(i, reg), None)
    assert sorted(x['kind'] for x in pairs) == ['FAT_CRC','FAT_CRC','PACK_CRC','PACK_CRC','PACK_USED_MINUS_8'], pairs
    oldchunks = [0x89600095,0x1c9a93de,0xc05a8c03,0xbdb74c79,0xfb84c30e,0xf2b49e90,0xd46c18f0,0x1c00ad40,
                 0xe61c595f,0x509e9501,0xd7a32f3f,0xcf64e6e0,0xef82a570,0x7c3d9d5e,0xd7b64009,0xecd76370]
    chunk = mem.find(struct.pack('<16I', *oldchunks)); assert chunk == 0x19ed0
    pins = dict(base_bds_sha256=SHA_BDS, reconstructed_by='Capstone AArch64 crossrefs and PE .pdata; no maps',
                symbols={k:dict(rva=hex(v), first16=bytes(mem[v:v+16]).hex()) for k,v in syms.items()},
                string_xrefs=evidence, mutable_constant_pairs=pairs, chunk_crc_rva=hex(chunk))
    return syms, pins, pairs, chunk


def pin_final_bds(base, final, pairs, chunk, pack):
    p, q = pe_layout(base), pe_layout(final)
    assert p['sections'] == q['sections'] and len(base) == len(final)
    allowed, changes, constants = set(), [], {}
    def raw(rva):
        for s in p['sections'].values():
            if s['rva'] <= rva < s['rva']+s['raw_size']:
                return s['raw']+rva-s['rva']
        raise AssertionError(hex(rva))
    for item in pairs:
        values = []
        for site in item['sites']:
            a, b = u32(p['mem'], site), u32(q['mem'], site)
            assert a & ~(0xffff << 5) == b & ~(0xffff << 5), 'changed MOV opcode/register'
            allowed.update(range(raw(site), raw(site)+4))
            values.append((b >> 5) & 0xffff)
        value = values[0] | (values[1] << 16)
        constants.setdefault(item['kind'], []).append(value)
        changes.append(dict(kind=item['kind'], sites=[hex(a) for a in item['sites']], old=hex(item['value']), new=hex(value)))
    assert len(set(constants['PACK_CRC'])) == len(set(constants['FAT_CRC'])) == 1
    assert constants['PACK_CRC'][0] == zlib.crc32(pack)
    used = u32(pack, 0)+8
    assert used < PACK_BYTES and pack[used:] == b'\0'*(PACK_BYTES-used)
    assert constants['PACK_USED_MINUS_8'] == [used-8]
    allowed.update(range(raw(chunk), raw(chunk)+64))
    diffs = [i for i,(a,b) in enumerate(zip(base,final)) if a != b]
    assert all(o in allowed for o in diffs), 'BDS changed outside CRC / PACK_USED immediates and chunk CRCs'
    return dict(byte_differences=len(diffs), allowed_only=True, control_flow_unchanged=True,
                constant_changes=changes, pack_used=used, fat_crc=constants['FAT_CRC'][0],
                chunk_crcs=list(struct.unpack_from('<16I', q['mem'], chunk)))


def fat_file(read, size=FAT_BYTES):
    """FAT16 BPB + directory + cluster chain walk of the *native decoded memory*."""
    b = read(0, 512)
    assert b[510:512] == b'\x55\xaa'
    bs, spc, reserved, nf, roots, spf = u16(b,11), b[13], u16(b,14), b[16], u16(b,17), u16(b,22)
    assert bs == 512 and spc > 0 and spc & (spc-1) == 0 and nf == 2 and roots and spf
    rootbytes = align(roots*32, bs); root = (reserved+nf*spf)*bs; data = root+rootbytes
    assert data < size
    fat = read(reserved*bs, spf*bs)
    assert fat == read((reserved+spf)*bs, spf*bs), 'FAT copies differ'
    clusterbytes = bs*spc
    def chain(c):
        out, seen = [], set()
        while c < 0xfff8:
            assert 2 <= c < len(fat)//2 and c not in seen
            seen.add(c); out.append(c); c = u16(fat,c*2)
        return out
    def directory(d, name):
        for o in range(0,len(d),32):
            e = d[o:o+32]
            if e[0] == 0: break
            if e[0] == 0xe5 or e[11] == 0x0f or e[11] & 8: continue
            if e[:11] == name: return e
        raise FileNotFoundError(name)
    def cluster_data(c):
        offsets = [data+(x-2)*clusterbytes for x in chain(c)]
        assert all(0 <= o <= size-clusterbytes for o in offsets)
        return b''.join(read(o,clusterbytes) for o in offsets), offsets
    d = read(root,rootbytes)
    for name in [b'EFI        ',b'BOOT       ']:
        e = directory(d,name); assert e[11] & 0x10
        d,_ = cluster_data(u16(e,26))
    e = directory(d,b'BOOTAA64EFI'); assert not e[11] & 0x10
    n = u32(e,28); payload, offsets = cluster_data(u16(e,26))
    assert n <= len(payload) and len(payload)-n < clusterbytes
    return payload[:n], dict(file_size=n, first_cluster=u16(e,26), clusters=len(offsets),
                            first_data_offset=offsets[0], sectors_per_cluster=spc)


SCENARIOS = ['success','packed_input_corrupt','packed_padding_corrupt','packed_crc_service_error',
             'private_crc_service_error','snapshot_corrupt','fat_crc_service_error',
             'sfs_read_wrong_bytes','sfs_short_read','post_decode_corrupt_SYNTHETIC',
             'live_fdt_magic_reject','alloc_pages_fail']


def run(parts, symbols, mode, fdt, case_seconds):
    started = time.monotonic()
    st = dict(scenario=mode, image=Path(parts['path']).name, image_sha256=parts['image_sha256'],
              events=[], rows={}, rowlog=[], status_calls=[], crc_calls=[], allocations=[], free_pages=[],
              pools={}, decoder_entry=0, getinfo_entry=0, loadimage=0, startimage=0, unload=0,
              rootclose=0, fileclose=0, file_reads=0, connect=0, install=0, owned=False,
              ebs=0, guard_writes=[], error=None, native_returned=False)
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    bm = relocate(parts['bds'],LOAD)
    uc.mem_map(LOAD,align(len(bm),4096)); uc.mem_write(LOAD,bm)
    uc.mem_map(HEAP,0x3000000)
    uc.mem_map(FB & ~4095,0xa00000)
    guards = [PACK_BASE-4096,PACK_BASE+PACK_BYTES,FAT_BASE-4096,FAT_BASE+FAT_BYTES]
    for a in guards:
        uc.mem_map(a,4096); uc.mem_write(a,b'\xa5'*4096)
        def watch(u,access,address,size,value,data):
            st['guard_writes'].append([hex(address),size])
        uc.hook_add(UC_HOOK_MEM_WRITE,watch,begin=a,end=a+4095)
    pack = bytearray(parts['pack']); used = u32(pack,0)+8
    if mode == 'packed_input_corrupt': pack[min(0x12345,used-1)] ^= 0x80
    if mode == 'packed_padding_corrupt':
        assert used+0x1000 < PACK_BYTES
        pack[used+0x1000] ^= 1
    uc.mem_map(PACK_BASE,PACK_BYTES,UC_PROT_READ); uc.mem_write(PACK_BASE,bytes(pack))
    uc.mem_map(FAT_BASE,FAT_BYTES,UC_PROT_READ|UC_PROT_WRITE); uc.mem_write(FAT_BASE,b'\x5a'*FAT_BYTES)
    live = bytearray(fdt)
    if mode == 'live_fdt_magic_reject': live[0] ^= 0xff
    uc.mem_map(FDT_BASE,FDT_LIMIT,UC_PROT_READ); uc.mem_write(FDT_BASE,bytes(live))
    SYS,BS,LI,CON = HEAP+0x1000,HEAP+0x2000,HEAP+0x3000,HEAP+0x4000
    FS,ROOTF,FILEF,GOP,GMODE,GINFO = [HEAP+x for x in [0x6000,0x6100,0x6200,0x7000,0x7100,0x7200]]
    SP,DISK,CHILD,PARENT = HEAP+0x2ff0000,0x12347000,0x12345000,0x12346000
    p64 = lambda a,v: uc.mem_write(a,struct.pack('<Q',v & ALL))
    p32 = lambda a,v: uc.mem_write(a,struct.pack('<I',v & 0xffffffff))
    r64 = lambda a: u64(bytes(uc.mem_read(a,8)),0)
    r32 = lambda a: u32(bytes(uc.mem_read(a,4)),0)
    regs = lambda n: [uc.reg_read(UC_ARM64_REG_X0+i) for i in range(n)]
    hooks, dynamic = {}, []
    state = dict(stub=HEAP+0x10000,pool=HEAP+0x100000)
    protos = {}
    def ret(value=0):
        uc.reg_write(UC_ARM64_REG_X0,value & ALL)
        uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
    def cstr(a): return bytes(uc.mem_read(a,512)).split(b'\0')[0].decode('ascii','replace')
    def ustr(a):
        out = []
        for k in range(512):
            c = u16(bytes(uc.mem_read(a+k*2,2)),0)
            if c == 0: return ''.join(out)
            out.append(chr(c))
        raise AssertionError('unterminated UTF16')
    def stub(fn):
        a = state['stub']; state['stub'] += 16
        hooks[a] = fn; uc.mem_write(a,bytes.fromhex('c0035fd6')); return a
    def alloc(n):
        a = state['pool']; state['pool'] = align(a+n,64)
        assert n > 0 and state['pool'] < HEAP+0x2e00000
        st['pools'][a] = n; return a
    def allocate():
        ty,n,out = regs(3)
        assert ty == 4
        p64(out,alloc(n)); ret()
    def free():
        a, = regs(1)
        assert a in st['pools'], 'FreePool unknown '+hex(a)
        del st['pools'][a]; ret()
    def pages():
        ty,mt,n,out = regs(4)
        st['allocations'].append(dict(type=ty,memory_type=mt,pages=n,address=hex(r64(out))))
        assert ty == mt == 2 and n == FAT_BYTES//4096 and r64(out) == FAT_BASE and not st['owned']
        if mode == 'alloc_pages_fail': ret(OUTRES); return
        st['owned'] = True; p64(out,FAT_BASE); ret()
    def freepages():
        a,n = regs(2); assert a == FAT_BASE and n == FAT_BYTES//4096 and st['owned']
        st['free_pages'].append([hex(a),n]); st['owned'] = False; ret()
    def locate():
        g,res,out = regs(3); assert bytes(uc.mem_read(g,16)) == GOPG and res == 0
        p64(out,GOP); ret()
    def section():
        g,ty,out,n = regs(4)
        assert bytes(uc.mem_read(g,16)) == APPG and ty == 0x10
        app = parts['app'] # ONLY the section extracted from THIS final firmware FV.
        a = alloc(len(app)); uc.mem_write(a,app); p64(out,a); p64(n,len(app))
        st['firmware_section_sha256'] = sha(app); st['firmware_section_bytes'] = len(app)
        st['events'].append('firmware PE32 section from final FV supplied'); ret()
    def install():
        out,gd,dp,gb,blk,end = regs(6)
        assert bytes(uc.mem_read(gd,16)) == DPG and bytes(uc.mem_read(gb,16)) == BLKG and end == 0
        p64(out,DISK); protos.update(dp=dp,block=blk); st['install'] += 1; ret()
    def handle():
        h,g,out = regs(3); guid = bytes(uc.mem_read(g,16))
        if h == DISK:
            value = {BLKG:protos['block'],DPG:protos['dp'],SFSG:FS}.get(guid)
        elif h == CHILD and guid == LIG: value = LI
        else: raise AssertionError('unexpected HandleProtocol '+hex(h)+' '+str(uuid.UUID(bytes_le=guid)))
        assert value
        p64(out,value); ret()
    def crc():
        a,n,out = regs(3)
        kind = ('packed' if a == PACK_BASE and n == PACK_BYTES else
                'private' if n == PACK_BYTES else 'fat' if a == FAT_BASE and n == FAT_BYTES else 'chunk')
        entry = dict(kind=kind,address=hex(a),bytes=n,return_rva=hex(uc.reg_read(UC_ARM64_REG_LR)-LOAD))
        st['crc_calls'].append(entry)
        if mode == kind+'_crc_service_error':
            entry['status'] = hex(DEVICE); ret(DEVICE); return
        if mode == 'snapshot_corrupt' and kind == 'private':
            val = bytes(uc.mem_read(a+0x100,1))[0]; uc.mem_write(a+0x100,bytes([val^1]))
            st['events'].append('SYNTHETIC private snapshot byte flip before private CRC')
        value = zlib.crc32(bytes(uc.mem_read(a,n)))
        p32(out,value); entry.update(crc32=hex(value),status='0x0'); ret()
    def get_file():
        payload,meta = fat_file(lambda a,n: bytes(uc.mem_read(FAT_BASE+a,n)))
        st['decoded_fat_file'] = dict(sha256=sha(payload),**meta)
        return payload
    def openvolume():
        h,out = regs(2); assert h == FS
        p64(out,ROOTF); ret()
    def openfile():
        h,out,name,flags,attrs = regs(5)
        assert h == ROOTF and flags == 1 and attrs == 0 and ustr(name) == EFI_PATH
        st['opened_path'] = ustr(name); p64(out,FILEF); ret()
    def close_root(): st['rootclose'] += 1; ret()
    def close_file(): st['fileclose'] += 1; ret()
    def getinfo():
        h,g,n,out = regs(4); assert h == FILEF and bytes(uc.mem_read(g,16)) == INFOG
        payload = get_file(); assert r64(n) >= 128
        uc.mem_write(out,b'\0'*128); p64(out,128); p64(out+8,len(payload)); p64(n,128); ret()
    def readfile():
        h,n,out = regs(3); assert h == FILEF
        payload = get_file(); assert r64(n) == len(payload)
        assert st['install'] == 1 and next(c for c in st['crc_calls'] if c['kind'] == 'fat')['status'] == '0x0'
        st['file_reads'] += 1
        if mode == 'sfs_read_wrong_bytes':
            payload = bytearray(payload); payload[len(payload)//2] ^= 1; payload = bytes(payload)
            st['events'].append('SFS.Read fault: output byte flipped AFTER native FAT CRC verify; FAT memory untouched')
        if mode == 'sfs_short_read': payload = payload[:-1]
        uc.mem_write(out,payload); p64(n,len(payload))
        st['sfs_read_sha256'] = sha(payload); st['sfs_read_bytes'] = len(payload); ret()
    def devicepath(a):
        nodes,raw,paths = [],bytearray(),[]
        for k in range(32):
            hdr = bytes(uc.mem_read(a,4)); n = u16(hdr,2)
            assert 4 <= n < 2048
            data = bytes(uc.mem_read(a,n)); raw.extend(data)
            nodes.append(dict(type=hdr[0],subtype=hdr[1],bytes=n,hex=data.hex()))
            if hdr[:2] == b'\x04\x04': paths.append(data[4:].decode('utf-16le').rstrip('\0'))
            if hdr[:2] == b'\x7f\xff': return bytes(raw),nodes,paths
            a += n
        raise AssertionError('invalid device path')
    def loadimage():
        boot,parent,path,source,n,out = regs(6)
        st['loadimage'] += 1
        raw,nodes,paths = devicepath(path)
        diskraw,_,_ = devicepath(protos['dp'])
        assert boot == 0 and parent == PARENT and source == n == 0
        assert paths == [EFI_PATH] and raw.startswith(diskraw[:-4])
        payload = get_file() # LoadImage's source NULL: BS resolves native device path through decoded FAT.
        st['loadimage_args'] = dict(BootPolicy=boot,ParentImageHandle=hex(parent),DevicePath=hex(path),
                                   SourceBuffer=hex(source),SourceSize=n,ImageHandle_out=hex(out))
        st['loadimage_device_path'] = dict(file_path=paths[0],hex=raw.hex(),nodes=nodes,native_FileDevicePath=True)
        st['loadimage_payload_sha256'] = sha(payload); st['loadimage_payload_bytes'] = len(payload)
        assert payload == parts['app'] and sha(payload) == SHA_APP
        state['loaded_payload'] = payload; p64(out,CHILD); p64(LI+24,DISK)
        (H/'chain-loadimage-payload.bin').write_bytes(payload)
        (H/'chain-loadimage-device-path.bin').write_bytes(raw)
        st['loadimage_payload_capture'] = 'chain-loadimage-payload.bin'
        st['events'].append('LoadImage resolved exact decoded-FAT payload'); ret()
    def startimage():
        child,exitn,exitdata = regs(3); assert child == CHILD and 'loaded_payload' in state
        st['startimage'] += 1
        payload = state['loaded_payload']
        st['startimage_args'] = dict(ImageHandle=hex(child),ExitDataSize_out=hex(exitn),ExitData_out=hex(exitdata))
        st['startimage_payload_sha256'] = sha(payload); st['startimage_payload_bytes'] = len(payload)
        st['startimage_stub_status'] = hex(UNSUP)
        assert sha(payload) == SHA_APP
        (H/'chain-startimage-payload.bin').write_bytes(payload)
        st['startimage_payload_capture'] = 'chain-startimage-payload.bin'
        st['events'].append('StartImage recorded exact handle-bound payload; stub returns EFI_UNSUPPORTED'); ret(UNSUP)
    def unload():
        child, = regs(1); assert child == CHILD
        st['unload'] += 1; ret()
    def connect():
        h,drivers,remaining,recursive = regs(4)
        assert h == DISK and drivers == remaining == 0 and recursive == 1
        st['connect'] += 1; ret()
    def ebs():
        st['ebs'] += 1; raise AssertionError('ExitBootServices forbidden')
    def unexpected(): raise AssertionError('unexpected firmware service invoked')
    def diag():
        row,text,color = regs(3); txt = cstr(text)
        st['rows'][str(row)] = txt; st['rowlog'].append([row,txt])
        ret() # Explicit diagnostic-only interception; integrity/status formatting stays native.
    def status_probe():
        row,label,status = regs(3)
        st['status_calls'].append(dict(row=row,label=cstr(label),status=hex(status)))
    def getinfo_probe(): st['getinfo_entry'] += 1
    def decoder_probe():
        src,dst,scratch = regs(3); st['decoder_entry'] += 1
        assert src != PACK_BASE and src in st['pools'] and st['pools'][src] == PACK_BYTES
        assert bytes(uc.mem_read(src,PACK_BYTES)) == parts['pack'] and dst == FAT_BASE and scratch in st['pools']
        st['events'].append('entered native UefiDecompress with private packed snapshot')
        state['decoder_pending'] = (LOAD+symbols['UefiDecompress'],uc.reg_read(UC_ARM64_REG_LR))
        # Stop BEFORE the first decoder instruction, then execute to its real LR
        # with zero code hooks. Do NOT substitute, JIT-patch, skip, or memoize it.
        uc.emu_stop()
    services = {24:lambda:ret(4),32:lambda:ret(),40:pages,48:freepages,64:allocate,72:free,
                152:handle,200:loadimage,208:startimage,224:unload,232:ebs,264:connect,
                320:locate,328:install,344:crc}
    for off in range(24,376,8): p64(BS+off,stub(services.get(off,unexpected)))
    p64(SYS+96,BS); p64(SYS+64,CON); p64(CON+8,stub(lambda:ret()))
    p64(FS,0x10000); p64(FS+8,stub(openvolume))
    p64(ROOTF,0x10000); p64(ROOTF+8,stub(openfile)); p64(ROOTF+16,stub(close_root))
    p64(FILEF,0x10000); p64(FILEF+16,stub(close_file)); p64(FILEF+32,stub(readfile)); p64(FILEF+64,stub(getinfo))
    # Minimal GOP protocol: no real GOP driver or GOP initialization is substituted into BDS.
    p64(GOP+24,GMODE); p32(GMODE,1); p32(GMODE+4,0); p64(GMODE+8,GINFO)
    p64(GMODE+16,36); p64(GMODE+24,FB); p64(GMODE+32,1080*2400*4)
    p32(GINFO+4,1080); p32(GINFO+8,2400); p32(GINFO+12,1); p32(GINFO+32,1080)
    for name,val in [('gBS',BS),('gST',SYS),('gImageHandle',PARENT)]: p64(LOAD+symbols[name],val)
    hooks[LOAD+symbols['GetSectionFromAnyFv']] = section
    hooks[LOAD+symbols['RubyDiagLine']] = diag
    probes = {LOAD+symbols['RubyDiagStatus']:status_probe,
              LOAD+symbols['UefiDecompressGetInfo']:getinfo_probe,
              LOAD+symbols['UefiDecompress']:decoder_probe}
    def dispatch(u,a,size,data):
        if a in probes: probes[a]()
        if a in hooks: hooks[a]()
    dispatch_handles = []
    def install_dispatch():
        for a in set(hooks)|set(probes):
            dispatch_handles.append(uc.hook_add(UC_HOOK_CODE,dispatch,begin=a,end=a))
    install_dispatch()
    uc.reg_write(UC_ARM64_REG_CPACR_EL1,3 << 20)
    uc.reg_write(UC_ARM64_REG_SP,SP); uc.reg_write(UC_ARM64_REG_LR,STOP)
    pc,total = LOAD+symbols['RubyEfiLaunch'],0
    def step(pc,end):
        nonlocal total
        assert time.monotonic()-started < case_seconds, 'case wall-time budget exhausted'
        assert total < 4_000_000_000, 'instruction budget exhausted'
        uc.emu_start(pc,end,timeout=15_000_000,count=100_000_000)
        total += 100_000_000
        return uc.reg_read(UC_ARM64_REG_PC)
    try:
        while pc != STOP:
            pc = step(pc,STOP)
            if 'decoder_pending' in state:
                dpc,lr = state.pop('decoder_pending')
                assert pc == dpc
                for h in dispatch_handles: uc.hook_del(h)
                dispatch_handles.clear()
                decstart = time.monotonic()
                while dpc != lr: dpc = step(dpc,lr)
                st['decoder_seconds'] = round(time.monotonic()-decstart,3)
                st['decoder_status'] = hex(uc.reg_read(UC_ARM64_REG_X0))
                st['decoded_before_fault_sha256'] = sha(bytes(uc.mem_read(FAT_BASE,FAT_BYTES)))
                st['events'].append('native UefiDecompress returned to real LR; no code hooks/replacement during decode')
                if mode == 'post_decode_corrupt_SYNTHETIC':
                    payload,meta = fat_file(lambda a,n: bytes(uc.mem_read(FAT_BASE+a,n)))
                    off = meta['first_data_offset']+5
                    old = bytes(uc.mem_read(FAT_BASE+off,1))[0]
                    uc.mem_write(FAT_BASE+off,bytes([old^1]))
                    st['synthetic_post_decode_flip_offset'] = off
                    st['events'].append('SYNTHETIC byte flip after real decoder return; not a decoder replacement')
                install_dispatch()
                pc = lr
        st['native_returned'] = True
    except Exception:
        st['error'] = traceback.format_exc()[-2500:]
    fat = bytes(uc.mem_read(FAT_BASE,FAT_BYTES))
    actual_chunks = [zlib.crc32(fat[i:i+0x100000]) for i in range(0,FAT_BYTES,0x100000)]
    contract = parts['contract']
    st.update(final_pc=hex(uc.reg_read(UC_ARM64_REG_PC)),instruction_budget_upper_bound=total,
              decoded_fat_sha256=sha(fat),decoded_fat_crc32=hex(zlib.crc32(fat)),
              compiled_fat_crc32=hex(contract['fat_crc']),
              compiled_crc_matches_decoded=zlib.crc32(fat) == contract['fat_crc'],
              decoded_chunk_crcs=[hex(c) for c in actual_chunks],
              compiled_chunks_match_decoded=actual_chunks == contract['chunk_crcs'],
              decoded_fat_sentinel=fat == b'\x5a'*FAT_BYTES, pool_bytes_left=sum(st['pools'].values()),
              guards_intact=all(bytes(uc.mem_read(a,4096)) == b'\xa5'*4096 for a in guards),
              pack_readonly_unchanged=bytes(uc.mem_read(PACK_BASE,PACK_BYTES)) == bytes(pack),
              seconds=round(time.monotonic()-started,3))
    st['pools'] = {hex(a):n for a,n in st['pools'].items()}
    return st


def judge(r, old=False):
    mode = r['scenario']; problems = []
    def check(ok,text):
        if not ok: problems.append(text)
    def status(label):
        return [int(c['status'],16) for c in r['status_calls'] if c['label'] == label]
    check(r['error'] is None,'exception: '+str(r['error']))
    check(r['native_returned'],'native RubyEfiLaunch did not return')
    check(r['guards_intact'] and not r['guard_writes'],'guard overwrite')
    check(r['pack_readonly_unchanged'],'packed input changed')
    check(r['ebs'] == 0,'ExitBootServices called')
    check(r['pool_bytes_left'] == 0,'pool leak')
    check(r['firmware_section_sha256'] == SHA_APP,'FV source was not exact target application')
    launched = mode == 'success' and not old
    check(r['loadimage'] == r['startimage'] == int(launched),'unexpected LoadImage/StartImage count')
    check(r['rows'].get('35') == 'XNU ENTRY STOPPED - send photo','missing final diagnostic')
    check(r['rows'].get('0') == 'ruby v0.30a - checked RAM transport','native heading not reached')
    if mode == 'success' or mode.startswith('sfs_'):
        check(r['decoder_entry'] == r['getinfo_entry'] == 1,'real decoder not executed exactly once')
        check(r.get('decoder_status') == '0x0','real decoder did not succeed')
        check(r['compiled_crc_matches_decoded'] and r['compiled_chunks_match_decoded'],'compiled FAT/chunk CRCs differ from real decoded FAT')
        check(r['connect'] == r['install'] == 1,'native RAM-disk/ConnectController not reached')
        if old:
            check(r['decoded_fat_file']['sha256'] != SHA_APP,'old FAT did not contain stale app bytes')
            check(r['file_reads'] == 1,'old mismatch did not exercise real read/compare')
            check(r['rows'].get('7') == 'RAM EFI file integrity: Compromised Data','old code-33 integrity failure text not reproduced')
        if old or mode.startswith('sfs_'):
            check(status('RAM EFI file integrity') == [COMPROMISED],'file integrity must return EFI_COMPROMISED_DATA (33)')
            check(status('Launcher stopped before EBS') == [COMPROMISED],'wrong terminal rejection status')
        else:
            check(status('RAM EFI file integrity') == [0],'native file-integrity compare failed')
            check(status('Launcher stopped before EBS') == [UNSUP],'StartImage return not propagated')
            check(r.get('decoded_fat_file',{}).get('sha256') == SHA_APP,'decoded FAT app wrong')
            check(r.get('loadimage_payload_sha256') == r.get('startimage_payload_sha256') == SHA_APP,'launched bytes wrong')
            check(r.get('loadimage_device_path',{}).get('file_path') == EFI_PATH,'LoadImage file path wrong')
            check(r['unload'] == 1,'native unload missing')
        if mode.startswith('sfs_'):
            check(r['file_reads'] == 1,'SFS fault not exercised after FAT verify')
            check(r['decoded_fat_file']['sha256'] == SHA_APP,'SFS negative changed decoded FAT instead of read output')
    else:
        check(r['loadimage'] == r['startimage'] == r['file_reads'] == 0,'negative progressed to file or launch')
        want = DENIED if mode == 'live_fdt_magic_reject' else OUTRES if mode == 'alloc_pages_fail' else COMPROMISED
        check(status('Launcher stopped before EBS') == [want],'wrong terminal rejection status')
        decoded = mode in ['fat_crc_service_error','post_decode_corrupt_SYNTHETIC']
        check(r['decoder_entry'] == int(decoded),'unexpected native decoder count')
        check(r['decoded_fat_sentinel'] == (not decoded),'unexpected decoded FAT state')
        if decoded:
            check(r['free_pages'] == [[hex(FAT_BASE),FAT_BYTES//4096]],'rejected decoded pages not reclaimed')
            check(len(r['crc_calls']) == 19,'missing full FAT + 16 chunk diagnostic checks')
        elif mode in ['packed_input_corrupt','packed_padding_corrupt','packed_crc_service_error']:
            check(len(r['crc_calls']) == 1 and not r['allocations'],'packed fault not stopped before allocation/decode')
    r['problems'] = problems; r['pass'] = not problems
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--old-only',action='store_true')
    ap.add_argument('--scenario',choices=SCENARIOS)
    ap.add_argument('--image',type=Path,default=H/'Mu-ruby-v0.32b-appsync.img')
    ap.add_argument('--fdt',type=Path,default=H/'chain-live-fdt.bin')
    ap.add_argument('--case-seconds',type=int,default=90)
    ap.add_argument('--suite-seconds',type=int,default=240)
    args = ap.parse_args(); t0 = time.monotonic()
    fdt = args.fdt.read_bytes(); assert sha(fdt) == SHA_FDT and len(fdt) <= FDT_LIMIT
    base = final_fv(boot_parts(H/'base/Mu-ruby-v0.32a-lzmafix.img'))
    assert base['bds'] == (H/'base/BdsDxe.efi').read_bytes() and sha(base['bds']) == SHA_BDS
    assert (H/'loader.efi').read_bytes() == base['app'] # verification only, NEVER source of FV/SFS bytes.
    syms,pins,pairs,chunk = reconstruct_symbols(base['bds'])
    (H/'chain-symbols.json').write_text(json.dumps(pins,indent=2)+'\n')
    oldstatic = pin_final_bds(base['bds'],base['bds'],pairs,chunk,base['pack'])
    results, block = [], []
    def execute(parts,mode,old=False):
        if time.monotonic()-t0 >= args.suite_seconds:
            block.append('suite wall-time budget reached before '+mode); return
        parts['contract'] = oldstatic if old else final_static
        r = judge(run(parts,syms,mode,fdt,args.case_seconds),old)
        results.append(r)
        line = ('PASS' if r['pass'] else 'FAIL')+' '+('OLD ' if old else 'NEW ')+mode
        line += f" decode={r['decoder_entry']} LoadImage={r['loadimage']} StartImage={r['startimage']} t={r['seconds']}s"
        print(line,flush=True)
        if r['problems']: print(json.dumps(r['problems']),flush=True)
        (H/('chain-old.json' if old else 'chain-'+mode+'.json')).write_text(json.dumps(r,indent=2)+'\n')
    if not args.scenario:
        execute(base,'success',old=True)
    final_static = None; new = None
    if not args.old_only:
        if not args.image.exists():
            block.append('new final boot image not yet present: '+str(args.image))
        else:
            new = final_fv(boot_parts(args.image))
            assert base['app'] == new['app'] and base['dtb'] == new['dtb']
            final_static = pin_final_bds(base['bds'],new['bds'],pairs,chunk,new['pack'])
            assert final_static['byte_differences'] > 0 and new['pack'] != base['pack']
            for mode in [args.scenario] if args.scenario else SCENARIOS:
                execute(new,mode)
    summary = dict(all_pass=not block and all(r['pass'] for r in results),blockers=block,
                   scenarios=len(results),scenarios_passed=sum(r['pass'] for r in results),
                   old_reproduced=any(r['image']==Path(base['path']).name and r['pass'] for r in results),
                   new_tested=new is not None,old_image_sha256=base['image_sha256'],
                   new_image_sha256=new['image_sha256'] if new else None,
                   source_app_sha256=SHA_APP,live_fdt_sha256=sha(fdt),
                   old_constants=oldstatic,final_BDS_constraints=final_static,
                   native=['final-FV BdsDxe RubyEfiLaunch and all integrity branches','UefiDecompressGetInfo and real Tiano UefiDecompress',
                           'RAM-disk creation and native write-protection probe','FileDevicePath and native PrintLib status formatting'],
                   doubles=['BootServices tables, CRC32 via zlib, allocation, protocol/SFS services',
                            'FV section service returns actual app section from the SAME final boot FV',
                            'minimal GOP protocol; RubyDiagLine records text and returns',
                            'LoadImage resolves real native device path through native-decoded FAT in emulator memory',
                            'StartImage binds to recorded LoadImage bytes and returns EFI_UNSUPPORTED'],
                   limitations=['entry is RubyEfiLaunch, not complete SEC/DXE/BDS dispatch',
                                'outer guided FV extracted with Python lzma; RAM pack uses ONLY native BDS UefiDecompress',
                                'SFS mounting/file protocol are doubles using independent FAT16 chain walk of decoded memory',
                                'StartImage is a stub; actual app entry not executed',
                                'no MMU/cache/interrupt, UFS, USB, disk or phone/hardware validation'],
                   seconds=round(time.monotonic()-t0,3))
    out = dict(summary=summary,results=results)
    suffix = '-'+args.scenario if args.scenario else '-old-only' if args.old_only else ''
    (H/('chain-report'+suffix+'.json')).write_text(json.dumps(out,indent=2)+'\n')
    lines = [f"{'PASS' if r['pass'] else 'FAIL'} {r['image']} {r['scenario']}: "
             f"decoder={r['decoder_entry']} LoadImage={r['loadimage']} StartImage={r['startimage']} "
             f"row7={r['rows'].get('7')} final={r['rows'].get('32')} {r['seconds']}s" for r in results]
    lines += ['BLOCKER '+s for s in block]
    lines.append(f"ALL PASS: {summary['all_pass']} ({summary['scenarios_passed']}/{summary['scenarios']})")
    (H/('chain-report'+suffix+'.log')).write_text('\n'.join(lines)+'\n')
    print(lines[-1],flush=True)
    return 0 if summary['all_pass'] else 2

if __name__ == '__main__':
    raise SystemExit(main())
