# v0.41 overlap failure (evidence only)

Artifacts of the failing v0.41 run: guard patches overwrote `.extra`
(`0xfffffff007472dcc` = `NtPreemptArm+8`, `0xfffffff007472f64`), jumping to `NtPanic`.
`ESR=0xf2000526` is the intentional `brk #0x526` from `transition.S`, not a new fault.

Nothing here is a build input. `ebsprobe/native_patches.h` here is the broken generated
header; it must never return to `ebsprobe/` (`build_app.py` refuses to build if it does).
`quality-tools/test_patch_ranges.py` uses `runtime_manifest.json` as a regression fixture.
The image `inputs/current-test-base.img` is a blob (`tools/blobs.sh restore`).
