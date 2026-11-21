# HakiOS / XNU для Redmi Note 12 Pro 5G (`ruby`)

> **Язык:** [English](README.md) | Русский

Портирование нативного ядра **XNU (Darwin 22.5)** на **Xiaomi Redmi Note 12 Pro 5G (`ruby`, MediaTek MT6877 / Dimensity 1080)** на базе загрузчика **Mu-Silicium UEFI** и ядра **HakiOS**, собранного из исходников.

Весь прогресс ведётся в одной линейной ветке `main`: от ранних тестов кооперативного переключения и прерываний (`v0.40 → v0.41b`) до полноценного ядра XNU из исходников (`v0.50 → v0.52`) с **8 ядрами SMP**, **GICv3 + таймером**, **живой видеоконсолью**, **IOKit / BSD** и **userland PID 1** на реальном телефоне.

---

## Подтверждение на реальном телефоне

| `v0.50` — Живая консоль XNU + PID 1 | `v0.50` — Чистый возврат | `v0.51b` — Все 8 ядер SMP (`hw.ncpu=8`) |
| :---: | :---: | :---: |
| ![v0.50 XNU Console](docs/media/v0.50-xnu-console.png) | ![v0.50 PID 1 Pass](docs/media/v0.50-pid1-pass.png) | ![v0.51b 8-Core SMP Pass](docs/media/v0.51b-smp-8core-pass.png) |
| GICv3, IOKit, MiniIOMFB 60 Гц, BSD `md0`, `/sbin/launchd` | `GICv3:OK BSD:OK PID1:OK`, `ESR=0` | `hw.ncpu=8`, `hw.activecpu=8`, `640 МБ` RAM |

---

## Текущий статус подсистем

| Подсистема | Статус | Подробности |
| :--- | :---: | :--- |
| **Mu-Silicium UEFI и упаковка boot v2** | Работает на телефоне | RAM-образ FAT16 (`BOOTAA64.EFI` + `PAYLOAD.XNP`) |
| **Парсер live-FDT и выбор окна RAM** | Работает на телефоне | Проверка live-FDT (`0x47C80000`), чистое окно `<4 ГБ` (`640 МБ`) |
| **Ядро XNU из исходников (`Darwin 22.5`)** | Работает на телефоне | `GenericARMPE`, MMU, VM, `IOKit`, `IONVRAM`, `AMFI/SandBox` |
| **GICv3 и виртуальный таймер (`PPI 27`)** | Работает на телефоне | `GICD 0x0C000000`, `GICR 0x0C040000`, `CNTFRQ = 13 МГц` |
| **8-ядерный PSCI SMP** | Работает на телефоне | 6× Cortex-A55 + 2× Cortex-A78/A76 через PSCI SMC (`v0.51b`) |
| **Экран (`video_console` + IOMFB)** | Работает на телефоне | Framebuffer LK `0x7C406000` (`1080×2400`, 60 Гц) |
| **BSD `mockfs` + Userland PID 1** | Работает на телефоне | `/sbin/launchd` (`userland/hello`), чистый возврат с логом на экран |
| **Кнопки (`KPD`) и тачскрин (`SPI3`)** | Проверено в QEMU (`v0.52`) | `KPD @ 0x10010000` + `SPI3 @ 0x11013000` (`FT3680`/`GT9916R`), ждёт теста на телефоне |
| **Полная RAM (`>4 ГБ` / Rank 1)** | В работе (SError) | Область `0x140000000` вызывает SError при снятии маски `DAIF.A` |
| **Накопитель (MT6877 UFS)** | В планах | `ufshci@11270000` пока не портирован; **все образы работают только в RAM** |
| **Полный userland / SpringBoard** | В планах | После UFS, `dyld` и расширения RAM |

---

## История вех (`main`)

- **`v0.40`** — подтверждённое на железе кооперативное переключение потоков XNU (`Switch_context`).
- **`v0.41a` / `v0.41b`** — одно прерывание таймера `PPI 27` и вытеснение потока через нативный AST XNU в QEMU.
- **`v0.50`** — переход от бинарных патчей к сборке **HakiOS XNU из исходников** под MT6877; первый запуск userland PID 1 на телефоне.
- **`v0.51` → `v0.51b`** — включение **8 ядер PSCI SMP** на телефоне (640 МБ RAM `<4 ГБ`) и локализация SError в области `>4 ГБ`.
- **`v0.52`** — драйверы кнопок (`KPD`) и тачскрина (`SPI3`: `FT3680`/`GT9916R`), интерактивный тест ввода в PID 1.

---

## Быстрая сборка (WSL2 Debian)

```sh
cd ~/hackios/ruby
tools/blobs.sh restore
~/hackios/env/run.sh python -B build_v50.py
```

Итоговый образ сохраняется в `out/v52/Mu-ruby-v0.52-hakios-input.img`.
Подробности сборки, проверки в QEMU и тестирования на телефоне — в [docs/BUILD.md](docs/BUILD.md), карта адресов MT6877 — в [docs/HARDWARE.md](docs/HARDWARE.md), архив ранних отчётов `v0.40–v0.41` — в [docs/history/README.md](docs/history/README.md).
