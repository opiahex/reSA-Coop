# reSA-Coop 0.2.26-3p

Кооператив на троих для GTA San Andreas 1.0 US. / Three-player co-op for GTA San Andreas 1.0 US.

**Скачайте `reSA-Coop-v0.2.26-3p-win32.zip` из Assets. / Download the compiled `reSA-Coop-v0.2.26-3p-win32.zip` from Assets.** Архивы Source code не устанавливают мод. / Source-code archives are not the installable mod.

## Русский — исправления 0.2.26

- Исправлен бесконечный гудок сетевых машин: нативный флаг прошлого воспроизведения больше не затирается перед остановкой аудиоканала. Исправление относится и к машинам трафика.
- Розыск гостя теперь используется в нативных решениях общих полицейских, а не только в звёздах над игроком. Сохраняются личный розыск и сценарные запреты на преследование.
- Полицейские выбирают разыскиваемого участника; ближайший невиновный игрок не становится целью только из-за расстояния. При завершении преследования очищаются списки нужного игрока.
- Выбор полицейского транспорта и его управление учитывают разыскиваемого гостя в общей зоне населения.
- Фильтр школьных столкновений распознаёт водителя до регистрации машины и завершения передачи владения.
- Контакт между участниками школы не записывается как отложенное повреждение кузова и не пересылается повторно сетевым импульсом. Вне активного школьного сценария действует штатный урон.
- Протокол 37 передаёт также личные флаги игнорирования полицией. Обновить нужно хоста, гостей и сервер вместе.

## English — 0.2.26 fixes

- Fixed endlessly sounding horns on network vehicles: native previous-playback state is no longer overwritten before the audio channel can stop. This also fixes traffic vehicle replicas.
- Guest wanted levels now feed native shared-police decisions instead of only the stars above their name tags. Personal wanted levels and scripted police back-off remain separate.
- Police select a wanted participant; an innocent player is no longer selected merely for being closer. Pursuit cleanup removes the cop from the correct player's lists.
- Shared police vehicle selection and steering take nearby wanted guests into account.
- School contact filtering recognises the driver before vehicle registration and ownership handover finish.
- Contact between school participants no longer records deferred body damage or forwards another network impulse. Native damage applies outside an active school script.
- Protocol 37 also carries personal police back-off flags. The host, guests and server must all update together.

<details>
<summary>Включённые изменения 0.2.25 / Included 0.2.25 changes</summary>

### Русский

- Исправлен порядок группового перехода в грузовой отсек Stowaway: учитываются положение транспорта, завершение записанного движения, загруженная коллизия и безопасный пол.
- Исправлено сохранение блокировки управления гостя после сценарной камеры и переноса между областями.
- При сценарных переносах обновляется граница этапа. Запоздавшие действия, фотографии, повреждения и перенос предметов прежнего этапа отбрасываются.
- Продолжение сюжетного скрипта после действия гостя больше не обрывается по одному пятисекундному таймеру; учитываются завершение сценария, участник и поколение сущности.
- Восстановление профиля после переподключения согласовано с текущими миром, миссией, этапом, транспортом и готовностью сцены. Несовместимое старое восстановление отменяется целиком.
- Разделены восстановление личного снаряжения и повторное назначение постановочной роли. Восстановление гостевого транспорта отменяется, если исходная машина или этап уже завершились.
- Добавлена передача нативного состояния джетпака: тяга, наклон, положение ног, стрельба и эффекты. Удалённая физика не добавляет второе ускорение поверх сетевой скорости.
- Во время сюжетной выдачи джетпака гости получают собственные доступные поблизости пикапы. Подбор одним участником не забирает снаряжение у остальных.
- В N.O.E. гости получают дополнительные припаркованные Rustler на площадке перед сюжетным самолётом и самостоятельно выбирают, когда садиться. Новые копии этого самолёта в воздухе не создаются.
- Гудки передаются отдельными событиями включения/выключения, включая короткие нажатия, с проверкой владельца и поколения машины. Диагностика фиксирует выделение звука и запуск аудиоканала.
- Расширена передача водомётов пожарной машины и SWAT: положение струи привязано к текущей машине, начало/конец поливания передаются надёжно, воздействие на сетевые копии не применяется повторно.
- Во время нативной катсцены обычные пакеты движения и анимации игрока не заменяют её позы. Сценарные машины корректируются после локального шага физики.
- Миссионные NPC сохраняются в более широком диапазоне постановочной камеры; учитываются пассажиры уже существующей машины.
- Добавлены данные контакта рук и согласование относительного движения нативных пар прохожих. Учитываются мировая точка контакта и временная задача обхода препятствия.
- Исправлено условие снятия транспорта с синхронизации после передачи: учитываются близость нового владельца и защитный интервал. Это закрывает путь повторного назначения ID стоящей машине, когда остальные игроки находятся далеко или в школе/интерьере.
- После отключения владельца сохраняется ID существующей машины и незавершённой загрузки её модели; один выбранный преемник публикует состояние.
- Зоны населения на разной высоте больше не объединяются только из-за совпадения координат на карте.
- Диагностика транспорта дополнена нативным идентификатором, поколением, сроком защиты после передачи и данными наследования. Ожидание сюжетного этапа записывает конкретного участника и отсутствующую зависимость.

### English

- Fixed the order of the shared Stowaway cargo-hold transition, including vehicle position, recorded movement completion, loaded collision and safe floor placement.
- Fixed guest controls remaining locked after a scripted camera or area transition.
- Scripted relocations advance the stage boundary; late actions, photos, damage and cargo events from the previous stage are discarded.
- Guest story-action bindings no longer expire on a single five-second timer; script completion, participant identity and entity generation determine their lifetime.
- Reconnect profile recovery is coordinated with the current world, mission, stage, vehicle and scene readiness. Incompatible old recovery is cancelled as a whole.
- Personal equipment recovery is separate from scripted role assignment. Guest vehicle recovery stops when its source vehicle or stage has ended.
- Added native jetpack thrust, tilt, leg pose, firing and effects replication. Remote physics does not apply a second acceleration over network velocity.
- Story jetpack handouts provide separate nearby pickups for guests. One participant collecting theirs does not remove another's equipment.
- N.O.E. provides additional parked Rustlers in front of the story aircraft. Guests choose when to board; new copies are not created in the air.
- Horn on/off events preserve short taps and validate vehicle owner/generation. Diagnostics record sound allocation and channel playback.
- Firetruck and SWAT water cannons replicate nozzle position relative to the moving vehicle and reliable start/stop events, without double-applying effects to replicas.
- Regular movement/animation packets no longer overwrite native cutscene player poses. Scripted vehicles are corrected after the local physics step.
- Mission NPC relevance covers wider scripted camera views, including passengers of an existing vehicle.
- Native pedestrian couples replicate hand contact and relative movement, including world-space contact points and temporary avoidance tasks.
- Vehicle retirement after handover checks the new owner's proximity and a protection interval, preventing a parked vehicle from receiving another ID near schools/interiors.
- Owner disconnection retains an existing vehicle's ID and pending model load; one elected successor publishes its state.
- Population zones at different heights no longer merge merely because their map coordinates match.
- Diagnostics include native vehicle handles, entity generations, handover protection and inheritance. Story-stage waits identify the participant and missing dependency.

</details>

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`, сохранив свои настройки и `coop/server-data`. При необходимости установите ASI-загрузчик через `install.bat`; меню мода — F9. Отдельный `coop.dll` не нужен.

Extract the ZIP beside `gta_sa.exe`, preserving your settings and `coop/server-data`. Run `install.bat` if an ASI loader is needed; open F9 in-game. A separate `coop.dll` is not required.

**Протокол 37: обновите всех игроков и сервер. / Protocol 37: update every client and the server.**

[Полная накопленная история / Complete cumulative history](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md).

Разработка продолжается; весь сюжет и любые сочетания сторонних модов не проверены без исключений. Произвольные подэтапы SCM, любой переносимый груз при переподключении и все NPC вне загруженного мира не объявляются полностью поддержанными.

Development continues; the entire campaign and arbitrary third-party mod combinations are not certified bug-free. Arbitrary SCM substages, every carried-object reconnect case and every NPC outside the loaded world are not claimed as fully supported.

SHA-256 — `SHA256SUMS.txt`. Игровые файлы и личные материалы не включены. / Game files and personal materials are not included.
