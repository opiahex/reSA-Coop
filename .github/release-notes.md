# reSA-Coop 0.2.35-3p

**Готовая сборка / Compiled build: `reSA-Coop-v0.2.35-3p-win32.zip` в Assets / in Assets.**

## Русский

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

## English

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

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`. Если ASI-загрузчика нет, запустите `install.bat`. Меню мода — F9; отдельный `coop.dll` не нужен.

Extract the ZIP beside `gta_sa.exe`. Run `install.bat` if an ASI loader is needed. Open F9 in-game; a separate `coop.dll` is not required.

[История изменений RU / EN](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md).

SA:COOP — [Karimov-Akbar](https://github.com/Karimov-Akbar/sa-coop). Разработка reSA-Coop / reSA-Coop development — [opiahex](https://github.com/opiahex). MIT.

SHA-256 — `SHA256SUMS.txt`.
