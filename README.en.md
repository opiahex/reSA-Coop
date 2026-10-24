**reSA-Coop is an actively maintained, fixed version of the GTA San Andreas cooperative mod, developed for three players.**

Key fixes and improvements:

- Vehicle duplication/disappearance during ownership changes, carjacking and interior transitions.
- Host-priority population around nearby players, with separate population zones for distant players.
- Guest engine starting, simultaneous seat conflicts and motorcycle passenger entry.
- Vehicle colours, body variants, cargo, rotors, wheel rotation, punctures and repairs.
- Story dialogue queues and recovery of cameras, screen fades, HUD and controls.
- Fire extinguishing, mission objects, pickups, checkpoints and missing replica recovery.
- Guest story actions, scripted rides, hit processing, animations and stealth kills.
- Crash prevention around projectiles, occupied vehicle deletion, world loading and reconnects.

[Download ready-to-install builds](https://github.com/opiahex/reSA-Coop/releases) · [Complete cumulative changelog](CHANGELOG.md) · [Русский](README.md)

## About

An independent fork of [SA:COOP](https://github.com/Karimov-Akbar/sa-coop), prioritising more active testing, fuller guest participation and a consistent three-player world.
The host runs the original story from their save. Two guests join with separate money, equipment, skills and appearance.
Mandatory story characters and native mission logic remain intact; no extra personal objectives or role HUD are added.
Side activities remain personal, and each player controls their own radio.

Current baseline: **0.2.28-3p, protocol 39**. Version 0.2.28 fixes Photo Opportunity cameras and photographic evidence, repeated script barriers, overlapping control holds, horn/siren mixing and local wanted levels charged by deferred guest events. Distant NPC animation and safe world loading have also been updated. Previous fixes are included. The entire campaign and arbitrary third-party mod combinations are not certified bug-free.

## Install and play

1. Use GTA San Andreas **1.0 US**. Definitive Edition is unsupported.
2. Download **reSA-Coop-…-win32.zip** from [Release Assets](https://github.com/opiahex/reSA-Coop/releases), rather than the source-code archives.
3. Extract beside `gta_sa.exe`, including `coop.asi`, `install.bat` and `coop/`.
4. Run `install.bat` if an ASI loader is needed. The original game library is retained as `vorbisHooked.dll`.
5. Open **F9** in the game. The host creates a session and shares its code; guests enter it under Connect.

Update all clients and the server together. Keep your settings and `coop/server-data` when updating.
`coop.asi` is the game module; players do not need an additional `coop.dll`.
Allow the game/server through Windows Firewall. Direct connectivity depends on the network; connection and relay settings are available in the mod menu.

Important story markers wait for connected participants. Regular health, money and weapon pickups remain independent.
Any participant's death triggers the shared mission's native failure path. Successful positive mission rewards are credited once per player.
Supported scripted transport mechanics provide overflow rides when required; ordinary escorts are optional, and a gunner role requires an actual scripted shooting section.
Use compatible story scripts, handling data and model collisions across clients.

## Build and contribute

Windows, Python 3.10+ and PowerShell are required:

```powershell
./tools/get-toolchain.ps1
python tools/fsd_check.py
python tools/i18n_check.py
python tools/build.py dist
```

The installer archive is `dist/SA-COOP.zip`. Existing binary names are retained for compatibility.
GitHub Actions builds `main`; a `v…` tag publishes a compiled ZIP and checksum to Releases.
Private gameplay fixtures, session recordings, saves and game assets are excluded.

For [bug reports](https://github.com/opiahex/reSA-Coop/issues), include the mod version, mission, host/guest roles, reproduction steps and installed modifications.
Attach `coop/coop.log`, `coop/crash.log` for crashes, and matching session diagnostics from all affected players when possible. Review attachments for private data before publishing.

## Credits

Based on [Karimov-Akbar/sa-coop](https://github.com/Karimov-Akbar/sa-coop); original copyright notices are retained.
Licensed under [MIT](LICENSE), with ENet/MinHook notices in the binary package.
Not affiliated with Rockstar Games. GTA San Andreas is not distributed with this mod.
