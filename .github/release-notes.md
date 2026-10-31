# reSA-Coop 0.2.39-3p

### Русский

**Протокол 46: обновите хост, сервер и обоих гостей вместе.** Сборки протокола 45 несовместимы. Формат профилей 7 сохранён; сохраняйте настройки, сохранения и `coop/server-data`.

- Сервер согласует владельца, поколение и версию полномочий сущности, а также готовность машины и экипажа при передаче управления.
- Исправлено восстановление использованной машины и её NPC после обычного переподключения и загрузки мира, включая сохранение задачи пассажира.
- Исправлены проверенные пути HP/смерти игрока из транспорта, смерти NPC после наезда, передачи сюжетных дорожных заграждений и ходьбы/бега в постановке.
- Пустая бывшая машина игрока после `/tp`, выхода и возврата хосту больше не получает газ от старой AI трафика. Нативная физика не замораживается.
- Наружные служебные охранники SF impound больше не рассылаются поверх локальных NPC каждого клиента. Общий бой этих охранников не реализован.
- Добавлены ограниченные наблюдения за задачами и событиями NPC. Условия атрибуции оригинальной диагностики сохранены; перечень компонентов обновлён, MIT не заменён.

Настоящие Host + 2 Guest проверки: обычный reconnect машины с живым пассажиром 78/78, дорожные заграждения 103/103; отдельно подтверждены смерть NPC после BMX/NRG и отсутствие горизонтального самодвижения после TP на протяжении 5 секунд у всех троих.

Полные TP и NRG прогоны не объявляются PASS: у TP остаются краткие вертикальные скорости выше строгого порога и 217 потерь диагностической очереди у одного гостя; у NRG два соседних диагностических события имеют равную временную метку. Смерть/анимация NPC и устранение AI-тяги подтверждены отдельно.

Открыты начальное состояние самолётов, другие случаи полиции/пустого трафика, дрожание после столкновений, общий бой статичных NPC и дополнительные варианты reconnect. Полное прохождение и финал Photo Opportunity не подтверждены. Диагностическое покрытие остаётся частичным.

### English

**Protocol 46: update the host, server and both guests together.** Protocol 45 builds are incompatible. Profile format 7 is unchanged; preserve settings, saves and `coop/server-data`.

- The server coordinates entity owners, generations, authority leases and native readiness for vehicle and occupant handoff.
- Fixed restoration of a used vehicle and its NPCs after an ordinary reconnect and world load, including the passenger's native task.
- Fixed verified transport-origin player HP/death, NPC vehicle-impact death, story road barriers and scripted walk/run paths.
- An empty former player vehicle no longer receives throttle from old traffic AI after `/tp`, exit and host handoff. Native physics is not frozen.
- SF impound exterior service guards no longer produce overlapping network copies on each client. Shared guard combat is not implemented.
- Added bounded NPC task/event observations. Existing diagnostic attribution terms are preserved and the component inventory is updated; MIT is not replaced.

Real Host + 2 Guest checks: ordinary vehicle/live-passenger reconnect 78/78, road barriers 103/103; NPC death following BMX/NRG impacts and no horizontal self-driving for 5 seconds after TP were confirmed separately on all three clients.

The complete TP and NRG runs are not labelled PASS: TP retains short vertical velocities above its strict threshold and 217 diagnostic queue drops on one guest; NRG has equal timestamps for two adjacent diagnostic events. NPC death/animation and removal of AI propulsion were verified separately.

Initial aircraft state, other police/driverless-traffic cases, collision jitter, shared static-NPC combat and additional reconnect variants remain open. A full Photo Opportunity playthrough and finale are not verified. Diagnostic coverage remains partial.

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`. Если ASI-загрузчика нет, запустите `install.bat`. Меню — F9.

Extract the ZIP beside `gta_sa.exe`. Run `install.bat` if an ASI loader is needed. Open F9 in-game.

SA:COOP — Karimov-Akbar; reSA-Coop — opiahex. The original reSA-Coop Diagnostic System by opiahex is available at https://github.com/opiahex/reSA-Coop. MIT and the separate original diagnostic component attribution terms are preserved.

[CHANGELOG RU / EN](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md). SHA-256: `SHA256SUMS.txt`.
