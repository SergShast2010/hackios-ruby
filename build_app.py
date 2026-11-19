#!/usr/bin/env python3
import argparse
from pathlib import Path
import subprocess

from v41_artifacts import ROOT, OUT


def build(out):
    if (ROOT / 'ebsprobe' / 'native_patches.h').exists():
        raise ValueError('stale ebsprobe/native_patches.h would shadow the generated header')
    if not (out / 'native_patches.h').is_file():
        raise ValueError('run build_native.py with the same --out first')
    flags = ['clang', '--target=aarch64-unknown-windows', '-ffreestanding', '-fno-builtin',
             '-fno-stack-protector', '-fshort-wchar', '-mgeneral-regs-only', '-mstrict-align',
             '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-I', str(out)]
    objects = []
    for name in ('loader.c', 'transition.S', 'irq_vectors.S'):
        obj = out / (name + '.obj')
        subprocess.run([*flags, '-c', ROOT / 'ebsprobe' / name, '-o', obj], check=True)
        objects.append(obj)
    app = out / 'loader.efi'
    subprocess.run(['lld-link', '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib',
                    '/machine:arm64', '/base:0xa1000000', '/dynamicbase', '/timestamp:0x6af4c4de',
                    '/filealign:512', f'/map:{out / "loader.map"}', f'/out:{app}', *objects], check=True)
    return app.read_bytes()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Build v0.41 EFI loader with the generated patch header')
    parser.add_argument('--out', type=Path, default=OUT)
    build(parser.parse_args().out)
