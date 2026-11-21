# Hardware Map — Redmi Note 12 Pro 5G (`ruby`, MediaTek MT6877)

All addresses and values below are verified against the live LK device tree captured from `ruby` (`chain-live-fdt.bin`) and confirmed by on-device boot runs (`v0.38a`, `v0.40`, `v0.50`, `v0.51b`).

## SoC & CPU Topology

| Property | Value |
| :--- | :--- |
| Device | Xiaomi Redmi Note 12 Pro 5G (`ruby`) |
| SoC | MediaTek MT6877 / Dimensity 1080 |
| Little cluster (6 cores) | `arm,cortex-a55`, MPIDR `0x000`, `0x100`, `0x200`, `0x300`, `0x400`, `0x500` |
| Big cluster (2 cores) | `arm,cortex-a78` / `a76`, MPIDR `0x600`, `0x700` |
| SMP bring-up | PSCI via `SMC` (`CPU_ON`, `AFFINITY_INFO`) |

> Note: `small-core` DT tags are intentionally omitted in `v0.51b+` bring-up images so `GenericARMPE` starts all 8 cores immediately instead of deferring little cores by 28 seconds.

## Interrupt Controller (GICv3) & Timer

| Component | Base / Value | Size / Details |
| :--- | :--- | :--- |
| GIC Distributor (`GICD`) | `0x0C000000` | `0x40000` (`GICD_CTLR = 0x10` at boot) |
| GIC Redistributor (`GICR`) | `0x0C040000` | `0x200000`, stride `0x20000` per core |
| System Register Interface | `ICC_SRE_EL1 = 1` | `ICC_CTLR_EL1.EOImode` cleared in `gic_v3_irq_cpu_init` |
| Architectural Timer | `CNTFRQ_EL0 = 0x00C65D40` | `13 000 000 Hz` (13 MHz) |
| Virtual Timer Interrupt | `PPI 27` (DT PPI `11`) | Confirmed driving XNU scheduler & IOMFB 60 Hz |

## Boot Memory, Live-FDT & Display

| Region | Physical Address | Size / Notes |
| :--- | :--- | :--- |
| Live LK FDT | `0x47C80000` | Limit `0x80000` (parsed dynamically at boot) |
| LK Framebuffer (GOP) | `0x7C406000` | `1080×2400`, 32-bit BGRA, 60 Hz |
| Clean RAM window (`<4 GiB`) | `0x90000000` | `640 MiB` (`0x28000000`) — **confirmed on hardware** (`v0.50`, `v0.51b`) |
| High RAM window (`>4 GiB`) | `0x140000000` | `4064 MiB` (`rank 1` in `mblock_info`, total `7160 MiB`) — triggers SError on `DAIF.A` unmask (under investigation) |

## Input & Storage Peripherals (DTB Reference)

| Peripheral | Base Address | Notes |
| :--- | :--- | :--- |
| Keypad (`KPD`) | `0x10010000` | `KP_MEM1/2`: `VolDown` (bit 0), `Power` (bit 8), `VolUp` (bit 17); driver in `v50-kernel-config/drivers/hakios.mtk_buttons.c` |
| GPIO Controller | `0x10005000` | `DIR=0x000`, `DOUT=0x100`, `DIN=0x200`, `MODE=0x300`, step `0x10` |
| `infracfg_ao` Clocks | `0x10001000` | `IFRAO2_CG_CLR=0xA8`, `STA=0xAC`, bit 10 = `CLK_IFRAO_SPI3` |
| Touch SPI (`spi3`) | `0x11013000` | `m16-touch@0` (`GPIO 160–163`), supports `FT3680` & `GT9916R`; driver in `v50-kernel-config/drivers/hakios.mtk_touch.c` |
| UFS Controller (`ufshci`) | `0x11270000` | Not enabled yet — all current images avoid storage access completely |
