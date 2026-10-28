**reSA-Coop is an actively developed SA:COOP fork with fixes for three-player cooperative play.**

[![Release](https://img.shields.io/github/v/release/opiahex/reSA-Coop?label=release)](https://github.com/opiahex/reSA-Coop/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/opiahex/reSA-Coop/total?label=downloads)](https://github.com/opiahex/reSA-Coop/releases)
[![License](https://img.shields.io/badge/license-MIT%20%2B%20Diagnostics-97CA00)](DIAGNOSTICS-LICENSE.txt)

[![Telegram group](https://img.shields.io/badge/Telegram%20group-26A5E4?logo=telegram&logoColor=white)](https://t.me/resacoop)

# reSA-Coop

Play GTA San Andreas with a host and two guests. The host loads a save and starts missions; each player keeps their own money, weapons, skills and appearance. Gyms, schools and side jobs can be played separately.

[Download](https://github.com/opiahex/reSA-Coop/releases/latest) · [Changelog RU / EN](CHANGELOG.md) · [Русский](README.md)

## Fixes

- Vehicle duplication and disappearance during carjacking, disconnects and interior transitions.
- Guests starting engines, seat conflicts and NPCs dragging drivers out of cars.
- Vehicle colours, body variants, cargo, wheels, damage, repairs and rotor speed.
- Traffic and NPC population: a shared zone near the host, separate zones when players travel apart.
- Cameras, subtitles, story dialogue and control recovery after scenes.
- Mission objects, fire, map changes and recovery of missing network entities.

Supported story rides provide extra vehicles or seats for guests when the mission calls for them. A normal ride does not become a scripted shooting sequence.

See the [changelog](CHANGELOG.md) for the full history. Not every mission or third-party mod combination has been tested. Report problems in [Issues](https://github.com/opiahex/reSA-Coop/issues).

## Installation

Requires **GTA San Andreas 1.0 US for Windows**. Definitive Edition is not supported.

1. Download **reSA-Coop-…-win32.zip** from the [latest release](https://github.com/opiahex/reSA-Coop/releases/latest).
2. Extract the entire archive beside `gta_sa.exe`.
3. Run `install.bat` if you do not already have an ASI loader.
4. Start the game and press **F9**.

The archive includes `coop.asi` and the `coop` folder containing the server and other mod files. Players do not need a separate `coop.dll`. The **Source code** archives are for building the project yourself.

Keep your settings and `coop/server-data` player profiles when updating. **All players and the server must use the same mod version.**

## Connecting

The host opens **F9 → Create game**, selects a save and shares the session code. Guests enter it through **F9 → Connect**. The built-in server starts automatically; allow network access when Windows asks.

The host starts story missions. Important story markers wait for the other participants, and a participant's death fails the shared mission. Regular pickups and personal activities do not require everyone to gather.

Use matching story scripts, vehicle handling data and model collisions across players.

## Reporting a problem

Include the version, mission name and steps leading to the problem in an [issue](https://github.com/opiahex/reSA-Coop/issues). For synchronization problems, attach logs from the affected players for the same session:

- `coop/coop.log`;
- `coop/crash.log` if the game crashed;
- matching files from `coop/diagnostics`.

List your installed modifications and check attachments for personal information before posting.

## Building

Requires Windows, Python 3.10+ and PowerShell:

```powershell
./tools/get-toolchain.ps1
python tools/fsd_check.py
python tools/i18n_check.py
python tools/build.py dist
```

The installable archive is `dist/SA-COOP.zip`. Game files are not included.

## Credits

SA:COOP by [Karimov-Akbar](https://github.com/Karimov-Akbar/sa-coop). reSA-Coop development by [opiahex](https://github.com/opiahex).

Base SA:COOP, previously released reSA-Coop material and existing MIT integration code retain the [MIT license](LICENSE). The original [reSA-Coop Diagnostic System by opiahex](https://github.com/opiahex/reSA-Coop) uses [separate attribution terms](DIAGNOSTICS-LICENSE.txt) for the [listed component files](DIAGNOSTICS-COMPONENTS.md). Preserve opiahex and the original-source link when distributing those components; see [the notice](DIAGNOSTICS-NOTICE.md).

ENet and MinHook retain their own licenses, included in the archive. This project is not affiliated with Rockstar Games.
