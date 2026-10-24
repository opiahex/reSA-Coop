# reSA-Coop 0.2.28-3p

Кооператив на троих для GTA San Andreas 1.0 US. / Three-player co-op for GTA San Andreas 1.0 US.

**Скачайте `reSA-Coop-v0.2.28-3p-win32.zip` из Assets. / Download the compiled `reSA-Coop-v0.2.28-3p-win32.zip` from Assets.**

## Русский — исправления 0.2.28

- Для Photo Opportunity передаются вертикальный диапазон и начальный сектор обзора прикреплённой камеры. Гости управляют своими камерами; соседние фотографы не перекрывают объектив.
- Передаётся видимость сценарных машин, включая скрытую опору фотокамеры. Снимки гостей содержат масштаб приближения; отметка фотографирования расходуется один раз, как в одиночной игре.
- Повторное включение управления в каждом кадре сценария больше не открывает новую сетевую паузу. Условия готовности не включают посторонние защищённые копии транспорта и NPC; отсутствующая зависимость запрашивается повторно.
- Исправлено пересечение блокировок перемещения при переносе игрока и смене камеры. Позднее включение управления у хоста доходит до гостей после завершения сцены.
- Дальние NPC в сценарных камерах получают походку и анимацию действия; движение между сетевыми отсчётами сглаживается по времени отправителя.
- Разделены гудок, обычная сирена и ускоренная сирена. Состояние сирены не превращается в постоянный гудок; звук повторно запускается после временной недоступности аудиоканала.
- Убрана ненужная синхронная загрузка модели телефона при его удалении из руки сетевого игрока.
- Отложенные события урона от сетевой копии гостя больше не начисляют преступление локальному игроку. Угон полицейской машины учитывает контекст угонщика.
- Асинхронная проверка сохранения больше не удаляет графические объекты текущего кадра. Обработка сценариев приостановлена на время переноса указателей загружаемого сохранения.
- Протокол 39: обновите всех игроков и сервер. Личные профили версии 5 сохраняются.

## English — 0.2.28 fixes

- Photo Opportunity now replicates the attached camera's vertical limits and initial viewing sector. Guests control their own cameras; nearby photographers do not cover the lens.
- Scripted vehicle visibility is replicated, including the photographic camera's hidden support. Guest photographs carry their zoom level; photograph evidence is consumed once, matching single-player behavior.
- Per-frame control-enable commands no longer create another network barrier every frame. Readiness excludes unrelated protected vehicle/ped replicas and reannounces missing dependencies.
- Fixed overlapping relocation/camera movement holds. A later host control-enable transition reaches guests after the scene ends.
- Distant actors in scripted camera shots receive their gait and action animations; position samples are smoothed on the sender's timeline.
- Separated the horn, regular siren and fast siren. Siren state no longer becomes a sustained horn; playback retries after a temporary sound-channel allocation failure.
- Removed unnecessary synchronous cellphone-model loading when a replica puts the phone away.
- Deferred damage events from a guest replica no longer charge the local player. Police-car theft uses the hijacker's player context.
- Asynchronous save validation no longer deletes the current frame's render objects. Script execution is held while loaded save pointers are being relocated.
- Protocol 39 requires every client and the server to update. Version 5 personal profiles are retained.

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`, сохранив настройки и `coop/server-data`. При необходимости установите ASI-загрузчик через `install.bat`; меню мода — F9. Отдельный `coop.dll` не нужен.

Extract the ZIP beside `gta_sa.exe`, preserving settings and `coop/server-data`. Run `install.bat` if an ASI loader is needed; open F9 in-game. A separate `coop.dll` is not required.

**Протокол 39: обновите всех игроков и сервер. / Protocol 39: update every client and the server.**

[Полная история изменений / Complete changelog](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md).

Разработка продолжается; полное прохождение сюжета и произвольные сочетания сторонних модов требуют дальнейших проверок.

Development continues; full campaign playthroughs and arbitrary third-party mod combinations require further testing.

SHA-256 — `SHA256SUMS.txt`. Игровые файлы и личные материалы не включены. / Game files and personal materials are not included.
