# reSA-Coop 0.2.31-3p

Кооператив на троих для GTA San Andreas 1.0 US. / Three-player co-op for GTA San Andreas 1.0 US.

**Готовая сборка / Compiled build: `reSA-Coop-v0.2.31-3p-win32.zip` в Assets / in Assets.**

## Русский

- Исправлена синхронизация состава и окраски миссионных поездов. Ящики, NPC и прикреплённая техника движутся вместе с нужным вагоном, в том числе при смене направления и повторном создании копии.
- Убрано долгое ожидание поезда из-за выгрузки локомотива во время загрузки вагонов. Запоздавшие события удалённого состава отбрасываются вместе с его прежними привязками.
- Вспомогательные сюжетные сценарии передают гостям реплики, субтитры, изменения карты, переносы и выдачу оружия. Привязка к общей миссии очищается при завершении или замене сценария.
- Сюжетные предметы из вспомогательных сценариев появляются у гостей. Предметы спортзала и других личных занятий остаются локальными; их тексты сохраняются во время общих реплик.
- Временный сюжетный боезапас отделён от личных патронов. После эпизода возвращаются собственное оружие и оставшиеся личные боеприпасы; подобранные патроны и постоянные сюжетные награды сохраняются.
- Исправлена утечка дополнительных 30 000 патронов, которые GTA выдаёт при прикреплении стрелка. Посадка и отсоединение больше не превращают этот запас в личные боеприпасы гостя.
- После выхода из режима стрелка вновь выбирается личное оружие, которым игрок пользовался перед эпизодом.
- Выполнение заданий медика, полицейского и пожарного и их постоянные бонусы сохраняются в личном профиле после переподключения. Новый гость не получает эти достижения из сохранения хоста.
- В Deconstruction касания объектов машиной гостя учитываются сюжетными проверками с её настоящей скоростью, массой и моделью.
- Гостевые самолёты удерживают положение во время обязательной сцены, затем получают управление и скорость полёта. Самостоятельный выход из выданного транспорта сохраняется.
- Исправлена остановка переноса, когда гость уже прикреплён к нужному летящему объекту: переход завершается без ожидания земли под ним.
- Исправлены места гостевых стрелков вертолёта: тело, камера и точка выстрела располагаются снаружи корпуса с обзором в нужную сторону. Для прикреплённых стрелков применяются заданные сценарием направление и диапазон обзора.
- В New Model Army гости получают свои RC-вертолёты с магнитами и могут переносить общие грузы. После вступительной камеры возвращаются собственный обзор и управление; один груз удерживает только один вертолёт.
- Сюжетное затемнение из End of the Line передаётся гостям и снимается вместе со сценой. При выходе из её области восстанавливаются личные эффекты изображения.
- Устранён конфликт обработки полиции и синхронизации спуска спецназа. При входе хоста в интерьер приземлившийся боец продолжает преследование нужного игрока снаружи.
- Личные профили сохраняются в формате 6; поддерживается загрузка профилей предыдущей версии.

Обновите мод у хоста, гостей и сервера.

Основано на [SA:COOP](https://github.com/Karimov-Akbar/sa-coop) Karimov-Akbar. Доработки reSA-Coop — opiahex. Лицензия MIT.

## English

- Fixed mission train consist and colour replication. Crates, NPCs and attached vehicles stay with the correct carriage, including direction changes and replica recreation.
- Fixed long train creation waits caused by the locomotive model unloading while carriages were loading. Delayed events from a retired consist cannot restore its old attachments.
- Mission helper scripts forward dialogue, captions, map changes, relocations and weapon grants to guests. Their mission association ends when the script terminates or its slot is reused.
- Story pickups created by helper scripts appear for guests. Gym and other personal-activity pickups remain local, and their text remains available during shared dialogue.
- Temporary mission supply is tracked separately from personal ammunition. Personal weapons and remaining ammunition return after the episode; collected ammunition and permanent story awards are retained.
- Fixed the additional 30,000 rounds granted by GTA when attaching a gunner leaking into personal ammunition. Attaching and detaching no longer converts that supply into the guest's own rounds.
- Leaving a gunner role reselects the personal weapon that was active before the episode.
- Paramedic, Vigilante and Firefighter completion and permanent benefits survive reconnects in each player's profile. New guests do not inherit those achievements from the host's save.
- In Deconstruction, a guest vehicle's contact with objects is evaluated using its actual speed, mass and model.
- Guest aircraft hold their position during mandatory scenes, then receive flight control and velocity. Leaving the supplied vehicle remains the player's choice.
- Relocation completes when the guest is already attached to the correct airborne object, instead of waiting for ground below it.
- Corrected guest helicopter gunner positions: the body, camera and shot origin sit outside the hull with the view facing the intended direction. Attached gunners also receive the script's view direction and range.
- In New Model Army, guests receive their own RC helicopters and magnets and can move shared cargo. Their own view and controls return after the fixed intro; each item can be held by only one helicopter.
- Guests receive the scripted darkness effect used in End of the Line, including its removal. Leaving the scene's area restores personal visual effects.
- Resolved a conflict between police processing and SWAT descent synchronization. When the host enters an interior, a landed officer continues pursuing the correct player outside.
- Personal profiles use format 6 and retain support for profiles from the previous version. Update all clients and the server together.

Based on [SA:COOP](https://github.com/Karimov-Akbar/sa-coop) by Karimov-Akbar. reSA-Coop development by opiahex. MIT license.

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`, сохранив настройки и `coop/server-data`. При необходимости установите ASI-загрузчик через `install.bat`; меню мода — F9. Отдельный `coop.dll` не нужен.

Extract the ZIP beside `gta_sa.exe`, preserving settings and `coop/server-data`. Run `install.bat` if an ASI loader is needed; open F9 in-game. A separate `coop.dll` is not required.

**Протокол 42: обновите всех игроков и сервер. / Protocol 42: update every client and the server.**

[Полная история изменений / Complete changelog](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md).

Разработка продолжается; полное прохождение сюжета и произвольные сочетания сторонних модов требуют дальнейших проверок.

Development continues; full campaign playthroughs and arbitrary third-party mod combinations require further testing.

SHA-256 — `SHA256SUMS.txt`. Игровые файлы и личные материалы не включены. / Game files and personal materials are not included.
