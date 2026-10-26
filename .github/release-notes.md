# reSA-Coop 0.2.34-3p

**Готовая сборка / Compiled build: `reSA-Coop-v0.2.34-3p-win32.zip` в Assets / in Assets.**

## Русский

- Исправлена проверка сохранений mission packs, включая используемый DYOM режим: проверяется соответствующий `MPACK…/SCR.SCM`, а не скрипт обычного сюжета. [#2](https://github.com/opiahex/reSA-Coop/issues/2)
- При загрузке выбирается номер набора из сохранения. Отсутствующий набор, несовместимая структура скриптов и ошибка чтения сообщаются отдельно. Проверка обычного сюжета сохранена.

Исправление касается подключения и загрузки мира. Полное совместное прохождение DYOM-миссий пока не проверено.

Протокол 43, личные профили 7. При обновлении сохраняйте настройки, сохранения и `coop/server-data`.

## English

- Fixed compatibility checks for native mission-pack saves, including the mode used by DYOM: the matching `MPACK…/SCR.SCM` is checked instead of the base story script. [#2](https://github.com/opiahex/reSA-Coop/issues/2)
- Loading selects the pack number stored in the save. Missing packs, incompatible script layouts and read failures have separate messages. The base-story check is retained.

This change covers joining and loading the world. Complete co-op DYOM mission playthroughs have not been verified.

Protocol 43, personal profiles 7. Preserve settings, saves and `coop/server-data` when updating.

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`. Если ASI-загрузчика нет, запустите `install.bat`. Меню мода — F9; отдельный `coop.dll` не нужен.

Extract the ZIP beside `gta_sa.exe`. Run `install.bat` if an ASI loader is needed. Open F9 in-game; a separate `coop.dll` is not required.

[История изменений RU / EN](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md).

SA:COOP — [Karimov-Akbar](https://github.com/Karimov-Akbar/sa-coop). Разработка reSA-Coop / reSA-Coop development — [opiahex](https://github.com/opiahex). MIT.

SHA-256 — `SHA256SUMS.txt`. Игровые файлы и личные материалы не включены. / Game files and personal materials are not included.
