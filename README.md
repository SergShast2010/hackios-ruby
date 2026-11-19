# HackiOS / Mu-Silicium for Redmi Note 12 Pro 5G (`ruby`)

One development line: **v0.39b → v0.40 → v0.41**. The v0.40 directory is an immutable reference; active development is here.

**Current target: v0.41b QEMU-only timer preemption. Do not flash this image.** The checkpoint verifies one virtual-timer PPI 27, the native EL1t frame/restore path, XNU `thread_wakeup_prim` + AST preemption, and a scheduler round-trip back to the interrupted native thread. The phone path is still refused before any GIC write. The old v0.41 crash artifacts (overlapping `.extra` patches) are preserved in `archive/v0.41-overlap-failure/` and are not build inputs. New builds and results go under `out/`.

- [Build and test in WSL2](START_HERE_RU.md)
- [Current handoff and historical failure](HANDOFF_V41.md)
- [v0.39b/v0.40 on-device evidence](PHONE_EVIDENCE.md)
- [Archived v0.40 README](README_V40_ARCHIVE.md)
