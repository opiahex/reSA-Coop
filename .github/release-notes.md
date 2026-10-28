# reSA-Coop 0.2.36-3p

## Русский

Диагностическое меню для хоста и двух гостей: Network, Entities, Events, Hooks / SCM и Performance.

- Снимки игровых данных обновляются раз в 250 мс; недоступные значения отмечены явно.
- Реестр хранит сведения об установке хуков и патчей. Inspector отдельно показывает сохранённые байты установки и текущее чтение памяти.
- Ограниченный Flight Recorder записывает поддерживаемые события в ротационные JSONL-файлы, показывает счётчики потерь и сохраняет crash-tail.
- Для HP пассажира добавлена диагностика от отправителя через сервер до применения у получателя.
- Сохранены существующее меню F9, MIT и авторство Karimov-Akbar и opiahex.

Версия клиента и сервера: 0.2.36-3p; протокол 45. Хост, сервер и оба гостя должны обновляться вместе; версия 0.2.35 использует протокол 44.

Ограничения: записываются только инструментированные события; полного трассирования SCM и всех сетевых сообщений пока нет. Запись включается через COOP_FLIGHT_RECORDER=1. Кнопок управления записью и полного экспорта в меню пока нет. Сохраняются известные игровые проблемы с NPC, транспортом и отдельными этапами Photo Opportunity. Этот выпуск не означает, что они все устранены.

Отдельные условия атрибуции применяются только к перечисленным оригинальным диагностическим компонентам.

## English

Diagnostic menu for one host and two guests: Network, Entities, Events, Hooks / SCM, and Performance.

- Game snapshots refresh every 250 ms; unavailable values are identified explicitly.
- The registry records hook and patch installation metadata. Inspector separates historical installation bytes from a current memory sample.
- A bounded Flight Recorder writes supported events to rolling JSONL files, exposes loss counters, and preserves a crash tail.
- Passenger HP diagnostics follow publication, server relay, receipt, and native application.
- The existing F9 menu, MIT license, and Karimov-Akbar and opiahex credits are preserved.

Client/server version: 0.2.36-3p; protocol 45. Update the host, server, and both guests together; 0.2.35 uses protocol 44.

Limitations: only instrumented events are recorded; full SCM and all-packet tracing are not implemented. Enable recording with COOP_FLIGHT_RECORDER=1. In-menu recording controls and full export are not implemented. Known NPC, vehicle, and Photo Opportunity issues remain; this release does not claim to resolve all of them.

Separate attribution terms apply only to the listed original diagnostic components.

## Установка / Installation

Распакуйте ZIP рядом с `gta_sa.exe`. Если ASI-загрузчика нет, запустите `install.bat`. Меню мода — F9; отдельный `coop.dll` не нужен.

Extract the ZIP beside `gta_sa.exe`. Run `install.bat` if an ASI loader is needed. Open F9 in-game; a separate `coop.dll` is not required.

[История изменений RU / EN](https://github.com/opiahex/reSA-Coop/blob/main/CHANGELOG.md).

SA:COOP — [Karimov-Akbar](https://github.com/Karimov-Akbar/sa-coop). Разработка reSA-Coop / reSA-Coop development — [opiahex](https://github.com/opiahex). [MIT](https://github.com/opiahex/reSA-Coop/blob/main/LICENSE); [диагностические компоненты / diagnostic components](https://github.com/opiahex/reSA-Coop/blob/main/DIAGNOSTICS-LICENSE.txt).

SHA-256 — `SHA256SUMS.txt`.
