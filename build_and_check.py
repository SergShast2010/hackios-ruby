#!/usr/bin/env python3
"""Pack ruby v0.26 from an already rebuilt DEBUG FD; test bytes from the result.
Hardware, GIC, MMU transitions, timers, and actual on-device boot are NOT emulated.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BASE = 0x40080000
FD_BASE = 0x40400000
FD_SIZE = 0x200000
FB = 0x7C406000
PAGE_SIZE = 0x9F6000
VISIBLE_SIZE = 1080 * 2400 * 4
RES_BASE, RES_SIZE = 0x7BA10000, 0x1DF0000
WDT = 0x10007000


def run(*args):
    subprocess.run([str(a) for a in args], check=True, cwd=HERE)


def unpack(path):
    data = path.read_bytes()
    assert data[:8] == b'ANDROID!'
    vals = struct.unpack_from('<10I', data, 8)
    ks, ka, rs, ra, ss, sa, tags, page, version, osv = vals
    rec, _, _, ds, da = struct.unpack_from('<IQIIQ', data, 1632)
    assert page == 2048 and version == 2 and ss == rec == 0
    al = lambda x: (x + page - 1) // page * page
    ro = page + al(ks)
    do = ro + al(rs)
    cmd = data[64:576].split(b'\0')[0] + data[608:1632].split(b'\0')[0]
    h = dict(kernel_addr=ka, ramdisk_addr=ra, second_addr=sa, tags=tags, page=page, version=version, os_version=osv, dtb_addr=da)
    return h, cmd, data[page:page+ks], data[ro:ro+rs], data[do:do+ds]


def pe_info(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[pe:pe+4] == b'PE\0\0'
    sections = struct.unpack_from('<H', data, pe+6)[0]
    opt_size = struct.unpack_from('<H', data, pe+20)[0]
    opt = pe + 24
    assert struct.unpack_from('<H', data, opt)[0] == 0x20b
    base = struct.unpack_from('<Q', data, opt+24)[0]
    size = struct.unpack_from('<I', data, opt+56)[0]
    header_size = struct.unpack_from('<I', data, opt+60)[0]
    entry = struct.unpack_from('<I', data, opt+16)[0]
    image = bytearray(size)
    image[:header_size] = data[:header_size]
    for i in range(sections):
        off = opt + opt_size + i*40
        vsize, rva, rsize, raw = struct.unpack_from('<4I', data, off+8)
        image[rva:rva+rsize] = data[raw:raw+rsize]
    return base, entry, bytes(image)


def symbols(path):
    out = {}
    for ln in path.read_text().splitlines():
        s = ln.split()
        if len(s) == 4 and s[2] == '0':
            try:
                out[s[-1]] = int(s[0], 16)
            except ValueError:
                pass
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--repo', type=Path, default=ROOT/'Mu-Silicium')
    ap.add_argument('--template', type=Path, default=HERE/'template-v27.img')
    ap.add_argument('--verify', action='store_true')
    args = ap.parse_args()
    repo = args.repo.resolve()
    bld = repo/'Build/rubyPkg/DEBUG_CLANGPDB'
    fd = (bld/'FV/SILICIUM_UEFI.fd').read_bytes()
    assert len(fd) == FD_SIZE
    run('clang', '--target=aarch64-linux-gnu', '-c', HERE/'shim.S', '-o', HERE/'shim.o')
    run('ld.lld', '-Ttext=0x40080000', '-e', '_start', HERE/'shim.o', '-o', HERE/'shim.elf')
    run('llvm-objcopy', '-O', 'binary', HERE/'shim.elf', HERE/'shim.bin')
    rawshim = (HERE/'shim.bin').read_bytes()
    nm = subprocess.check_output(['llvm-nm', '-n', str(HERE/'shim.elf')], text=True)
    syms = {s.split()[-1]: int(s.split()[0],16) for s in nm.splitlines() if len(s.split()) == 3}
    payload_off = syms['_payload'] - BASE
    assert len(rawshim) <= payload_off and payload_off % 128 == 0
    raw = rawshim.ljust(payload_off, b'\0') + fd
    assert BASE+len(raw) < FD_BASE
    assert raw[0x38:0x3c] == b'ARMd'
    (HERE/'kernel.gz').write_bytes(gzip.compress(raw, compresslevel=9, mtime=0))
    h, cmd, _, original_ramdisk, dtb = unpack(args.template.resolve())
    ramdisk = (HERE/'ramfat-packed.bin').read_bytes()
    assert len(ramdisk)==0x600000
    assert h['kernel_addr'] == BASE and h['ramdisk_addr'] == 0x51100000
    (HERE/'ramdisk.bin').write_bytes(ramdisk)
    (HERE/'ruby.dtb').write_bytes(dtb)
    image = HERE/'Mu-ruby-v0.30a-rampack.img'
    run(sys.executable, repo/'Resources/Scripts/mkbootimg.py', '--kernel', HERE/'kernel.gz', '--ramdisk', HERE/'ramdisk.bin', '--dtb', HERE/'ruby.dtb', '--base', '0x40078000', '--kernel_offset', '0x8000', '--ramdisk_offset', '0x11088000', '--second_offset', '0xBFF88000', '--tags_offset', '0x07C08000', '--dtb_offset', '0x07C08000', '--header_version', '2', '--pagesize', '2048', '--cmdline', cmd.decode(), '--output', image)
    oh, ocmd, kernel, ordisk, odtb = unpack(image)
    assert h == oh and cmd == ocmd and ramdisk == ordisk and dtb == odtb
    delivered = gzip.decompress(kernel)
    assert delivered == raw
    report = dict(image=image.name, sha256=hashlib.sha256(image.read_bytes()).hexdigest(), bytes=image.stat().st_size, fd_sha256=hashlib.sha256(fd).hexdigest(), fd_bytes=len(fd), framebuffer=hex(FB), stride_pixels=1080, stride_bytes=4320, dtb_and_boot_addresses_preserved=True, ramdisk_is_compressed_transport=True, decoded_fat_base='0x66000000', on_device_tested=False, actual_mmu_transition_emulated=False)
    if args.verify:
        import numpy as np
        from PIL import Image
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_WRITE
        from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2
        emu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        emu.mem_map(0x40000000, 0x800000)
        emu.mem_write(BASE, delivered)
        emu.mem_map(RES_BASE, RES_SIZE)
        emu.mem_write(RES_BASE, b'\x5a' * RES_SIZE)
        emu.mem_map(0x47c80000, 0x80000)
        emu.mem_write(0x47c80000, (HERE/'captured-lk-fdt.bin').read_bytes())
        emu.mem_map(WDT, 0x1000)
        seed = 0x17f
        emu.mem_write(WDT, struct.pack('<I',seed))
        wdt_writes = []
        emu.hook_add(UC_HOOK_MEM_WRITE, lambda uc,a,addr,sz,v,ud: wdt_writes.append([hex(addr),sz,hex(v)]), begin=WDT, end=WDT+0xfff)
        emu.emu_start(BASE, FD_BASE, timeout=180_000_000, count=15_000_000)
        assert emu.reg_read(UC_ARM64_REG_PC) == FD_BASE
        assert bytes(emu.mem_read(FD_BASE, FD_SIZE)) == fd
        assert wdt_writes == [[hex(WDT),4,hex((seed&~1)|0x22000000)]]
        pixels = np.frombuffer(bytes(emu.mem_read(FB,VISIBLE_SIZE)),dtype='<u4').reshape(2400,1080)
        expected = np.zeros((2400,1080),dtype='<u4')
        expected[32:80,32:184] = 0xff0000ff
        assert np.array_equal(pixels,expected)
        assert bytes(emu.mem_read(RES_BASE,FB-RES_BASE)) == b'\x5a'*(FB-RES_BASE)
        assert bytes(emu.mem_read(FB+VISIBLE_SIZE, PAGE_SIZE-VISIBLE_SIZE-8)) == b'\x5a'*(PAGE_SIZE-VISIBLE_SIZE-8)
        assert bytes(emu.mem_read(FB+PAGE_SIZE-8,8)) == b'\0'*8
        assert bytes(emu.mem_read(FB+PAGE_SIZE,RES_BASE+RES_SIZE-FB-PAGE_SIZE)) == b'\x5a'*(RES_BASE+RES_SIZE-FB-PAGE_SIZE)
        # SEC is a TE image fixed up in place by GenFds, inside the delivered FD.
        secpath = bld/'AARCH64/SiliciumPkg/Sec/Sec/DEBUG'
        tepath = bld/'FV/Ffs/9AFFB503-E643-4141-8B90-17E8588B1D35Sec/9AFFB503-E643-4141-8B90-17E8588B1D35Te.raw'
        te = tepath.read_bytes()
        sec_offset = fd.find(te[:16])
        assert sec_offset >= 0 and fd.find(te[:16],sec_offset+1) < 0
        signature, machine, nsec, subsystem, stripped, entry, code, base = struct.unpack_from('<HHBBHIIQ',fd,sec_offset)
        assert signature==0x5a56 and machine==0xaa64
        assert FD_BASE+sec_offset-(stripped-40)==base
        ss = symbols(secpath/'Sec.map')
        assert entry==ss['_ModuleEntryPoint']
        report.update(sec_image_base=hex(base), sec_offset_in_fd=hex(sec_offset), sec_format='TE, final FD bytes')
        emu.reg_write(UC_ARM64_REG_SP,0x4063f000)
        stop=0x40001000
        def call(address, x0=0, x1=0, x2=0, limit=5_000_000):
            emu.reg_write(UC_ARM64_REG_LR,stop)
            for reg,val in [(UC_ARM64_REG_X0,x0),(UC_ARM64_REG_X1,x1),(UC_ARM64_REG_X2,x2)]:
                emu.reg_write(reg,val)
            emu.emu_start(address,stop,timeout=90_000_000,count=limit)
            assert emu.reg_read(UC_ARM64_REG_PC)==stop, hex(emu.reg_read(UC_ARM64_REG_PC))
        # Run the actual SEC entry until its C entry, not a replacement routine.
        emu.emu_start(base+entry, base+ss['SecEntry'],timeout=90_000_000,count=500_000)
        assert emu.reg_read(UC_ARM64_REG_PC)==base+ss['SecEntry']
        assert np.all(np.frombuffer(bytes(emu.mem_read(FB+(32*1080+200)*4,152*4)),dtype='<u4')==0xffff0000)
        # Compiled checkpoint writer: verify all five UEFI slots pixel-by-pixel.
        mark = ss['RubyBootMark']
        for slot,color in enumerate([0xff0000ff,0xffff0000,0xffffff00,0xff00ff00,0xffff00ff,0xff00ffff]):
            if slot >= 2:
                call(base+mark,slot,color)
            expected[32:80,32+slot*168:184+slot*168]=color
        current=np.frombuffer(bytes(emu.mem_read(FB,VISIBLE_SIZE)),dtype='<u4').reshape(2400,1080)
        assert np.array_equal(current,expected)
        # Use the actual library inside SEC to display text. It must not touch markers.
        text = b'ruby v0.26 - 1080x2400\nFRAMEBUFFER TEXT TEST\n'
        emu.mem_write(0x40002000,text)
        before_top = bytes(emu.mem_read(FB,96*1080*4))
        call(base+ss['SerialPortWrite'],0x40002000,len(text),limit=8_000_000)
        assert bytes(emu.mem_read(FB,96*1080*4))==before_top
        body=np.frombuffer(bytes(emu.mem_read(FB+96*1080*4,(2400-96)*1080*4)),dtype='<u4')
        assert np.any(body==0xffffffff)
        # Force console wrap and verify top checkpoints remain untouched.
        emu.mem_write(FB+PAGE_SIZE-8,struct.pack('<II',0,95))
        emu.mem_write(0x40002000,b'\n')
        call(base+ss['SerialPortWrite'],0x40002000,1,limit=12_000_000)
        assert bytes(emu.mem_read(FB,96*1080*4))==before_top
        assert bytes(emu.mem_read(FB+PAGE_SIZE-8,8))==b'\0'*8
        call(base+ss['SerialPortWrite'],0x40002000+0,0)
        emu.mem_write(0x40002000,text)
        call(base+ss['SerialPortWrite'],0x40002000,len(text),limit=8_000_000)
        pix=np.frombuffer(bytes(emu.mem_read(FB,VISIBLE_SIZE)),dtype='<u4').reshape(2400,1080)
        rgb=np.stack([(pix>>16)&255,(pix>>8)&255,pix&255],axis=2).astype('uint8')
        Image.fromarray(rgb).resize((360,800)).save(HERE/'trace-preview.png')
        report.update(shim_emulated_to_uefi_jump=True, fd_copy_exact=True, wdt_write=wdt_writes, adjacent_pages_unchanged=True, actual_sec_assembly_entry_reaches_c=True, all_six_checkpoint_slots_checked=True, compiled_serial_library_prints_text=True, markers_survive_console_wrap=True)
    (HERE/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)

if __name__ == '__main__':
    main()
