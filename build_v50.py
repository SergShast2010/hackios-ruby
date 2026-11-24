#!/usr/bin/env python3
"""Build Mu-ruby v0.51: source-built HakiOS XNU (MT6877, 8-core SMP + full RAM) + userland/hello."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory

import package_image
from v41_artifacts import PACK_BASE, ROOT

DEFAULT_KERNEL = Path('/home/sergsh/hackios/hakios-xnu-kernel/hakios-kernel')
DEFAULT_RAMDISK = Path('/home/sergsh/hackios/hakios-xnu-kernel/userland/hello')
MKPAYLOAD = Path('/home/sergsh/hackios/hakios-xnu-kernel/xnuloader/mkpayload.py')
V50_OUT = ROOT / 'out' / 'v55b'
V50_IMAGE = 'Mu-ruby-v0.55c-hakios-usb-ecm.img'


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def build_v50_efi(work: Path, apple_splash: bool = False) -> Path:
    flags = [
        'clang', '--target=aarch64-unknown-windows', '-ffreestanding', '-fno-builtin',
        '-fno-stack-protector', '-fshort-wchar', '-mgeneral-regs-only', '-mstrict-align', '-mno-stack-arg-probe',
        '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
    ]
    if apple_splash:
        flags.append('-DV50_APPLE_SPLASH=1')
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


def build(out: Path, kernel: Path, ramdisk: Path, cmdline: str, kver: str, pack_base: Path = PACK_BASE, apple_splash: bool = False):
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix='.v50-', dir=out) as w:
        work = Path(w)
        efi = build_v50_efi(work, apple_splash=apple_splash)
        payload = work / 'PAYLOAD.XNP'
        subprocess.run([
            'python3', str(MKPAYLOAD), '-o', str(payload),
            f'kernel={kernel}', f'ramdisk={ramdisk}',
            '--cmdline', cmdline, '--kernel-version', kver,
        ], check=True)

        efi_bytes = efi.read_bytes()
        payload_bytes = payload.read_bytes()
        image_bytes, report, _chunks = package_image.build(
            pack_base.read_bytes(), efi_bytes, payload=payload_bytes
        )
        image_name = 'Mu-ruby-v0.55c-apple-splash.img' if apple_splash else V50_IMAGE
        img = out / image_name
        img.write_bytes(image_bytes)
        (out / 'v50_loader.efi').write_bytes(efi_bytes)
        (out / 'PAYLOAD.XNP').write_bytes(payload_bytes)

        manifest = {
            'version': 'v0.55c',
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
    ap = argparse.ArgumentParser(description='Build Mu-ruby v0.50 with source-built XNU + hello')
    ap.add_argument('--out', type=Path, default=V50_OUT)
    ap.add_argument('--kernel', type=Path, default=DEFAULT_KERNEL)
    ap.add_argument('--ramdisk', type=Path, default=DEFAULT_RAMDISK)
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--kernel-version', default='ruby-v0.55c-mt6877')
    ap.add_argument('--pack-base', type=Path, default=PACK_BASE)
    ap.add_argument('--apple-splash', action='store_true', help='Boot with Apple splash logo instead of verbose console')
    a = ap.parse_args()
    build(a.out, a.kernel, a.ramdisk, a.cmdline, a.kernel_version, a.pack_base, apple_splash=a.apple_splash)
