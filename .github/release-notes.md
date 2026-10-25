# reSA-Coop 0.2.30-3p

Кооператив на троих для GTA San Andreas 1.0 US. / Three-player co-op for GTA San Andreas 1.0 US.

**Готовая сборка / Compiled build: `reSA-Coop-v0.2.30-3p-win32.zip` в Assets / in Assets.**

## Русский — исправления 0.2.30

- Ускорено создание сетевых копий: миссионный транспорт, используемые игроками машины, экипажи и ближайшие участники боя получают приоритет. Причины ожидания моделей, мест и пулов выводятся в диагностику.
- Загрузка сетевых моделей удерживается до создания копии, включая полицейские вертолёты и спецназ у гостей без розыска. Временные удержания моделей и текстур освобождаются; обслуживание загрузки не зависит от отрисовки окна.
- Попадания полицейского вертолёта определяет его симулятор. Передаются конкретная цель, часть тела, урон и поколение сущности; наблюдатели не пересчитывают попадание независимо. Учитываются задержка доставки, удаление вертолёта и переподключение цели.
- Исправлен порядок обновления гостевых красных маркеров: GTA успевает активировать их перед отрисовкой. Готовность перехода проверяет IPL нужной точки; явно удалённые части карты не загружаются заново.
- Убраны прежние пределы 512 изменений карты и 128 IPL. Позднее подключение получает полный снимок актуальных изменений; их ревизия, IPL и заменённые модели входят в готовность сюжетного этапа и катсцены.
- Общие правила полиции включаются ближе 280 метров и отпускаются дальше 340 метров. Личные звёзды розыска остаются индивидуальными.
- Для спуска спецназа сохраняется найденная высота земли при уходе владельца в интерьер. После приземления обычный боец передаётся игре, которая продолжает загружать его район; копии не создают собственную физическую верёвку.
- Бильярд передаёт 16 игровых шаров, исключая вспомогательные объекты прицела и счёта. Забитые шары, завершение и повторный запуск не оставляют прежние сетевые копии.
- Проверены присед и оба переката с несколькими типами оружия и направлениями. Передача плавания исправлена по реальным полям задачи GTA: наклон, крен, торс и направление; запас воздуха приходит от владельца игрока.
- Урон от утопления сохраняет дробную часть между кадрами: высокий FPS больше не округляет каждое небольшое повреждение в ноль. Затопление машины, остановка двигателя и состояния пассажиров передаются согласованно.
- Исправлены пропадающие субтитры гостей при отключённых субтитрах у хоста. Настройка каждого игрока действует независимо; поддерживаются строки с подстановкой чисел и текста.
- Субтитр связанной реплики ждёт фактического начала звука у гостя и получает своё время показа. Завершённые реплики не запускаются повторно при восстановлении состояния.
- Подключение посреди катсцены восстанавливает текущее время, камеру, анимации и положение звуковой дорожки. Периодическая коррекция ограничивает накопление расхождения часов.
- Общие сюжетные переводы загружаются отдельно от местной таблицы гостя: спортзал, магазин и другие личные занятия сохраняют свои надписи.
- Разрушенные обычные объекты сохраняют состояние до смены мира. Для подвижных механизмов карты добавлены выбор симулятора, передача положения, поворота, видимости и коллизии, интерполяция и восстановление при смене источника.
- Очередь одноразовых звуков отделена от состояния сюжетной дорожки. При перегрузке отбрасываются устаревшие эффекты, а команда остановки сохраняется.
- Управляемые ракеты и тепловые ловушки следуют полёту исходного симулятора. Передаются смена и потеря цели, завершение полёта; поздний запуск после уже полученного удаления отбрасывается.
- Гараж получает финальное состояние машины от её владельца, включая последний ремонт и тюнинг. Повторные запросы не занимают второй слот; штатное сохранение не очищает уже переданную гостем машину.
- Усилена пересборка внешности при одновременной загрузке множества моделей: одежда дожидается завершения обоих каналов загрузки перед использованием геометрии.
- Протокол 41: обновите всех игроков и сервер. Личные профили сохраняют формат 5.

## English — 0.2.30 fixes

- Prioritized replica creation for mission vehicles, player-used cars, crews and nearby combatants. Diagnostics identify model, seat and native pool waits.
- Network model requests remain retained until replicas can be created, including police helicopters and SWAT when the observer has no wanted level. Temporary model/texture leases are released; background loading no longer depends on window rendering.
- Police-helicopter simulators resolve hits once and transmit the target, body part, damage and entity generation. Observers do not independently raycast damage. Delivery delay, helicopter removal and target reconnects are fenced.
- Guest red markers are placed before GTA activates markers for rendering. Transition readiness checks IPLs at the destination without reloading explicitly removed map sections.
- Removed the previous 512-map-edit and 128-IPL limits. Late joiners receive a complete current snapshot; its revision, IPLs and replacement models participate in story-stage and cutscene readiness.
- Shared police rules enter within 280 metres and release beyond 340 metres. Personal wanted levels remain individual.
- SWAT descent retains a known ground height when its simulator enters an interior. After landing, an ambient officer moves to the simulator still loading that area; observers do not allocate additional physical ropes.
- Billiards replicates the 16 physical game balls, excluding aiming/score interface objects. Pocketed balls, exit and restart remove obsolete replicas.
- Checked crouching and both rolls with several weapons and directions. Swimming now transmits the correct native task fields for body pitch, roll, torso and heading; oxygen comes from the owning player.
- Drowning damage retains fractional damage between frames, preventing high FPS from rounding every small hit to zero. Vehicle submersion, engine shutdown and passenger states replicate consistently.
- Guest subtitles work even when the host disables subtitles. Each participant retains their own preference; numeric and string substitutions are supported.
- Captions associated with a voice line wait for actual guest playback and receive their own display duration. Finished lines are not replayed during state recovery.
- Joining a running cutscene restores its current time, camera, animation and soundtrack position. Periodic clock correction limits accumulated drift.
- Shared story translations use an independent text table, preserving local gym, shop and other activity captions.
- Broken world objects retain their state until a world reset. Moving map mechanisms receive simulator election, transform/visibility/collision replication, interpolation and source-change recovery.
- One-shot sound backlog is separate from desired soundtrack state. Overload may discard stale effects while retaining STOP commands.
- Guided missiles and flares follow their original simulator's trajectory, including target changes, lock loss and retirement. A delayed launch cannot revive an already retired projectile.
- Garages receive final vehicle state from its owner, including the latest repair/tuning. Repeated requests reuse one slot; native saving preserves cars already transferred by guests.
- Clothing rebuilds finish both streaming channels before using part geometry during concurrent model loading.
- Protocol 41 requires updating all clients and the server. Personal profiles retain format 5.

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`, сохранив настройки и `coop/server-data`. При необходимости установите ASI-загрузчик через `install.bat`; меню мода — F9. Отдельный `coop.dll` не нужен.

Extract the ZIP beside `gta_sa.exe`, preserving settings and `coop/server-data`. Run `install.bat` if an ASI loader is needed; open F9 in-game. A separate `coop.dll` is not required.

**Протокол 41: обновите всех игроков и сервер. / Protocol 41: update every client and the server.**

[Полная история изменений / Complete changelog](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md).

Разработка продолжается; полное прохождение сюжета и произвольные сочетания сторонних модов требуют дальнейших проверок.

Development continues; full campaign playthroughs and arbitrary third-party mod combinations require further testing.

SHA-256 — `SHA256SUMS.txt`. Игровые файлы и личные материалы не включены. / Game files and personal materials are not included.
