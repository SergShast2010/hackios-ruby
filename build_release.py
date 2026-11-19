#!/usr/bin/env python3
"""Build the v0.41 QEMU-only diagnostic; never change inputs or flash a phone."""
import argparse
import json
from pathlib import Path
from tempfile import TemporaryDirectory

import build_app
import build_native
from fat_reader import fat_path
import package_image
import qemu_helpers as q
from v41_artifacts import IMAGE, KERNEL_BASE, OUT, PACK_BASE, sha


def build(out, kernel_base=KERNEL_BASE, pack_base=PACK_BASE):
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix='.v41-', dir=out) as work:
        temp = Path(work)
        manifest = build_native.build(kernel_base, temp)
        app = build_app.build(temp)
        image, report, _ = package_image.build(pack_base.read_bytes(), app)
        image_path = temp / IMAGE
        image_path.write_bytes(image)
        packed_app, fat = q.final_app(image_path)
        payload = fat_path(fat, [b'PAYLOAD XNP'])
        if (packed_app != app or fat_path(fat, [b'EFI        ', b'BOOT       ', b'BOOTAA64EFI']) != app
                or sha(payload) != manifest['payload_sha256']):
            raise ValueError('final image does not carry the expected loader/payload')
        report.update(version='0.41', stage='prepare-only', qemu_only=True, image=IMAGE,
                      kernel_sha256=manifest['kernel_sha256'], kernel_base_sha256=manifest['input_image_sha256'],
                      runtime_manifest_sha256=sha((temp / 'runtime_manifest.json').read_bytes()),
                      loader_map_sha256=sha((temp / 'loader.map').read_bytes()))
        (temp / 'build.json').write_text(json.dumps(report, indent=2) + '\n', encoding='ascii')
        # Publish metadata last. The observer rejects mismatched artifact hashes.
        for path in sorted(temp.iterdir(), key=lambda path: path.name == 'build.json'):
            path.replace(out / path.name)
    print('BUILT', out / IMAGE, 'SHA256', report['image_sha256'])
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=OUT)
    parser.add_argument('--kernel-base', type=Path, default=KERNEL_BASE)
    parser.add_argument('--pack-base', type=Path, default=PACK_BASE)
    args = parser.parse_args()
    build(args.out, args.kernel_base, args.pack_base)
