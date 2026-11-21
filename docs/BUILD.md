# Build, QEMU & On-Device Testing Guide

## 1. Environment (WSL2 Debian on ext4)

Keep both the repository (`~/hackios/ruby`) and the portable toolchain (`~/hackios/env`) inside the Linux filesystem (ext4 in WSL2), **not** on Windows NTFS — NTFS loses 16 case-sensitive netfilter headers when unpacking Linux sysroots.

```sh
ENV_DIR="$HOME/hackios/env"
cd "$ENV_DIR"
sh restore-permissions.sh

cd "$HOME/hackios/ruby"
tools/blobs.sh restore   # restores large base images from ~/hackios/blobs (verified via BLOBS.sha256)
```

## 2. Building the Current `v0.5x` Image

`build_v50.py` compiles `ebsprobe/v50_loader.c` + `ebsprobe/v50_asm.S` into `v50_loader.efi`, packs the source-built `hakios-kernel` and `userland/hello` into `PAYLOAD.XNP`, and embeds both into the Mu-Silicium Android `boot.img` container.

```sh
cd ~/hackios/ruby
~/hackios/env/run.sh python -B build_v50.py
```

Outputs are written to `out/v52/`:
- `Mu-ruby-v0.52-hakios-input.img` — Android `boot` v2 image ready for testing
- `v50_loader.efi` & `PAYLOAD.XNP` — extracted payloads for QEMU verification
- `manifest.json` — SHA-256 hashes and packaging margins

The matching kernel configuration (`device.conf`, `ruby_defconfig`), custom MT6877 drivers (`hakios.mtk_buttons.c`, `hakios.mtk_touch.c`), and PID 1 source (`hello.c`) are tracked under `v50-kernel-config/`.

## 3. Testing on Redmi Note 12 Pro 5G (`ruby`)

- `fastboot boot` does not work reliably on this MediaTek LK build, so testing uses temporary `boot` flashing with an immediate restore of your stock `boot.img` afterward.
- Every image in this repository is strictly **RAM-only** (no UFS writes).

```powershell
# 1. Make sure you have a verified stock boot.img backup first!
.\fastboot flash boot .\Mu-ruby-v0.52-hakios-input.img
.\fastboot reboot

# 2. After photographing the diagnostic/XNU screens, reboot to fastboot and restore stock Android boot:
.\fastboot flash boot .\boot.img
.\fastboot reboot
```

## 4. Historical `v0.40` / `v0.41b` Builds

The legacy QEMU-only binary-patch harness (`v0.41b`) remains intact and runnable:

```sh
cd ~/hackios/ruby
~/hackios/env/run.sh python -B quality-tools/test_patch_ranges.py
~/hackios/env/run.sh python -B build_release.py
~/hackios/env/run.sh python -B test_preempt.py --image out/Mu-ruby-v0.41-QEMU-ONLY.img --inherited 3
~/hackios/env/run.sh python -B test_preempt.py --image out/Mu-ruby-v0.41-QEMU-ONLY.img --negative-phone-guard
```

To inspect or rebuild the exact `v0.40` on-device baseline:

```sh
git worktree add ../ruby-v0.40 v0.40
(cd ../ruby-v0.40 && tools/blobs.sh restore)
```
