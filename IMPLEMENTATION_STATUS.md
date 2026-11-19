# v40 native switch — implementation status

## COMPLETE — final production bytes tested

Final image: `Mu-ruby-v0.40-native-switch.img`

SHA-256: `03222cebb1e928c37d87d3a20f77a7f3cc6ff99fc6271e9ee96ad15c217050e7`.

Real native XNU boot→second→boot cooperative round-trip implemented and verified. Two actual calls each to unmodified native thread_invoke/machine_switch_context/Switch_context; native creation/runqueues/stacks; distinct TPIDR/SP; original boot resumed at original SP. Final reason=0x40, phase=5, switch_pass=1, restored=1, restore_error=0. Inherited phone-compatible PMR=0xf0 / RPR=0xff passed.

All 9 exact-final-image tests PASS (6 native round-trips including relocation, 2 snapshot rejection cases, 1 phone-guard rejection). Passive observer/data-write allowlist audit PASS. Original embedded Mach-O remains byte-identical. Observer source rebuild byte-identical. Clean production-image rebuild byte-identical. All root Python files pass syntax compilation. Primary screenshot was viewed and matches the success fields.

Only the existing 2184-byte approved cave is used: main 1572 bytes + second block 328 bytes within the independently fenced 512-byte legacy tail. NativeState 472 bytes, bounded by existing 2048-byte reservation. Fourteen inherited guard/transition sources plus guest fixture remain byte-identical to v39b.

Definitive details: `REPORT.md`, `verification/final-proof.json`, `verification/rebuild-proof.json`, `verification/guard-lineage.json`, and `tests/*/result.json` / `audit-summary.json` / compressed logs.

No phone touched or flashed. Phone result remains UNVERIFIED. No preemption/full scheduler startup, IOKit, PSCI, storage/PMIC, new MMIO, SMC/HVC, IRQ unmask, or open Group1 claimed. Native service threads deliberately bounded off; diagnostic thread remains waiting before terminal stop. No QEMU job is left running.

## Historical work notes (superseded by COMPLETE above)

FINAL PRODUCTION IMAGE FIRST PASS: SHA-256 `03222cebb1e928c37d87d3a20f77a7f3cc6ff99fc6271e9ee96ad15c217050e7`. `test_native_switch.py --inherited 3` completed PASS on final bytes, including actual saved-context LR/SP and common kernel_task evidence, exact forward/reverse XNU routine arguments, unchanged original kernel SHA, masked IRQs, and fixture-release-only data writes. Full 7-state matrix, relocation and phone-guard negative tests now running sequentially, one QEMU at a time. Native switch code is 328 bytes; total remains in original 2184-byte approved cave.

Observed first genuine native boot→second→boot round-trip on the production v40 path in QEMU, inherited PMR=0xf0 / RPR=0xff. Observer saw two calls each to thread_invoke, machine_switch_context, Switch_context, distinct TPIDR/SP, and NtSwitchResumed on the original boot thread. Restored=1, restore_error=0, reason=0x40 reached APP_TERMINAL. The test's last assertion incorrectly counted breakpoint commands as data writes; fixing this observer assertion and tightening proof before claiming final test PASS. A small production assert_wait return-value guard is being added, requiring final-byte retest.

First observed (superseded) image a25daf484a065332f413a4a8148873baf5c2cf1cf6a6d9d0192e6857a680fba7. Boot TPIDR 0xfffffff3d54da4e8 / SP 0xfffffffbfef4ff30; second 0xfffffff3d54dad30 / SP 0xfffffffbfef5ff30; original boot resumed with its original TPIDR/SP. No phone test performed.

The diagnostic fits entirely inside the existing approved legacy cave: main bridge 1572 bytes at 0xfffffff007472368; switch code currently 320 bytes at 0xfffffff0074729f0, after the separately fenced loader_jump entry and below 0xfffffff007472bf0. No extra executable region required. NativeState is 472 bytes of the bounded 2048-byte reservation.

Planned production path: preserve native sched_init/task_init/thread_init/load_context; diagnostic boot thread calls native idle_thread_create then sched_startup; startup calls native kernel_thread_start_priority with bounded second-thread continuation, native thread_block_reason/thread_invoke/machine_switch_context/Switch_context perform both directions. The second thread assert_wait + thread_block returns execution to the original boot thread. Native sched_startup's powered-core service creation is fenced off after that return. thread_continue passes enable_interrupts=false to preserve masked IRQs. No direct thread/queue/context-field writes.

Base: proven v39b image SHA-256 `7cfe57f42fbe05ef7d7288b32f06eef6d450ca1812a4b966c6a7f5bebb356f07`; original kernel SHA-256 `9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c`.

Target: bounded cooperative switch between native XNU kernel threads using genuine XNU scheduler/context-switch routines, IRQ masked and Group1 closed. No preemption claim, no phone actions. All changes isolated to this folder. Kernel file remains original; executable patches applied only to staged RAM after exact-byte checks.

Copied only root build/test sources, ebsprobe, base, and quality-tools. Native cave capacity 2184 bytes requires review before implementation. No QEMU currently started.
