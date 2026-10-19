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

