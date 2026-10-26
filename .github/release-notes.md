# reSA-Coop 0.2.32-3p

Кооператив на троих для GTA San Andreas 1.0 US. / Three-player co-op for GTA San Andreas 1.0 US.

**Готовая сборка / Compiled build: `reSA-Coop-v0.2.32-3p-win32.zip` в Assets / in Assets.**

## Русский

Исправления сюжетных действий гостей, транспорта, огня и личного прогресса. **Протокол 43: обновите всех игроков и сервер вместе.** Личные профили переведены в формат 7; старые профили поддерживаются.

- Исправлен вылет при появлении осколков стекла с VehDeform. Перехват крепления объектов больше не нарушает вычисление адреса камеры в GTA San Andreas 1.0 US. [#1](https://github.com/opiahex/reSA-Coop/issues/1)
- В The Da Nang Thang сюжетное снятие оружия применяется и к личному инвентарю гостей. Переход от вертолётного стрелка к воде, ножу и плаванию возвращает управление.
- Исправлено восстановление камеры прикреплённого стрелка после сюжетной выдачи оружия.
- В салоне Freefall гости получают корректное положение относительно самолёта и направление обзора при смене этапа. Самостоятельный выход гостя сохраняется.
- Миссионный поезд продолжает движение после ухода владельца и восстанавливается при его переподключении без второго состава. Груз, NPC и техника получают свои прежние вагоны.
- Настоящий таран машиной гостя учитывается сюжетными проверками, в том числе в Intensive Care. Следующие действия NPC сохраняют связь с машиной, которая нанесла урон.
- Выстрелы гостевого RC передают свой источник: ответное действие курьера направляется на атакующий самолётик.
- В T-Bone Mendez добавлена обработка гостевой кнопки и анимации захвата пакета с мотоцикла.
- Миссионные ящики сохраняют заданные массу, инерцию и заморозку при синхронизации. После сброса с вил погрузчика сетевая копия больше не подменяет эти свойства стандартными.
- Ворота аэропорта учитывают допуск и положение гостей, включая временный сюжетный доступ Mike Toreno. Ворота согласованно открываются и закрываются и не закрывают проход стоящему в нём игроку.
- В Supply Lines гостевые RC получают отдельный конечный запас топлива и штатную проверку возврата. Смена этапа не заправляет самолётик заново; после завершения роли возвращаются камера и управление персонажем.
- Подтверждённые убийства гостя могут начать одну общую войну банд в его районе. Первая волна противников передаётся остальным участникам.
- Исправлены пропуски синхронизации Молотова и обычного огня. Огонь в другой области не занимает местный пул; старое событие тушения не гасит новый огонь, занявший тот же слот.
- Парные NPC используют общие часы движения, что уменьшает расхождение их взаимного положения у наблюдателей.
- Скрытный удар ножом дожидается нужной сетевой копии и оружия; результат передаётся от владельца жертвы.
- Исправлен вылет при создании вооружённых NPC до завершения загрузки оружейных анимаций.
- Личные результаты лётной, мото- и лодочной школ сохраняются после переподключения. Штатные одноразовые награды сохраняются вместе с результатами и не выдаются повторно.
- Уточнён учёт виновника преступления при уроне и угоне полицейской машины внутри обработки другого игрока.
- После паузы загрузки сеть отбрасывает устаревшие положения и голос, сохраняя свежие состояния и порядок надёжных событий. Данные закрытого соединения не применяются после переподключения; причины переполнения очередей отражаются в сообщении и журнале.

SA:COOP — [Karimov-Akbar](https://github.com/Karimov-Akbar/sa-coop). Разработка reSA-Coop — [opiahex](https://github.com/opiahex).

## English

Fixes for guest mission actions, vehicles, fire and personal progress. **Protocol 43: update every player and the server together.** Personal profiles use format 7 and retain support for older profiles.

- Fixed a crash when VehDeform creates glass particles. The attachment hook no longer corrupts the camera-pointer calculation in GTA San Andreas 1.0 US. [#1](https://github.com/opiahex/reSA-Coop/issues/1)
- In The Da Nang Thang, scripted weapon removal also clears guests' personal weapons. The transition from helicopter gunner to the water, knife and swimming restores control.
- Fixed attached-gunner camera restoration after a scripted weapon grant.
- Guests in Freefall's cabin receive the correct position relative to the aircraft and the intended view direction when stages change. Guests can still leave independently.
- Mission trains continue under a new simulator when their owner leaves. Rejoining restores the same consist and its cargo, NPC and vehicle attachments without creating a second train.
- Actual impacts from a guest's vehicle count in mission checks, including Intensive Care. Subsequent NPC actions retain the vehicle that caused the damage.
- Guest RC gunfire carries its actual source, so a courier's retaliation targets the attacking aircraft.
- T-Bone Mendez now handles a guest's button press and animation when snatching a package from a motorcycle.
- Mission crates retain their assigned mass, inertia and frozen state during synchronization. Dropping a crate from a forklift no longer replaces those properties with defaults on a replica.
- Airport gates consider guests' access and position, including Mike Toreno's temporary mission access. Gates open and close consistently and stay open for a player standing in the doorway.
- Guest RC aircraft in Supply Lines have their own finite fuel supply and native return check. Stage changes do not refill the tank; ending the role restores the character's camera and controls.
- Confirmed guest kills can start one shared gang war in the guest's territory. The first enemy wave is replicated to the other participants.
- Fixed missing Molotov and regular-fire synchronization paths. Fire in another area does not occupy the local pool, and an old extinguish event cannot stop a new fire that reused its slot.
- Paired NPCs share a movement clock, reducing disagreement in their relative positions on observers.
- Stealth knife attacks wait for the required replica and weapon; the victim's owner replicates the result.
- Fixed a crash when armed NPCs were created before their weapon animations had finished loading.
- Personal flight, bike and boat school records survive reconnects. Native one-time rewards are stored with the records and are not awarded again.
- Corrected offender selection for damage crimes and police-car theft while another player's processing context is active.
- After a loading pause, networking discards expired movement and voice while preserving fresh states and reliable-event order. Closed-connection data cannot apply after reconnecting; queue-overflow reasons appear in the message and log.

SA:COOP by [Karimov-Akbar](https://github.com/Karimov-Akbar/sa-coop). reSA-Coop development by [opiahex](https://github.com/opiahex).

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`, сохранив настройки и `coop/server-data`. При необходимости установите ASI-загрузчик через `install.bat`; меню мода — F9. Отдельный `coop.dll` не нужен.

Extract the ZIP beside `gta_sa.exe`, preserving settings and `coop/server-data`. Run `install.bat` if an ASI loader is needed; open F9 in-game. A separate `coop.dll` is not required.

**Протокол 43: обновите всех игроков и сервер. / Protocol 43: update every client and the server.**

[Полная история изменений / Complete changelog](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md).

Разработка продолжается; полное прохождение сюжета и произвольные сочетания сторонних модов требуют дальнейших проверок.

Development continues; full campaign playthroughs and arbitrary third-party mod combinations require further testing.

Лицензия / License — MIT. SHA-256 — `SHA256SUMS.txt`.
