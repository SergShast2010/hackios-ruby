#!/usr/bin/env python3
"""Build Mu-ruby v0.56b: source-built HakiOS XNU (MT6877, 4 GiB RAM, 8-core SMP, RO-UFS, USB CDC-ECM) + PID 1."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

import package_image
from v41_artifacts import PACK_BASE, ROOT

DEFAULT_KERNEL = Path('/home/sergsh/hackios/hakios-xnu-kernel/hakios-kernel')
DEFAULT_RAMDISK = Path('/home/sergsh/hackios/hakios-xnu-kernel/userland/hello')
MKPAYLOAD = Path('/home/sergsh/hackios/hakios-xnu-kernel/xnuloader/mkpayload.py')
MKAPFS = Path('/home/sergsh/hackios/hakios-xnu-kernel/tools/apfs/mkapfs.py')
V50_OUT = ROOT / 'out' / 'v56b'
V50_IMAGE = 'Mu-ruby-v0.56b-hakios-nbd-apfs.img'


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def build_v50_efi(work: Path, apple_splash: bool = False, darwin: bool = False) -> Path:
    flags = [
        'clang', '--target=aarch64-unknown-windows', '-ffreestanding', '-fno-builtin',
        '-fno-stack-protector', '-fshort-wchar', '-mgeneral-regs-only', '-mstrict-align', '-mno-stack-arg-probe',
        '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-I/home/sergsh/hackios/hakios-xnu-kernel/xnuloader',
    ]
    if apple_splash:
        flags.append('-DV50_APPLE_SPLASH=1')
    if darwin:
        flags.append('-DV50_DARWIN=1')
    objs = []
    for name in ('v50_loader.c', 'v50_asm.S'):
        obj = work / (name + '.obj')
        subprocess.run([*flags, '-c', ROOT / 'ebsprobe' / name, '-o', obj], check=True)
        objs.append(obj)
    efi = work / 'v50_loader.efi'
    subprocess.run([
        'lld-link', '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib',
        '/machine:arm64', '/base:0xa1000000', '/dynamicbase', '/timestamp:0x6af4c4e0',
        '/filealign:512', f'/map:{work / "v50_loader.map"}', f'/out:{efi}', *objs,
    ], check=True)
    sz = efi.stat().st_size
    if not (128 <= sz <= 0x40000):
        raise ValueError(f'v50_loader.efi size {sz} outside Mu-ruby BDS 256 KiB bound')
    return efi



def build_mini_apfs_ramdisk(work: Path, hello_bin: Path) -> Path:
    """Wrap freestanding PID 1 binary into a deterministic mini-APFS root container with mount points."""
    stage = work / 'miniroot'
    for d in ('sbin', 'dev', 'mnt', 'mnt2', 'flash', 'redwood', 'private/preboot', 'private/var'):
        (stage / d).mkdir(parents=True, exist_ok=True)
    dst = stage / 'sbin' / 'launchd'
    shutil.copy2(hello_bin, dst)
    os.chmod(dst, 0o755)
    out_apfs = work / 'boot_ramdisk.apfs'
    subprocess.run(
        [
            sys.executable, str(MKAPFS),
            '-o', str(out_apfs),
            '-z', '--plain', 'sbin',
            '--time', '1683900000',
            '--slack', '8',
            '--min-blocks', '128',
            f'HakiOS-Boot={stage}',
        ],
        check=True,
    )
    return out_apfs


def build(out: Path, kernel: Path, ramdisk: Path, cmdline: str, kver: str, pack_base: Path = PACK_BASE, apple_splash: bool = False, darwin: bool = False, image_name: str | None = None):
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix='.v50-', dir=out) as w:
        work = Path(w)
        efi = build_v50_efi(work, apple_splash=apple_splash, darwin=darwin)
        rd_path = ramdisk if ramdisk.suffix == ".apfs" else build_mini_apfs_ramdisk(work, ramdisk)
        payload = work / 'PAYLOAD.XNP'
        subprocess.run([
            'python3', str(MKPAYLOAD), '-o', str(payload),
            f'kernel={kernel}', f'ramdisk={rd_path}',
            '--cmdline', cmdline, '--kernel-version', kver,
        ], check=True)

        efi_bytes = efi.read_bytes()
        payload_bytes = payload.read_bytes()
        image_bytes, report, _chunks = package_image.build(
            pack_base.read_bytes(), efi_bytes, payload=payload_bytes
        )
        if image_name is None:
            if darwin:
                image_name = 'Mu-ruby-v0.62e-darwin-splash.img' if apple_splash else 'Mu-ruby-v0.62e-darwin.img'
            else:
                image_name = 'Mu-ruby-v0.56b-apple-splash.img' if apple_splash else V50_IMAGE
        img = out / image_name
        img.write_bytes(image_bytes)
        (out / 'v50_loader.efi').write_bytes(efi_bytes)
        (out / 'PAYLOAD.XNP').write_bytes(payload_bytes)

        manifest = {
            'version': 'v0.62e' if darwin else 'v0.56b',
            'image': image_name,
            'apple_splash': apple_splash,
            'image_bytes': len(image_bytes),
            'image_sha256': sha256_bytes(image_bytes),
            'efi_bytes': len(efi_bytes),
            'efi_sha256': sha256_bytes(efi_bytes),
            'kernel_path': str(kernel),
            'kernel_bytes': kernel.stat().st_size,
            'kernel_sha256': sha256_bytes(kernel.read_bytes()),
            'ramdisk_path': str(ramdisk),
            'ramdisk_bytes': ramdisk.stat().st_size,
            'ramdisk_sha256': sha256_bytes(ramdisk.read_bytes()),
            'payload_bytes': len(payload_bytes),
            'payload_sha256': sha256_bytes(payload_bytes),
            'cmdline': cmdline,
            'kernel_version': kver,
            'packaging': report,
        }
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(f'BUILT  {img} ({len(image_bytes)} bytes)')
        print(f'EFI    {len(efi_bytes)} bytes ({manifest["efi_sha256"][:16]}...)')
        print(f'XNP    {len(payload_bytes)} bytes ({manifest["payload_sha256"][:16]}...)')
        print(f'PACKED {report["packed_used"]} / {report["packed_bytes"]} bytes (margin {report["packed_bytes"] - report["packed_used"]} B)')
        print(f'SHA256 {manifest["image_sha256"]}')
        return manifest


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Build Mu-ruby v0.56b with source-built XNU + TCP console PID 1')
    ap.add_argument('--out', type=Path, default=V50_OUT)
    ap.add_argument('--kernel', type=Path, default=DEFAULT_KERNEL)
    ap.add_argument('--ramdisk', type=Path, default=DEFAULT_RAMDISK)
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--kernel-version', default='ruby-v0.56b-mt6877')
    ap.add_argument('--pack-base', type=Path, default=PACK_BASE)
    ap.add_argument('--apple-splash', action='store_true', help='Boot with Apple splash logo instead of verbose console')
    ap.add_argument('--darwin', action='store_true', help='Build v0.62e Darwin 16.5 NBD-boot image (flashboot.apfs + root_apfs=nbd0)')
    ap.add_argument('--image-name', default=None, help='Override output boot image filename')
    a = ap.parse_args()
    if a.darwin:
        if a.out == V50_OUT:
            a.out = ROOT / 'out' / 'v60f'
        if a.ramdisk == DEFAULT_RAMDISK:
            a.ramdisk = Path('/home/sergsh/hackios/hakios-xnu-kernel/userland/flashboot.apfs')
        if a.kernel_version == 'ruby-v0.56b-mt6877':
            a.kernel_version = 'ruby-v0.62e-darwin16.5'
    build(a.out, a.kernel, a.ramdisk, a.cmdline, a.kernel_version, a.pack_base, apple_splash=a.apple_splash, darwin=a.darwin, image_name=a.image_name)
