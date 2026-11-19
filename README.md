# HackiOS / Mu-Silicium for Redmi Note 12 Pro 5G (`ruby`)

One development line: **v0.39b → v0.40 → v0.41**. The v0.40 directory is an immutable reference; active development is here.

**Current target: v0.41 QEMU-only IRQ preparation. Do not flash this image.** The checkpoint verifies that the native XNU cooperative round trip still works and that execution returns from `NtPreemptArm` after checking the staged IRQ vector branches. DAIF.I remains set; no timer IRQ or preemption is claimed. The old v0.41 crash artifacts (overlapping `.extra` patches) are preserved in `archive/v0.41-overlap-failure/` and are not build inputs. New builds and results go under `out/`.

- [Build and test in WSL2](START_HERE_RU.md)
- [Current handoff and historical failure](HANDOFF_V41.md)
- [v0.39b/v0.40 on-device evidence](PHONE_EVIDENCE.md)
- [Archived v0.40 README](README_V40_ARCHIVE.md)
