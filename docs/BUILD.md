# Build, QEMU, USB ECM & On-Device Testing Guide

## 1. Environment (WSL2 Debian on ext4)

Keep both this repository (`~/hackios/ruby`), the kernel tree (`~/hackios/hakios-xnu-kernel`), and the portable toolchain (`~/hackios/env`) inside the Linux filesystem (ext4 in WSL2), **not** on Windows NTFS — NTFS breaks case-sensitive headers when unpacking Linux/Darwin sysroots.

```sh
ENV_DIR="$HOME/hackios/env"
cd "$ENV_DIR"
sh restore-permissions.sh

cd "$HOME/hackios/ruby"
tools/blobs.sh restore   # restores large base images from ~/hackios/blobs (verified via BLOBS.sha256)
```

## 2. Building the Current `v0.55f` Image

`build_v50.py` compiles `ebsprobe/v50_loader.c` + `ebsprobe/v50_asm.S` into `v50_loader.efi`, packs the source-built `hakios-kernel` and `userland/hello` into `PAYLOAD.XNP`, and embeds both into the Mu-Silicium Android `boot.img` v2 container.

```sh
cd ~/hackios/ruby
~/hackios/env/run.sh python -B build_v50.py
```

Outputs are written to `out/v55f/`:
- `Mu-ruby-v0.55f-hakios-usb-ecm.img` — Android `boot` v2 image ready for testing
- `v50_loader.efi` & `PAYLOAD.XNP` — extracted payloads for QEMU verification
- `manifest.json` — SHA-256 hashes and packaging margins

To build the optional Apple splash boot variant (black screen + white logo; switches to red verbose log on panic, `VolUp+VolDown` returns to UEFI summary):

```sh
~/hackios/env/run.sh python -B build_v50.py --apple-splash
```

Reproducible kernel configuration, XNU patches (`xnu-v0.54d-fixes.patch` for `io_map()` 16 KiB VA overlap + Cortex-A78 `FEAT_CCIDX`), MT6877 drivers (`hakios.mtk_buttons.c`, `hakios.mtk_touch.c`, `hakios.mtk_ufs_ro.c`, `hakios.mtk_usb_ecm.c`), and PID 1 source (`userland/hello.c`) are tracked under `v50-kernel-config/`.

## 3. Verifying in QEMU (8-Core SMP + TCP Console / Bench)

Every release is verified in 8-core QEMU with 8 GiB RAM and `virtio-net-device` port forwarding before touching real hardware:

```sh
qemu-system-aarch64 \
  -M virt,gic-version=3 -cpu cortex-a76 -smp 8 -m 8192 \
  -bios ~/hackios/env/runtime/rootfs/usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
  -device ramfb \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2323-192.168.7.2:2323,hostfwd=tcp:127.0.0.1:2324-192.168.7.2:2324,hostfwd=tcp:127.0.0.1:2325-192.168.7.2:2325 \
  -device virtio-net-device,netdev=n0 \
  ...
```

Once `TCP READY` appears, test the full TCP stack against `127.0.0.1`:

```sh
python3 tools/ruby_net.py --host 127.0.0.1 cmd info
python3 tools/ruby_net.py --host 127.0.0.1 bench --mb 2
python3 tools/ruby_net.py --host 127.0.0.1 cmd return
```

## 4. Testing on Redmi Note 12 Pro 5G (`ruby`) + USB ECM via `usbipd-win`

- `fastboot boot` is unreliable on this MediaTek LK build, so testing uses temporary `boot` flashing with an immediate restore of your verified stock `boot.img` afterward.
- Every image in this repository is strictly **RAM-only / UFS read-only** (zero writes to phone storage).

### One-time Windows host setup

Install `usbipd-win` in an Administrator PowerShell (reopen PowerShell afterward):

```powershell
winget install --exact dorssel.usbipd-win
```

### Flash and attach USB ECM to WSL2 Debian

```powershell
# 1. Make sure you have a verified stock boot.img backup first!
cd C:\platform-tools
.\fastboot flash boot .\Mu-ruby-v0.55f-hakios-usb-ecm.img
.\fastboot reboot

# 2. Wait for "TCP READY" on the phone screen, then find & attach 1209:0003 (HakiOS USB ECM):
usbipd list
usbipd bind --busid <BUSID>          # only needed once per port/device
usbipd attach --wsl --busid <BUSID>  # keep a WSL2 Debian terminal open
```

### Bring up `cdc_ether` and talk to XNU from WSL2 Debian

```sh
sudo modprobe cdc_ether
ip -br link                          # look for MAC 02:00:5e:77:ee:01 (typically enx02005e77ee01)
sudo ip addr add 192.168.7.1/24 dev enx02005e77ee01
sudo ip link set enx02005e77ee01 up

cd ~/hackios/ruby
python3 tools/ruby_net.py cmd info
python3 tools/ruby_net.py cmd mem
python3 tools/ruby_net.py cmd ufs
python3 tools/ruby_net.py bench --mb 16
python3 tools/ruby_net.py shell
```

When finished, run `python3 tools/ruby_net.py cmd return` (or hold `VolUp+VolDown` for ~1.5 s) to return cleanly to the UEFI summary screen, reboot to fastboot, and restore stock `boot.img`:

```powershell
.\fastboot flash boot .\boot.img
.\fastboot reboot
```

## 5. Historical `v0.40` / `v0.41b` Builds

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
