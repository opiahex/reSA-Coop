# reSA-Coop 0.2.27-3p

Кооператив на троих для GTA San Andreas 1.0 US. / Three-player co-op for GTA San Andreas 1.0 US.

**Скачайте `reSA-Coop-v0.2.27-3p-win32.zip` из Assets. / Download the compiled `reSA-Coop-v0.2.27-3p-win32.zip` from Assets.**

## Русский — исправления 0.2.27

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

## English — 0.2.27 fixes

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

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`, сохранив свои настройки и `coop/server-data`. При необходимости установите ASI-загрузчик через `install.bat`; меню мода — F9. Отдельный `coop.dll` не нужен.

Extract the ZIP beside `gta_sa.exe`, preserving your settings and `coop/server-data`. Run `install.bat` if an ASI loader is needed; open F9 in-game. A separate `coop.dll` is not required.

**Протокол 38: обновите всех игроков и сервер. / Protocol 38: update every client and the server.**

[Полная накопленная история / Complete cumulative history](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md).

Разработка продолжается; весь сюжет и любые сочетания сторонних модов не проверены без исключений. Произвольные подэтапы SCM, любой переносимый груз при переподключении и все NPC вне загруженного мира не объявляются полностью поддержанными.

Development continues; the entire campaign and arbitrary third-party mod combinations are not certified bug-free. Arbitrary SCM substages, every carried-object reconnect case and every NPC outside the loaded world are not claimed as fully supported.

SHA-256 — `SHA256SUMS.txt`. Игровые файлы и личные материалы не включены. / Game files and personal materials are not included.
