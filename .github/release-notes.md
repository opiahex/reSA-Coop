# reSA-Coop 0.2.33-3p

**Готовая сборка / Compiled build: `reSA-Coop-v0.2.33-3p-win32.zip` в Assets / in Assets.**

## Русский

- Исправлено зависание гостей после пропуска или завершения катсцены. Специальные модели сюжетных NPC загружаются по имени; пустой локальный слот больше не вызывает бесконечный повтор чтения в GTA. Это также применяется при подключении к уже начатой миссии.
- Исправлен конфликт перехватов сюжетных команд с модами, которые используют один общий обработчик SCM. Каждый диапазон команд сохраняет свою цепочку обработки.
- Исправлена сверка времени сюжетной музыки: номер трека больше не принимается за время воспроизведения. Убраны вызванные этим повторные перезагрузки звука.
- Сверка игровых файлов использует путь запущенного EXE, включая переименованный файл. Невозможность чтения отмечается отдельно от настоящего различия сборок; проверка CRC и отсутствующих файлов сохраняется.

Протокол 43, личные профили 7. Обновите мод у всех игроков. Настройки, сохранения и `coop/server-data` сохраняйте при обновлении.

## English

- Fixed guests hanging after cutscenes are skipped or end. Special story NPC models are resolved by name before loading, preventing an unbound local slot from entering GTA's endless file-read retry. This also applies when joining an ongoing mission.
- Fixed story-command hook conflicts with mods that share one SCM dispatcher. Each command range retains its own dispatch chain.
- Fixed story soundtrack time comparison: the track ID is no longer treated as playback time, preventing unnecessary audio reloads.
- Simulation-file comparison uses the running executable's actual path, including renamed files. Read failures are reported separately from confirmed differences; CRC and missing-file checks remain active.

Protocol 43, personal profiles 7. Update all players. Preserve settings, saves and `coop/server-data` when updating.

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`. Если ASI-загрузчика нет, запустите `install.bat`. Меню мода — F9; отдельный `coop.dll` не нужен.

Extract the ZIP beside `gta_sa.exe`. Run `install.bat` if an ASI loader is needed. Open F9 in-game; a separate `coop.dll` is not required.

[История изменений RU / EN](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md).

SA:COOP — [Karimov-Akbar](https://github.com/Karimov-Akbar/sa-coop). Разработка reSA-Coop / reSA-Coop development — [opiahex](https://github.com/opiahex). MIT.

SHA-256 — `SHA256SUMS.txt`. Игровые файлы и личные материалы не включены. / Game files and personal materials are not included.
