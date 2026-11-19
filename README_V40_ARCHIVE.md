# Mu ruby v0.40 — native cooperative XNU switch

> Historical v0.40 documentation. Use [START_HERE_RU.md](START_HERE_RU.md) for v0.41 and [PHONE_EVIDENCE.md](PHONE_EVIDENCE.md) for later device observations.

Final image: **Mu-ruby-v0.40-native-switch.img**

SHA-256: **03222cebb1e928c37d87d3a20f77a7f3cc6ff99fc6271e9ee96ad15c217050e7**

Native boot kernel thread → genuinely created native XNU diagnostic thread → original boot thread resumed. QEMU verified, **not yet phone verified**. Cooperative only, **not preemption**. IRQ masked / Group1 closed. No IOKit, PSCI, storage, PMIC, new MMIO, or firmware-call bring-up. Terminal diagnostic only; manual reboot.

See **REPORT.md** for implementation bounds, exact native addresses, observed state, all tests, and limitations. `release-proof-v0.40.json` is the compact evidence summary.

## Build/test

Install the inherited Python requirements and host tools (Clang, LLD, AArch64 binutils, QEMU with plugin API v4, a C compiler, pkg-config and GLib headers), then:

```sh
python build_release.py
python test_native_switch.py --inherited 3
for n in 0 1 2 4 5 6; do python test_native_switch.py --inherited "$n"; done
python test_native_switch.py --base 0x46000000 --inherited 3
python test_native_switch.py --negative-phone-guard
python audit_native_switch.py
python rebuild.py
```

All nine scenarios and a clean byte-identical production-image rebuild passed. Do not run multiple QEMU tests concurrently. The full proof is in `verification/final-proof.json`, `verification/rebuild-proof.json`, and `verification/guard-lineage.json`; per-test state, actual framebuffer images and compressed audits are under `tests/`.

The original embedded Mach-O remains byte-identical: `9239d4adb7a2c433d7ed5729b47b63acc5281af73d8a81640ef3c678c8afd26c`. Only byte-checked staged RAM is patched. The new assembly and the native linker script fit in the existing approved executable cave. The actual switch/creation/blocking routines remain native and unmodified.
