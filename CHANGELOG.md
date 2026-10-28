# reSA-Coop — накопленные изменения

Первая публикация форка объединяет локальные версии **0.2.6-3p–0.2.24-3p** и перенесённые исправления SA:COOP.
Это перечень итоговых изменений по подсистемам: промежуточные экспериментальные решения, которые были заменены, отдельными возможностями не считаются.
Текущая база — **0.2.36-3p, протокол 45**. Все участники и сервер должны обновляться вместе.

## 0.2.36-3p

### Русский

- Добавлены отдельные условия атрибуции оригинальной диагностической системы и ссылка на Telegram-группу в README.

Диагностическое меню для хоста и двух гостей: Network, Entities, Events, Hooks / SCM и Performance.

- Снимки игровых данных обновляются раз в 250 мс; недоступные значения отмечены явно.
- Реестр хранит сведения об установке хуков и патчей. Inspector отдельно показывает сохранённые байты установки и текущее чтение памяти.
- Ограниченный Flight Recorder записывает поддерживаемые события в ротационные JSONL-файлы, показывает счётчики потерь и сохраняет crash-tail.
- Для HP пассажира добавлена диагностика от отправителя через сервер до применения у получателя.
- Сохранены существующее меню F9, MIT и авторство Karimov-Akbar и opiahex.

Версия клиента и сервера: 0.2.36-3p; протокол 45. Хост, сервер и оба гостя должны обновляться вместе; версия 0.2.35 использует протокол 44.

Ограничения: записываются только инструментированные события; полного трассирования SCM и всех сетевых сообщений пока нет. Запись включается через COOP_FLIGHT_RECORDER=1. Кнопок управления записью и полного экспорта в меню пока нет. Сохраняются известные игровые проблемы с NPC, транспортом и отдельными этапами Photo Opportunity. Этот выпуск не означает, что они все устранены.

### English

- Added attribution terms for the original diagnostic system and a Telegram group link in the README.

Diagnostic menu for one host and two guests: Network, Entities, Events, Hooks / SCM, and Performance.

- Game snapshots refresh every 250 ms; unavailable values are identified explicitly.
- The registry records hook and patch installation metadata. Inspector separates historical installation bytes from a current memory sample.
- A bounded Flight Recorder writes supported events to rolling JSONL files, exposes loss counters, and preserves a crash tail.
- Passenger HP diagnostics follow publication, server relay, receipt, and native application.
- The existing F9 menu, MIT license, and Karimov-Akbar and opiahex credits are preserved.

Client/server version: 0.2.36-3p; protocol 45. Update the host, server, and both guests together; 0.2.35 uses protocol 44.

Limitations: only instrumented events are recorded; full SCM and all-packet tracing are not implemented. Enable recording with COOP_FLIGHT_RECORDER=1. In-menu recording controls and full export are not implemented. Known NPC, vehicle, and Photo Opportunity issues remain; this release does not claim to resolve all of them.

## 0.2.35-3p

### Русский

**Протокол 44: обновите всех игроков и сервер вместе.** Формат личных профилей 7 сохранён. При обновлении сохраняйте настройки, сохранения и `coop/server-data`.

- Исправлен вылет при возрождении напарника после удаления его прежнего персонажа.
- Исправлены конфликты мест с NPC. Запоздалое решение о посадке больше не возвращает игрока в прежнюю машину после выхода или пересадки.
- Гость сохраняет управление своей машиной, когда сценарий принимает её в общий миссионный транспорт. Краткое отсутствие водителя на сетевой копии не передаёт машину хосту.
- Синхронизируются сюжетное крепление машины к другой машине, смена опоры и отсоединение. Запоздалое состояние не возвращает прежнее крепление.
- При смене владельца NPC короткое ожидание загрузки модели больше не удаляет ожидающего персонажа. Устаревшее ожидание не восстанавливает уже удалённого NPC.
- Исправлена синхронизация подъёма, загрузки, перевозки и сброса ящиков погрузчиком. Миссионные предметы сохраняют заданную физику, скорости и заморозку и корректно возвращаются к обычной симуляции.
- Уточнена привязка миссионных ворот и платформ к исходным объектам карты при перемещении, удалении и замене. Удалённый оригинал не подменяет созданную на его месте новую сущность.
- Исправлен сброс повреждений при сюжетной перезагрузке части карты, включая плантацию. Запоздалое состояние прежней загрузки не повреждает новые объекты.
- Личные задания медика, полицейского и пожарного защищены от сюжетного переноса, смены роли и перезагрузки мира хоста. Защита личного занятия также учитывает активную танцевальную мини-игру.
- Сюжетные гонки отделены от личных турниров при определении владельца сценария.
- Гость получает необходимый сюжетный парашют и тогда, когда у хоста он уже есть. Такая выдача сохраняет выбранное гостем оружие.
- Сюжетные приборы ночного и теплового видения учитывают личный выбор гостя; его собственный эффект не перезаписывается чужой командой.
- При обороне территории около гостя вдали от хоста загружаются нужные местность, пути и модели противников. Освобождение от розыска ограничено зоной боя.

### English

**Protocol 44: update every player and the server together.** Personal profile format 7 is unchanged. Preserve settings, saves and `coop/server-data` when updating.

- Fixed a crash when a remote player respawns after their previous ped is removed.
- Fixed seat conflicts with NPCs. A delayed seat decision no longer puts a player back in their previous vehicle after they leave or switch vehicles.
- Guests retain control of their vehicle when a script adopts it as a mission vehicle. A temporary missing driver on a replica does not transfer the vehicle to the host.
- Scripted vehicle-to-vehicle attachment, reparenting and detachment are synchronized. Delayed state cannot restore an obsolete attachment.
- When NPC ownership changes, a short model-loading wait no longer deletes the pending character. An obsolete wait cannot recreate an NPC that has already been removed.
- Fixed synchronization of forklift crate lifting, loading, transport and dropping. Mission objects retain their assigned physics, velocities and frozen state, and return correctly to normal simulation.
- Improved matching of mission gates and platforms to their original map objects when moved, deleted or replaced. A deleted original is kept distinct from a new entity created in its place.
- Fixed damage resets when a script reloads part of the map, including the plantation. Delayed state from the previous load cannot damage newly created objects.
- Personal Paramedic, Vigilante and Firefighter attempts are protected from the host's story relocations, role changes and world reloads. Personal-activity protection also recognizes an active dance minigame.
- Story races are distinguished from personal tournaments when determining script ownership.
- Guests receive a required mission parachute even when the host already has one. This availability grant preserves the guest's selected weapon.
- Scripted night and thermal vision respect each guest's personal choice instead of overwriting their own effect with another player's command.
- Territory defense near a guest loads the required terrain, paths and enemy models even when the host is far away. The wanted-level exemption is limited to the battle area.

## 0.2.34-3p

### Русский

Протокол 43 и формат личных профилей 7 сохранены.

- Исправлена проверка сохранений mission packs, включая используемый DYOM режим: проверяется соответствующий `MPACK…/SCR.SCM`, а не скрипт обычного сюжета. [#2](https://github.com/opiahex/reSA-Coop/issues/2)
- При загрузке выбирается номер набора из сохранения. Отсутствующий набор, несовместимая структура скриптов и ошибка чтения сообщаются отдельно. Проверка обычного сюжета сохранена.

Исправление касается подключения и загрузки мира. Полное совместное прохождение DYOM-миссий пока не проверено.

### English

Protocol 43 and personal profile format 7 are unchanged.

- Fixed compatibility checks for native mission-pack saves, including the mode used by DYOM: the matching `MPACK…/SCR.SCM` is checked instead of the base story script. [#2](https://github.com/opiahex/reSA-Coop/issues/2)
- Loading selects the pack number stored in the save. Missing packs, incompatible script layouts and read failures have separate messages. The base-story check is retained.

This change covers joining and loading the world. Complete co-op DYOM mission playthroughs have not been verified.

## 0.2.33-3p

### Русский

Протокол 43 и формат личных профилей 7 сохранены.

- Исправлено зависание гостей после пропуска или завершения катсцены. Специальные модели сюжетных NPC загружаются по имени; пустой локальный слот больше не вызывает бесконечный повтор чтения в GTA. Это также применяется при подключении к уже начатой миссии.
- Исправлен конфликт перехватов сюжетных команд с модами, которые используют один общий обработчик SCM. Каждый диапазон команд сохраняет свою цепочку обработки.
- Исправлена сверка времени сюжетной музыки: номер трека больше не принимается за время воспроизведения. Убраны вызванные этим повторные перезагрузки звука.
- Сверка игровых файлов использует путь запущенного EXE, включая переименованный файл. Невозможность чтения отмечается отдельно от настоящего различия сборок; проверка CRC и отсутствующих файлов сохраняется.

### English

Protocol 43 and personal profile format 7 are unchanged.

- Fixed guests hanging after cutscenes are skipped or end. Special story NPC models are resolved by name before loading, preventing an unbound local slot from entering GTA's endless file-read retry. This also applies when joining an ongoing mission.
- Fixed story-command hook conflicts with mods that share one SCM dispatcher. Each command range retains its own dispatch chain.
- Fixed story soundtrack time comparison: the track ID is no longer treated as playback time, preventing unnecessary audio reloads.
- Simulation-file comparison uses the running executable's actual path, including renamed files. Read failures are reported separately from confirmed differences; CRC and missing-file checks remain active.

## 0.2.32-3p

### Русский

**Протокол 43: обновите всех игроков и сервер вместе.** Формат личных профилей — 7; старые профили поддерживаются.

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

### English

**Protocol 43: update every player and the server together.** Personal profiles use format 7 and retain support for older profiles.

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

SA:COOP — [Karimov-Akbar](https://github.com/Karimov-Akbar/sa-coop). Разработка reSA-Coop / reSA-Coop development — [opiahex](https://github.com/opiahex).

## 0.2.31-3p

### Русский

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

### English

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

## 0.2.30-3p

### Русский

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

### English

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

## 0.2.29-3p

### Русский

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

### English

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

## 0.2.28-3p

### Русский

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

### English

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

## 0.2.27-3p

### Русский

- Исправлено вытаскивание гостя из машины NPC: команда доходит до владельца персонажа и запускает штатную физическую реакцию GTA.
- Событие выхода больше не перебивает анимацию угона и не возвращает жертву на водительское место запоздавшим пакетом.
- Угонщик передаёт свою фазу анимации; сетевые копии не проводят независимый повторный угон. После завершения машина переходит к симулятору NPC.
- Открывание и закрывание двери NPC передаются и тогда, когда машиной ещё владеет гость.
- Исправлены вложенный выбор контекста полиции и продолжение старой погони за участником без розыска. Новая погоня проверяет личный розыск выбранной цели.
- Диагностика записывает изменение личного розыска, его нативный источник и состояние при загрузке мира.
- Результаты двенадцати испытаний автошколы и её завершение сохраняются в личном профиле гостя; меню школы не подхватывает медали хоста.
- Сохраняются штатные награды навыка вождения: +30 за впервые полученное золото в испытании и +40 за первое завершение школы, без повторного начисления за тот же результат.
- Профиль обновлён до версии 5. Старые профили сохраняют деньги, навыки и внешность; отсутствовавшие личные записи автошколы начинаются отдельно от медалей хоста. Общие сюжетные флаги мира за пределами школьного сценария сохраняются.
- Протокол 38: обновить нужно всех клиентов и сервер.

### English

- Fixed NPCs dragging guests out of vehicles: the character owner receives the action and runs GTA's native physical response.
- Exit acknowledgements no longer interrupt the carjack animation or let delayed driver snapshots put the victim back in the seat.
- The carjacker replicates its animation phase; replicas do not independently repeat the theft. Vehicle authority transfers to the NPC simulator after completion.
- NPC door opening/closing also replicates while the vehicle is still owned by a guest.
- Fixed nested police player contexts and persistence of an old pursuit against an innocent participant. New pursuits validate the selected target's personal wanted level.
- Diagnostics record personal wanted-level changes, their native source and the state loaded with the world.
- Guests keep their twelve driving-school best scores and completion flag in their personal profile; school menus no longer inherit host medals.
- Native driving-skill awards remain intact: +30 for first gold in a test and +40 for first school completion, without awarding the same result again.
- Profile version 5 retains money, skills and appearance from older profiles. Previously unstored personal school records start separately from host medals. Shared story unlock flags are preserved outside the school interpreter.
- Protocol 38 requires all clients and the server to update.

## 0.2.26-3p

### Русский

- Исправлен бесконечный гудок сетевых машин: нативный флаг прошлого воспроизведения больше не затирается перед остановкой аудиоканала. Исправление относится и к машинам трафика.
- Розыск гостя теперь используется в нативных решениях общих полицейских, а не только в звёздах над игроком. Сохраняются личный розыск и сценарные запреты на преследование.
- Полицейские выбирают разыскиваемого участника; ближайший невиновный игрок не становится целью только из-за расстояния. При завершении преследования очищаются списки нужного игрока.
- Выбор полицейского транспорта и его управление учитывают разыскиваемого гостя в общей зоне населения.
- Фильтр школьных столкновений распознаёт водителя до регистрации машины и завершения передачи владения.
- Контакт между участниками школы не записывается как отложенное повреждение кузова и не пересылается повторно сетевым импульсом. Вне активного школьного сценария действует штатный урон.
- Протокол 37 передаёт также личные флаги игнорирования полицией. Обновить нужно хоста, гостей и сервер вместе.

### English

- Fixed endlessly sounding horns on network vehicles: native previous-playback state is no longer overwritten before the audio channel can stop. This also fixes traffic vehicle replicas.
- Guest wanted levels now feed native shared-police decisions instead of only the stars above their name tags. Personal wanted levels and scripted police back-off remain separate.
- Police select a wanted participant; an innocent player is no longer selected merely for being closer. Pursuit cleanup removes the cop from the correct player's lists.
- Shared police vehicle selection and steering take nearby wanted guests into account.
- School contact filtering recognises the driver before vehicle registration and ownership handover finish.
- Contact between school participants no longer records deferred body damage or forwards another network impulse. Native damage applies outside an active school script.
- Protocol 37 also carries personal police back-off flags. The host, guests and server must all update together.

## 0.2.25-3p

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

Протокол изменён: обновите хоста, гостей и сервер одновременно. Полное автоматическое распознавание произвольных подэтапов SCM, восстановление любого груза после смены этапа и непрерывная симуляция всех NPC за пределами загруженного мира не заявляются. Полное прохождение каждой миссии и все сочетания сторонних модов требуют дальнейшей проверки.

## Накопленные изменения до 0.2.24-3p

## Кооператив на троих и сеанс

- Хост и два независимых гостя вместо единственного напарника.
- Раздельные контексты управления, камер, оружия, состояния и отображения двух удалённых игроков при ограничениях нативных слотов GTA.
- Списки игроков, чат, голос, отметки на карте, здоровье, броня, оружие и розыск для обоих гостей.
- Подключение, отключение, повторный вход с тем же ником и освобождение зависшего соединения.
- Раздельные профили денег, оружия, навыков и внешности; загрузка прежних форматов профиля.
- Восстановление транспорта, мест, оружия, действий и состояния миссии после повторной синхронизации.
- Смена сохранения хоста отделяет новый мир от прошлого сеанса; устаревшие пакеты загрузки не применяются пачкой.
- Поколение мира, соединения и конкретного экземпляра сетевой сущности защищают повторно используемые ID.
- Ограниченные очереди, история событий, карантин и выдача ID без перезаписи живых записей.
- Проверки размера и формата пакетов, диапазонов, NaN/Inf, отправителя, владельца и зависимостей.

## Трафик, прохожие и владение миром

- Согласованный источник генерации населения рядом с игроками; приоритет доступного хоста и отдельные зоны для отдалившихся участников.
- Гистерезис при объединении и разделении зон, чтобы близкие игроки не выключали трафик друг другу.
- Исправления исчезновения, повторного создания и размножения машин/NPC при поворотах камеры и переходах в интерьер.
- Сохранение нативных сценарных ограничений плотности населения вместо постоянного принудительного увеличения трафика.
- Передача симуляции оставшихся людей и машин после ухода владельца или его входа в интерьер.
- Согласование машины с NPC-водителем, очередей их создания и посадки; устранение ряда пустых незаведённых копий трафика.
- Здоровье, броня, смерть и реакции принятых NPC не теряются при передаче владельца.
- Мёртвый водитель не продолжает управлять машиной; труп не оживает после отложенного создания.
- Передаются походка, приседание, стрельба, drive-by, цели атаки, преследование, бегство и посадка NPC.
- Сценарные AI-задачи исполняет владелец; удалённая игра воспроизводит состояние без копирования внутренних указателей GTA.
- Полицейские машины, заграждения, вертолёт и прожектор используют общий мир; личный розыск сохраняется.

## Транспорт и физика

- Передача владельца при посадке, выходе, угоне у другого гостя, смерти водителя и отключении.
- Исправления дрожания, дублирования, возврата удалённых машин и их телепортации после катсцен/интерьеров.
- Сценарное удаление машины распространяется на всех и очищает ссылки пассажиров.
- Повторное использование нативного слота транспорта не подменяет прежнюю сетевую запись новой машиной.
- Гости могут запускать заглушённый двигатель; состояние двигателя согласуется с действующим водителем.
- Детерминированное разрешение одновременной посадки: сохранение действующего водителя, свободное место либо безопасный выход.
- Исправления пассажирского места мотоцикла и конфликтов водительской/пассажирской посадки.
- Более своевременные выход, выбивание водителя, угон и переходы между местами.
- Последние использованные игроком машины сохраняются в ограниченном количестве; занятый транспорт не удаляется вместе с завершением вспомогательной роли.
- Исправления исчезновения использованного транспорта при старте миссии и возвращении из интерьера.
- Контакты с машиной другого игрока передают физический импульс владельцу с отсечением повторов и устаревших ударов.
- Буфер состояний и ограниченная коррекция движения при задержке, потерях и изменении владельца.
- Синхронизация ориентации, скорости, поворота, управления и посадочных мест самолётов и вертолётов.
- Обороты винта и его остановка/раскрутка передаются отдельно от одного флага двигателя.
- Вращение колёс и связанные состояния подвески согласованы с движением, устранены причины ложного волочения колёс.
- Синхронизируются четыре цвета, покраска, компоненты, тюнинг и варианты кузова, включая встроенные ящики.
- Повреждения дверей, капота, багажника, стёкол, панелей и шин сохраняются в снимках.
- Прострел шины передаётся владельцу; ревизия ремонта не позволяет старому повреждению испортить уже починенную машину.
- Исправления чрезмерного урона и взрыва удалённой машины от одного сильного удара.
- Гудок автомобиля передаётся участникам. Радио оставлено личным, общая принудительная синхронизация станции отменена.
- Прицепы и буксировка передают сцепление, отцепление и зависимости связанного транспорта.
- Поезда имеют согласованные ID локомотива/вагонов, путь, движение, остановку, реверс и удаление.
- Нитро, гидравлика, вилы погрузчика, башни и подвижные части используют сетевое состояние.
- Гаражное хранение защищено от повторного STORE и устаревшего подтверждения; возвращается один экземпляр машины.

## Движение, позы и бой

- Передаются переходы прыжок → падение → подъём, смерть → возрождение и посадка → угон → выход.
- Устаревшее действие не возвращает игрока к прошлому состоянию после более нового перехода.
- Плавание, ныряние, парашют, перелезание, телефон, предметы в руках и сценарные позы.
- Исправления скользящей походки и времён анимаций удалённых персонажей.
- Стиль рукопашного боя передаётся отдельно от дефолтных анимаций CJ.
- Реакция на удар машиной, падение и смерть согласованы без повторного запуска одной реакции от давки.
- Движение на крыше, лодке, вагоне и других движущихся опорах использует относительную связь и корректное освобождение опоры.
- Исправлены причины ходьбы по воздуху и последующей смерти после схода с транспорта.
- Согласованы позиции прицела, направление огня, пассажирская стрельба и возврат пассажира в салон.
- Передаются выстрелы, трассы и урон постановочных стрелков; они занимают настоящее пассажирское место, а не стоят на крыше.
- Перед проверкой попадания обновляются необходимые кости пеших персонажей, пассажиров и игроков на опорах.
- Ограниченная история положений используется для проверки попаданий с учётом задержки, жизни, области и поколения цели.
- Согласованы урон, падение и смерть; один подтверждённый удар не применяется повторно.
- Тихое убийство ножом передаётся владельцу NPC с проверкой оружия, подхода сзади, расстояния и исторического попадания.
- Нативная задача жертвы создаёт повреждение и смерть один раз; копии не запускают вторую независимую задачу урона.
- Для тихого убийства сохраняются фактическая анимация смерти, её время, слои, ориентация и признак тихой смерти.
- Парные действия NPC используют одного пространственного якоря, общего партнёра и согласованные фазы вместо взаимного перетягивания копий.
- Передаются цепочка/шаг сценарной задачи и дополнительные слои анимации; отмена защищена ревизией.
- Устранены зависшие пассажирские позы и повторное применение root motion в контактных анимациях.

## Снаряды, огонь и груз

- Проверки создания ракет/RPG и их зависимостей устраняют ряд причин вылета клиента.
- Передаются старт, скорость, источник, событие и место подрыва снаряда.
- Снаряд сохраняет исходную сторону симуляции при передаче стрелявшего NPC другому владельцу.
- Повторный подрыв копии подавляется; удаление одного из нескольких снарядов не завершает чужой.
- Короткое ожидание ещё не созданного NPC/транспорта вместо немедленного отбрасывания зависимого снаряда.
- Дистанционные заряды разных игроков имеют раздельную идентичность и однократный подрыв.
- Тушение пожара, состояние огня и результат воздействия воды/огнетушителя передаются участникам.
- Исправлено обновление огня/прохода в Burning Desire; повторно использованный слот огня не получает чужое тушение.
- Синхронизируются направление и воздействие пожарного водомёта.
- Переносимый предмет имеет одного согласованного держателя; одновременный подбор разрешает хост.
- Удаление или уничтожение переносимого предмета освобождает руки и связи у всех.
- Физический груз использует одного симулятора, контакты с вилочным погрузчиком и состояние опоры.
- Отрыв груза при падении/опрокидывании освобождает связь; предмет не остаётся висеть у старого владельца.
- При завершении миссии или утрате тела держателя старые связи груза очищаются.

## Общая миссия и действия гостей

- Хост выполняет исходный сюжетный SCM; гостям передаются состояние миссии, цель, тексты, субтитры, таймеры и счётчики.
- Миссионные NPC, транспорт, предметы, прикрепления, пикапы, красные маркеры и гоночные чекпоинты имеют согласованное создание/удаление.
- Автосверка и ограниченное восстановление потерянной копии охватывают машины, NPC, предметы, огонь и наборы сюжетных маркеров.
- Зависимая посадка или прикрепление ждёт фактического создания родителя; живые копии не пересоздаются без причины.
- У важных сюжетных маркеров ожидаются все подключённые участники; обычные пикапы не требуют группового подтверждения.
- Маркеры завершённой миссии удаляются у гостей, а не остаются висеть до следующей загрузки.
- Сценарные замки транспорта передаются гостям: открытая хостом миссионная машина доступна и у них.
- Смерть любого участника вызывает штатную ветку провала общей миссии, включая отложенную проверку после сцены.
- Положительная награда выдаётся после штатной регистрации успеха, с токеном миссии и подтверждением сохранённого профиля.
- Повторная доставка и переподключение не удваивают выплату; неподтверждённые выплаты сохраняются в ограниченной очереди хоста.
- Сюжетное оружие не затирает личное оружие того же типа без соответствующего сценарного действия.
- Поддерживаемые условия CJ учитывают действия гостей: посадку, нужный транспорт, местоположение, оружие, перенос предмета, фотографирование, повреждение и уничтожение целей.
- После принятого действия поддерживаемые запросы машины, позиции, направления, оружия, здоровья, брони, скорости и патронов продолжают цепочку с тем же участником.
- Привязка результата проверяет поток скрипта, миссию, поколение сущности и актуальность игрока; управление и награды не переадресуются произвольно.
- Поддерживаемые контакты/переключатели используют реальное свежее нажатие нужного гостя, сохраняя логику AND/OR/NOT исходного условия.
- Обнаружение гостя миссионным охранником, слух от выстрела и поиск сетевого трупа используют нативные проверки видимости и событий GTA.
- Миссионные враги, которым приказано атаковать CJ, могут выбирать живого участника с учётом области, видимости и недавнего агрессора.
- Мирные прохожие не превращены в общих врагов из-за нападения на одного игрока.
- Сюжетные союзники сохраняют лидера при смене симулятора и резервируют обязательные места по команде скрипта или задаче посадки.

## Постановочные поездки и готовность этапов

- При нехватке мест дополнительные реальные сетевые машины копируют подходящую модель и внешний вид сюжетного транспорта.
- Для обычной поездки гости получают добровольное сопровождение под своим управлением.
- В поддерживаемой постановочной стрельбе гости получают NPC-водителя и настоящее место стрелка со своим прицелом.
- Само наличие оружия или обычного NPC за рулём не включает стрелковую роль.
- Дополнительные лётные, вертолётные, RC и стационарные роли привязаны к явному нативному механизму сценария.
- Смена транспорта/места/роли имеет отдельный токен назначения и освобождает прежнее состояние.
- Добровольный выход не приводит к постоянной насильственной посадке обратно.
- Завершение роли сохраняет занятую другим игроком машину и не выбрасывает пилота в воздух.
- Потерянный вспомогательный транспорт/водитель может восстанавливаться с ограничением числа попыток и задержкой.
- Перед поддерживаемыми ключевыми командами движения/таймера хост ждёт подтверждения загрузки сущностей, ролей и коллизии у гостей.
- Игровые часы и нативные таймеры учитывают время ожидания готовности; старое подтверждение другого этапа не открывает новый.
- Провал, окончание, смена поколения зависимости и повторная синхронизация отменяют устаревшие ожидания и связи.
- Камера учитывает роль игрока, а обязательные сюжетные показы сохраняют приоритет.

## Сцены, звук, интерфейс и интерьеры

- Реплики имеют ограниченные очереди, номера, стадии загрузки/готовности/воспроизведения и отмену по границе сцены.
- Таймаут загрузки начинается при запросе аудио, а не во время ожидания предыдущей реплики.
- Согласованы нативные аудиослоты; диагностика различает приём команды, загрузку и фактический запуск звука.
- Исправления пропуска живых сюжетных реплик, включая разговор перед грузовиком в Home Invasion.
- Смерть, пропуск, провал и завершение сцены освобождают старую очередь без удаления реплик следующей сцены.
- Передаются речь прохожих, гудок и необходимые игровые события звука, без общей принудительной радиостанции.
- Перед дальней постановочной камерой запрашивается загрузка её точки.
- После пропуска/выхода хоста возвращаются камера, затемнение, HUD и управление гостей.
- Личные переходы хоста в интерьер не должны затемнять экран остальных как общая катсцена.
- Исправления зависания управления, повторного чёрного экрана и потери HUD после сцены/интерьера.
- Гость, подключившийся во время сцены, получает актуальное состояние; голосование не ждёт игрока, у которого сцена не запущена.
- Воздушный/водный транспорт и парашют не высаживают гостя только из-за начала катсцены хоста.
- Предметы в руках, телефон, поднос, банка, сигарета и раскрытый парашют видны удалённым игрокам.
- Чат, меню F9 и код подключения сохраняют отрисовку при перехвате HUD другим плагином.

## Личные занятия и прогресс

- Школы, гонки, тир, такси, скорая, пожарные, полиция и другие личные занятия не объявляются общей сюжетной миссией.
- Их локальные тексты, награды, камера и переменные отделены от общей истории.
- Исправлено мгновенное закрытие меню спортзала: выбор веса, скорости и обычные упражнения сохраняют нативное управление.
- Одиночные тренажёры, автоматы, бильярд и поддерживаемые казино/услуги используют занятость с освобождением при выходе/отключении.
- Отдельные имена скриптов парикмахерской, тату и казино исключены из общей сюжетной камеры/аудио.
- Сюжетные покупки хоста передаются в общий мир; личные деньги, навыки и оружие остаются у своего игрока.
- Общие находки не выдают второму участнику лишние деньги только из-за чужого подбора.
- Смерть/арест хоста не переводят время всех на 12 часов при активном кооперативе.
- Возрождение рядом с занятым выходом больницы/участка смещено от другого игрока.

## Защита от вылетов и диагностика

- Перенесены и адаптированы исправления upstream для Sweet's Girl, пропуска сцены Wrong Side of the Tracks, Cleaning the Hood, розыска после The Green Sabre и застрявшего флага миссии в сохранении.
- Добавлены проверки исчерпания пулов и различающихся моделей/тюнинга при создании сетевых сущностей.
- Защищены ссылки на удаляемый миссией занятый транспорт и поздние события уже удалённых сущностей.
- Устранён ряд небезопасных путей анимации, снарядов и теней; завершение процесса после выхода не смешивается с игровым вылетом.
- Предыдущий обычный лог сохраняется, вылеты содержат адреса и стек для сопоставления с конкретной сборкой.
- Ограниченная JSONL-диагностика фиксирует владельцев, поколения, водителей, двигатель, зоны населения, создание/удаление и исправления расхождений.
- Раздельные замеры сети, загрузки моделей, создания сущностей, скриптов, сохранения и записи логов помогают искать микрофризы.
- Сверка файлов симуляции выявляет различия скриптов, характеристик и коллизий между участниками.

## Границы реализации

Перечислены реализованные исправления и механизмы, а не обещание идеального прохождения каждой миссии.
Адаптеры распознают конкретные нативные команды; произвольный SCM, все сложные AI-задачи и все сочетания модов автоматически не преобразуются в кооператив.
Восстановление отдельного состояния не гарантирует любой сложный crash/rejoin посреди смены этапа.
Компенсация задержки не является соревновательной серверной симуляцией всей геометрии.
Измерение микрофризов помогает находить причины, но не отменяет ограничения движка, диска и сторонних модов.

## Публикация

- Независимые README на русском и английском с коротким списком главных исправлений.
- Публичный репозиторий содержит исходники и необходимые инструменты сборки; личные инструкции, локальные игровые тесты, записи, логи и сохранения исключены.
- Сборка по тегам создаёт готовый установочный ZIP и SHA-256 в GitHub Releases.
- Авторство исходного SA:COOP и лицензии зависимостей сохранены.
