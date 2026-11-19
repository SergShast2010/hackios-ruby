#!/usr/bin/env python3
"""Independent, read-only static verifier for v33 packaging.
Does NOT import the packager, its helpers, build.json, or firmware sources.
Checks final image bytes against pinned v32b and the exact requested EFI app.
Does not emulate SEC or claim a real-device/full-firmware boot test.
"""
import argparse
import gzip
import hashlib
import json
import lzma
from pathlib import Path
import re
import struct
import uuid
import zlib

BASE_HASH = '773dd3f7ba58e482f183c6154fd1626e38c06a76ec48f4948eebf6a9aa822b1c'
BDS_HASH = '07aa5089e975039423a079fd03c3939f65b0ad676f8fbde5184915ff73a94843'
A_GUID = '81da3d6b-1d18-4702-bd0f-a9a2551d6075'
B_GUID = '6d33944a-ec75-4855-a54d-809c75241f6c'
Z_GUID = '9e21fd93-9c72-4c15-8c4b-e77f1db2d792'
# From v32b build.json, independently pinned quantities and the instruction
# skeletons. In contrast to the packager, reconstruct old opcode immediates
# from OLD_VALUES instead of accepting the packager's instruction list.
OLD_VALUES = {'packed_crc': 0x9760987b, 'fat_crc': 0xe8787936, 'compressed_size': 6197249-8}
SITES = (
    (0x1500c, 0x52800016, 'packed_crc', 0),
    (0x1501c, 0x72a00016, 'packed_crc', 16),
    (0x1525c, 0x52800004, 'packed_crc', 0),
    (0x15270, 0x72a00004, 'packed_crc', 16),
    (0x15ac8, 0x52800015, 'fat_crc', 0),
    (0x15ad4, 0x72a00015, 'fat_crc', 16),
    (0x15f4c, 0x52800004, 'fat_crc', 0),
    (0x15f64, 0x72a00004, 'fat_crc', 16),
    (0x17914, 0x52800008, 'compressed_size', 0),
    (0x17918, 0x72a00008, 'compressed_size', 16),
)


def check(ok, message):
    if not ok:
        raise ValueError(message)


def rd16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def rd32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def round_up(n, a):
    return (n+a-1)//a*a


def digest(b):
    return hashlib.sha256(b).hexdigest()


def boot(b):
    check(len(b) >= 2048 and b[:8] == b'ANDROID!', 'Android header magic/length')
    ks, ka, rs, ra, ss, sa, tags, page, ver, osver = struct.unpack_from('<10I', b, 8)
    recovery_size, recovery_offset, header_size, ds, da = struct.unpack_from('<IQIIQ', b, 1632)
    check(page == 2048 and ver == 2 and header_size == 1660, 'Android header version/page/size')
    check(ss == recovery_size == recovery_offset == 0, 'unexpected Android secondary/recovery component')
    check(ka == 0x40080000 and ra == 0x51100000 and rs == 0x600000, 'Android load contract')
    check(ks > 0 and ds > 0 and da == 0x47c80000, 'Android kernel/DTB fields')
    ro = page+round_up(ks, page)
    do = ro+round_up(rs, page)
    end = do+ds
    check(round_up(end, page) == len(b), 'Android component bounds/trailer')
    check(b[page+ks:ro] == bytes(ro-page-ks) and b[ro+rs:do] == bytes(do-ro-rs) and
          b[end:] == bytes(len(b)-end), 'Android nonzero component padding')
    kernel, pack, dtb = b[page:page+ks], b[ro:ro+rs], b[do:end]
    h = hashlib.sha1()
    for part in (kernel, pack, b'', b'', dtb):
        h.update(part)
        h.update(struct.pack('<I', len(part)))
    check(b[576:608] == h.digest()+bytes(12), 'Android boot ID mismatch')
    check(dtb[:4] == b'\xd0\x0d\xfe\xed' and struct.unpack_from('>I', dtb, 4)[0] == len(dtb), 'DTB header')
    raw = gzip.decompress(kernel)
    check(len(raw) > 0x200000 and raw[0x38:0x3c] == b'ARMd', 'kernel shim/FD layout')
    return b[:page], raw[:-0x200000], raw[-0x200000:], pack, dtb


def volume(b):
    check(len(b) >= 72 and b[40:44] == b'_FVH', 'FV signature')
    check(struct.unpack_from('<Q', b, 32)[0] == len(b) and b[55] == 2, 'FV length/revision')
    hl = rd16(b, 48)
    check(72 <= hl <= len(b) and hl % 2 == 0, 'FV header length')
    check(sum(struct.unpack('<'+'H'*(hl//2), b[:hl])) & 65535 == 0, 'FV header checksum')
    p, blocks = 56, 0
    while p+8 <= hl:
        count, size = struct.unpack_from('<II', b, p)
        p += 8
        if count == size == 0:
            break
        check(count > 0 and size > 0, 'FV block map entry')
        blocks += count*size
    else:
        raise ValueError('unterminated FV block map')
    check(blocks == len(b), 'FV block map length mismatch')
    check(rd32(b, 44) & 0x800, 'unsupported FV erase polarity')
    records, p = [], hl
    while p < len(b):
        aligned = round_up(p, 8)
        check(b[p:aligned] == b'\xff'*(aligned-p), 'FFS file alignment padding')
        p = aligned
        if p == len(b):
            break
        if b[p:p+24] == b'\xff'*24:
            check(b[p:] == b'\xff'*(len(b)-p), 'FV free-space residue')
            break
        check(p+24 <= len(b), 'truncated FFS header')
        size = int.from_bytes(b[p+20:p+23], 'little')
        check(24 <= size < 0xffffff and p+size <= len(b), 'FFS bounds/size')
        blob = b[p:p+size]
        check(blob[19] & 0x3b == 0 and (p+24) % 8 == 0, 'FFS data/file alignment attributes')
        check((sum(blob[:23])-blob[17]) & 255 == 0, 'FFS header checksum')
        if blob[19] & 0x40:
            check((sum(blob[24:])+blob[17]) & 255 == 0, 'FFS data checksum')
        else:
            check(blob[17] == 0xaa, 'FFS fixed data checksum')
        check(blob[23] == 0xf8, 'FFS state not valid for pinned erase polarity')
        guid = str(uuid.UUID(bytes_le=blob[:16]))
        records.append((guid, p, blob))
        if blob[18] != 0xf0:
            sec_list(blob)  # Every non-padding FFS section is bounded/aligned.
        p += size
    check(len({g for g, _, _ in records}) == len(records), 'duplicate FFS GUID')
    return records


def sec_list(blob):
    result, p = [], 24
    while p < len(blob):
        check(p % 4 == 0 and p+4 <= len(blob), 'FFS section alignment/header')
        n = int.from_bytes(blob[p:p+3], 'little')
        check(4 <= n < 0xffffff and p+n <= len(blob), 'FFS section bounds')
        result.append((blob[p+3], blob[p+4:p+n]))
        end = p+n
        p = round_up(end, 4)
        if p < len(blob):
            check(blob[end:p] in (bytes(p-end), b'\xff'*(p-end)), 'FFS inter-section padding')
    return result


def pe_file(blob):
    found = [payload for typ, payload in sec_list(blob) if typ == 0x10]
    check(len(found) == 1, 'missing/duplicate PE32 section')
    return found[0]


def app_layout(old, new):
    # Everything except the PE payload, section size, FFS size/checksum remains.
    a, b = sec_list(old), sec_list(new)
    check([t for t, _ in a] == [t for t, _ in b], 'changed app/BDS section order/types')
    for (typ, ap), (_, bp) in zip(a, b):
        if typ != 0x10:
            check(ap == bp, 'changed non-PE application/BDS section')
    ah, bh = bytearray(old[:24]), bytearray(new[:24])
    for p in (16, 17, 20, 21, 22):
        bh[p] = ah[p]
    check(ah == bh, 'changed FFS identity/type/attributes/state')


def extract(fd):
    files = volume(fd)
    by_guid = {g: (p, b) for g, p, b in files}
    _, blob = by_guid[Z_GUID]
    s = sec_list(blob)
    check(len(s) == 1 and s[0][0] == 2, 'outer guided section layout')
    data = s[0][1]
    # sec_list removes the standard 4-byte section header.
    check(data[:16] == uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf').bytes_le,
          'unexpected guided-section decompressor GUID')
    offset = rd16(data, 16)-4
    check(20 <= offset <= len(data)-13, 'LZMA guided offset')
    stream = data[offset:]
    declared = struct.unpack_from('<Q', stream, 5)[0]
    check(8 < declared < 0x1000000 and declared != 0xffffffffffffffff,
          'SEC requires finite exact LZMA uncompressed length')
    decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE, memlimit=64*1024*1024)
    decoded = decoder.decompress(stream, max_length=declared+1)
    check(decoder.eof and not decoder.unused_data and len(decoded) == declared,
          'LZMA exact decoded length/stream termination')
    check(decoded[:4] == b'\x04\0\0\x19' and decoded[7] == 0x17 and
          int.from_bytes(decoded[4:7], 'little') == len(decoded)-4, 'FV image section wrapper size')
    inner = volume(decoded[8:])
    # No hidden nested FV_IMAGE exists in this pinned layout. If it did, it
    # would need its own checksum verification instead of being ignored.
    check(not any(t == 0x17 for _, _, f in inner if f[18] != 0xf0 for t, _ in sec_list(f)),
          'unexpected unverified nested FV')
    return files, inner, decoded, stream, data[:offset]


class ReadFat:
    """Separate read-only allocation/path validator; never used by packager."""
    def __init__(self, b):
        self.b = b
        check(len(b) == 0x1000000 and b[510:512] == b'\x55\xaa', 'FAT16 bytes/signature')
        self.bps, self.spc = rd16(b, 11), b[13]
        self.res, self.copies = rd16(b, 14), b[16]
        self.entries, self.spf = rd16(b, 17), rd16(b, 22)
        check(self.bps == 512 and self.spc > 0 and self.spc & (self.spc-1) == 0 and
              self.res > 0 and self.copies == 2 and self.entries and self.spf, 'FAT16 geometry')
        check((rd16(b, 19) or rd32(b, 32))*self.bps == len(b), 'FAT BPB total sectors')
        self.start, self.fbytes = self.res*self.bps, self.spf*self.bps
        self.root = (self.res+self.copies*self.spf)*self.bps
        self.data = self.root+round_up(self.entries*32, self.bps)
        self.unit = self.spc*self.bps
        self.count = (len(b)-self.data)//self.unit
        check(4085 <= self.count < 65525 and (self.count+2)*2 <= self.fbytes, 'FAT16 cluster count')
        self.table = b[self.start:self.start+self.fbytes]
        check(self.table == b[self.start+self.fbytes:self.start+2*self.fbytes], 'FAT mirrors mismatch')
        check(rd16(self.table, 0) & 255 == b[21] and rd16(self.table, 2) >= 0xfff8,
              'FAT reserved/media entries')
        self.owners, self.files, self.dirs = {}, {}, {}
        self.walk((), list(range(self.root, self.root+self.entries*32, 32)))
        allocated = {i for i in range(2, self.count+2) if self.link(i) != 0}
        check(allocated == set(self.owners), 'FAT lost clusters or unowned allocations')
        self.free = self.count-len(allocated)

    def link(self, c):
        return rd16(self.table, c*2)

    def pos(self, c):
        check(2 <= c < self.count+2, 'FAT data cluster bounds')
        return self.data+(c-2)*self.unit

    def chain(self, c):
        result, seen = [], set()
        if c == 0:
            return result
        while c < 0xfff8:
            check(2 <= c < self.count+2 and c not in seen, 'FAT chain cycle/bounds/free/bad link')
            seen.add(c)
            result.append(c)
            c = self.link(c)
        return result

    def walk(self, path, entries):
        for p in entries:
            if self.b[p] == 0:
                break
            e = self.b[p:p+32]
            if e[0] == 0xe5 or e[11] == 15 or e[11] & 8 or e[:11] in (b'.          ', b'..         '):
                continue
            name = e[:11]
            key = path+(name,)
            check(key not in self.files and key not in self.dirs, 'FAT duplicate directory name')
            chain = self.chain(rd16(e, 26))
            for c in chain:
                check(c not in self.owners, 'FAT cross-linked file/directory')
                self.owners[c] = key
            if e[11] & 16:
                check(chain and rd32(e, 28) == 0, 'invalid FAT directory')
                self.dirs[key] = (p, chain)
                slots = [o for c in chain for o in range(self.pos(c), self.pos(c)+self.unit, 32)]
                self.walk(key, slots)
            else:
                n = rd32(e, 28)
                check(len(chain) == round_up(n, self.unit)//self.unit, 'FAT file chain/size mismatch')
                content = b''.join(self.b[self.pos(c):self.pos(c)+self.unit] for c in chain)[:n]
                self.files[key] = (p, chain, content)


def unpack_transport(pack):
    check(len(pack) == 0x600000, 'transport must remain exactly 6 MiB')
    used, decoded = rd32(pack, 0)+8, rd32(pack, 4)
    check(8 < used <= len(pack) and decoded == 0x1000000, 'UEFI compression header')
    check(pack[used:] == bytes(len(pack)-used), 'transport padding must be zero')
    import EfiCompressor
    fat = bytes(EfiCompressor.UefiDecompress(pack[:used], used))
    check(len(fat) == decoded, 'UEFI decompressed length')
    return used, ReadFat(fat)


def verify_fat(old, new, app):
    key = (b'EFI        ', b'BOOT       ', b'BOOTAA64EFI')
    check(old.files.keys() == new.files.keys() and old.dirs.keys() == new.dirs.keys(), 'FAT paths changed')
    op, oc, old_app = old.files[key]
    np, nc, new_app = new.files[key]
    check(new_app == app and op == np, 'FAT EFI bytes or directory-entry position')
    for name in old.files:
        if name != key:
            check(old.files[name] == new.files[name], 'non-app FAT file bytes/chain changed: '+repr(name))
    check(old.dirs == new.dirs, 'FAT directory allocation changed')
    payload = new.files[(b'PAYLOAD XNP',)][2]
    check(payload == old.files[(b'PAYLOAD XNP',)][2], 'kernel payload changed')
    check(digest(payload[32:32+15623056]) == '9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c',
          'pinned XNU payload hash mismatch')
    # Strong byte-level whitelist: only old/new app cluster data, their FAT
    # entries, and app directory start-cluster/file-size may differ.
    restored = bytearray(new.b)
    for c in set(oc+nc):
        if c not in oc:
            check(old.link(c) == 0, 'new app stole an allocated cluster')
        if c not in nc:
            check(new.link(c) == 0, 'released app cluster not free')
        p = new.pos(c)
        restored[p:p+new.unit] = old.b[p:p+old.unit]
        for i in range(2):
            p = new.start+i*new.fbytes+c*2
            restored[p:p+2] = old.b[p:p+2]
    restored[np+26:np+32] = old.b[op+26:op+32]
    check(restored == old.b, 'FAT changes outside app-only allocation whitelist')
    return payload, old_app, len(oc), len(nc)


def pe_map(b, app=False):
    check(len(b) >= 64 and b[:2] == b'MZ', 'PE DOS header')
    pe = rd32(b, 0x3c)
    check(pe+24 <= len(b) and b[pe:pe+4] == b'PE\0\0' and rd16(b, pe+4) == 0xaa64, 'PE AArch64 signature')
    n, osize = rd16(b, pe+6), rd16(b, pe+20)
    opt = pe+24
    check(n > 0 and osize >= 112 and opt+osize+n*40 <= len(b), 'PE section table')
    check(rd16(b, opt) == 0x20b and rd32(b, opt+56) and rd32(b, opt+60) <= len(b), 'PE32+ image sizes')
    if app:
        check(rd16(b, opt+68) == 10, 'PE EFI application subsystem')
    rows = []
    for i in range(n):
        p = opt+osize+i*40
        va, size, raw = rd32(b, p+12), rd32(b, p+16), rd32(b, p+20)
        check(raw+size <= len(b), 'PE raw section bounds')
        rows.append((va, size, raw))
    return rows


def check_bds(old, new, oldfat, newfat, pack, used):
    check(digest(old) == BDS_HASH and len(new) == len(old), 'pinned BDS identity/length')
    mapping = pe_map(old)
    values = {'packed_crc': zlib.crc32(pack), 'fat_crc': zlib.crc32(newfat), 'compressed_size': used-8}
    restored = bytearray(new)
    decoded = {}
    for rva, skeleton, quantity, shift in SITES:
        matches = [raw+rva-va for va, size, raw in mapping if va <= rva and rva+4 <= va+size]
        check(len(matches) == 1, 'BDS RVA mapping')
        p = matches[0]
        a, b = rd32(old, p), rd32(new, p)
        expected_old = skeleton | ((OLD_VALUES[quantity] >> shift & 65535) << 5)
        check(a == expected_old, 'OLD v32b opcode not equal to build.json-derived pinned value: '+hex(rva))
        check(b & ~0x1fffe0 == skeleton, 'BDS non-immediate instruction bits changed')
        imm = b >> 5 & 65535
        check(imm == values[quantity] >> shift & 65535, 'BDS immediate not equal to actual RAM-pack value')
        decoded[hex(rva)] = imm
        restored[p:p+4] = old[p:p+4]
    old_chunks = struct.pack('<16I', *(zlib.crc32(oldfat[p:p+1048576]) for p in range(0, len(oldfat), 1048576)))
    check(old.count(old_chunks) == 1 and old.find(old_chunks) == 99536, 'pinned old BDS CRC table')
    p = old.find(old_chunks)
    new_chunks = struct.pack('<16I', *(zlib.crc32(newfat[p:p+1048576]) for p in range(0, len(newfat), 1048576)))
    check(new[p:p+64] == new_chunks, 'BDS MiB CRC table mismatch')
    restored[p:p+64] = old[p:p+64]
    check(restored == old, 'BDS logic/data changed outside allowed CRC/size fields')
    return values, decoded


def metadata(directory, values, used, fat):
    text = (directory/'RamPack.h').read_text()
    def macro(name):
        m = re.search(r'^#define\s+'+re.escape(name)+r'\s+(0x[0-9a-fA-F]+|[0-9]+)(?:ULL|U)?\s*$', text, re.M)
        check(m is not None, 'missing metadata macro '+name)
        return int(m.group(1), 0)
    expected = {'RUBY_PACK_BASE': 0x51100000, 'RUBY_PACK_BYTES': 0x600000,
                'RUBY_PACK_USED': used, 'RUBY_PACK_CRC': values['packed_crc'],
                'RUBY_FAT_BASE': 0x66000000, 'RUBY_FAT_BYTES': 0x1000000}
    check(all(macro(k) == v for k, v in expected.items()), 'RamPack.h does not match final bytes/BDS')
    text = (directory/'EbsPayload.h').read_text()
    check(macro('EBS_FAT_CRC') == values['fat_crc'], 'EbsPayload.h FAT CRC')
    m = re.search(r'^#define\s+EBS_FAT_MIB_CRC\s+\{([^}]*)\}\s*$', text, re.M)
    check(m is not None, 'missing chunk macro')
    chunk_list = [int(x.strip().rstrip('U'), 0) for x in m.group(1).split(',')]
    check(chunk_list == [zlib.crc32(fat[p:p+1048576]) for p in range(0, len(fat), 1048576)], 'metadata CRC chunks')


def verify(base, image, app, metadata_dir=None):
    check(digest(base) == BASE_HASH, 'base is not confirmed v32b')
    pe_map(app, app=True)
    oh, oshim, ofd, opack, odtb = boot(base)
    nh, nshim, nfd, npack, ndtb = boot(image)
    check(oshim == nshim and odtb == ndtb, 'shim/DTB/LK handoff policy changed')
    restored = bytearray(nh)
    restored[8:12], restored[576:608] = oh[8:12], oh[576:608]
    check(restored == oh, 'Android load addresses/cmdline/header changed')
    outer0, inner0, decoded0, stream0, guide0 = extract(ofd)
    outer1, inner1, decoded1, stream1, guide1 = extract(nfd)
    check([g for g, _, _ in outer0] == [g for g, _, _ in outer1], 'outer firmware files/order changed')
    check(guide0 == guide1, 'LZMA guided-section metadata changed')
    for (g, p, a), (_, q, b) in zip(outer0, outer1):
        check(p == q, 'outer FFS position changed')
        if g != Z_GUID:
            check(a == b, 'SEC or other outer FFS changed')
        else:
            ah, bh = bytearray(a[:24]), bytearray(b[:24])
            for i in (16, 17, 20, 21, 22):
                bh[i] = ah[i]
            check(ah == bh, 'outer compressed FFS policy changed')
    check(ofd[:outer0[-1][1]] == nfd[:outer1[-1][1]], 'outer FD header/SEC bytes changed')
    check([g for g, _, _ in inner0] == [g for g, _, _ in inner1], 'inner FV files/order changed')
    vf0, vf1 = decoded0[8:], decoded1[8:]
    h0, h1 = vf0[:rd16(vf0, 48)], bytearray(vf1[:rd16(vf1, 48)])
    check(len(h0) == len(h1) and len(vf1) >= len(vf0), 'inner FV header/shrink')
    for start, count in ((32, 8), (50, 2), (56, 4)):
        h1[start:start+count] = h0[start:start+count]
    check(h1 == h0, 'inner FV header policy changed')
    fs0, fs1 = {g: f for g, _, f in inner0}, {g: f for g, _, f in inner1}
    for g in fs0:
        if g in (A_GUID, B_GUID):
            app_layout(fs0[g], fs1[g])
        else:
            check(fs0[g] == fs1[g], 'unrelated firmware module changed: '+g)
    check(pe_file(fs1[A_GUID]) == app, 'FV EFI app is not exact CLI input')
    oldused, oldfat = unpack_transport(opack)
    used, newfat = unpack_transport(npack)
    check(oldused == OLD_VALUES['compressed_size']+8 and zlib.crc32(opack) == OLD_VALUES['packed_crc'] and
          zlib.crc32(oldfat.b) == OLD_VALUES['fat_crc'], 'base RAM-pack provenance')
    payload, old_app, old_clusters, new_clusters = verify_fat(oldfat, newfat, app)
    check(old_app == pe_file(fs0[A_GUID]), 'base EFI copies not synchronized')
    values, immediates = check_bds(pe_file(fs0[B_GUID]), pe_file(fs1[B_GUID]), oldfat.b, newfat.b, npack, used)
    if metadata_dir is not None:
        metadata(metadata_dir, values, used, newfat.b)
    return {'result': 'PASS', 'version': '0.38', 'base_sha256': digest(base),
            'image_sha256': digest(image), 'image_bytes': len(image),
            'app_sha256': digest(app), 'app_bytes': len(app), 'both_app_copies_exact': True,
            'payload_sha256': digest(payload), 'payload_bytes': len(payload),
            'exact_kernel_payload_preserved': True, 'shim_DTB_LK_policy_preserved': True,
            'android_v2_header_and_ID_valid': True, 'all_FV_FFS_checksums_and_alignment_valid': True,
            'fixed_FD_bytes': len(nfd), 'decoded_fv_bytes': len(vf1),
            'LZMA_finite_uncompressed_bytes': struct.unpack_from('<Q', stream1, 5)[0],
            'LZMA_decoded_bytes': len(decoded1), 'transport_bytes': len(npack), 'packed_used': used,
            'decoded_FAT_bytes': len(newfat.b), 'packed_crc': hex(values['packed_crc']),
            'fat_crc': hex(values['fat_crc']), 'BDS_compressed_size': values['compressed_size'],
            'BDS_pinned_OLD_opcodes_valid': True, 'BDS_only_allowed_immediate_and_CRC_table_changes': True,
            'BDS_logic_unchanged': True, 'BDS_decoded_immediates': immediates,
            'old_app_clusters': old_clusters, 'new_app_clusters': new_clusters,
            'FAT_free_clusters': newfat.free, 'FAT_crosslinks_or_lost_clusters': False,
            'metadata_headers_checked': metadata_dir is not None,
            'byte_identical_to_base': image == base, 'on_device_tested': False,
            'scope': 'independent final-byte static validation; no SEC execution, full firmware or hardware boot'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--app', type=Path, required=True)
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--metadata-dir', type=Path)
    ap.add_argument('--report', type=Path)
    args = ap.parse_args()
    if args.report:
        inputs = (args.base.resolve(), args.app.resolve(), args.image.resolve())
        check(args.report.resolve() not in inputs, 'report must not overwrite an input')
    report = verify(args.base.read_bytes(), args.image.read_bytes(), args.app.read_bytes(), args.metadata_dir)
    text = json.dumps(report, indent=2)+'\n'
    if args.report:
        args.report.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, struct.error, lzma.LZMAError) as exc:
        raise SystemExit('verification FAILED: '+str(exc))
