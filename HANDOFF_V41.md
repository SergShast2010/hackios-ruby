# Mu ruby v0.41 — handoff

## Текущий рубеж: подготовка IRQ, QEMU only

Актуальные команды в [START_HERE_RU.md](START_HERE_RU.md), аппаратные фото предыдущих версий описаны в [PHONE_EVIDENCE.md](PHONE_EVIDENCE.md). Новая сборка создаёт `out/Mu-ruby-v0.41-QEMU-ONLY.img`; не использовать её на телефоне.

В архивном образе v0.41 защитные переходы перезаписывали `.extra`: по `0xfffffff007472dcc` (`NtPreemptArm+8`) и `0xfffffff007472f64`. Первое пересечение приводило в `NtPanic`. `0xf2000526` в ESR был виден до этого и относится к намеренному `brk #0x526` из `transition.S`, а не к установленной ошибке чтения IRQ-вектора. Текущая сборка валидирует непересекающиеся диапазоны всех патчей. В состоянии отделены stale exception-регистры, panic LR и IRQ-поля.

Текущий `NtPreemptArm` проверяет обе ветки вектора, не включая IRQ. Необратимая настройка GIC MMIO и вызовы wakeup/`thread_block_reason` из самодельного IRQ убраны из пути. Во всех штатных сценариях контракт остаётся quiet. Следующий шаг после подтверждённого `NtSwitchPrepared`: один реальный PPI 27 → сохранение EL1 frame → `eret` без scheduler-вызова; затем отдельная проверка допустимой точки планирования XNU и wakeup того же wait event. Не считать достижение подготовки доказательством таймера.

## Архивный handoff от 15.11.2026

Дата: 15.11.2026, Москва

## Что передано

Это текущая рабочая папка ветки `v41-preempt`, а не готовый телефонный релиз. В архив входят актуальные `.c/.h/.S`, сборочные и QEMU-тестовые скрипты, linker script, manifest, зависимости для сборки, базовые входные образы и результат последнего прогона.

## Последняя подтверждённая точка

v0.40 — настоящий cooperative context switch XNU: native boot thread → созданный native diagnostic thread → возврат в boot thread. Это рабочая базовая версия. Timer-driven preemption на v0.40 не заявляется.

## Текущая задача v0.41

Подключить ограниченный timer-driven preemption на одном CPU: virtual timer → GIC PPI 27 → native EL1 IRQ entry → сохранение frame → scheduler/block/wakeup → возврат из IRQ.

## Последний прогон

Команда:

```sh
python -u test_preempt.py --inherited 3 --seconds 30
```

Сборка перед ним завершилась успешно. Cooperative switch проходит, затем выполнение доходит до `NtPreemptArm`. До `NtPreemptIrq`/`NtPreemptBody` тест не доходит.

Результат `tests/44000000-inherited-3/result.json`:

- `pass=false`
- `reason=0xef`
- `ESR_EL1=0xf2000526`
- `restore_error=0x1`
- `switch_pass=0`
- `phase=6`
- `preempt_count=0`
- `preempt_phase=0`
- `preempt_error=0xf2000526`
- `PMR=0xf0`, `RPR=0xff`
- `NtPreemptArm` наблюдается; `NtPreemptIrq` не наблюдается

Это QEMU-only результат на `virt`, Cortex-A55, 1 CPU, TCG. Телефон не прошивался и не проверялся на этой ветке. Текущий v0.41 не прошивать.

## Главный блокер

Падение происходит внутри или сразу после `NtPreemptArm`, до marker phase 3/4/5. Точный faulting instruction ещё не доказан; чтение вектора через `V32(0xfffffff00745a080)` — только рабочая гипотеза для следующего instrumented прогона, не установленный факт.

## Следующий безопасный шаг

1. Не менять v0.40 и не прошивать v0.41.
2. Добавить минимальный capture вокруг первых инструкций `NtPreemptArm`.
3. Проверить, проходит ли запись `vector_patch` и чтение `cntpct/cntfrq/mapped GIC` в текущем отображении.
4. Только после прохождения marker 5 искать причину отсутствия IRQ и проверять `NtPreemptIrq`.
5. После QEMU — отдельная проверка на телефоне, но только с новым proof и явным разрешением на прошивку.

## Важные файлы

- `ebsprobe/native_quiet.c` — текущие `NtPreemptArm`, `NtPreemptBody`, marker и wakeup.
- `ebsprobe/native_bridge.S` — текущий native IRQ frame/return и panic capture.
- `ebsprobe/native_state.h` — ABI состояния, включая preemption-поля.
- `test_preempt.py` — последний QEMU observer/test.
- `runtime_manifest.json` — адреса bridge и список staged-RAM patches.
- `tests/44000000-inherited-3/result.json` — последний failure state.

Документы `README.md`, `REPORT.md` и `IMPLEMENTATION_STATUS.md` в папке содержат историческое описание v0.40 и не являются доказательством v0.41.
