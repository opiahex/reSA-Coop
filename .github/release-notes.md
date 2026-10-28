# reSA-Coop 0.2.37-3p

### Русский

- В диагностическом меню появились управление записью и сохранение последних событий. Выгрузка выполняется в фоне; результат и потери видны в меню.
- Обычные сообщения журнала после инициализации и JSONL-диагностика передаются ограниченной очереди записи. Переполнение и ошибки учитываются отдельно.
- Снимки показывают доступные данные владельца, поколения сущности, подключения и этапа миссии. Недоступные поля остаются отмеченными как неизвестные.
- Добавлены связанные числовые события входа и выхода для инструментированных обработчиков SCM. Аварийный хвост сохраняет эти поля и контрольную сумму.
- У критических Core CALL и SCM-патчей проверяются исходные байты и результат установки. Inspector сохраняет различие между данными установки и текущим чтением памяти.
- Добавлены измерения времени снимка и отрисовки диагностического меню.

Версия клиента и сервера: 0.2.37-3p; протокол 45, формат профиля 7. Формат сетевых сообщений не изменён.

Проверено в трёх GTA: запись и повторный запуск, экспорт, доступный контекст, инструментированный SCM-вызов и сравнительная нагрузка записи и меню. Это не полная проверка всех миссий или сторонних ASI. Ручное нажатие новых кнопок в финальном прогоне не проверялось. Существующие проблемы транспорта, NPC и отдельных этапов Photo Opportunity остаются открытыми.

### English

- Added recording controls and recent-event export to Diagnostics. Export runs in the background; the menu reports its result and losses.
- Runtime text logging after initialization and JSONL diagnostics use a bounded writer queue. Overflow and I/O errors have separate counters.
- Snapshots expose available owner, entity generation, connection and mission-stage data. Unavailable fields remain explicitly unknown.
- Added paired numeric entry/exit events for instrumented SCM handlers. The crash tail preserves these fields and a checksum.
- Critical Core CALL and SCM patches validate preimages and installation results. Inspector keeps installation metadata separate from current memory samples.
- Added diagnostic snapshot and draw-time measurements.

Client/server version: 0.2.37-3p; protocol 45, profile format 7. Network message formats are unchanged.

Tested in three GTA instances: recording and restart, export, available context, an instrumented SCM call, and comparative recording/menu overhead. This is not full mission or third-party ASI coverage. Physical clicks on the new buttons were not tested in the final run. Existing vehicle, NPC and Photo Opportunity issues remain open.


## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`. Если ASI-загрузчика нет, запустите `install.bat`. Меню — F9.

Extract the ZIP beside `gta_sa.exe`. Run `install.bat` if an ASI loader is needed. Open F9 in-game.

SA:COOP — Karimov-Akbar; reSA-Coop — opiahex. MIT и отдельные условия оригинальных диагностических компонентов сохранены. / MIT and the separate original diagnostic component terms are preserved.

[CHANGELOG RU / EN](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md). SHA-256: `SHA256SUMS.txt`.
