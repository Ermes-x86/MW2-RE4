# MW2-RE4 (Beta)

> ⚠️ **WARNING: this mod is in beta and still contains bugs.** Expect the
> player to sometimes snag on geometry, camera or control glitches, and
> missing or unfinished features. Back up your Resident Evil 4 saves before
> playing, and press F6 at any time to return to normal RE4 controls.

> ⚖️ **YOU NEED TO OWN BOTH GAMES TO USE THIS MOD.** You need your own legal
> copies of **Resident Evil 4 (2005) Ultimate HD Edition** and **Call of Duty:
> Modern Warfare 2 (2009)**. This repository and its releases **do not contain
> any game files**: no maps, models, textures, sounds, videos or executables
> from either game. The mod only reads the games already installed on your PC.
> Do not ask for game files here, and do not use pirated copies. See
> [Legal notice](#legal-notice).

> **Note:** this README was (mostly) AI generated (Human Tested). If something in the
> installation doesn't work, please let me know by
> [opening an issue](../../issues).

**Call of Duty: Modern Warfare 2 (2009) gameplay inside Resident Evil 4.**

MW2-RE4 is a passthrough mod. Press F6 in Resident Evil 4 and you play in
first person with MW2's own movement, weapons, animations, HUD and combat
sounds, in RE4's world: the Ganados, Ashley, the merchant, Leon's health,
items and story all stay RE4's.

It is not a recreation of MW2's controls. The mod runs a modified
[IW4L](https://github.com/vladtrc/iw4L), an open-source MW2 runtime that loads
your own MW2 installation, alongside RE4. A native RE4 plugin (`MW2RE4.asi`)
connects the two:

```
RE4 UHD (x86) + MW2RE4.asi              IW4L runtime (x64)
  world, Ganados, Leon's health,  <-->    MW2 movement, weapons, animations,
  items, merchant, doors, story            camera, HUD, combat audio
```

- MW2 sprint, jump, crouch, prone, ADS, recoil and bob, with collision against
  RE4's rooms.
- Bullets hit Ganados, crates, barrels and other breakable objects through
  RE4's own hit code, with RE4's blood, stagger and damage.
- **Your guns are Leon's guns.** The gun equipped in Leon's attache case
  becomes its MW2 counterpart (the Chicago Typewriter is an AK-47, the Striker
  is the Striker, the rocket launcher is the RPG...). Buy them from the
  merchant as usual; press 1 for the next gun in the case.
- **Ammo is RE4's.** The loaded rounds are MW2's clip and the matching ammo
  boxes in the case are MW2's reserve: reloading spends the boxes and only
  RE4 pickups refill them. Hand grenades and flash grenades in the case are
  MW2's frags (G) and flashes (H).
- **Upgrades count.** A gun's firepower upgrades set its damage (the same as
  the RE4 gun's) and capacity upgrades set the MW2 clip size.
- Only Leon can die: the MW2 player never dies or respawns. Leon's health bar
  and Ashley's are shown on screen.
- RE4 takes over by itself for doors, ladders, jumps, kicks, quick-time events,
  grabs, cutscenes, the merchant, the typewriter and the attache case (E is
  RE4's action button), and MW2 comes back right after.

> **Beta.** Expect bugs. Not every story section is supported yet (see
> [Known limitations](#known-limitations)).

## Requirements

- **Resident Evil 4 (2005) Ultimate HD Edition on Steam**, with its original
  `bio4.exe` (version 1.1.0). Other editions and patched executables are not
  supported.
- **Call of Duty: Modern Warfare 2 (2009)**, installed, with its multiplayer data
  (`zone\<language>\common_mp.ff`).
- Windows 10/11 64-bit and a DirectX 12 capable GPU.

**Recommended** for smooth play: at least a 4-core CPU, 16 GB of RAM and a GPU
with 2 GB of VRAM. RE4 and MW2 run at the same time, so the PC needs room for
both.

## Install

1. Download `MW2-RE4-<version>.zip` from [Releases](../../releases) and extract it
   anywhere.
2. Close Resident Evil 4.
3. Run **`Install-MW2RE4.bat`**. It:
   - finds Resident Evil 4 in your Steam libraries (or asks you to pick the
     folder) and warns if `bio4.exe` is not the Steam 1.1.0 executable;
   - finds Modern Warfare 2 in your Steam libraries (or asks you to pick it);
   - installs the included Ultimate ASI Loader (`dinput8.dll`) into
     `Resident Evil 4\Bin32` if no ASI loader is there yet (an existing one,
     such as re4_tweaks', is kept);
   - copies `MW2RE4.asi` and `MW2RE4.ini` into `Resident Evil 4\Bin32` and the
     MW2 runtime into `Resident Evil 4\MW2RE4` (or `%LOCALAPPDATA%\MW2RE4` when
     RE4 is under Program Files);
   - optionally creates a desktop shortcut.

The mod only reads Modern Warfare 2. Other Call of Duty games installed on the
PC (MW3, Black Ops, Black Ops II) are ignored.

The installer **never deletes or changes save games** (RE4's are kept by
Steam; MW2's folder is only read). Files it replaces are backed up to
`Resident Evil 4\MW2RE4-backup\<date>`. Running it again updates the mod and
keeps your options.

To remove the mod, run **`Uninstall-MW2RE4.bat`**.

## Play

Start Resident Evil 4, load your game, and press **F6**. The first start
prepares MW2 assets and takes about a minute.

| Key | Action |
|---|---|
| F6 | MW2 mode on/off |
| WASD, Shift, Space | Move, sprint, jump |
| C / Ctrl | Crouch / prone |
| Left / right mouse | Fire / aim |
| R | Reload (takes ammo boxes from the case) |
| 1 | Next gun in the attache case |
| G / H | Hand grenade (frag) / flash grenade, from the case |
| V | Melee |
| E | RE4's action button: doors, items, ladders, jumps, the merchant |
| RE4 keys | Attache case, map, menus and Ashley's commands work as in RE4 |
| F5 | Show or hide the RE4 -> MW2 gun list in the case and at the merchant |
| F7 | Show or hide the controls panel |
| F10 | Settings: field of view, mouse sensitivity, infinite ammo, MW2 character skin |
| F11 | Gun damage multipliers per class (1.0 = exactly the RE4 gun's damage) |

Which MW2 gun each RE4 gun becomes can be changed in the `[Guns]` section of
`Resident Evil 4\Bin32\MW2RE4.ini`.

| RE4 gun | MW2 gun |
|---|---|
| Handgun, Punisher, Red9, Blacktail, Matilda | USP .45, M9, Desert Eagle, Glock 18, M93 Raffica |
| Broken Butterfly, Killer7, Handcannon | .44 Magnum, gold Desert Eagle, .44 Magnum |
| Shotgun, Riot Gun, Striker | SPAS-12, M1014, Striker |
| Rifle, Semi-auto Rifle | Intervention, Barrett .50cal |
| TMP, Chicago Typewriter | TMP, AK-47 |
| Rocket Launcher | RPG-7 |

## Known limitations

- The **Mine Thrower** and the **P.R.L. 412** have no MW2 gun yet (the Mine
  Thrower's mines stick and then go off, which no MW2 gun does). Equipping one
  keeps your last MW2 gun; press F6 to use them as Leon.
- Firing speed and reload speed upgrades do not change the MW2 gun yet
  (firepower and capacity do).
- Incendiary grenades and the thermal scope are not adapted yet.
- Vehicle and turret sections (the boat, mounted guns, the mine cart, the jet
  ski) and the chapters where you play as Ashley are RE4's: press F6 to switch
  MW2 off for them.
- Moving floors and elevators are not adapted to MW2 movement; you may need
  F6 there too.
- Boss weak spots are not specially handled yet, and the MW2 knife does not
  use RE4's knife damage yet.
- The merchant shows the RE4 gun names (F5 shows the MW2 names next to them).
- Later chapters are not fully tested.
- Only tested with the Steam release of RE4 UHD, without other RE4 mods.

## Troubleshooting

- **Nothing happens on F6.** Check that `dinput8.dll` and `MW2RE4.asi` are in
  `Resident Evil 4\Bin32` and that `MW2RE4.log` appears there after starting
  the game.
- **The first start is slow.** The first time, the mod builds about 1 GB of
  MW2 texture and shader cache in `<runtime folder>\iw4l-artifacts\cache`.
  That can take several minutes on a slower CPU or a hard drive; later starts
  take about a minute. Keep the game running until it is ready. If it is
  stopped, the part already built is kept, so the next try is faster.
- **"MW2 runtime stopped" or "MW2 startup timed out".** Press F6 to retry. The
  runtime log is `<runtime folder>\iw4l-artifacts\logs\latest.log`; check that
  the `Modern Warfare 2.lnk` shortcut in the runtime folder points to your MW2
  folder (running the installer again fixes it). When reporting a problem,
  attach `Resident Evil 4\Bin32\MW2RE4.log` and the newest files in
  `iw4l-artifacts\logs`.
- **MW2 is in the way in a special section.** Press F6 to give RE4 full
  control, and F6 again afterwards.

## Building from source

| Path | What |
|---|---|
| `host/` | Native RE4 plugin (`MW2RE4.asi`), C++ (MSVC x86) |
| `runtime-patches/` | Changes to the MW2-GTA IW4L runtime for RE4 (`re4-bridge.patch`) |
| `installer/` | Release installer and uninstaller |
| `scripts/Build-Release.ps1` | Packages the release zip |

Prerequisites: Visual Studio Build Tools with C++ (x86), and Rust (rustup) for
the runtime.

```powershell
host\Build-ASI.cmd                       # -> host\build\MW2RE4.asi
# runtime: MW2-GTA's IW4L source with runtime-patches\re4-bridge.patch applied
$env:CARGO_PROFILE_DEV_DEBUG = '0'; $env:CARGO_PROFILE_DEV_OPT_LEVEL = '1'
cargo build -p launcher                  # -> <target>\debug\iw4l.exe (shipped as iw4l-re4.exe)
scripts\Build-Release.ps1 -Version 0.1.0-beta
```

The native and runtime halves share a memory layout (`host/src/Protocol.h` and
the runtime's `crates/bootstrap/src/gta_bridge.rs`) and must always be built
and shipped together. Developer notes and the version history are in
[CLAUDE-CODE-HANDOVER.md](CLAUDE-CODE-HANDOVER.md).

## Credits

- **[IW4L](https://github.com/vladtrc/iw4L)** by vladtrc: the open-source MW2
  runtime this mod is built on (Apache-2.0). See `licenses\` in the release for
  IW4L's own notice and third-party licences.
- **[re4_tweaks](https://github.com/nipkownix/re4_tweaks)** by nipkownix and
  contributors: its research was the reference for RE4's memory layouts and
  functions.
- **[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)** by
  ThirteenAG (MIT), included as `dinput8.dll`.
- MW2-RE4 plugin, installer and IW4L changes: **Ermes_x86 (Payne)**, based on
  MW2-GTA.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE). The
licence covers this project's own code only, not the games it works with.

## Legal notice

- **You must own both games.** MW2-RE4 works only with your own legally
  obtained copies of Resident Evil 4 (2005) Ultimate HD Edition and Call of
  Duty: Modern Warfare 2 (2009), installed on your PC.
- **No game content is distributed.** This repository and its releases
  contain only the mod's own source code and the programs built from it
  (`MW2RE4.asi`, `iw4l-re4.exe`, the installer) plus the Ultimate ASI Loader.
  They contain no game files or assets from either game. Game content is read
  at run time from your own installations and stays on your PC.
- **Do not share game files.** Issues, pull requests or comments that
  contain or link to game files, cracks or pirated copies will be removed.
- **Unofficial fan project.** MW2-RE4 is a free, non-commercial fan mod. It
  is not affiliated with, endorsed by or sponsored by Activision, Infinity
  Ward or Capcom.
- **Trademarks.** Resident Evil, Call of Duty and Modern Warfare are
  trademarks of their respective owners and are used here only to describe
  which games the mod works with.
- **Use at your own risk.** The mod is provided "as is", without warranty
  (see the [LICENSE](LICENSE)). Back up your saves before installing mods.
- **Rights holders:** if you have a concern about this project, please
  [open an issue](../../issues) and it will be addressed promptly.
