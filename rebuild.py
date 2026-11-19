#!/usr/bin/env python3
"""Repeat the v0.41 build in a fresh directory and compare the output bytes."""
import argparse
from pathlib import Path
from tempfile import TemporaryDirectory

from build_release import build
from v41_artifacts import IMAGE, OUT, sha

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reference', type=Path, default=OUT)
args = parser.parse_args()
names = (IMAGE, 'loader.efi', 'runtime_manifest.json', 'native_patches.h')
reference = {name: sha((args.reference / name).read_bytes()) for name in names}
with TemporaryDirectory(prefix='ruby-v41-rebuild-') as directory:
    root = Path(directory)
    build(root)
    for name in names:
        if sha((root / name).read_bytes()) != reference[name]:
            raise SystemExit(f'rebuild differs: {name}')
print('BYTE-IDENTICAL', reference[IMAGE])
