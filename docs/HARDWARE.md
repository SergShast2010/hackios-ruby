# Hardware Map — Redmi Note 12 Pro 5G (`ruby`, MediaTek MT6877)

All addresses and behaviors below are verified against the live LK device tree captured from `ruby` (`chain-live-fdt.bin`) and confirmed by on-device boot runs (`v0.40`, `v0.50`, `v0.51b`, `v0.53`, `v0.54d`, `v0.54e`, `v0.55e`, `v0.55f`).

## SoC & CPU Topology

| Property | Value |
| :--- | :--- |
| Device | Xiaomi Redmi Note 12 Pro 5G (`ruby`, 8 GiB RAM / 256 GiB UFS) |
| SoC | MediaTek MT6877 / Dimensity 1080 |
| Little cluster (6 cores) | `arm,cortex-a55`, MPIDR `0x000`–`0x500` |
| Big cluster (2 cores) | `arm,cortex-a78`, MPIDR `0x600`, `0x700` (`FEAT_CCIDX` extended `CCSIDR_EL1` format) |
| SMP bring-up | PSCI via `SMC` (`CPU_ON`, `AFFINITY_INFO`), all `8/8` cores active |

> **Cache note (`v0.54d`):** Cortex-A78 implements `FEAT_CCIDX` (`ID_AA64MMFR2_EL1.CCIDX != 0`). XNU's `GET_CACHE_CONFIG` in `caches_asm.s` must decode the 64-bit `CCSIDR_EL1` layout (`NumSets = [55:32]`, `Associativity = [23:3]`), otherwise cache maintenance flushes only a single set on the big cores.

## Interrupt Controller (GICv3) & Timer

| Component | Base / Value | Size / Details |
| :--- | :--- | :--- |
| GIC Distributor (`GICD`) | `0x0C000000` | `0x40000` (`GICD_CTLR = 0x10` at boot) |
| GIC Redistributor (`GICR`) | `0x0C040000` | `0x200000`, stride `0x20000` per core |
| System Register Interface | `ICC_SRE_EL1 = 1` | `ICC_CTLR_EL1.EOImode` cleared in `gic_v3_irq_cpu_init` |
| Architectural Timer | `CNTFRQ_EL0 = 0x00C65D40` | `13 000 000 Hz` (13 MHz) |
| Virtual Timer Interrupt | `PPI 27` (DT PPI `11`) | Confirmed driving XNU scheduler & IOMFB 60 Hz |
| USB MTU3 Interrupt | `INTID 160` (`SPI 128`, `0x80`) | Level-high; confirmed driving `hakios.mtk_usb_ecm.c` |

## Boot Memory, Live-FDT & Display

| Region | Physical Address | Size / Notes |
| :--- | :--- | :--- |
| Live LK FDT | `0x47C80000` | Limit `0x80000` (parsed dynamically at boot) |
| LK Framebuffer (GOP) | `0x7C406000` | `1080×2400`, 32-bit BGRA (`0x9E3400` bytes), 60 Hz (offset `+0x2000` inside a 16 KiB page) |
| High RAM window (`>4 GiB`) | `0x140000000` | **`4064 MiB`** (`0x140000000..0x23E000000`) — **confirmed on hardware** (`v0.54e`–`v0.55f`) |
| Top-of-DRAM reserved page | `0x23FFFF000` | `4 KiB` (`mblock-2-dramc-rk1`), excluded by `v50_ram_select.h` |
| Fallback RAM window (`<4 GiB`) | `0x90000000` | `640 MiB` (`0x28000000`) — **confirmed on hardware** (`v0.50`, `v0.51b`) |

> **`>4 GiB` SError root cause (`v0.54d`):** When `memSize > 1 GiB`, XNU enables 16 KiB logical `PAGE_SIZE` (`PAGE_SHIFT_CONST = 14`). Because `fb_base = 0x7C406000` is 4 KiB-aligned (`start_offset = 0x2000`), an upstream bug in `osfmk/arm/io_map.c` (`virtual_space_start += round_page(size)` instead of `alloc_size`) caused the next `ml_io_map(0x0C000000)` (`GICD`) to overlap the end of the framebuffer VA. `vc_clear_screen()` then wrote zeroes over `GICD` registers, raising `SLVERR` (`ESR=0xBE000011`). Fixed in `v50-kernel-config/xnu-v0.54d-fixes.patch`.

## Confirmed Peripherals (On-Device)

| Peripheral | Base Address | Status & Verified Details |
| :--- | :--- | :--- |
| Keypad (`KPD`) | `0x10010000` | **Confirmed**: `KP_MEM1` (`+0x0004`) bit 0 = `VolDown` (active low) |
| PMIC Wrapper (`PWRAP` → `MT6359`) | `0x10026000` | **Confirmed (read-only)**: SWINF reads `MT6359 TOPSTATUS` (`0x002a`): bit 1 = `Power`, bit 3 = `VolUp` (active low) |
| UFS Host (`ufshci`) | `0x11270000` | **Confirmed (strictly read-only)**: reuses LK-initialized link; `LUN 0/1` `4 MiB`, `LUN 2` `244 064 MiB` (4 KiB blocks), `77` active GPT partitions (`userdata` `230 807 MiB`) |
| USB Controller (`MTU3` + `IPPC`) | `0x11201000` / `0x11203E00` | **Confirmed**: USB 2.0 High-Speed gadget, `IRQ 160`, 36-bit QMU DMA (`TX1`/`RX1`), `VID 0x1209 / PID 0x0003`, `en0` `192.168.7.2/24`, **38.74 MB/s RX / 37.64 MB/s TX** |
| USB PHY (`TPHY v2`) | `0x11E40000` | **Confirmed**: `U2` port (`ACR0 0x0300`, `ACR5 0x0314`, `ACR6 0x0318`), `U3_CHIP` (`0x0800`); `PS5169` Type-C mux on `GPIO 121` |
| GPIO Controller | `0x10005000` | `DIR=0x000`, `DOUT=0x100`, `DIN=0x200`, `MODE=0x300`, step `0x10` |
| `infracfg_ao` Clocks | `0x10001000` | `IFRAO2_CG_CLR=0xA8`, `STA=0xAC` (bit 10 = `CLK_IFRAO_SPI3`) |
| Touch SPI (`spi3`) | `0x11013000` | **Deferred**: `GPIO 160–163`; this hardware unit carries FocalTech `FT3680` (`fts_ts`), bring-up deferred until after iOS userland |
