# ruby v0.40 — real native XNU cooperative context switch

## Result

**Implemented and QEMU-verified on the exact final production image:** original native boot kernel thread → a genuinely created native XNU diagnostic kernel thread → the original boot thread, resumed on its original native stack.

This is a bounded **cooperative** round-trip. It is **not preemption**, not an EFI mini-scheduler, not complete scheduler-service startup, and not a phone-tested result. IRQ stays masked and ICC Group1 stays closed. The diagnostic stops at its terminal screen and requires manual reboot.

Final image: `Mu-ruby-v0.40-native-switch.img`

- Image SHA-256: `03222cebb1e928c37d87d3a20f77a7f3cc6ff99fc6271e9ee96ad15c217050e7`
- Final PE application SHA-256: `86d9dc82107ee97d45137176a8fb9aba832be5e00ecfc0c86834dc8fb6f67ff9`
- Unchanged payload container SHA-256: `cdbfddb34ed5f3a90b606c4b178f67e40c80bd4ea37831e6609a9eabd36e9b0c`
- Original embedded Mach-O SHA-256: `9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c`

The supplied v39b proven phone milestone is the lineage reference: image `7cfe57f42fbe05ef7d7288b32f06eef6d450ca1812a4b966c6a7f5bebb356f07`, first native kernel thread entered, restored=1/error=0. v40 retains its PMR=0xf0 / RPR=0xff support.

## Production path

The existing native first-thread initialization remains: `sched_init`, `task_init`, `thread_init`, `load_context`, `machine_load_context`, then the bounded replacement of `kernel_bootstrap_thread`.

The replacement now performs:

1. Record the actual boot TPIDR_EL1 and native SP.
2. Call the **original XNU `current_processor` and `idle_thread_create`**. A native idle-thread prerequisite is genuinely allocated; it is not run by the diagnostic.
3. Call **native `sched_startup`**, bounded at two explicitly described sites: its first continuation is the diagnostic second-thread entry; after the original boot thread returns from its first native `thread_block_reason`, startup branches to its own native epilogue before creating the powered-core service thread.
4. Startup calls **native `kernel_thread_start_priority` → `kernel_thread_create`**. Native XNU allocates/initializes the thread, kernel stack, continuation, state, locking, and runqueue membership.
5. Native **`thread_block_reason` → `thread_invoke` → `machine_switch_context` → `Switch_context`** switches boot → diagnostic thread. No code in the diagnostic writes thread structures, runqueues, saved contexts, SP, TPIDR, or return-PC fields to manufacture this switch.
6. The diagnostic thread records its real TPIDR/SP, calls native **`assert_wait`** on its own bounded state event with `THREAD_UNINT`, requires the native return value `THREAD_WAITING` (`0xffffffff` in W0), then calls native **`thread_block_reason(NULL,NULL,0)`**.
7. XNU's same original switch pipeline switches diagnostic → original boot. The diagnostic thread remains waiting; it is not terminated or cleaned up. The original boot thread reaches **`NtSwitchResumed`**, verifies its original TPIDR/SP, the distinct second TPIDR/SP, and phase=4, then marks phase=5/pass=1 and invokes the existing guarded quiet return.
8. The PE terminal screen requires reason=0x40, switch_pass=1, phase=5, boot_before=boot_after, a distinct second thread, restored=1/error=0, and the existing return-state checks.

There are two executing kernel threads, plus the idle-thread prerequisite object. All are native XNU objects/stacks, not private EFI task records. No timer-driven scheduling is claimed.

## Deliberate bounds / patches

- `_kernel_bootstrap_thread` retains the existing v39 bounded-diagnostic entry replacement.
- `_thread_continue+0x58` (`0xfffffff007305b00`): `enable_interrupts=false` is passed to the original continuation-call path. This applies to the initial boot continuation and new diagnostic continuation and prevents the native default IRQ-unmask operation.
- `_sched_startup` at `0xfffffff007309b10`: its original first continuation-address pair is redirected to `NtSwitchSecond`.
- `_sched_startup` at `0xfffffff007309b64`: branch to native epilogue `0xfffffff007309b98`; no powered-core background service is created.
- The native bodies of `thread_invoke`, `machine_switch_context`, `Switch_context`, `kernel_thread_create`, `kernel_thread_start_priority`, `idle_thread_create`, `thread_block_reason`, and `assert_wait` are unchanged. Full symbol-bounded range non-overlap with runtime patches is verified by the audit.

These bounds intentionally do **not** run the ordinary scheduler-maintenance continuation or full `kernel_bootstrap_thread`/IOKit sequence. This is real native cooperative switching, not full boot or complete scheduler startup.

## Executable/state placement

Everything fits in the existing approved legacy cave; no new code region or arbitrary RAM is used:

- Main bridge/backend: 1572 bytes at `0xfffffff007472368`, ending before separately fenced `_loader_jump`.
- `_loader_jump` entry at `0xfffffff0074729ec` remains independently fenced.
- Switch diagnostic: 328 bytes at `0xfffffff0074729f0`, within its 512-byte tail reservation and below `0xfffffff007472bf0`.
- Original 512-byte tail SHA-256: `6a817a0498bcb501e24c578cfd41cec168a6a2c4f7a396657ffdfeec203d26d3`.
- `NativeState`: 472 bytes in the existing bounded 2048-byte reservation beginning at physical `0x63e7d800` / kernel VA `0xfffffff007e7d800`.

All 52 staged-RAM patch records have exact original bytes, and the patcher's existing preflight validates every old byte before any patch write. The original payload/Mach-O file is not patched. Independent legacy/IOKit/watchdog/SMC/HVC/guard fences remain. Fourteen inherited guard/transition source files and the entire guest-side fixture are byte-identical to v39b; hashes are in `verification/guard-lineage.json`.

No new MMIO access, SMC/HVC, IRQ unmask, IOKit, PSCI, UFS, or PMIC operation was added. Native scheduler timer-system-register activity is not suppressed or misrepresented: it is audited with IRQ masked and Group1 closed, and the inherited timer/ICC state is restored through the existing exit.

## Exact primary proof: inherited PMR=0xf0 / RPR=0xff

File: `tests/44000000-inherited-3/result.json`.

| Observation | Actual value |
|---|---|
| Boot TPIDR_EL1 before | `0xfffffff5035e8ee0` |
| Boot SP before | `0xfffffff7d4c3bf30` |
| Diagnostic TPIDR_EL1 | `0xfffffff5035e6dc0` |
| Diagnostic SP | `0xfffffff7d4c4bf30` |
| Boot TPIDR_EL1 resumed | `0xfffffff5035e8ee0` |
| Boot SP resumed | `0xfffffff7d4c3bf30` |
| Shared native task pointer read from both thread objects | `0xfffffff03699d200` |
| Native saved LR read from both saved contexts | `0xfffffff007468450` (after the native call to `Switch_context`) |
| Boot native saved SP while second runs | `0xfffffff7d4c3bdb0` |
| Second native saved SP after boot resumes | `0xfffffff7d4c4be10` |
| Second/boot-resumed DAIF | `0x3c0` / `0x3c0` |
| Final reason / phase / pass | `0x40` / `5` / `1` |
| Hardware restored / restore error | `1` / `0` |
| Inherited PMR / RPR | `0xf0` / `0xff` |

Each of `thread_invoke`, `machine_switch_context`, and `Switch_context` is observed **twice**, with native argument pairs boot→second and second→boot. TPIDR matches the outgoing thread at each routine entry. `NtSwitchSecond` is observed with the new native SP/TPIDR; `NtSwitchResumed` is observed after the reverse switch with the original SP/TPIDR.

The screenshots are actual QEMU framebuffer captures of this final image, not mockups. `tests/44000000-inherited-3/screen.png` and `screen-preview.png` were viewed and show the successful round-trip, distinct thread/stack, restored=1/error=0, and IRQ-masked/nonperiodic disclaimer.

## Tests and audit

All **9** final-image scenarios passed, run sequentially with one QEMU at a time:

| Scenario | Result |
|---|---|
| PMR=0, RPR=0xff (mode 0) | Native round-trip PASS |
| PMR=0, inherited AP0 active/RPR=0 (mode 1) | Native round-trip PASS |
| Inherited AP1 active (mode 2) | Snapshot rejected before kernel entry, PASS |
| **PMR=0xf0, RPR=0xff (mode 3, observed phone-compatible state)** | **Native round-trip PASS** |
| Group1 open (mode 4) | Snapshot rejected before kernel entry, PASS |
| PMR=0x80, RPR=0xff (mode 5) | Native round-trip PASS |
| PMR=0xf0, inherited AP0 active/RPR=0 (mode 6) | Native round-trip PASS |
| PE relocated to 0x46000000, PMR=0xf0/RPR=0xff | Native round-trip PASS |
| QEMU mapping without the fixture profile | Phone guard rejects PrepareXnu; kernel not entered, PASS |

`audit_native_switch.py` passed over all nine exact-image results. It verifies the original kernel/payload, exact old patch bytes, native routine ranges, image/observer hashes, all debugger-write allowlists, and byte-identical rebuilt observer.

The passive observer records all native system instructions and native external-memory callbacks, plus selected diagnostic/return instructions. It is a **selected instruction audit, not a full instruction trace**; repetitive low-return cache-clean operations are counted by the inherited observer rather than logged individually. In the primary positive run it recorded 11 native GIC MMIO reads, zero native GIC writes, zero unapproved MMIO/out-of-owned-RAM accesses, zero executed SMC/HVC, zero IRQ-unmask operations or open-Group1 writes, and no independently fenced path entry. Relevant source registers are recorded for DAIF/SPSR/ICC checks. The guest fixture branch is unchanged from v39b.

Host debugger writes are limited to hardware breakpoint add/remove commands and exactly one data write, the pre-existing fixture release flag: `M40180050,8:0100000000000000`. No PC/register/context/thread-field injection is used. The debugger reads real saved native contexts; it does not create them. Breakpoint step-over executes genuine next instructions without rewriting guest code/registers.

`python rebuild.py` also completed a clean-source **byte-identical** image rebuild. Proof: `verification/rebuild-proof.json`. Full syntax check `python -m py_compile *.py` passed. The freshly rebuilt observer matches the tested observer exactly.

## Reproduce

With the inherited build/test Python requirements and native host tools installed:

```sh
python build_release.py
python test_native_switch.py --inherited 3
for n in 0 1 2 4 5 6; do python test_native_switch.py --inherited "$n"; done
python test_native_switch.py --base 0x46000000 --inherited 3
python test_native_switch.py --negative-phone-guard
python audit_native_switch.py
python rebuild.py
```

Use `test_native_switch.py`, not the copied v39 test scripts. Obsolete copied v39-specific test/packaging scripts were removed to avoid accidental use. Build needs the new `native.ld` and `ebsprobe/native_switch.S`. The build has no dependency on agent analysis directories. QEMU test logs are compressed immediately after each bounded run; no infinite/full QEMU trace is used.

## Remaining limits

No phone was accessed or flashed. v40 on real ruby is **unverified** until the user runs this exact image and returns its screen/photo. No IRQ-delivered tick, preemption, ordinary scheduler-maintenance service, IOKit, PSCI, storage, or PMIC bring-up is claimed. The native idle and diagnostic thread allocations are intentionally not torn down before the terminal diagnostic stop. Existing phone snapshot, RAM ownership/mapping, return, and manifest guards remain in force.
