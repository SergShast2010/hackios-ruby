# Mu-ruby v0.41b: вытеснение по одному IRQ таймера в QEMU

Это продолжение v0.40, не новый порт. v0.40 проверял нативное кооперативное переключение XNU; v0.41b проверяет следующий шаг — настоящее однотактное timer-driven preemption в QEMU. **PPI 27 приходит в собственный EL1t-обработчик, XNU будит тот же waiter через `thread_wakeup_prim`, AST вызывает `thread_preempted_in_kernel`, второй native-поток выполняется, затем boot-поток возвращается. Телефон не прошивать.**

## Запуск

Скопируй/распакуй рабочую папку и переносимое окружение в Linux home Debian WSL2. Проверяй переносимое окружение до сборки. В текущем Windows-наборе и в `ruby.7z` отсутствуют 16 указанных в manifest netfilter-заголовков, у которых есть имена, отличающиеся только регистром; остальные контрольные суммы совпали, и недостающие заголовки для этих сборок не понадобились. Для полностью верифицированного окружения нужен оригинал с полным набором файлов, распакованный на чувствительную к регистру Linux-файловую систему. Не менять manifest ради зелёного статуса.

```sh
ENV_DIR="$HOME/hackios/env"
cd "$ENV_DIR"
sh restore-permissions.sh
sha256sum --quiet -c manifest.sha256
cd "$HOME/hackios/ruby"
tools/blobs.sh restore   # образы и логи вне git, по sha256 из BLOBS.sha256
"$ENV_DIR/run.sh" python -B quality-tools/test_patch_ranges.py
"$ENV_DIR/run.sh" python -B build_release.py
"$ENV_DIR/run.sh" python -B test_preempt.py --image out/Mu-ruby-v0.41-QEMU-ONLY.img --inherited 3 --screenshot
"$ENV_DIR/run.sh" python -B test_preempt.py --image out/Mu-ruby-v0.41-QEMU-ONLY.img --negative-phone-guard
"$ENV_DIR/run.sh" python -B rebuild.py
```

Сборка читает kernel-base v0.38b и упаковочную базу v0.32b из `inputs/`. В `out/` попадают образ, манифест, карта ELF/EFI, заголовок патчей и `build.json`. Для другого выходного каталога передай `build_release.py --out DIR`, `test_preempt.py --image DIR/Mu-ruby-v0.41-QEMU-ONLY.img --results DIR/tests` и `rebuild.py --reference DIR`. Не запускать две тяжёлые виртуальные машины одновременно.

В свежем `out/tests/*/result.json` для IRQ-сценария ожидаются `NtSwitchResumed → NtPreemptArm → NtIrqEl1t → NtPreemptIrq → NtPreemptBody → _thread_invoke → _Switch_context → NtSwitchPrepared → APP_LOW_RETURN → APP_TERMINAL`, `switch_pass=1`, `preempt_phase=7`, `preempt_count=1`, `preempt_sched=3`, `preempt_iar=27`, `restore_error=0`. Для inherited 1/6 ожидается отказ до IRQ (`preempt_error=0x301`, `irq_mod=0`); для 2/4 — ранняя остановка до XNU. В тесте без QEMU fixture отказ должен произойти до выделения RAM под XNU. `pass=true` указывает на успех **конкретного сценария**, а не на телефонную совместимость. Тест читает байты всех staged patches до запуска XNU; аудит проверяет контракт GIC и ровно один ACK/EOI.

Свидетельства старого сбоя (`runtime_manifest.json`, `build.json`, `RamPack.h`, `EbsPayload.h`, `native_patches.h`, `tests/`, `current-test-base.img`) лежат в `archive/v0.41-overlap-failure/` и в сборке не участвуют. Скрипты v0.40 удалены из рабочего дерева, они есть в теге `v0.40`. Baseline v0.40: `git worktree add ../ruby-v0.40 v0.40 && (cd ../ruby-v0.40 && tools/blobs.sh restore)`.

Репозиторий: одна ветка `main`, теги `v0.40` (подтверждено на телефоне) и `v0.41-wip` (как получено). Крупные бинарники хранятся в `../blobs` по sha256.
