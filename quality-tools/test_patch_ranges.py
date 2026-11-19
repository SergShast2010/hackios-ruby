#!/usr/bin/env python3
"""Pure host checks, no build tools or emulator needed."""
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from v41_artifacts import LINK, STAGE, check_patches, covered_by, firmware_instruction


def row(va, n, name='patch'):
    return dict(va=hex(va), pa=hex(STAGE + va - LINK), old='00'*n, new='aa'*n, name=name)


class PatchRanges(unittest.TestCase):
    def test_historical_overlaps(self):
        historical = json.loads((ROOT / 'archive/v0.41-overlap-failure/runtime_manifest.json').read_text())['patches']
        for address in (0xfffffff007472dcc, 0xfffffff007472f64):
            collision = next(r for r in historical if int(r['va'], 16) == address)
            with self.assertRaisesRegex(ValueError, f'overlapping patches at {address:#x}'):
                check_patches([historical[2], collision])

    def test_padding_is_a_write(self):
        ranges = [(0xfffffff007472c14, 0xfffffff007472f98, '.extra')]
        self.assertTrue(covered_by(ranges, 0xfffffff007472dcc))
        self.assertTrue(covered_by(ranges, 0xfffffff007472f64))
        self.assertFalse(covered_by(ranges, 0xfffffff007472f98))

    def test_adjacent_but_not_duplicate(self):
        a, b = row(LINK + 0x10, 4), row(LINK + 0x14, 8)
        check_patches([b, a])
        with self.assertRaisesRegex(ValueError, 'overlapping patches'):
            check_patches([a, a])

    def test_inconsistent_mapping_and_size(self):
        for changes in (dict(pa=hex(STAGE+8)), dict(old='00')):
            with self.assertRaisesRegex(ValueError, 'inconsistent patch'):
                check_patches([row(LINK, 4) | changes])
        with self.assertRaisesRegex(ValueError, 'outside staged RAM'):
            check_patches([row(LINK - 4, 4)])

    def test_forbidden_opcodes(self):
        self.assertTrue(firmware_instruction(0xd4000022))  # HVC #1
        self.assertTrue(firmware_instruction(0xd40000a3))  # SMC #5
        self.assertFalse(firmware_instruction(0xd503201f))  # NOP


if __name__ == '__main__':
    unittest.main()
