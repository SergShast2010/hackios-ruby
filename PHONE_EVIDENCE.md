# Наблюдения на ruby / MT6877

Источник: два фото, присланные владельцем устройства вместе с разрешением продолжать v0.41. Связь фото с конкретным хэшем бинарника из самих фото не устанавливается. Архивные отчёты v0.40 были составлены до этого аппаратного прогона.

| Фото | Видимые результаты |
|---|---|
| v0.39b, `NATIVE FIRST KERNEL THREAD ENTERED` | `Native reason=0x39`, `GIC bound=1`, `Hardware restored=1`, `Restore error=0`, `RPR=0xff`. Достигнут первый поток XNU, IRQ маскирован. |
| v0.40, `NATIVE BOOT - SECOND - BOOT: RESUMED` | `Native reason=0x40`, `GIC bound=1`, `Hardware restored=1`, `Restore error=0`, `RPR=0xff`, `PMR=0xf0`, `Switch pass=1`, `Phase=5`. На экране `Boot before` и `Boot resumed` совпадают, `Second thread` отличается. |

Это подтверждение отображённых checkpoint'ов на устройстве. Оно не подтверждает периодический таймер, вытеснение, полный запуск iOS, IOKit, PSCI или storage. SHA-256 `03222cebb1e928c37d87d3a20f77a7f3cc6ff99fc6271e9ee96ad15c217050e7` относится к архивному образу v0.40 из QEMU-проверок. **v0.41 на телефоне не проверялась.**
