# Handoff / packaging notes

> Historical packaging notes. Use [START_HERE_RU.md](START_HERE_RU.md) for the active v0.41 QEMU-only build.

Implementation and all final tests are complete; no QEMU job is left running. Do not change/rebuild production bytes unnecessarily: tested image SHA-256 is `03222cebb1e928c37d87d3a20f77a7f3cc6ff99fc6271e9ee96ad15c217050e7`.

## User-facing deliverables ready (not sent by this worker)

- `Mu-ruby-v0.40-native-switch.img`
- `release-proof-v0.40.json`
- `REPORT.md`, `README.md`
- Production source: required root build/parser/Python/linker files + requirements, `ebsprobe` source including NEW `native_switch.S`, NEW `native.ld`, both inherited base image inputs, and `qemu-test` source.
- Verification: `verification/*.json`, `runtime_manifest.json`, `build.json`; per-test result/audit summary/actual screenshots/compressed observer audit. The primary phone-compatible fixture is `tests/44000000-inherited-3/`.

## Exclude from clean source archives

- `rebuild-*` clean-build verification directory (already proved byte-identical)
- Generated `qemu-decoded.img` / `qemu-packed.uefi` in the root/base directories (unpack intermediates, not inputs)
- `__pycache__`, `.o`, `.obj`; large test logs/caches can go in a separate evidence subset.
- `disasm_selected.py` / `selected.disasm.txt` are analysis-only; the helper references the read-only agent analysis path and is not a build dependency.

The build/runtime do NOT depend on agent analysis directories. Tests build the guest fixture and passive observer from the included `qemu-test` source. Guest fixture code remains byte-identical to v39b; only the passive observer's register list was extended. The source archive should retain `native.ld`, which is now a real two-section linker script rather than generated text.

## Main findings

9/9 final-image cases passed: six native round-trips (including observed phone-compatible PMR=0xf0/RPR=0xff and relocated PE), two pre-entry snapshot rejects, one PrepareXnu phone-guard reject. Actual boot→second→boot is through native unmodified thread_invoke/machine_switch_context/Switch_context, once each direction, with real native thread allocation and original saved contexts. All full-body ranges are checked for no runtime patch overlap. No fake scheduler or register/context injection. Restored=1/error=0, phase=5/pass=1, reason=0x40. Original embedded kernel unchanged. All 14 inherited guard/transition source files and guest fixture identical to v39b. Native creation of the idle prerequisite is preserved, but that idle thread is not executed.

The primary QEMU screenshot has been visually reviewed. Final native phone execution is still unverified, and no phone was touched. Do not claim preemption, complete scheduler-service startup, IOKit or hardware bring-up.
