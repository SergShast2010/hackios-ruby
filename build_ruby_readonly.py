#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import build_app
import build_native
from fat_reader import fat_path
import package_image
import qemu_helpers as q
from v41_artifacts import KERNEL_BASE, OUT, PACK_BASE, sha

READONLY_IMAGE = 'Mu-ruby-v0.41-ruby-fdt-readonly.img'

def build(out, kernel_base=KERNEL_BASE, pack_base=PACK_BASE):
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix='.ruby-ro-', dir=out) as work:
        temp = Path(work)
        manifest = build_native.build(kernel_base, temp)
        app = build_app.build(temp, ('-DHACKIOS_RUBY_READONLY=1','-Wno-unused-function','-Wno-unused-variable'))
        image, report, _ = package_image.build(pack_base.read_bytes(), app)
        image_path = temp / READONLY_IMAGE
        image_path.write_bytes(image)
        packed_app, fat = q.final_app(image_path)
        payload = fat_path(fat, [b'PAYLOAD XNP'])
        if packed_app != app or sha(payload) != manifest['payload_sha256']:
            raise ValueError('read-only image payload mismatch')
        report.update(version='0.41-ruby-readonly', stage='live-fdt-read-only',
                      qemu_only=False, phone_read_only=True, no_ebs=True,
                      no_gic_mmio_writes=True, image=READONLY_IMAGE,
                      kernel_sha256=manifest['kernel_sha256'],
                      kernel_base_sha256=manifest['input_image_sha256'],
                      runtime_manifest_sha256=sha((temp / 'runtime_manifest.json').read_bytes()),
                      loader_map_sha256=sha((temp / 'loader.map').read_bytes()))
        (temp / 'build.json').write_text(json.dumps(report, indent=2) + '\n', encoding='ascii')
        for path in sorted(temp.iterdir(), key=lambda p: p.name == 'build.json'):
            path.replace(out / path.name)
    print('BUILT', out / READONLY_IMAGE, 'SHA256', report['image_sha256'])

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=OUT / 'ruby-readonly')
    parser.add_argument('--kernel-base', type=Path, default=KERNEL_BASE)
    parser.add_argument('--pack-base', type=Path, default=PACK_BASE)
    args = parser.parse_args()
    build(args.out, args.kernel_base, args.pack_base)
