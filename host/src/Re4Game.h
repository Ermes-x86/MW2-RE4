// RE4 UHD (Steam, bio4.exe 1.1.0) memory layout used by MW2RE4.
// Offsets, struct layouts and byte patterns come from re4_tweaks
// (https://github.com/nipkownix/re4_tweaks, commit 92c0208b): dllmain/SDK, Game.cpp, Trainer.cpp,
// DebugDisplay.cpp and KeyboardMouseTweaks.cpp. The camera hand-off call was found by disassembling
// CameraControl::move. Only patterns are used to find addresses; nothing here is a fixed exe address.
#pragma once
#include <cstdint>
#include <cstddef>

struct Vec3 { float x, y, z; };
static_assert(sizeof(Vec3) == 0xC, "Vec3");

// CAMERA_POINT (SDK/camera.h)
struct CameraPoint
{
    Vec3 campos;  // 0x00
    Vec3 target;  // 0x0C
    float roll;   // 0x18
    float fovy;   // 0x1C, vertical field of view in degrees
};
static_assert(sizeof(CameraPoint) == 0x20, "CameraPoint");

namespace re4
{
    // Game units are about 1 mm. Y is up; an actor facing yaw t looks along (sin t, 0, cos t).

    // cCoord / cModel / cEm / cPlayer (SDK/cCoord.h, model.h, em.h, player.h)
    constexpr size_t Unit_beFlag   = 0x04;
    constexpr size_t Coord_pos     = 0x94;  // Vec, feet position
    constexpr size_t Coord_ang     = 0xA0;  // Vec, rotation in radians (y = facing)
    constexpr size_t Model_rno0    = 0xFC;  // uint8 routine: 0 init, 1 move, 2 damage, 3 die (cEm convention)
    constexpr size_t Model_id      = 0x100; // uint8 character type id
    constexpr size_t Em_hp         = 0x324; // int16
    constexpr size_t Em_dmgFlag    = 0x32C; // cDmgInfo.m_Flag_4: a hit waiting for the enemy to process

    // GLOBAL_WK, pointed to by pG (SDK/global.h)
    constexpr size_t G_rno0        = 0x20;   // 3 = main loop
    constexpr size_t G_dispFlags   = 0x58;   // Flags_DISP word 0
    constexpr size_t G_deltaTime   = 0x70;   // 1.0 at 30 fps
    constexpr size_t G_camera      = 0x74;   // CAMERA (0xF8 bytes), final render camera
    constexpr size_t G_camPoint    = G_camera + 0xA4;
    constexpr size_t G_stopFlags   = 0x170;  // Flags_STOP word 0
    constexpr size_t G_curRoomId   = 0x4FAC; // uint16
    constexpr size_t G_plHp        = 0x4FB4; // int16
    constexpr size_t G_plHpMax     = 0x4FB6; // int16
    constexpr size_t G_plType      = 0x4FC8; // uint8, 0 = Leon
    constexpr size_t G_statusFlags = 0x501C; // Flags_STATUS word 0
    constexpr size_t G_statusFlags1 = 0x5020; // Flags_STATUS word 1
    constexpr uint32_t STA1_ITEM_GET = 0x00000002; // word 1: RE4's item pickup screen is up

    // Flag bits (GameFlags.h): flag n is bit (0x80000000 >> n) of its word.
    constexpr uint32_t DPF_PL         = 0x40000000; // set = player model hidden (seen 0 normally)
    constexpr uint32_t DPF_COCKPIT    = 0x00010000; // set = RE4 HUD hidden
    constexpr uint32_t SPF_PL         = 0x10000000; // stop player logic (used by re4_tweaks' free camera)
    constexpr uint32_t SPF_CAMERA     = 0x40000000; // stop camera logic
    constexpr uint32_t SPF_EM         = 0x20000000; // stop enemy logic
    constexpr uint32_t STA_MOVIE_ON   = 0x10000000;
    constexpr uint32_t STA_MOVIE2_ON  = 0x04000000;
    constexpr uint32_t STA_DIEDEMO    = 0x00100000;
    constexpr uint32_t STA_SUB_SCRN   = 0x00040000;
    constexpr uint32_t STA_EVENT      = 0x00001000;
    constexpr uint32_t STA_SUSPEND    = STA_MOVIE_ON | STA_MOVIE2_ON | STA_DIEDEMO | STA_SUB_SCRN | STA_EVENT;

    // KEY_BTN (SDK/pad.h): RE4's 64-bit button words Key_btn_on / Key_btn_trg.
    // While MW2 has control only these reach Leon: action/check, pick up, QTE, Ashley, inventory, map, menus.
    constexpr uint64_t KEY_ASHLEY = 0x200, KEY_CK = 0x400, KEY_ST = 0x1000, KEY_OPTION = 0x2000;
    constexpr uint64_t KEY_FACE = 0xF0000; // Y X B A
    constexpr uint64_t KEY_SSCRN = 0x100000, KEY_MAP = 0x200000;
    constexpr uint64_t KEY_EV_CANCEL = 0x20000000, KEY_CANCEL = 0x40000000, KEY_OK = 0x80000000;
    constexpr uint64_t KEY_PICK_UP = 0x4000000000, KEY_MAP_MENU = 0x8000000000;
    constexpr uint64_t KEY_ACT_A = 0x10000000000, KEY_ACT_B = 0x40000000000;
    constexpr uint64_t KEY_ALLOWED = KEY_ASHLEY | KEY_CK | KEY_ST | KEY_OPTION | KEY_FACE | KEY_SSCRN | KEY_MAP |
        KEY_EV_CANCEL | KEY_CANCEL | KEY_OK | KEY_PICK_UP | KEY_MAP_MENU | KEY_ACT_A | KEY_ACT_B;
    constexpr uint64_t KEY_USE = KEY_CK | KEY_PICK_UP | KEY_ACT_A; // what MW2's use key (E) presses in RE4
    // JOY (SDK/pad.h): analog sticks RE4 also moves Leon with
    constexpr size_t Joy_leftX = 0x0, Joy_leftY = 0x4, Joy_pcLeftX = 0x118, Joy_pcLeftY = 0x11C;

    // CAMERA (SDK/camera.h)
    constexpr size_t Camera_point  = 0xA4;

    // cManager<T> (SDK/cManager.h) and cSat, the loaded room collision (SDK/atari.h)
    constexpr size_t Mgr_array     = 0x04;
    constexpr size_t Mgr_count     = 0x08;
    constexpr size_t Mgr_stride    = 0x0C;
    constexpr size_t Sat_vert      = 0x0C;  // Vec*
    constexpr size_t Sat_poly      = 0x18;  // uint16_t (*)[10]; [0..2] vertex indices
    constexpr size_t Sat_vertNum   = 0x1C;  // uint16
    constexpr size_t Sat_polyNum   = 0x1E;  // uint16
    constexpr size_t Sat_flag      = 0x2A;  // uint8, 4 = enabled
    constexpr size_t Sat_mat       = 0x60;  // Mtx 3x4, local -> world

    // Byte patterns ('?' = any byte) and where the wanted value sits in the match.
    struct Pattern { const char* name; const char* bytes; int offset; bool sameValueOk; };

    // pG: GLOBAL_WK** at match+1 (Game.cpp)
    constexpr Pattern kGlobalPtr  { "pG",       "A1 ? ? ? ? B9 FF FF FF 7F 21 48 ? A1", 1, false };
    // pPL: cPlayer** at match+1 (Game.cpp)
    constexpr Pattern kPlayerPtr  { "pPL",      "A1 ? ? ? ? D8 CC D8 C9 D8 CA D9 5D ? D9 45 ?", 1, false };
    // Call to CameraQuasiFPS::hitCheck inside CameraQuasiFPS::calcOffset; E8 at match+0xE (Trainer.cpp)
    constexpr Pattern kHitCheckCall { "hitCheck call", "52 8D 85 ? ? ? ? 50 8D 4D ? 51 8B CB E8", 0xE, false };
    // CameraControl::move: copies the smoothed camera point into Camera_60, then calls the
    // function that builds the view matrices from it (E8 at match+0x16); Camera_60 is then
    // copied to the global render camera.
    constexpr Pattern kCameraSetCall { "camera set call", "8D 53 60 8D BB 04 01 00 00 B9 08 00 00 00 BE ? ? ? ? 52 F3 A5 E8", 0x16, false };
    // DirectInput mouse deltas, int32 written each tick (KeyboardMouseTweaks.cpp)
    constexpr Pattern kMouseX     { "mouse X",  "A3 ? ? ? ? E8 ? ? ? ? A3 ? ? ? ? 8B 85 ? ? ? ? EB ? D9 C9", 1, false };
    constexpr Pattern kMouseY     { "mouse Y",  "A3 ? ? ? ? E8 ? ? ? ? A3 ? ? ? ? 8B 85 ? ? ? ? EB ? E8", 1, false };
    // Call to Direct3DCreate9 in the renderer init; E8 at match+0 (D3D9hook.cpp)
    constexpr Pattern kD3DCreateCall { "Direct3DCreate9 call", "E8 ? ? ? ? A3 ? ? ? ? 85 C0 75 17", 0, false };
    // Key_btn_on / Key_btn_trg: uint64 at match+1 (Game.cpp)
    constexpr Pattern kKeyOn      { "Key_btn_on",  "A1 ? ? ? ? 33 C9 83 E0 ? 83 3D", 1, false };
    constexpr Pattern kKeyTrg     { "Key_btn_trg", "A1 ? ? ? ? 25 ? ? ? ? 0B C1 74 ? 88 0D", 1, false };
    // Joy[0]: pointer at match+0x10, minus 9 (Game.cpp)
    constexpr Pattern kJoy        { "Joy",      "83 E0 10 33 C9 0B C1 0F 84 ? ? ? ? 0F BE 05 ? ? ? ?", 0x10, false };
    // Call to PadRead in the main loop; E8 at match+0 (KeyboardMouseTweaks.cpp). PadRead fills the key words.
    constexpr Pattern kPadReadCall { "PadRead call", "E8 ? ? ? ? E8 ? ? ? ? 68 ? ? ? ? 6A 04 E8 ? ? ? ? 83 C4 18 E8 ? ? ? ? E8 ? ? ? ? A1", 0, false };
    // EmMgr: cManager<cEm>* at match+3, in the enemy spawn routine (push 0x51; mov ecx, EmMgr; ...; call construct).
    // re4_tweaks' EmMgr pattern matches 19 places / 3 managers in this exe.
    constexpr Pattern kEmMgr      { "EmMgr",    "6A 51 B9 ? ? ? ? 89 45 D4 E8", 3, false };
    // RE4's attack-hit routine used by Leon's guns (and ~100 other attackers): traces start->end against every
    // enemy's hit zones (YARARE: head, arms, legs, torso) and fills each hit enemy's cDmgInfo; the enemy then
    // computes damage from the weapon number and plays its own reaction. Found with the v8 damage trace:
    // Leon's handgun calls it as BulletHit(Leon, &muzzle, &muzzle + 50 m, pG->weapon_no, 0, 6000.0f).
    constexpr Pattern kBulletHit  { "BulletHit", "55 8B EC 81 EC D8 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 53 8B 5D 14 0F B7 D3", 0, false };
    // cActionButton m_active_flag: an action prompt ("press ... to open") is on screen (Misc.cpp)
    constexpr Pattern kActBtn     { "action prompt flag", "80 3D ? ? ? ? ? 75 ? 6A ? E8 ? ? ? ? 83 C4 ? EB", 2, false };
    // RE4 weapon numbers (pG->weapon_no; item -> weapon_no table at exe VA 0xB24B10)
    constexpr int WEP_HANDGUN = 0x01, WEP_BROKEN_BUTTERFLY = 0x02, WEP_SHOTGUN = 0x07, WEP_STRIKER = 0x08,
                  WEP_RIFLE_BOLT = 0x09, WEP_RIFLE_SEMI = 0x0A, WEP_TMP = 0x0B, WEP_CHICAGO = 0x0C, WEP_RPG = 0x0D,
                  WEP_KNIFE = 0x10, WEP_GRENADE = 0x13, WEP_FLASH = 0x17;
    // RE4 grenade explosion (cObj grenade, exe VA 0x73A17F): effect(0, -1, &pos, 0, 0, type, 0, 0, 0, 0) at match+0x17,
    // then sound(1, id, &pos, 0, 0, 0) at match+0x27. Grenade: effect 0x0D, sound 0x14, BulletHit(0, &pos, &pos, 0x13, 0, 6000).
    // Flash (0x73A620): effect 0x0C, sound 0x13, BulletHit(0, &pos+0.5 m up, same, 0x17, 0, 15000). Both set status flags.
    constexpr Pattern kExplosionFx { "explosion effect", "8B 55 D4 6A 00 6A 00 6A 00 6A 00 52 0F B6 C0 50 6A 00 56 6A FF 6A 00 E8 ? ? ? ? 6A 00 6A 00 6A 00 56 6A 14 6A 01 E8", 0, false };
    // A call to MessageControl::MesSet (Misc.cpp, item examine): E8 at +13 targets the jmp thunk every caller uses.
    // MesSet(this=cMes, num, px, py, attr, wk, col, font_no) __thiscall, ret 0x1C. font_no is the LAYOUT_TYPE:
    // 0 caption ("Will you use the emblem?"), 1 action button, 2 inventory, 5 system. py is screen y (down).
    constexpr Pattern kMesSetCall { "MesSet call", "68 84 00 02 00 51 53", 13, false };
    constexpr uint8_t IDC_MSG_WINDOW = 0x2F;
    // LifeDownSet2(cEm* em, int damage, int random, ...) __cdecl: subtracts damage from an enemy (or Leon, model id 0).
    // Called through a JMP thunk; the hook scales `damage` ([esp+8] on entry).
    constexpr Pattern kLifeDown   { "LifeDownSet2", "55 8B EC 53 56 57 E8 ? ? ? ? 0F B6 F0 C1 E6 08 E8 ? ? ? ? 8B 4D 10 8B 7D 0C", 0, false };
    // Leon's attache case (re4_tweaks Game.cpp, SDK/item.h). cItemMgr is a global object: address at match+0xE
    // (2 matches, same value). The functions are reached through the E8 call at the given offset.
    constexpr Pattern kItemMgr     { "ItemMgr", "80 B9 C0 4F 00 00 10 0F 84 ? ? ? ? B9", 0xE, true };
    constexpr Pattern kItemArm     { "cItemMgr::arm call", "8B 0D ? ? ? ? 51 B9 ? ? ? ? E8 ? ? ? ? 5F 5E 5B 8B E5", 12, false };
    constexpr Pattern kItemErase   { "cItemMgr::erase call", "E8 ? ? ? ? 8A 45 ? 8B 4D ? 24 ? 66 0F ? ? 8D 04 FD ? ? ? ? 66 0B ? 66 89 53", 0, false };
    constexpr Pattern kChargeNum   { "WeaponId2ChargeNum call", "E8 ? ? ? ? B9 ? ? ? ? 83 C4 ? 66 3B ? 0F 84", 0, false };
    constexpr Pattern kWeaponNo    { "WeaponId2WeaponNo call", "E8 ? ? ? ? 0F B7 15 ? ? ? ? 0F B6 C8 52 89 4D E8 E8 ? ? ? ? 0F B6 C0", 0, false };
    constexpr Pattern kWeaponChange { "WeaponChange call", "6A 01 E8 ? ? ? ? 83 C4 04 E8 ? ? ? ? F6", 0xA, false };
    constexpr Pattern kSpecialTuned { "cItem::specialTuned call", "8B CE E8 ? ? ? ? 84 C0 74 ? C6", 2, false };
    // cItemMgr: equipped weapon item, owner, item array and its last index
    constexpr size_t ItemMgr_wep = 0x0C, ItemMgr_char = 0x13, ItemMgr_items = 0x14, ItemMgr_count = 0x1C;
    // cItem (0xE bytes): id, count, flags (bit 0 = in use), owner, weapon levels (4 bits each: firepower,
    // firing speed, reload speed, capacity; stored minus one), loaded ammo << 3
    constexpr size_t Item_size = 0xE, Item_id = 0x0, Item_num = 0x2, Item_flag = 0x4, Item_chr = 0x5,
                     Item_levels = 0x6, Item_ammo = 0x8;
    // Item ids (re4_tweaks SDK/item.h EItemId)
    constexpr uint16_t ITEM_GRENADE = 1, ITEM_FLAME_GRENADE = 2, ITEM_FLASH_GRENADE = 14;
    // pAS: the partner (Ashley, or Luis) cPlayer** at match+6 (Game.cpp); her health is in GLOBAL_WK
    constexpr Pattern kPartnerPtr { "pAS", "A8 02 74 16 8B 15", 6, false };
    constexpr size_t G_subHp = 0x4FB8, G_subHpMax = 0x4FBA; // int16 (re4_tweaks subHpCur_4FB8 / subHpMax_4FBA)
    // QTE active byte (QTEFixes.cpp): RE4 is showing a quick-time button prompt
    constexpr Pattern kQteActive  { "QTE active", "C6 05 ? ? ? ? ? E8 ? ? ? ? 83 C4 ? 84 C0 0F 84", 2, false };
    // IDSystem (RE4's 2D interface): m_maxId at +0, ID_UNIT array at +0x50 (Game.cpp, SDK/ID.h)
    constexpr Pattern kIdSys      { "IDSystem", "B9 ? ? ? ? E8 ? ? ? ? 8B ? ? ? ? ? 8B C8 D9", 1, false };
    constexpr size_t IdSys_count = 0x0, IdSys_units = 0x50, IdUnit_size = 0x144;
    constexpr size_t IdUnit_class = 0x2, IdUnit_parent = 0x68, IdUnit_pos0 = 0x94;
    constexpr uint8_t IDC_ACT_BUTTON = 0x20;
    constexpr uint32_t STA_PL_FIRE = 0x00800000;       // status word 0: set by RE4 grenades
    constexpr uint32_t STA1_EXPLOSION = 0x20000000;    // status word 1 (+0x5020): set by RE4 grenades
    // SatMgr: cSatMgr* at match+0xF; 18 matches, all the same address (DebugDisplay.cpp)
    constexpr Pattern kSatMgr     { "SatMgr",   "8D 8E B4 02 00 00 E8 ? ? ? ? 6A 00 56 B9", 0xF, true };
}
