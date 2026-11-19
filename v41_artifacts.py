"""Paths and staged-RAM invariants shared by the v0.41 build and observer."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'out'
KERNEL_BASE = ROOT / 'inputs/Mu-ruby-v0.38b-gicgate.img'
PACK_BASE = ROOT / 'inputs/Mu-ruby-v0.32b-appsync.img'
IMAGE = 'Mu-ruby-v0.41-QEMU-ONLY.img'
KERNEL_BASE_SHA = 'ccf5e56b8221b6c79d7e277ed2e78d231098b5c60084bf0f5315ed4adfd39637'
KERNEL_SHA = '9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c'
LINK = 0xfffffff007000000
STAGE = 0x63000000
STAGE_BYTES = 0x2000000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def check_ranges(ranges):
    """Reject even identical overlapping writes. Adjacent ranges are fine."""
    previous = None
    for start, end, name in sorted(ranges):
        if start >= end or (start | end) & 3:
            raise ValueError(f'invalid patch range {name}: [{start:#x}, {end:#x})')
        if previous is not None and start < previous[1]:
            raise ValueError(f'overlapping patches at {start:#x}: {previous[2]} / {name}')
        previous = (start, end, name)


def check_patches(rows):
    ranges = {'va': [], 'pa': []}
    for row in rows:
        size = len(bytes.fromhex(row['new']))
        va, pa = (int(row[key], 16) for key in ranges)
        if size != len(bytes.fromhex(row['old'])) or pa != STAGE + va - LINK:
            raise ValueError(f'inconsistent patch row: {row["name"]}')
        if not STAGE <= pa < pa + size <= STAGE + STAGE_BYTES:
            raise ValueError(f'patch outside staged RAM: {row["name"]}')
        for key, start in (('va', va), ('pa', pa)):
            ranges[key].append((start, start + size, row['name']))
    for entries in ranges.values():
        check_ranges(entries)


def covered_by(ranges, address, size=4):
    return any(address < end and start < address + size for start, end, _ in ranges)


def firmware_instruction(word):
    return (word & 0xffe0001f) in (0xd4000002, 0xd4000003)
