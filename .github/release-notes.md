# reSA-Coop 0.2.29-3p

Кооператив на троих для GTA San Andreas 1.0 US. / Three-player co-op for GTA San Andreas 1.0 US.

**Скачайте `reSA-Coop-v0.2.29-3p-win32.zip` из Assets. / Download the compiled `reSA-Coop-v0.2.29-3p-win32.zip` from Assets.**

## Русский — исправления 0.2.29

- Сетевые копии игроков больше не ограничиваются штатным локальным кооперативом GTA на расстоянии около 20 метров. Для них отключено преобразование прицеливания по осям второго локального игрока, мешавшее приседу и перекатам.
- Счётчики сюжетного прохождения и общего прогресса передаются от хоста и сохраняются при восстановлении личного профиля гостя. Деньги, навыки и личные результаты школ остаются индивидуальными.
- Передаются выстрелы полицейского вертолёта: точный транспорт-источник, траектория, попадание и звук. Повторные события отбрасываются; короткое ожидание загрузки копии защищено поколением сущности.
- Синхронизируются спуск спецназа, его анимация и форма троса. Копия получает движение владельца без самостоятельного падения и дополнительной физической верёвки.
- Добавлена передача бильярдных шаров от владельца занятого стола с интерполяцией движения. Сетевые копии не публикуют собственные дубликаты шаров.
- Передаются сюжетные изменения видимости и коллизии моделей карты, замены ближайшей модели и запросы IPL. Сохраняется точная точка исходного объекта: повторная доставка замены не переключается на соседнее здание. Изменения повторно применяются после загрузки области и переподключения.
- Близкие гости в той же области получают правила полиции текущей сюжетной сцены и войны банд хоста. Удалённые участники и личные занятия в школах сохраняют свои правила.
- Дополнена защита от начисления преступлений локальному игроку за сетевую копию другого участника: наезд, повреждение транспорта, сбивание с байка и уклонение NPC от машины.
- Пустые сетевые полицейские машины больше не удерживают звезду розыска своим техническим статусом физики. Живые экипажи продолжают учитываться.
- Штатная очистка при возрождении сохраняет сетевой транспорт и живых NPC рядом с другим игроком. Сценарное удаление сущностей работает отдельно; удалённые сетевые ID не восстанавливаются заново.
- Копия поезда создаётся на конкретном узле переданной железнодорожной ветки, включая параллельные пути.
- Добавлено угловое предсказание вращения транспорта между отсчётами. Изменение здоровья стоящей машины отправляется с активной частотой; момент взрыва горящей копии определяет владелец.
- Сценарное плавное удаление NPC больше не блокируется удержанием населения рядом с напарником. Ускоренное заполнение населения при разделении зон выполняется только за затемнением.
- Протокол 40: обновите всех игроков и сервер. Формат личных профилей остаётся версией 5.

## English — 0.2.29 fixes

- Network player replicas bypass GTA's native local-co-op distance leash at roughly 20 metres. They also bypass the second local player's aim-axis conversion that interfered with crouched aiming and rolls.
- Host campaign counters and overall progress replicate to guests and survive restoration of an older personal profile. Money, skills and personal school records remain individual.
- Police-helicopter rounds replicate their exact source vehicle, trajectory, hit and audio. Duplicate events are rejected; a short native-instance loading wait retains entity-generation checks.
- SWAT descent, animation and rope shape replicate from their simulator. Replicas follow the authoritative descent without falling independently or allocating another physical rope.
- Billiard balls replicate from the player holding the table's interaction lease, with motion interpolation. Replicas do not publish duplicate balls.
- Story map visibility/collision changes, nearest-model swaps and IPL requests replicate. Edits retain the original target's exact point so repeated swaps do not spread to neighbouring buildings. Edits retry after area streaming and reconnects.
- Nearby guests in the same area receive police rules for the host's current story scene and gang war. Distant participants and personal school activities retain their own rules.
- Extended protection against assigning another player's replica crimes to the local player: hit-and-run, vehicle damage, knocking a rider off a bike and NPC evasive dives.
- Empty police-vehicle replicas no longer hold a wanted star because of their technical physics status. Live police crews continue to count.
- Native resurrection cleanup retains network vehicles and living NPCs near another participant. Scripted deletion remains separate; retired entity IDs are not resurrected.
- Train replicas start on an explicit node of their transmitted railway track, including parallel tracks.
- Added angular prediction between vehicle samples. Stationary vehicle health changes use the active update rate; the simulator determines when a burning replica explodes.
- Scripted NPC fade-out is no longer blocked by nearby population retention. Accelerated population filling after areas separate runs only behind a fade.
- Protocol 40 requires every client and the server to update. Personal profiles retain format version 5.

Переносы и адаптации основаны на [изменениях upstream 95d336a](https://github.com/Karimov-Akbar/sa-coop/commit/95d336a), с сохранением собственных механизмов авторитета, поколений сущностей и синхронизации трёх игроков.

Ports and adaptations build on [upstream changes 95d336a](https://github.com/Karimov-Akbar/sa-coop/commit/95d336a), retaining this fork's authority, entity-generation and three-player synchronization mechanisms.

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`, сохранив настройки и `coop/server-data`. При необходимости установите ASI-загрузчик через `install.bat`; меню мода — F9. Отдельный `coop.dll` не нужен.

Extract the ZIP beside `gta_sa.exe`, preserving settings and `coop/server-data`. Run `install.bat` if an ASI loader is needed; open F9 in-game. A separate `coop.dll` is not required.

**Протокол 40: обновите всех игроков и сервер. / Protocol 40: update every client and the server.**

[Полная история изменений / Complete changelog](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md).

Разработка продолжается; полное прохождение сюжета и произвольные сочетания сторонних модов требуют дальнейших проверок.

Development continues; full campaign playthroughs and arbitrary third-party mod combinations require further testing.

SHA-256 — `SHA256SUMS.txt`. Игровые файлы и личные материалы не включены. / Game files and personal materials are not included.
