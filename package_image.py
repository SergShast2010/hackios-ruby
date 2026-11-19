#!/usr/bin/env python3
"""Local-only v33 packaging from the exact confirmed v32b image.
No build tools, source downloads, device access or firmware recompilation.
Dependency already used by v32b: EfiCompressor (UEFI/Tiano compression).
Run verify_packaging.py separately against the resulting image.
"""
import argparse
import gzip
import hashlib
import json
import lzma
from pathlib import Path
import struct
import uuid
import zlib

BASE_SHA256 = '773dd3f7ba58e482f183c6154fd1626e38c06a76ec48f4948eebf6a9aa822b1c'
BDS_SHA256 = '07aa5089e975039423a079fd03c3939f65b0ad676f8fbde5184915ff73a94843'
APP_GUID = '81da3d6b-1d18-4702-bd0f-a9a2551d6075'
BDS_GUID = '6d33944a-ec75-4855-a54d-809c75241f6c'
LZMA_GUID = '9e21fd93-9c72-4c15-8c4b-e77f1db2d792'
FD_SIZE, PACK_SIZE, FAT_SIZE = 0x200000, 0x600000, 0x1000000
OLD_PACK_CRC, OLD_FAT_CRC, OLD_USED = 0x9760987b, 0xe8787936, 6197249
# These are the *new_word* values in confirmed v32b/build.json: now OLD v33
# opcodes. They are deliberately NOT the v32a old_word values in that file.
# rva, exact old v32b word, quantity, halfword shift
MOV_PATCHES = (
    (0x1500c, 0x52930f76, 'pack_crc', 0),
    (0x1501c, 0x72b2ec16, 'pack_crc', 16),
    (0x1525c, 0x52930f64, 'pack_crc', 0),
    (0x15270, 0x72b2ec04, 'pack_crc', 16),
    (0x15ac8, 0x528f26d5, 'fat_crc', 0),
    (0x15ad4, 0x72bd0f15, 'fat_crc', 16),
    (0x15f4c, 0x528f26c4, 'fat_crc', 0),
    (0x15f64, 0x72bd0f04, 'fat_crc', 16),
    (0x17914, 0x5291ff28, 'compressed_bytes', 0),
    (0x17918, 0x72a00bc8, 'compressed_bytes', 16),
)
IMM_MASK = 0x1fffe0


def need(condition, message):
    if not condition:
        raise ValueError(message)


def align(n, a):
    return (n + a - 1) // a * a


def u16(b, p):
    return struct.unpack_from('<H', b, p)[0]


def u32(b, p):
    return struct.unpack_from('<I', b, p)[0]


def sha(b):
    return hashlib.sha256(b).hexdigest()


def split_boot(b):
    need(b[:8] == b'ANDROID!', 'bad Android magic')
    page, ver = u32(b, 36), u32(b, 40)
    need(page == 2048 and ver == 2 and u32(b, 24) == 0, 'unsupported Android header')
    need(struct.unpack_from('<I', b, 1632)[0] == 0 and u32(b, 1644) == 1660,
         'unexpected recovery/header size')
    ks, rs, ds = u32(b, 8), u32(b, 16), u32(b, 1648)
    ro = page + align(ks, page)
    do = ro + align(rs, page)
    need(len(b) == align(do + ds, page), 'unexpected boot length/trailer')
    kernel, pack, dtb = b[page:page+ks], b[ro:ro+rs], b[do:do+ds]
    need(len(pack) == PACK_SIZE, 'transport must be exactly 6 MiB')
    digest = hashlib.sha1()
    for component in (kernel, pack, b'', b'', dtb):
        digest.update(component)
        digest.update(struct.pack('<I', len(component)))
    need(b[576:608] == digest.digest() + bytes(12), 'invalid Android boot ID')
    return page, b[:page], kernel, pack, dtb


def fv_files(fv):
    need(fv[40:44] == b'_FVH' and struct.unpack_from('<Q', fv, 32)[0] == len(fv),
         'invalid firmware volume')
    hl = u16(fv, 48)
    need(hl >= 64 and hl % 2 == 0 and hl <= len(fv), 'invalid FV header length')
    need(sum(struct.unpack('<' + 'H' * (hl//2), fv[:hl])) & 65535 == 0,
         'bad FV checksum')
    out = []
    p = hl
    while align(p, 8) + 24 <= len(fv):
        p = align(p, 8)
        if fv[p:p+24] == b'\xff' * 24:
            need(fv[p:] == b'\xff' * (len(fv)-p), 'unexpected FV free-space contents')
            break
        n = int.from_bytes(fv[p+20:p+23], 'little')
        need(24 <= n < 0xffffff and p+n <= len(fv), 'invalid FFS size')
        h = fv[p:p+24]
        need((sum(h[:23])-h[17]) & 255 == 0, 'bad FFS header checksum')
        need(((sum(fv[p+24:p+n])+h[17]) & 255 == 0) if h[19] & 0x40 else h[17] == 0xaa,
             'bad FFS data checksum')
        # All pinned v32b files use 8-byte file alignment and no stronger
        # data alignment. Reject any future incompatible layout, not guess.
        need(h[19] & 0x3b == 0, 'unsupported extended/aligned FFS')
        out.append((str(uuid.UUID(bytes_le=bytes(h[:16]))), p, bytes(fv[p:p+n])))
        p += n
    return out


def checksum_ffs(blob):
    b = bytearray(blob)
    need(24 <= len(b) < 0xffffff, 'FFS exceeds normal 24-bit size')
    b[20:23] = len(b).to_bytes(3, 'little')
    b[17] = (-sum(b[24:])) & 255 if b[19] & 0x40 else 0xaa
    b[16] = 0
    b[16] = (-sum(b[:23]) + b[17]) & 255
    return bytes(b)


def sections(blob):
    out, p = [], 24
    while p < len(blob):
        n = int.from_bytes(blob[p:p+3], 'little')
        need(n >= 4 and p+n <= len(blob) and n < 0xffffff, 'invalid section')
        out.append((p, n, blob[p+3]))
        p = align(p+n, 4)
    return out


def pe_of_ffs(blob):
    found = [(p, n) for p, n, kind in sections(blob) if kind == 0x10]
    need(len(found) == 1, 'expected exactly one PE32 section')
    p, n = found[0]
    return blob[p+4:p+n]


def replace_pe(blob, new_pe):
    out = bytearray(blob[:24])
    replaced = False
    for i, (p, n, kind) in enumerate(sections(blob)):
        if i:
            out.extend(bytes(align(len(out), 4) - len(out)))
        if kind == 0x10:
            need(not replaced, 'duplicate PE section')
            need(len(new_pe)+4 < 0xffffff, 'PE section too large')
            out.extend((len(new_pe)+4).to_bytes(3, 'little') + b'\x10' + new_pe)
            replaced = True
        else:
            out.extend(blob[p:p+n])
    need(replaced, 'missing PE section')
    return checksum_ffs(out)


def pe_offsets(pe):
    need(pe[:2] == b'MZ' and len(pe) >= 64, 'app must be a PE image')
    at = u32(pe, 0x3c)
    need(at + 24 <= len(pe) and pe[at:at+4] == b'PE\0\0', 'bad PE signature')
    n, opt_size = u16(pe, at+6), u16(pe, at+20)
    opt = at + 24
    need(u16(pe, at+4) == 0xaa64 and opt_size >= 112 and opt+opt_size+n*40 <= len(pe),
         'PE must be AArch64 with a bounded section table')
    need(u16(pe, opt) == 0x20b, 'PE must be PE32+')
    offsets = []
    for i in range(n):
        p = opt+opt_size+i*40
        va, size, raw = u32(pe, p+12), u32(pe, p+16), u32(pe, p+20)
        need(raw+size <= len(pe), 'PE section outside file')
        offsets.append((va, size, raw))
    need(n and u32(pe, opt+56) and u32(pe, opt+60) <= len(pe), 'bad PE sizes')
    return opt, offsets


class Fat16:
    def __init__(self, image):
        self.b = bytearray(image)
        need(len(image) == FAT_SIZE and image[510:512] == b'\x55\xaa', 'bad FAT size/signature')
        self.bps, self.spc = u16(image, 11), image[13]
        self.res, self.nf, self.roots, self.spf = u16(image, 14), image[16], u16(image, 17), u16(image, 22)
        need(self.bps == 512 and self.spc and self.spc & (self.spc-1) == 0,
             'unsupported FAT cluster geometry')
        need(self.nf == 2 and self.res and self.spf and self.roots, 'unsupported FAT16 BPB')
        total = u16(image, 19) or u32(image, 32)
        need(total*self.bps == len(image), 'FAT BPB volume length mismatch')
        self.fat_start = self.res*self.bps
        self.fat_bytes = self.spf*self.bps
        self.root = (self.res+self.nf*self.spf)*self.bps
        self.data = self.root+align(self.roots*32, self.bps)
        self.unit = self.bps*self.spc
        self.count = (len(image)-self.data)//self.unit
        need(4085 <= self.count < 65525 and (self.count+2)*2 <= self.fat_bytes, 'not FAT16')
        need(image[self.fat_start:self.fat_start+self.fat_bytes] ==
             image[self.fat_start+self.fat_bytes:self.fat_start+2*self.fat_bytes], 'FAT copies differ')

    def value(self, c):
        return u16(self.b, self.fat_start+c*2)

    def set_value(self, c, v):
        for i in range(self.nf):
            struct.pack_into('<H', self.b, self.fat_start+i*self.fat_bytes+c*2, v)

    def location(self, c):
        need(2 <= c < self.count+2, 'FAT cluster outside data area')
        return self.data+(c-2)*self.unit

    def chain(self, c):
        out, seen = [], set()
        if c == 0:
            return out
        while c < 0xfff8:
            need(2 <= c < self.count+2 and c not in seen, 'invalid/cyclic FAT chain')
            seen.add(c)
            out.append(c)
            c = self.value(c)
        return out

    def directory(self, clusters=None):
        if clusters is None:
            return list(range(self.root, self.root+self.roots*32, 32))
        return [p for c in clusters for p in range(self.location(c), self.location(c)+self.unit, 32)]

    def entry(self, path):
        places = self.directory()
        for i, name in enumerate(path):
            found = None
            for p in places:
                if self.b[p] == 0:
                    break
                if self.b[p] != 0xe5 and self.b[p+11] != 15 and self.b[p:p+11] == name:
                    found = p
                    break
            need(found is not None, 'FAT path missing: '+repr(name))
            if i+1 < len(path):
                need(self.b[found+11] & 16, 'FAT parent is not a directory')
                places = self.directory(self.chain(u16(self.b, found+26)))
            else:
                need(not self.b[found+11] & 16, 'FAT path is not a file')
                return found
        raise ValueError('empty FAT path')

    def read(self, p):
        chain = self.chain(u16(self.b, p+26))
        n = u32(self.b, p+28)
        need(n <= len(chain)*self.unit, 'FAT file exceeds chain')
        return b''.join(self.b[self.location(c):self.location(c)+self.unit] for c in chain)[:n]

    def replace(self, p, content):
        old = self.chain(u16(self.b, p+26))
        if self.read(p) == content:
            return {'old_clusters': len(old), 'new_clusters': len(old), 'added_clusters': 0}
        count = align(len(content), self.unit)//self.unit
        need(count > 0, 'empty EFI app')
        free = [c for c in range(2, self.count+2) if self.value(c) == 0]
        need(max(0, count-len(old)) <= len(free), 'insufficient free FAT16 clusters')
        new = old[:count]+free[:max(0, count-len(old))]
        for i, c in enumerate(new):
            self.set_value(c, new[i+1] if i+1 < len(new) else 0xffff)
            p0 = self.location(c)
            self.b[p0:p0+self.unit] = content[i*self.unit:(i+1)*self.unit].ljust(self.unit, b'\0')
        for c in old[count:]:
            self.set_value(c, 0)
            p0 = self.location(c)
            self.b[p0:p0+self.unit] = bytes(self.unit)
        struct.pack_into('<H', self.b, p+26, new[0])
        struct.pack_into('<I', self.b, p+28, len(content))
        return {'old_clusters': len(old), 'new_clusters': len(new),
                'added_clusters': max(0, count-len(old)), 'free_clusters_after':
                sum(self.value(c) == 0 for c in range(2, self.count+2))}


def patch_bds(old, old_fat, new_fat, pack, used):
    need(sha(old) == BDS_SHA256, 'base BDS SHA mismatch')
    _, offsets = pe_offsets(old)
    new = bytearray(old)
    values = {'pack_crc': zlib.crc32(pack), 'fat_crc': zlib.crc32(new_fat), 'compressed_bytes': used-8}
    old_values = {'pack_crc': OLD_PACK_CRC, 'fat_crc': OLD_FAT_CRC, 'compressed_bytes': OLD_USED-8}
    report = []
    for rva, pinned_word, which, shift in MOV_PATCHES:
        matches = [raw+rva-va for va, size, raw in offsets if va <= rva and rva+4 <= va+size]
        need(len(matches) == 1, 'ambiguous/missing BDS patch RVA')
        p = matches[0]
        need(u32(old, p) == pinned_word, 'pinned OLD v32b opcode mismatch at '+hex(rva))
        need((pinned_word >> 5 & 65535) == (old_values[which] >> shift & 65535),
             'pinned BDS old value inconsistent with build.json')
        value = values[which] >> shift & 65535
        word = (pinned_word & ~IMM_MASK) | (value << 5)
        struct.pack_into('<I', new, p, word)
        report.append({'rva': hex(rva), 'file_offset': p, 'old_word': hex(pinned_word),
                       'new_word': hex(word), 'new_immediate': value})
    old_chunks = struct.pack('<16I', *(zlib.crc32(old_fat[p:p+0x100000]) for p in range(0, FAT_SIZE, 0x100000)))
    chunks = [zlib.crc32(new_fat[p:p+0x100000]) for p in range(0, FAT_SIZE, 0x100000)]
    need(old.count(old_chunks) == 1 and old.find(old_chunks) == 99536, 'pinned BDS chunk table mismatch')
    table = old.find(old_chunks)
    new[table:table+64] = struct.pack('<16I', *chunks)
    return bytes(new), report, table, chunks


def repack_fv(old, replacements):
    files = fv_files(old)
    out = bytearray(old[:u16(old, 48)])
    for guid, _, blob in files:
        out.extend(b'\xff'*(align(len(out), 8)-len(out)))
        out.extend(replacements.get(guid, blob))
    out.extend(b'\xff'*(align(len(out), 8)-len(out)))
    new_size = max(len(old), align(len(out), 4096))
    need(new_size+8 < 0xffffff, 'decoded FV exceeds 24-bit FV section')
    out.extend(b'\xff'*(new_size-len(out)))
    if new_size != len(old):
        need(u32(old, 60) == 4096 and old[64:72] == bytes(8), 'unsupported FV block map')
        struct.pack_into('<Q', out, 32, new_size)
        struct.pack_into('<I', out, 56, new_size//4096)
        struct.pack_into('<H', out, 50, 0)
        hl = u16(out, 48)
        struct.pack_into('<H', out, 50, (-sum(struct.unpack('<'+'H'*(hl//2), out[:hl]))) & 65535)
    fv_files(out)
    return bytes(out)


def build(base, app):
    need(sha(base) == BASE_SHA256, 'base must be the exact confirmed v32b image')
    opt, _ = pe_offsets(app)
    need(u16(app, opt+68) == 10, 'app must be an EFI Application (subsystem 10)')
    page, header, old_kernel, old_pack, dtb = split_boot(base)
    raw = gzip.decompress(old_kernel)
    need(len(raw) > FD_SIZE, 'missing shim/FD')
    shim, fd = raw[:-FD_SIZE], raw[-FD_SIZE:]
    outer = {g: (p, blob) for g, p, blob in fv_files(fd)}
    zp, zfile = outer[LZMA_GUID]
    zs = sections(zfile)
    need(len(zs) == 1 and zs[0][2] == 2, 'unexpected outer guided section')
    sp, sn, _ = zs[0]
    section = zfile[sp:sp+sn]
    offset = u16(section, 20)
    need(24 <= offset <= len(section)-13, 'invalid guided data offset')
    compressed = section[offset:]
    decoded = lzma.decompress(compressed, format=lzma.FORMAT_ALONE)
    need(struct.unpack_from('<Q', compressed, 5)[0] == len(decoded), 'base has unknown/wrong LZMA length')
    need(decoded[:4] == b'\x04\0\0\x19' and decoded[7] == 0x17 and
         int.from_bytes(decoded[4:7], 'little') == len(decoded)-4, 'unexpected FV image wrapper')
    prefix, fv = decoded[:8], decoded[8:]
    inner = {g: blob for g, _, blob in fv_files(fv)}
    old_app = pe_of_ffs(inner[APP_GUID])
    used0 = u32(old_pack, 0)+8
    need(used0 == OLD_USED and zlib.crc32(old_pack) == OLD_PACK_CRC, 'base RAM-pack mismatch')
    import EfiCompressor
    fat0 = bytes(EfiCompressor.UefiDecompress(old_pack[:used0], used0))
    need(len(fat0) == FAT_SIZE and zlib.crc32(fat0) == OLD_FAT_CRC, 'base decoded FAT mismatch')
    fat = Fat16(fat0)
    app_entry = fat.entry([b'EFI        ', b'BOOT       ', b'BOOTAA64EFI'])
    need(fat.read(app_entry) == old_app, 'base EFI copies differ')
    payload_entry = fat.entry([b'PAYLOAD XNP'])
    payload0 = fat.read(payload_entry)
    allocation = fat.replace(app_entry, app)
    need(fat.read(app_entry) == app and fat.read(payload_entry) == payload0, 'FAT replacement damaged content')
    fat_bytes = bytes(fat.b)
    if fat_bytes == fat0:
        pack, used = old_pack, used0
    else:
        stream = bytes(EfiCompressor.UefiCompress(fat_bytes, len(fat_bytes)))
        need(len(stream) >= 8 and u32(stream, 0)+8 == len(stream) and u32(stream, 4) == FAT_SIZE,
             'invalid UEFI compression header')
        used = len(stream)
        need(used <= PACK_SIZE, 'compressed FAT does not fit the fixed 6 MiB transport')
        need(bytes(EfiCompressor.UefiDecompress(stream, len(stream))) == fat_bytes,
             'RAM-pack compression did not round-trip')
        pack = stream + bytes(PACK_SIZE-used)
    bds, patches, table, chunks = patch_bds(pe_of_ffs(inner[BDS_GUID]), fat0, fat_bytes, pack, used)
    fv_new = repack_fv(fv, {APP_GUID: replace_pe(inner[APP_GUID], app),
                             BDS_GUID: replace_pe(inner[BDS_GUID], bds)})
    # If app/pack did not change, retain every original compressed byte.
    if fv_new == fv:
        fd_new = fd
    else:
        prefix_new = prefix[:4] + (len(fv_new)+4).to_bytes(3, 'little') + prefix[7:8]
        source = prefix_new+fv_new
        comp = lzma.compress(source, format=lzma.FORMAT_ALONE, preset=6)
        # Python emits unknown (UINT64_MAX) length by default. SEC requires the
        # exact finite uncompressed byte count, including both wrapper sections.
        comp = comp[:5] + struct.pack('<Q', len(source)) + comp[13:]
        need(lzma.decompress(comp, format=lzma.FORMAT_ALONE) == source, 'LZMA round-trip failed')
        sec_head = bytearray(section[:offset])
        sec_head[:3] = (offset+len(comp)).to_bytes(3, 'little')
        znew = checksum_ffs(zfile[:24]+sec_head+comp)
        need(fd[zp+len(zfile):] == b'\xff'*(FD_SIZE-zp-len(zfile)), 'guided FFS is not last')
        need(zp+len(znew) <= FD_SIZE, 'repacked compressed firmware does not fit fixed 2 MiB FD')
        fd_new = fd[:zp] + znew + b'\xff'*(FD_SIZE-zp-len(znew))
        fv_files(fd_new)
    kernel = old_kernel if fd_new == fd else gzip.compress(shim+fd_new, compresslevel=9, mtime=0)
    hdr = bytearray(header)
    struct.pack_into('<I', hdr, 8, len(kernel))
    digest = hashlib.sha1()
    for component in (kernel, pack, b'', b'', dtb):
        digest.update(component)
        digest.update(struct.pack('<I', len(component)))
    hdr[576:608] = digest.digest()+bytes(12)
    out = bytearray(hdr)
    for component in (kernel, pack, dtb):
        out.extend(component)
        out.extend(bytes(align(len(out), page)-len(out)))
    split_boot(out)
    report = {'version': '0.38b', 'base_sha256': sha(base), 'image_sha256': sha(out),
              'image_bytes': len(out), 'app_sha256': sha(app), 'app_bytes': len(app),
              'old_app_bytes': len(old_app), 'byte_identical_to_base': bytes(out) == base,
              'fd_bytes': len(fd_new), 'decoded_fv_bytes': len(fv_new),
              'lzma_uncompressed_bytes': len(fv_new)+8, 'packed_bytes': len(pack),
              'packed_used': used, 'packed_crc': hex(zlib.crc32(pack)),
              'fat_bytes': len(fat_bytes), 'fat_crc': hex(zlib.crc32(fat_bytes)),
              'payload_sha256': sha(payload0), 'payload_bytes': len(payload0),
              'shim_sha256': sha(shim), 'dtb_sha256': sha(dtb), 'BDS_sha256': sha(bds),
              'BDS_patches': patches, 'BDS_chunk_table_offset': table,
              'BDS_change_scope': 'only ten pinned MOV immediate fields and 16 CRC32 words',
              'fat_allocation': allocation, 'on_device_tested': False,
              'verification': 'run independent verify_packaging.py; build metadata is not proof'}
    return bytes(out), report, chunks


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--app', type=Path, required=True)
    ap.add_argument('--output-dir', type=Path, required=True)
    args = ap.parse_args()
    base_path, app_path = args.base.resolve(), args.app.resolve()
    output = args.output_dir.resolve()
    image_path = output/'Mu-ruby-v0.38b-gicgate.img'
    destinations = (image_path, output/'build.json', output/'RamPack.h', output/'EbsPayload.h')
    need(not any(p in (base_path, app_path) for p in destinations), 'output must not overwrite either input')
    image, report, chunks = build(base_path.read_bytes(), app_path.read_bytes())
    output.mkdir(parents=True, exist_ok=True)
    # Writes happen only after all packaging guards have passed.
    image_path.write_bytes(image)
    (output/'build.json').write_text(json.dumps(report, indent=2)+'\n')
    (output/'RamPack.h').write_text(
        '#ifndef RUBY_RAM_PACK_H\n#define RUBY_RAM_PACK_H\n'
        '#define RUBY_PACK_BASE 0x51100000ULL\n#define RUBY_PACK_BYTES 0x600000U\n'
        '#define RUBY_PACK_USED 0x%xU\n#define RUBY_PACK_CRC %sU\n'
        '#define RUBY_FAT_BASE 0x66000000ULL\n#define RUBY_FAT_BYTES 0x1000000U\n#endif\n'
        % (report['packed_used'], report['packed_crc']))
    (output/'EbsPayload.h').write_text('#define EBS_FAT_CRC %sU\n#define EBS_FAT_MIB_CRC {%s}\n'
                                       % (report['fat_crc'], ','.join('0x%08xU' % c for c in chunks)))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, struct.error, lzma.LZMAError) as exc:
        raise SystemExit('packaging refused: '+str(exc))
