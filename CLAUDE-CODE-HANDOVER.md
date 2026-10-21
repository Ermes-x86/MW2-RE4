# MW2 inside RE4 UHD: Claude Code handover

Started 2026-10-17. Passthrough mod: Call of Duty: Modern Warfare 2 (2009) gameplay inside
Resident Evil 4 (2005) Ultimate HD Edition (Steam). Reuses the MW2-GTA design
(`C:\Users\Ermes\Projects\MW2-GTA-Bridge`, read-only for this project) and the playbook
https://claude.ai/code/artifact/55a573de-33a7-448e-93b7-aabf5ca02982.

## Rules (from the user)

- Work only in `F:\ModProjects\MW2-RE4`. Never modify, rebuild or deploy anything in the MW2-GTA folders; copy from them.
- Never touch `iw4l-runtime\iw4l.exe`, `account.dat` or any `checkpoints\` folder.
- Never touch RE4 or GTA saves. Back up any RE4 file before replacing it. Deploy only with RE4 closed.
- Never upload game files, MW2/RE4 assets or account.dat.
- C: is nearly full: build output and big files go on F:.
- Explain simply; ask before anything hard to undo. The user tests in game themselves.

## Paths

| Item | Value |
|---|---|
| RE4 | `F:\SteamLibrary\steamapps\common\Resident Evil 4` (app 254700, build 10112090, no update pending) |
| RE4 exe | `Bin32\bio4.exe`, 9,139,840 bytes, SHA-256 `19AED4AF0AB06A748FF8744D45AC5580FCD6BE6B6B7E944B1AB8822A00C8EE4A` (original, restored from the user's `.bak`) |
| MW2 | `F:\yiasuhd6hjxkhbeaAshcuxy2nmpsalk\Call of Duty - Modern Warfare 2` |
| Old RE4 mods | `re4-removed-mods\` (SkateMode, re4_tweaks leftovers, patched exe); see its README.txt to undo |
| re4_tweaks source | `third_party\re4_tweaks` (commit 92c0208b, reference only, not installed) |
| ASI loader | `third_party\ual-v9.7.4` (Ultimate ASI Loader v9.7.4, x86 `dinput8.dll`) |
| Host plugin | `host\src\MW2RE4.cpp`, `host\src\Re4Game.h`, build `host\Build-ASI.cmd` -> `host\build\MW2RE4.asi` |
| Numbered builds | `builds\MW2RE4.vN.asi` (+ .pdb) |

## Deployed in RE4 `Bin32` (all new files, nothing replaced)

`dinput8.dll` (UAL 9.7.4), `MW2RE4.asi` (v1), `MW2RE4.ini`. Log: `Bin32\MW2RE4.log` (previous run: `MW2RE4.prev.log`).
To remove the mod: delete those three files.

## Phase 1 (v1, built and deployed, awaiting the user's test)

- Finds pG (globals), pPL (Leon), CamCtrl and the `CameraQuasiFPS::hitCheck` call by re4_tweaks byte patterns.
  Each must match exactly once or nothing is hooked. Offline scan of bio4.exe: all 4 match once
  (pG at exe VA 0x6D19D0, pPL 0x598074, CamCtrl 0x4FF687, hitCheck call 0x59E213); the code is not encrypted.
- Redirects that one CALL to `HitCheckHook`, which runs once per camera frame of the normal over-the-shoulder camera.
  It calls the original, then logs every `LogEveryNFrames` and on room change: room, HP, Leon pos/rotation, final camera,
  plus the `pl_mat` frame and the camera offsets RE4 computed in that frame.
- F6: test camera. Overwrites the camera point (in `pl_mat` space) with eye height `TestEyeHeight` and a direction
  from `TestYawDeg/TestPitchDeg`, sweeping at `TestSweepDegPerSec`.
- A heartbeat line every 5 s shows whether the camera hook is running (it does not run in menus/cutscenes).

Open questions phase 1 should answer from the log: is `pl_mat` Leon's matrix (with his yaw) or world-aligned?
Where is its origin relative to Leon's feet? Does RE4's camera smoothing lag behind the test value?

## Phase 1 result (tested by the user 2026-10-17)

Works: hook runs 60x/s in r100, Leon pos/yaw read, camera driven. Learned: `pl_mat` = Leon's matrix (origin at his feet,
rotated with his yaw); RE4 adds ~1 s of camera smoothing and a ~21 cm shoulder offset after `hitCheck`; 1 unit ~ 1 mm.

## Phase 2 (v2, built and deployed 2026-10-17, awaiting the user's test)

Passthrough with the real MW2 runtime, unchanged: `iw4l-runtime\iw4l-re4.exe` = MW2-GTA `iw4l-gta.exe` v41
(SHA-256 2A181981...7114A, also kept as `iw4l-re4.v41-from-mw2gta.exe`), own settings `re4-bridge-settings.cfg`,
own mapping `Local\MW2RE4_v1`, account.dat NOT copied (the runtime creates its own). `host\src\Protocol.h` is MW2-GTA's, verbatim.

- Host space = metres, Z up: host = (x, -z, y) / 1000 from RE4. Proper rotation, no winding flip.
- Collision: RE4's loaded SAT polygons (SatMgr, re4_tweaks DebugDisplay.cpp) within 14 m, transformed by each cSat's
  `mat_60`, published every 80 ms (MW2 will not anchor or move until triangles arrive).
- Camera: hook on the call at exe VA 0x6F7111 in CameraControl::move (function 0x59E660 builds the view from
  Camera_60's camera point; Camera_60 is then copied to pG->Camera). MW2's eye/forward are written there, so RE4's
  smoothing and shoulder offset are bypassed. Vertical FOV derived from MW2's horizontal FOV and image aspect.
- Leon: `SPF_PL` (stop player logic) while active, pos/yaw written from MW2 each frame; `DPF_PL` display flag flipped to hide him.
- Input: keys via GetAsyncKeyState (same bit map as MW2-GTA), mouse from RE4's DirectInput delta globals.
- Overlay: D3D9 Present/Reset hooked through the shared device vtable (probe device); MW2-GTA chroma shader.
  Top-left square: yellow = MW2 loading, white = ready, red = error.
- Suspends on cutscene/event/menu/movie/death flags, non-Leon characters and outside the main loop.
- v2 test: F6 started MW2 and collision was sent (115-215 triangles, floor at Leon's feet), but the overlay never drew
  (the probe-device vtable hook never fired for RE4's device) and the user closed RE4 while MW2 was loading.
- v3: overlay now attaches by wrapping RE4's own Direct3DCreate9 call (re4_tweaks D3D9hook.cpp pattern) and
  hooking CreateDevice -> the real device's Present/Reset. Deployed 2026-10-17, awaiting test.
- v3 test: overlay works (yellow -> red). MW2 runtime crashed ~35 s in, after map load, inside the NVIDIA D3D12
  driver (nvwgf2umx.dll, c0000005), after a Windows RADAR_PRE_LEAK report. Cause: GPU is a GTX 750 with 1 GB VRAM;
  RE4 was 1080p + 4x AA + HD textures. With the user's OK: RE4 config.ini (Documents\My Games\Capcom\RE4) backed up to
  config.ini.before-mw2re4.bak, then antialiasing 0, useHDTexture 0, shadowQuality 1. MW2 image 1024x576 -> 960x540
  (16:9 to match RE4; backup re4-bridge-settings.cfg.1024x576.bak). Then, at the user's request, RE4 set to 1280x720 windowed (fullscreen 0). Commit memory is also tight (2.6 GB page file on C:).
- Not yet: shots do nothing in RE4 (phase 3: Ganado damage), doors/items need F6 off (Leon's logic is stopped).
## Phase 2 proposal (not started)

See the report in chat on 2026-10-17: IW4L copy + bridge protocol, MW2 camera/input, overlay, room collision
(SAT/EAT collision from the room data), Leon as a puppet, village (r100 area) prototype.
- v4 (deployed 2026-10-17): Leon no longer frozen (SPF_PL dropped). PadRead hook masks Key_btn_on/trg to
  action/pickup/QTE/inventory/menu bits and zeroes the move sticks; MW2 use (E) also presses KEY_CK|KEY_PICK_UP|KEY_ACT_A.
  Suspends while Leon routine (+0xFC) >= 2 (hurt/grabbed/dead). Overlay hidden and host.enabled=0 when RE4's
  camera hook has not run for 200 ms (pause/menus). Mouse Y sign flipped. Next: shots -> native Ganado damage, grenades.
- v5 (deployed 2026-10-17): v4 + controls panel (GDI text -> managed D3D texture) while MW2 is active, F7 toggles,
  saved as ShowControls in MW2RE4.ini; loading/ready/error status text next to the square. Links gdi32.
- v6 (deployed 2026-10-17): F8 weapon menu from MW2's catalog (feature request kind 1), filtered by FitsRe4() (no throwing knife, javelin, stinger, c4, claymore, riot shield, tactical insertion); F9 ammo refill (kind 2); menu blocks MW2 input and all RE4 keys; notices bottom-left.
- v7 (deployed 2026-10-17): mouse fixed (RE4 mouse delta globals == Joy[0] left stick; v4 zeroed them; deltas now collected in PadRead before zeroing). DPF_* bits mean 'hidden when set' (DPF_PL was 0 normally); DPF_COCKPIT set while active hides RE4's HUD. Health bar (pG HP) + red damage edges.
- Damage research 2026-10-17: enemies process hits themselves: each frame they check cDmgInfo flag (+0x32C), weapon type (+0x32E), hit zone YARARE* (+0x340), compute damage and call LifeDownSet2 (0x5B5AF0, thunk 0x4071C1; args em, damage, random, flags). Static search for the attacker side failed. v8 (deployed): DamageTrace=1 sets hardware watchpoints (DR0-3) on the 2 nearest enemies' hit flag and HP and logs writer EIP + callers as TRACE lines. EmMgr = 0xBFDB04 (file VA), unique pattern '6A 51 B9 ? ? ? ? 89 45 D4 E8' +3.
- v8 trace result: Leon's handgun -> 0x8FD611 calls BulletHit 0x686C40 (thunk 0x407865) as (Leon, &muzzle, &end = muzzle + 50 m, pG->weapon_no, 0, 6000.0f); it traces enemy YARARE zones and calls cDmgInfo set 0x5AEEF0 (thunk 0x403224); the enemy (e.g. 0x458260) then calls LifeDownSet2. RE4 weapon numbers from table 0xB24B10 (item->weapon_no). v9 (deployed): MW2 shots -> BulletHit with the closest RE4 weapon (Re4WeaponFor), melee -> knife 0x10; MW2 explosions -> BulletHit grenade 0x13 / flash 0x17 from the blast to each enemy within radius; MW2 image fades to ~30% while cActionButton m_active_flag is set (prompts readable). DamageTrace still 1 in the deployed INI to capture RE4's own grenade explosion.
- v10 (deployed 2026-10-17): MW2 explosions replay RE4's own grenade/flash explode code (effect 0x4014C4 type 0x0D/0x0C, sound 0x40ADC1 id 0x14/0x13, STA_PL_FIRE + status1 0x20000000, BulletHit(0,&p,&p,0x13,0,6000) / (0x17, p+500 up, 15000)). Prompts: top-level IDSystem units of class 0x20 raised by PromptRaise (+y = up) while active, restored on exit (fade removed). QTE active byte (QTEFixes pattern) suspends MW2. Overlay hidden after 2 presents without a camera frame. DamageTrace set back to 0.

## v11 (2026-10-17)
- Assault rifles and LMGs (ak47, m4, famas, fal, scar, tavor, fn2000, masada, m16, aug, rpd, sa80, mg4, m240) no longer use Chicago Typewriter damage (0x0C, one-shot kills at MW2 fire rate). They now use INI `RifleWeapon` (default 1 = handgun). Build builds/MW2RE4.v11.asi.

## v12 (2026-10-17)
- Activate now takes the starting yaw from Leon's facing (Coord_ang.y), pitch 0, instead of RE4's camera, which after cutscenes could look back at Leon. Build builds/MW2RE4.v12.asi.


## v13 (2026-10-17)
- F10 settings menu: FOV 60-120 (live), mouse sensitivity = MouseScale 0.1-5.0 (live), infinite ammo (silent kind-2 resupply when reserve < one clip or a grenade slot is empty, every >=1.5 s), skin. Saved to the INI.
- Skin = the map MW2 loads (the runtime takes arms/body from the map's allies soldier kit; FFA uses allies): rust TF141 desert, favela, derail, invasion (Rangers), subbase (SEALs). 'Restart MW2 now' kills and relaunches the runtime. The runtime has no skin request (request kinds are only 1 give, 2 resupply, 3 detonate). Verify the kit in the runtime's latest.log ('map soldier kits: allies=...').


## v14 (2026-10-17)
- RE4 actions (jump over/down, climb): E pressed while the action prompt is up starts a hand-over (ActionBlocker): MW2 steps aside once Leon's rno0/rno1 change from their values at the press, until they return (min 300 ms, max 10 s; no change within 500 ms = no action, MW2 keeps control). Previously ApplyMovement overwrote Leon's position each frame and cancelled the action. Log lines 'RE4 action: ...'.


## v15 (2026-10-17)
- Captions moved up while MW2 is active: MesSet's jmp thunk (found from the re4_tweaks item-examine call, pattern '68 84 00 02 00 51 53' +13) is redirected to MesSetHook, which lowers py by MessageRaise (150) for layouts 0 (caption) and 5 (system). IDC_MSG_WINDOW (0x2F) top-level units are raised by MessageRaise in RaisePrompts. Units/direction guessed (MesSet py screen-down, IDSystem y up); log lines 'caption ...' and 'caption window unit ...' give the real values.


## v16 (2026-10-19)
- Damage groups via INI: PistolWeapon 1 (usp, beretta), MachinePistolWeapon 11 TMP (glock, beretta393, pp2000, tmp), SmgWeapon 1 handgun (mp5k, ump45, kriss, p90, uzi; was TMP), MagnumWeapon 3 Red9 (deagle, anaconda; was Broken Butterfly).
- UAV radar (U): MW2's UAV can't see RE4 enemies, so an overlay radar top right draws live EmMgr enemies within 50 m relative to Leon's facing, UavSeconds 30 / UavCooldown 60. Left/right orientation derived on paper (host right-handed); unverified.


## v17 (2026-10-19)
- [Weapons] INI section: per-gun RE4 weapon (name prefix up to '_'), checked before the groups. Defaults ak47=3, m4=3 (Red9). Other rifles/LMGs stay RifleWeapon=1 (handgun; the log already showed 0x01 for m240, the user thought they used TMP).


## v18 (2026-10-19)
- F11 gun damage menu: rows = last gun fired (creates a [Weapons] entry on first change), the five groups, every [Weapons] entry. Left/Right steps TMP < handgun < Red9 < Blacktail < Chicago < Broken Butterfly; applies live, saved to the INI.


## v19 (2026-10-19)
- F11 now = damage multiplier sliders per gun class (normal pistols, tough pistols, SMGs, assault rifles incl. LMGs, 12 gauge, snipers), x0.1-x5.0, INI Damage0..5. Classes hit through a fixed RE4 gun (handgun; shotgun/Striker; bolt/semi rifle) for reactions; LifeDownSet2's JMP thunk (0x4071C1, pattern kLifeDown) is hooked with a naked stub that scales the damage arg for enemies (model id != 0) for 400 ms after an MW2 bullet hit; grenades reset it. [Weapons] and the *Weapon keys were removed (old INI kept as builds\MW2RE4.ini.before-v19.bak).



## v20 (2026-10-20): RE4 owns guns, ammo and health; own runtime build
- **Runtime**: `iw4l-source\` = copy of MW2-GTA's iw4l-source working tree (runtime v50, snapshot commit 43e09d1 in its own git), built here
  into `runtime-build\` (gitignored; `CARGO_TARGET_DIR=F:\ModProjects\MW2-RE4\runtime-build`, `CARGO_PROFILE_DEV_DEBUG=0`,
  `CARGO_PROFILE_DEV_OPT_LEVEL=1`, `CARGO_INCREMENTAL=0`, `cargo build -p launcher -j 4 --offline`; F: has little space, watch it).
  Output `runtime-build\debug\iw4l.exe` is deployed as `iw4l-runtime\iw4l-re4.exe` (v41 kept as `iw4l-re4.v41-from-mw2gta.exe`).
  Changes (env `IW4L_RE4_BRIDGE=1`, set by the plugin): god mode on the local player every tick (`SimWorld::bridge_god`);
  no saved loadout (gta-loadout.txt) and no free frag; request kind 4 = hold only `weapon` (other primaries taken, given if missing,
  ammo set, switched to) and kind 5 = set `weapon`'s clip/stock (unclamped up to 999/9999; an offhand's count is its clip and it
  replaces the same-class offhand). Clip/stock travel in Features reserved[1]/[2] (BASE+48/+52, the GTA announcer words; announcer off
  in RE4 mode). MW2-GTA's v47+ runtime already ignores the map's scripted suicide (MOD_SUICIDE 100000 at out-of-bounds spots) when
  `IW4L_GTA_BRIDGE` is set: that was the random death + loadout reset.
- **Plugin (Arsenal)**: ItemMgr 0xC70FD4 and cItemMgr::arm/erase, WeaponId2ChargeNum/WeaponNo, WeaponChange, cItem::specialTuned via
  re4_tweaks patterns. Equipped RE4 gun (`m_pWep_C`) -> MW2 gun from `kGuns` / INI [Guns]; clip = item loaded ammo (weapon_6[1] >> 3),
  reserve = ammo boxes of its type in the case. MW2 shots set the item's loaded ammo; an MW2 reload (stock drop) fills by RE4's rule up to
  WeaponId2ChargeNum(id, capacity level) and takes boxes; when already full, MW2's reserve is shown as 0 so MW2 stops reloading;
  pickups/merchant/RE4 reloads resend. Requests wait for MW2 to display the values (guest clip/stock are from the presented snapshot).
  Grenades: frag = hand + incendiary, flash = flash grenades. Rocket launcher erased after its shot (not the Infinite Launcher).
  '1' arms the next gun in the case. Hits use WeaponId2WeaponNo(item) so firepower upgrades count; F11 multipliers reset to 1.0.
  Gun list overlay while STA_SUB_SCRN is set (attache case; merchant untested). F8/F9 disabled when Re4Inventory=1.
  Not mapped: firing speed and reload speed upgrades (MW2 keeps its own rates).

## v21 (2026-10-20)
- User test of v20: god mode on, requests applied; bugs: shotgun reload loop (RE4 cap 6 < SPAS clip 8, the v20 'hide reserve' workaround showed 0 reserve and a looping left-hand reload), frag 1 / flash 2 from MW2's class never cleared (offhand count = clip + ammo row; v20 only cleared the clip).
- Runtime: `sim::world::set_bridge_clip_size(weapon, size)` (process-wide atomic, also used by client prediction) overrides the held gun's clip_size in `combat_facts_for`; kinds 4/5 carry size in Features reserved[3] (BASE+56). Offhand set also empties the ammo row.
- Plugin: sends RE4 capacity as size; an MW2 reload simply takes (agreed stock - MW2 stock) rounds from the boxes; capacity changes resend. Patch now `runtime-patches/re4-bridge.patch` (diff from snapshot 43e09d1).
- The user asked about MW2 resolution (it is 960x540 already) and then said not to change it.

## v22 (2026-10-20)
- Ashley/Luis health bar next to Leon's while MW2 has control: partner = pAS (pattern 'A8 02 74 16 8B 15' +6, ptr 0xC57060), shown while
  its beFlag bit 0 is set; HP = pG+0x4FB8 / +0x4FBA (subHpCur/subHpMax). Label LUIS for model id 0x04, else ASHLEY.
- Shooting objects: disassembly shows BulletHit (0x686C40) itself handles breakables: it ray-tests the background first (0x594160,
  walls stop the bullet), then loops EmMgr including object Ems 0x50-0x61 (box 0x53, barrel 0x58, door 0x51, window 0x56, torch 0x57,
  barred 0x5E...) with per-type handling. Leon's handgun makes no other hit call. MW2 shots already go through it with a 100 m ray,
  so objects should already break. v22 logs 'shot N hit RE4 Em id: ...' (Ems with cDmgInfo pending) for the first 80 shots to confirm.

## v23 (2026-10-21)
- Verification of v20 log: reload/pickup arithmetic was correct; the HUD mismatch was the v20 hidden reserve (gone in v21).
- `Account()` (MW2 -> RE4: shots, reloads, used-up rocket launcher) now runs first every frame, before a gun change, and also while RE4 has control (MW2 counts frozen), so the last shots before a switch or hand-over are spent.
- Self-correction: if MW2 clip/reserve differ from the case (gun loaded ammo / boxes) for 1.5 s while not firing, or frag/flash counts differ from the case for 1.5 s, the case values are sent again (log "fixing").
- Key 1 is queued while a request is in flight.

## v24 (2026-10-21)
- Class offhands: the MW2 class gave throwing knives + stun/flash; a set-to-0 for frag/flash did nothing when MW2 lacked that exact weapon. Runtime now takes every same-kind offhand (lethal 1/4/5 or tactical) before setting the host one, even at count 0.
- False "MW2 loading": after RE4 pauses (case, map, menus) the hook stops, so the guest copy looked >1.5 s old and MW2 restarted (new epoch, pitch reset). Blocker now measures guest age at the last Exchange (g_lastExchange - guest.tick).
- Activate resumes without a new epoch (same view) when Leon is within 10 cm / 15 deg of MW2 and the break was < 10 s.
- Doors: IW4 hull is +-15 in (0.76 m); against RE4 geometry the runtime scales x/y by 0.78 (0.6 m, Leon-like) in HostCollision trace/penetrations and the trap/unblock helpers (host_hull_scale, RE4 mode only).

## v25 (2026-10-21): RE4 screens get their keys and text back, gun list on F5

- Bug: the merchant, the attache case, Hunnigan's calls and the game-over screen stop RE4's camera, so
  `CameraSetHook` never saw them open and MW2 stayed in control (log: last status "MW2 ACTIVE", then silence
  until the menu closed). `PadReadHook` kept filtering RE4's keys (no arrows, rotate, Enter, mouse) and
  `MesSetHook` kept raising captions, which pushed the merchant's description up and hid the item names.
- Fix: `Re4ScreenBlocker()` (G_rno0 != 3, Leon's HP <= 0, STA_SUSPEND) checked in `PadReadHook`, plus "no camera
  frame for 4 pad reads and 200 ms". Either one calls `Restore()` right there. `MesSetHook` also skips the raise
  while one is set. MW2 comes back through the normal Activate (seamless resume when Leon has not moved).
- Gun list: hidden by default, and F5 toggles it (INI `ShowGunList`). The key is read in `DrawOverlay`, which
  keeps running while the case is open.

## v26 (2026-10-21): no Leon flash on pickups

- Bug: pressing E always suspended MW2 for at least 300 ms (ActionBlocker), and an item pickup also freezes RE4 for
  about 0.5 s (no camera frames, no RE4 screen flags). Leon was shown for that time.
- Fix: ActionBlocker suspends only once Leon's routine actually changes (jump, climb, kick). A freeze with no RE4
  screen flags no longer suspends: `PadReadHook` just stops filtering RE4's keys while it lasts. `DrawOverlay`
  keeps MW2's image on screen for up to 1.5 s when E was pressed within 1 s before the freeze (`g_lastUse`).
  Flagged screens (merchant, case, calls, game over) still hand everything back as in v25.

## Release installer (2026-10-21), modelled on MW2-GTA's

- `installer\`: Install-/Uninstall-MW2RE4.bat + .ps1, default `MW2RE4.ini` (RuntimeDir set by the installer) and
  `re4-bridge-settings.cfg`. `scripts\Build-Release.ps1 -Version x` builds `dist\MW2-RE4-x.zip` (gitignored) from
  host\build\MW2RE4.asi, runtime-build\debug\iw4l.exe, UAL dinput8.dll (third_party) and IW4L's licence files.
- Installer: finds RE4 (Steam libraries, or `-Re4Dir`, root or Bin32) and MW2 (`-Mw2Dir`); warns if bio4.exe is not
  9,139,840 bytes; installs the bundled UAL only when Bin32 has no dinput8.dll; plugin + INI into Bin32, runtime into
  `<RE4>\MW2RE4` (LOCALAPPDATA when under Program Files) with `Modern Warfare 2.lnk`; keeps the user's INI values;
  backups in `<RE4>\MW2RE4-backup\<date>`; manifest `Bin32\MW2RE4.install.json` (tracks the UAL hash across updates).
  `-Yes` unattended, `-DetectOnly` shows the folders it would use. A wrong folder given on purpose is an error.
- Tested 2026-10-21 on a test copy of the RE4 folder (never the real one): wrong folders, detect-only, fresh install,
  update, uninstall; the installed runtime loaded MW2 through the shortcut and reported ready in 73 s.
- The runtime contains `C:\Users\<name>\.cargo` paths (Rust panic locations), as MW2-GTA's release does, so the
  package is built with `-KeepLocalPaths`. Removing them needs a runtime rebuild with `--remap-path-prefix`.
- MW2 only: the runtime already carries MW2-GTA's fix (9e72af1 there: `discover::mw2_only()` when IW4L_GTA_BRIDGE is
  set, so no MW3/BO/BO2 shortcuts are linked or searched; the RE4 plugin sets that variable). The installer accepts a
  folder only when `zone\<lang>\common_mp.ff` has MW2's fastfile version 0x114 (MW3 0x1, BO 0x1d9, BO2 0x93 are
  refused). Tested with fake MW3/BO2 folders.

## v27 (2026-10-21): RE4's item pickup screen

- Bug (v26): on RE4's "take it?" pickup screen, MW2's hand stayed drawn over it (v26 even kept it for 1.5 s on
  purpose) and the screen's text stayed raised. That screen does not stop the camera hook and sets none of the
  STA_SUSPEND bits, so MW2 stayed fully in control.
- Fix: `ItemGetOpen()` = Flags_STATUS word 1 (G+0x5020) bit STA_ITEM_GET (0x2, re4_tweaks GameFlags). While it is
  set: no MW2 overlay, no caption raise, `RestorePrompts()` instead of `RaisePrompts()`, all keys to RE4
  (`PadReadHook` returns early), `host.enabled = 0`. MW2 keeps Leon (hidden), so there is no hand-over flash.
  The v26 "keep MW2's image through a pickup freeze" exception is removed. The log shows
  "RE4 item pickup screen open/closed (N ms after E)".

## v28 (2026-10-21): pickup screen text at the bottom

- v27's STA_ITEM_GET was never seen set at the "take it?" screen (no "item pickup screen open" log), so its text
  was still raised; the hand only vanished because RE4 stops for that screen (no camera frames).
- `PickupScreen()`: RE4 stopped (Flags_STOP word 0 has SPF_CAMERA/SPF_PL/SPF_EM, or STA_ITEM_GET, or no camera frame
  for 100 ms) and the stop began within 1.5 s of E. While true: captions are not raised, `RestorePrompts()` puts the
  window back (called from MesSetHook, PadReadHook and the camera hook), all keys go to RE4, host.enabled = 0.
  Other captions keep the raise. Logs: "pickup screen: RE4 stopped N ms after E (stop flags ..., status ...)",
  "pickup caption ... kept at (x, y)", "pickup screen closed".

## v29 (2026-10-21): Mine Thrower left to RE4

- The Mine Thrower's mines stick and then explode; MW2's m79 (Thumper) only fires grenades, so the mapping did not
  work. Its kGuns entries (items 54 and 171) are removed, like the P.R.L. 412, which never had one: equipping either
  keeps the last MW2 gun (and its RE4 ammo), and F6 hands them to Leon. `MineThrower=` is gone from the INIs (the
  installed INI was backed up to builds\MW2RE4.ini.before-v29.bak). README "Known limitations" says so.
- To bring it back: an MW2 projectile that sticks and detonates (e.g. the m79 round made sticky on impact in the
  runtime, then detonated by RE4's own mine logic or a timer).

## v30 (2026-10-21): first start may take minutes

- A subscriber's fresh install timed out: "MW2 loading" ran to ~200 s, then "MW2 startup timed out" just as the
  MW2 menu sound played. A fresh runtime has no `iw4l-artifacts\cache` (here 1.1 GB: mips 970 MB, wgsl 59 MB,
  ~2,100 pipelines), so the first start on a slow CPU or HDD runs past the old 180 s limit. Cache entries are
  written to a temp file and renamed (`asset_transport/src/artifact_cache.rs`), so a kill keeps what was built,
  which is why a later retry got through.
- WatchRuntime: the first-start limit is 900 s (was 180 s); the 15 s "connection lost" limit after READY is
  unchanged. After 60 s the loading text adds "the first start can take several minutes".
- Same report, not solved yet (no logs): after one successful start the player was invisible, the screen went
  full screen and back to windowed after ~5 s, then "MW2 runtime stopped". In bridge mode the runtime has no window
  (WinitPlugin disabled, `bootstrap/src/plugins.rs`), so the mode flicker is RE4's own display; it looks like a GPU
  device loss or a runtime crash. Needs their `Bin32\MW2RE4.log`, the runtime logs and the PC specs.

## v31 (2026-10-21): MW2 resolution in F10

- Like MW2-GTA's options row: F10 has MW2 RESOLUTION (640x360, 800x450, 960x540 default, 1024x576, 1280x720,
  all 16:9; the shared image holds at most 1280x720, Protocol.h MaxWidth/MaxHeight). Saved as `Resolution=WxH`
  in MW2RE4.ini (an unknown size snaps to the preset with the nearest width). StartRuntime rewrites only the
  `resolution=` line of re4-bridge-settings.cfg before launching (WriteRuntimeResolution, MW2-GTA's
  WriteGuestResolution). The RESTART MW2 row now applies a changed skin or resolution (RestartNeeded()).
- Going above 1280x720 (e.g. 1920x1080) needs a bigger shared image: Protocol.h and the runtime's MAX_WIDTH /
  MAX_HEIGHT in gta_bridge.rs together, so a runtime rebuild.

## v32 (2026-10-21): crash report

- After switching RE4 to full screen 1920x1080, RE4 crashed once while loading a save, before F6 (WER: BEX,
  c0000005, executing at 0x0F9207E8, no module; the WER dump folder needs admin). It did not happen again on the
  next try (MW2 ready after 97.5 s, no exceptions). Cause unknown.
- The plugin now reports crashes itself: a vectored handler logs the first 5 access violations / illegal
  instructions (even handled ones), and an unhandled-exception filter logs "RE4 CRASHED" with the fault, the
  registers and up to 16 code addresses from the stack (module+offset), then writes `Bin32\MW2RE4-crash.dmp`
  (MiniDumpWithIndirectlyReferencedMemory). The filter is installed at load and again at the first Present
  (bio4.exe sets its own at 0x0040C989, another DLL one later); the filter it replaced still runs after it
  (g_previousFilter), with a guard against filters calling each other. The uninstaller removes the dump.

## Subscriber report 2 (2026-10-21): MW2 never draws, then aborts

- Fresh 0.1.2 install (RE4 under Program Files, runtime in %LOCALAPPDATA%\MW2RE4, windowed 1280x720). MW2 ready
  after 258 s (the v30 limit worked). Then movement and camera ran at ~40 Hz but `frame=0 0x0` the whole time (on
  this PC frames arrive at once, ~50/s), the runtime stalled after ~12 s and exited 0xC0000409 (fail-fast abort).
- Their runtime log loads exactly like ours up to "GPU ready" and admission; it ends without an error (the abort
  message goes to stderr, which the plugin does not capture). The readback (`gta_bridge/readback.rs`) never
  completed a map_async, so the GPU most likely hung (TDR), which also fits the full-screen/windowed flicker of
  their first report. Asked for GPU model, driver and RAM.
- Tried a `Renderer=vulkan` option (WGPU_BACKEND; the exe does include Vulkan): on this PC Vulkan fails during
  loading with "Validation Error ... Texture is invalid" then "Quitting the application due to OutOfMemory
  RenderError", so it is not a workaround without runtime work. The option was reverted, not shipped.
- Idea for later: start the runtime with stderr redirected to a file (e.g. `iw4l-artifacts\logs\stderr.log`) so a
  user's abort reason reaches the logs they send.
