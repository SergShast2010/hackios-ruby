# Mu ruby v0.41 — handoff

## Текущий рубеж: v0.41b — одно timer-driven preemption, QEMU only

Это единая ветка `v0.40 → v0.41`; телефон не прошивался. В v0.41b после кооперативного XNU-переключения запускается виртуальный таймер на 1 мс, приходит PPI 27 и сохраняется EL1t frame. Обработчик:

1. забирает один `ICC_IAR1` и получает INTID 27;
2. гасит timer, делает один `ICC_EOIR1` и оставляет IRQ замаскированным;
3. вызывает закреплённые в этом XNU `thread_wakeup_prim(event, TRUE, THREAD_AWAKENED)` и `ast_on(AST_PREEMPT | AST_URGENT)`;
4. передаёт управление `ast_taken_kernel` → `thread_preempted_in_kernel` → нативный `thread_block_reason`/`Switch_context`;
5. второй native-поток выполняется, затем boot-поток возвращается к `NtSwitchPrepared`.

`event` — тот же адрес `NativeState + 344`, на котором второй поток заранее сделал `assert_wait`. Доказательство этапа — `preempt_sched=3`, четыре переключения в QEMU audit-трассе вместо двух, `preempt_iar=27`, `restore_error=0`, отсутствие panic и совпадение boot TPIDR/SP после возврата.

Сборка остаётся QEMU-only: профиль телефона отказывается до записи в GIC. Адреса XNU-вызовов взяты из закреплённого бинарного kernel v0.38b и не считаются переносимым ABI. Это диагностический образ, не релиз и не образ для прошивки.

## Проверенные сценарии

- inherited 0/3/5/6 и база `0x46000000`: одно IRQ; для 1/6 контракт отказывает до IRQ из-за активного RPR;
- inherited 2/4: остановка до XNU;
- 10 повторов inherited 0: PASS 10/10;
- повторная сборка: образ совпал байт-в-байт, SHA256 `78f584589ce4b8a5b0348f488f106fa06bfbf972df487e65b07cd2bbe087c6b7`.

## Ограничения и следующий шаг

Это ещё не телефонный профиль MT6877 и не полноценный периодический scheduler tick. Следующий этап — вынести XNU ABI/адреса в явный pinned-kernel manifest, добавить проверку сохранения полного EL1 frame и только потом делать отдельный ruby-профиль GIC/timer. До этого образ не прошивать.

## Исторический рубеж: v0.41a, одно IRQ без scheduler-вызова

Этот раздел оставлен как история промежуточного состояния. В v0.41a проверялись непересекающиеся patch ranges, собственный EL1t-вектор и ровно одно QEMU IRQ с `eret` без scheduler-вызова. После него v0.41b добавил проверенный QEMU-only путь `thread_wakeup_prim` → AST → `thread_preempted_in_kernel`; старые утверждения о «quiet-пути» ниже к текущей сборке не относятся.

Архивные артефакты старого сбоя (`NtPreemptArm+8` по `0xfffffff007472dcc`, второе пересечение по `0xfffffff007472f64`, ESR `0xf2000526` от намеренного `brk #0x526`) находятся в `archive/v0.41-overlap-failure/` и не являются входами текущей сборки.

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
