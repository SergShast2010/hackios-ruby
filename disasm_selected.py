#!/usr/bin/env python3
"""Disassemble symbols in the pinned original XNU Mach-O, not in a RAM patch."""
import argparse
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

from fat_reader import fat_path
import qemu_helpers as q
from v41_artifacts import KERNEL_BASE, KERNEL_SHA, sha

DEFAULT = ['_thread_run', '_thread_continue', '_sched_init_thread', '_sched_startup', '_thread_start',
           '_call_continuation', '_machine_switch_context', '_thread_dispatch', '_sched_timeshare_maintenance_continue',
           '_idle_thread_create', '_thread_wakeup_prim', '_waitq_wakeup64_all', '_assert_wait', '_thread_block_reason']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=KERNEL_BASE)
    parser.add_argument('symbols', nargs='*', default=DEFAULT)
    args = parser.parse_args()
    _, fat = q.final_app(args.image)
    kernel, segments, symbols, _ = q.kernel_metadata(fat_path(fat, [b'PAYLOAD XNP']))
    if sha(kernel) != KERNEL_SHA:
        parser.error('expected the pinned kernel from v0.41 build inputs')
    inverse = {value: name for name, value in symbols.items()}
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    for name in args.symbols:
        if name not in symbols:
            parser.error(f'unknown symbol: {name}')
        address = symbols[name]
        seg = next((s for s in segments if s['name'] == '__TEXT_EXEC' and
                    s['va'] <= address < s['va'] + s['files']), None)
        if seg is None:
            parser.error(f'not in executable code: {name}')
        limit = seg['va'] + seg['files']
        end = min((va for va in inverse if address < va < limit), default=limit)
        offset = seg['off'] + address - seg['va']
        print(f'{name}:')
        for ins in decoder.disasm(kernel[offset:offset+end-address], address):
            target = inverse.get(int(ins.op_str[1:], 16)) if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('#0x') else None
            print(f'{ins.address:016x}: {ins.mnemonic:9} {ins.op_str}' + (f' ; {target}' if target else ''))
        print()


if __name__ == '__main__':
    main()
