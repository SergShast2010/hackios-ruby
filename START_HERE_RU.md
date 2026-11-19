# Mu-ruby v0.41: подготовка IRQ в QEMU

Это продолжение v0.40, не новый порт. v0.40 проверял нативное кооперативное переключение XNU; v0.41 сейчас проверяет следующий шаг — проходит ли `NtPreemptArm` с корректными байтами IRQ-векторов. **В этом рубеже IRQ намеренно маскирован, таймер не вооружается и вытеснение не заявляется. Телефон не прошивать.**

## Запуск

Скопируй/распакуй рабочую папку и переносимое окружение в Linux home Debian WSL2. Проверяй переносимое окружение до сборки. В текущем Windows-наборе и в `ruby.7z` отсутствуют 16 указанных в manifest netfilter-заголовков, у которых есть имена, отличающиеся только регистром; остальные контрольные суммы совпали, и недостающие заголовки для этих сборок не понадобились. Для полностью верифицированного окружения нужен оригинал с полным набором файлов, распакованный на чувствительную к регистру Linux-файловую систему. Не менять manifest ради зелёного статуса.

```sh
ENV_DIR="$HOME/Mu-ruby-environment-v41"  # укажи фактический путь
cd "$ENV_DIR"
sh restore-permissions.sh
sha256sum --quiet -c manifest.sha256
cd "$HOME/Mu-ruby-v0.41-preempt-sources-current"
"$ENV_DIR/run.sh" python -B quality-tools/test_patch_ranges.py
"$ENV_DIR/run.sh" python -B build_release.py
"$ENV_DIR/run.sh" python -B test_preempt.py --image out/Mu-ruby-v0.41-QEMU-ONLY.img --inherited 3 --screenshot
"$ENV_DIR/run.sh" python -B test_preempt.py --image out/Mu-ruby-v0.41-QEMU-ONLY.img --negative-phone-guard
"$ENV_DIR/run.sh" python -B rebuild.py
```

Сборка читает kernel-base v0.38b и упаковочную базу v0.32b из `inputs/`. В `out/` попадают образ, манифест, карта ELF/EFI, заголовок патчей и `build.json`. Для другого выходного каталога передай `build_release.py --out DIR`, `test_preempt.py --image DIR/Mu-ruby-v0.41-QEMU-ONLY.img --results DIR/tests` и `rebuild.py --reference DIR`. Не запускать две тяжёлые виртуальные машины одновременно.

В свежем `out/tests/*/result.json` ожидаются `NtSwitchResumed → NtPreemptArm → NtSwitchPrepared → APP_LOW_RETURN → APP_TERMINAL`, `switch_pass=1`, `preempt_phase=3`, `preempt_count=0`, `restore_error=0`. В тесте без QEMU fixture отказ должен произойти до выделения RAM под XNU. `pass=true` указывает на успех **конкретного сценария подготовки или ожидаемого отказа**, не на успешное IRQ. Тест читает байты всех staged patches до запуска XNU; аудит проверяет отсутствие записи в GIC MMIO и ACK/EOI/DIR/IRQ-unmask в нативном пути.

Файлы `runtime_manifest.json`, `build.json`, `ebsprobe/native_patches.h` и `tests/` **вне `out/`** сохранены как свидетельство старого сбоя и не используются при новой сборке. Старые `test_native_switch.py` и `audit_native_switch.py` относятся к v0.40; для baseline запускай их из отдельной неизменённой папки v0.40.
