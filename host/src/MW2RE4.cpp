// MW2RE4.asi - host plugin that runs Call of Duty: Modern Warfare 2 gameplay inside RE4 UHD.
// The real MW2 runtime (IW4L, adapted in MW2-GTA, unchanged here) runs as its own process and owns
// movement, camera, weapons, animations, HUD and combat audio. This plugin feeds it RE4's room
// collision and the player's input, then makes Leon and RE4's camera follow it and draws MW2's
// weapon/HUD image over RE4. Shared memory protocol v4, same as MW2-GTA.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Protocol.h"
#include "Re4Game.h"

namespace
{
using namespace mw2gta;
constexpr const char* kVersion = "MW2RE4 v43 (MW2 frames synced to RE4)";
constexpr float Pi = 3.14159265358979323846f;

// ---------------------------------------------------------------- log
FILE* g_log = nullptr;
CRITICAL_SECTION g_logLock;
char g_dir[MAX_PATH]; // folder of the ASI, with trailing backslash

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_logLock);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

void OpenLog()
{
    InitializeCriticalSection(&g_logLock);
    char path[MAX_PATH], prev[MAX_PATH];
    snprintf(path, sizeof(path), "%sMW2RE4.log", g_dir);
    snprintf(prev, sizeof(prev), "%sMW2RE4.prev.log", g_dir);
    MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    g_log = _fsopen(path, "w", _SH_DENYWR);
}

// ---------------------------------------------------------------- crash report
// When RE4 crashes, MW2RE4.log gets the fault, the registers and the code addresses found on the stack (module
// + offset), and MW2RE4-crash.dmp a small minidump. The first access violations are logged even when something
// handles them, since a crash may start as one.
const char* ModuleOf(uintptr_t address, uintptr_t* offset)
{
    static char name[MAX_PATH];
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &module) || !module || !GetModuleFileNameA(module, name, MAX_PATH))
        return nullptr;
    *offset = address - (uintptr_t)module;
    const char* slash = strrchr(name, '\\');
    return slash ? slash + 1 : name;
}

bool IsCode(uintptr_t address)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery((void*)address, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

void LogAddress(const char* what, uintptr_t address)
{
    uintptr_t offset = 0;
    const char* module = ModuleOf(address, &offset);
    if (module)
        Log("  %s %08X = %s+0x%X", what, (unsigned)address, module, (unsigned)offset);
    else
        Log("  %s %08X (no module%s)", what, (unsigned)address, IsCode(address) ? ", executable memory" : "");
}

void LogException(EXCEPTION_POINTERS* info, const char* kind)
{
    EXCEPTION_RECORD* r = info->ExceptionRecord;
    CONTEXT* c = info->ContextRecord;
    Log("%s: exception 0x%08lX at %08X, thread %lu", kind, r->ExceptionCode, (unsigned)(uintptr_t)r->ExceptionAddress,
        GetCurrentThreadId());
    LogAddress("fault", (uintptr_t)r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        Log("  %s address %08X", r->ExceptionInformation[0] == 8 ? "executing" : r->ExceptionInformation[0] ? "writing" : "reading",
            (unsigned)r->ExceptionInformation[1]);
    Log("  eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX", c->Eax, c->Ebx, c->Ecx,
        c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    // return addresses: stack words that point into code
    int found = 0;
    __try
    {
        for (uintptr_t* sp = (uintptr_t*)c->Esp; found < 16 && sp < (uintptr_t*)c->Esp + 2048; ++sp)
            if (*sp > 0x10000 && IsCode(*sp))
            {
                char what[32];
                snprintf(what, sizeof(what), "stack+%03X", (unsigned)((uint8_t*)sp - (uint8_t*)c->Esp));
                LogAddress(what, *sp);
                ++found;
            }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

LONG WINAPI CrashVectored(EXCEPTION_POINTERS* info)
{
    static volatile LONG logged = 0;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         code == EXCEPTION_PRIV_INSTRUCTION) && InterlockedIncrement(&logged) <= 5)
        LogException(info, "first-chance");
    return EXCEPTION_CONTINUE_SEARCH;
}

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr; // RE4's (or Steam's) filter: still runs after ours
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
    static volatile LONG inside = 0;
    if (InterlockedExchange(&inside, 1)) // a chained filter that calls back into us
        return EXCEPTION_CONTINUE_SEARCH;
    LogException(info, "RE4 CRASHED");
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sMW2RE4-crash.dmp", g_dir);
    if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll"))
        if (auto write = (MiniDumpWriteDumpFn)GetProcAddress(dbghelp, "MiniDumpWriteDump"))
        {
            HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                struct { DWORD thread; EXCEPTION_POINTERS* pointers; BOOL client; } exception = { GetCurrentThreadId(), info, FALSE };
                // MiniDumpWithIndirectlyReferencedMemory: small, but holds the memory the registers point at
                BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, 0x40, &exception, nullptr, nullptr);
                CloseHandle(file);
                Log(ok ? "crash dump written: %s" : "crash dump failed: %s", path);
            }
        }
    return g_previousFilter ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

// Something else (the CRT, Steam) may install its own filter later; put ours back on top.
void InstallCrashFilter()
{
    LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(CrashFilter);
    static bool vectored = false;
    if (!vectored)
    {
        vectored = true;
        AddVectoredExceptionHandler(0, CrashVectored);
    }
    if (previous && previous != CrashFilter)
    {
        g_previousFilter = previous;
        Log("crash report: on top of another crash filter (%p), which still runs after it", (void*)previous);
    }
}

// ---------------------------------------------------------------- settings
struct Settings
{
    std::string runtimeDir = "F:\\ModProjects\\MW2-RE4\\iw4l-runtime";
    std::string runtimeExe = "iw4l-re4.exe";
    std::string settingsFile = "re4-bridge-settings.cfg";
    std::string mappingName = "MW2RE4_v1";
    int toggleKey = VK_F6;
    float fov = 90.f;              // MW2 field of view setting
    float unitsPerMeter = 1000.f;  // RE4 units per metre
    float mouseScale = 1.f;        // multiplies RE4's mouse deltas before MW2's own sensitivity
    float collisionRadius = 14.f;  // metres around the player sent to MW2
    int hidePlayer = 1;
    int invertMouseY = 0;
    int showControls = 1;
    int showGunList = 0;       // F5: RE4 -> MW2 gun list in the attache case and the merchant
    int shopGunNames = 1;
    int syncFrames = 1;        // MW2 draws one frame per RE4 frame (0 = its own 60 fps timer; takes effect at start)      // the merchant always shows the MW2 name of each RE4 gun (F5 hides it there)
    int damageTrace = 0; // research: log which RE4 code writes enemy damage/HP (RE4 mode only)
    int grenadeTrace = 0; // research: log RE4's own effect / sound / blast calls while MW2 is off
    int incendiary = 0;   // MW2's frag (G) throws RE4's incendiary grenades instead of hand grenades (F8)
    float promptRaise = 200.f; // RE4 2D units the "press ... to" prompts move up while MW2 has control
    float messageRaise = 150.f; // RE4 screen units captions ("Will you use the emblem?") move up while MW2 has control
    // Damage multiplier per MW2 gun class (F11), applied to the damage RE4 computes for the class's RE4 gun
    float damage[6] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    int re4Inventory = 1;      // MW2's gun and ammo come from Leon's attache case (0 = MW2's own, F8 menu)
    int infiniteAmmo = 0;      // keep MW2's reserve ammo topped up (MW2's own resupply)
    int skin = 0;              // index into kSkins: the MW2 map whose soldier kit (arms) MW2 uses
    int resolution = 2;        // index into kResolutions: the size MW2 renders at
} g_cfg;

// MW2's render size, all 16:9 like RE4. The shared image holds up to 1280x720 (Protocol.h). Larger is sharper
// and costs more GPU time; RE4 stretches the image to its own screen. Takes effect when MW2 starts.
struct Resolution { uint32_t w, h; };
const Resolution kResolutions[] = { { 640, 360 }, { 800, 450 }, { 960, 540 }, { 1024, 576 }, { 1280, 720 } };
constexpr int kResolutionCount = sizeof(kResolutions) / sizeof(kResolutions[0]);

// MW2 picks the player's arms and body from the map's soldier kit, so the skin is chosen by the map MW2
// loads (only its weapons and soldiers are used; RE4 supplies the world). Takes effect when MW2 starts.
// Leon: mp_subbase's two kits stand for Leon's two looks in RE4 (thick arctic arms = leather jacket, slim SEAL
// wetsuit arms = short sleeves and bare forearms), repainted by the runtime from the player's own MW2 images
// (IW4L_RE4_LEON_STYLE). The runtime shows the jacket all game for now (re4_leon_style::BARE_LOOK); LeonLook is
// still reported.
struct Skin { const char* name; const char* map; bool leon; };
const Skin kSkins[] = {
    { "Task Force 141 (desert)", "mp_rust", false },
    { "Task Force 141 (urban)", "mp_favela", false },
    { "Task Force 141 (snow)", "mp_derail", false },
    { "US Army Rangers", "mp_invasion", false },
    { "Navy SEALs", "mp_subbase", false },
    { "Leon (jacket)", "mp_subbase", true },
};
constexpr int kSkinCount = sizeof(kSkins) / sizeof(kSkins[0]);

std::string IniString(const char* path, const char* key, const std::string& def)
{
    char buf[MAX_PATH];
    GetPrivateProfileStringA("MW2RE4", key, def.c_str(), buf, sizeof(buf), path);
    return buf;
}

float IniFloat(const char* path, const char* key, float def)
{
    char buf[64], defBuf[64];
    snprintf(defBuf, sizeof(defBuf), "%g", def);
    GetPrivateProfileStringA("MW2RE4", key, defBuf, buf, sizeof(buf), path);
    return (float)atof(buf);
}

void LoadSettings()
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sMW2RE4.ini", g_dir);
    g_cfg.runtimeDir = IniString(path, "RuntimeDir", g_cfg.runtimeDir);
    g_cfg.runtimeExe = IniString(path, "RuntimeExecutable", g_cfg.runtimeExe);
    g_cfg.settingsFile = IniString(path, "RuntimeSettings", g_cfg.settingsFile);
    g_cfg.mappingName = IniString(path, "MappingName", g_cfg.mappingName);
    g_cfg.toggleKey = GetPrivateProfileIntA("MW2RE4", "ToggleKey", g_cfg.toggleKey, path);
    g_cfg.fov = std::clamp(IniFloat(path, "FOV", g_cfg.fov), 50.f, 120.f);
    g_cfg.unitsPerMeter = std::clamp(IniFloat(path, "UnitsPerMeter", g_cfg.unitsPerMeter), 10.f, 10000.f);
    g_cfg.mouseScale = std::clamp(IniFloat(path, "MouseScale", g_cfg.mouseScale), 0.01f, 100.f);
    g_cfg.collisionRadius = std::clamp(IniFloat(path, "CollisionRadius", g_cfg.collisionRadius), 4.f, 40.f);
    g_cfg.hidePlayer = GetPrivateProfileIntA("MW2RE4", "HideLeon", g_cfg.hidePlayer, path);
    g_cfg.invertMouseY = GetPrivateProfileIntA("MW2RE4", "InvertMouseY", g_cfg.invertMouseY, path);
    g_cfg.showControls = GetPrivateProfileIntA("MW2RE4", "ShowControls", g_cfg.showControls, path);
    g_cfg.showGunList = GetPrivateProfileIntA("MW2RE4", "ShowGunList", g_cfg.showGunList, path);
    g_cfg.shopGunNames = GetPrivateProfileIntA("MW2RE4", "ShopGunNames", g_cfg.shopGunNames, path);
    g_cfg.syncFrames = GetPrivateProfileIntA("MW2RE4", "SyncFrames", g_cfg.syncFrames, path);
    g_cfg.damageTrace = GetPrivateProfileIntA("MW2RE4", "DamageTrace", g_cfg.damageTrace, path);
    g_cfg.grenadeTrace = GetPrivateProfileIntA("MW2RE4", "GrenadeTrace", g_cfg.grenadeTrace, path);
    g_cfg.incendiary = GetPrivateProfileIntA("MW2RE4", "Incendiary", g_cfg.incendiary, path) != 0;
    g_cfg.promptRaise = IniFloat(path, "PromptRaise", g_cfg.promptRaise);
    g_cfg.messageRaise = IniFloat(path, "MessageRaise", g_cfg.messageRaise);
    for (int i = 0; i < 6; ++i)
    {
        char key[16];
        snprintf(key, sizeof(key), "Damage%d", i);
        g_cfg.damage[i] = std::clamp(IniFloat(path, key, g_cfg.damage[i]), .1f, 5.f);
    }
    g_cfg.infiniteAmmo = GetPrivateProfileIntA("MW2RE4", "InfiniteAmmo", g_cfg.infiniteAmmo, path);
    g_cfg.re4Inventory = GetPrivateProfileIntA("MW2RE4", "Re4Inventory", g_cfg.re4Inventory, path);
    g_cfg.skin = std::clamp((int)GetPrivateProfileIntA("MW2RE4", "Skin", g_cfg.skin, path), 0, kSkinCount - 1);
    // Resolution=WxH; anything else becomes the preset with the nearest width
    unsigned rw = 0, rh = 0;
    if (sscanf(IniString(path, "Resolution", "960x540").c_str(), "%ux%u", &rw, &rh) == 2)
        for (int i = 0; i < kResolutionCount; ++i)
            if (abs((int)kResolutions[i].w - (int)rw) < abs((int)kResolutions[g_cfg.resolution].w - (int)rw))
                g_cfg.resolution = i;
    Log("settings: RuntimeDir=%s RuntimeExecutable=%s RuntimeSettings=%s MappingName=%s ToggleKey=0x%X FOV=%.1f "
        "UnitsPerMeter=%.1f MouseScale=%.2f CollisionRadius=%.1f HideLeon=%d InvertMouseY=%d",
        g_cfg.runtimeDir.c_str(), g_cfg.runtimeExe.c_str(), g_cfg.settingsFile.c_str(), g_cfg.mappingName.c_str(),
        g_cfg.toggleKey, g_cfg.fov, g_cfg.unitsPerMeter, g_cfg.mouseScale, g_cfg.collisionRadius, g_cfg.hidePlayer,
        g_cfg.invertMouseY);
}

// ---------------------------------------------------------------- pattern scan
struct Module
{
    uint8_t* base = nullptr;
    struct Range { uint8_t* begin; size_t size; };
    std::vector<Range> code;
};

Module ReadModule()
{
    Module m;
    m.base = (uint8_t*)GetModuleHandleA(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)m.base;
    auto nt = (IMAGE_NT_HEADERS*)(m.base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            m.code.push_back({ m.base + sec->VirtualAddress, sec->Misc.VirtualSize });
    return m;
}

std::vector<uint8_t*> Scan(const Module& m, const char* text)
{
    std::vector<int> pat;
    for (const char* p = text; *p;)
    {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { pat.push_back(-1); ++p; continue; }
        pat.push_back((int)strtoul(p, nullptr, 16));
        p += 2;
    }
    std::vector<uint8_t*> hits;
    for (auto& r : m.code)
        for (size_t i = 0; r.size >= pat.size() && i + pat.size() <= r.size; ++i)
        {
            size_t k = 0;
            while (k < pat.size() && (pat[k] < 0 || r.begin[i + k] == (uint8_t)pat[k]))
                ++k;
            if (k == pat.size())
                hits.push_back(r.begin + i);
        }
    return hits;
}

// Returns the address of the wanted value (match + offset), or null.
uint8_t* Find(const Module& m, const re4::Pattern& p)
{
    auto hits = Scan(m, p.bytes);
    bool ok = hits.size() == 1;
    if (p.sameValueOk && hits.size() > 1)
    {
        ok = true;
        for (auto* h : hits)
            ok = ok && *(uint32_t*)(h + p.offset) == *(uint32_t*)(hits[0] + p.offset);
    }
    if (!ok)
    {
        Log("pattern %s: %zu matches (need exactly 1)", p.name, hits.size());
        return nullptr;
    }
    Log("pattern %s: found at bio4.exe+0x%X (%zu match%s)", p.name, (unsigned)(hits[0] - m.base), hits.size(),
        hits.size() == 1 ? "" : "es, same value");
    return hits[0] + p.offset;
}

bool HookCall(uint8_t* site, void* hook, void** orig)
{
    if (site[0] != 0xE8)
    {
        Log("hook: expected CALL (E8) at bio4.exe+0x%X, found %02X",
            (unsigned)(site - (uint8_t*)GetModuleHandleA(nullptr)), site[0]);
        return false;
    }
    *orig = site + 5 + *(int32_t*)(site + 1);
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
        return false;
    *(int32_t*)(site + 1) = (int32_t)((uint8_t*)hook - (site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

// The thunk is a JMP rel32 shared by every caller: point it at the hook.
bool HookThunk(uint8_t* thunk, void* hook, void** orig)
{
    if (thunk[0] != 0xE9)
    {
        Log("hook: expected JMP (E9) thunk at bio4.exe+0x%X, found %02X",
            (unsigned)(thunk - (uint8_t*)GetModuleHandleA(nullptr)), thunk[0]);
        return false;
    }
    *orig = thunk + 5 + *(int32_t*)(thunk + 1);
    DWORD old;
    if (!VirtualProtect(thunk, 5, PAGE_EXECUTE_READWRITE, &old))
        return false;
    *(int32_t*)(thunk + 1) = (int32_t)((uint8_t*)hook - (thunk + 5));
    VirtualProtect(thunk, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), thunk, 5);
    return true;
}

// ---------------------------------------------------------------- game access
uint8_t** g_pG = nullptr;      // GLOBAL_WK**
uint8_t** g_pPL = nullptr;     // cPlayer**
uint8_t** g_pAS = nullptr;     // cPlayer** partner (Ashley / Luis)
int32_t* g_mouseX = nullptr;   // DirectInput mouse deltas
int32_t* g_mouseY = nullptr;
uint8_t* g_satMgr = nullptr;   // cSatMgr*
uint64_t* g_keyOn = nullptr;   // RE4 button words
uint64_t* g_keyTrg = nullptr;
uint8_t* g_joy = nullptr;      // JOY[0]
uint8_t* g_emMgr = nullptr;    // cManager<cEm>*
uint8_t* g_actBtnActive = nullptr; // bool: RE4 action prompt on screen
using BulletHitFn = int(__cdecl*)(void* attacker, Vec3* start, Vec3* end, int weaponNo, int flags, float f);
BulletHitFn g_bulletHit = nullptr;
using EffectFn = void(__cdecl*)(int, int, Vec3*, int, int, int, int, int, int, int);
using SoundFn = void(__cdecl*)(int, int, Vec3*, int, int, int);
EffectFn g_effect = nullptr;
SoundFn g_sound = nullptr;
using FireAreaFn = int(__fastcall*)(uint8_t* list, void* edx, int type, int kind, Vec3* pos, float radius, float height);
FireAreaFn g_fireArea = nullptr;
uint8_t* g_fireList = nullptr;
uint8_t* g_qteActive = nullptr;
uint8_t* g_idSys = nullptr;
uint8_t* g_textBegin = nullptr, *g_textEnd = nullptr; // bio4.exe code range, for stack walks

template <class T> T& At(uint8_t* base, size_t off) { return *(T*)(base + off); }
uint8_t* Global() { return g_pG ? *g_pG : nullptr; }
uint8_t* Player()
{
    if (!g_pPL || !*g_pPL)
        return nullptr;
    if (**(uintptr_t**)g_pPL < 0x10000) // vtable sanity check, as re4_tweaks does
        return nullptr;
    return *g_pPL;
}

// Ashley (or Luis) while she is with Leon, else null.
uint8_t* Partner()
{
    if (!g_pAS || !*g_pAS || **(uintptr_t**)g_pAS < 0x10000)
        return nullptr;
    uint8_t* p = *g_pAS;
    if (!(At<uint32_t>(p, re4::Unit_beFlag) & 1))
        return nullptr;
    return p;
}

// Leon's look for Leon-style arms: 1 = arms in sleeves (jacket, also the RPD and mafia costumes), 2 = bare
// forearms (no jacket, vest costume), 0 = not Leon. GLOBAL_WK plCostume follows plType (re4_tweaks SDK).
uint32_t LeonLook()
{
    uint8_t* g = Global();
    if (!g)
        return 0;
    uint8_t type = At<uint8_t>(g, re4::G_plType), costume = At<uint8_t>(g, re4::G_plCostume);
    static int s_type = -1, s_costume = -1;
    if (type != s_type || costume != s_costume)
    {
        Log("RE4 player type %u costume %u", type, costume);
        s_type = type;
        s_costume = costume;
    }
    if (type != 0)
        return 0;
    return costume == 1 || costume == 2 ? 2 : 1;
}

// RE4 (mm, Y up) <-> bridge host space (metres, Z up, same axes as GTA/IW4). A proper rotation,
// so triangle winding and handedness are preserved.
void ToHost(const Vec3& v, float out[3])
{
    out[0] = v.x / g_cfg.unitsPerMeter;
    out[1] = -v.z / g_cfg.unitsPerMeter;
    out[2] = v.y / g_cfg.unitsPerMeter;
}
Vec3 ToRe4(const float h[3]) { return { h[0] * g_cfg.unitsPerMeter, h[2] * g_cfg.unitsPerMeter, -h[1] * g_cfg.unitsPerMeter }; }
Vec3 DirToRe4(const float h[3]) { return { h[0], h[2], -h[1] }; }

// ---------------------------------------------------------------- bridge state
HANDLE g_mapping = nullptr, g_controlMutex = nullptr, g_frameMutex = nullptr, g_sceneMutex = nullptr;
HANDLE g_tickEvent = nullptr; // set once per RE4 frame: MW2 renders a frame for each (SyncFrames)
uint8_t* g_shared = nullptr;
PROCESS_INFORMATION g_child{};
Host g_host{};
Guest g_guest{};
HostMovement g_hostMovement{};
GuestMovement g_guestMovement{};
GuestCamera g_guestCamera{};
bool g_requested = false, g_active = false;
DWORD g_runtimeStarted = 0, g_unreadySince = 0, g_lastTime = 0;
bool g_runtimeWasReady = false;
DWORD g_lastExchange = 0;   // when Exchange last copied the guest state
DWORD g_inactiveSince = 0;  // when MW2 last handed Leon back to RE4
std::string g_runtimeError;
float g_yaw = 0, g_pitch = 0, g_mouseTotalX = 0, g_mouseTotalY = 0;
uint32_t g_lastShot = 0, g_shotsSeen = 0, g_sceneSequence = 0;
DWORD g_lastScene = 0;
uint32_t g_sceneCount = 0;
bool g_sceneOverflow = false;
uint32_t g_savedStop = 0, g_savedDisp = 0;
bool g_keyWasDown = false;
bool g_helpKeyWasDown = false;
// Weapon menu (F8): MW2's own catalog, filtered to what fits RE4. F9 refills ammo. With Re4Inventory=1 it picks
// which MW2 gun stands for the RE4 gun Leon has equipped (saved to [Guns]); ammo stays RE4's.
Features g_features{};
std::vector<CatalogEntry> g_catalog;   // filtered, as shown
std::vector<CatalogEntry> g_catalogAll; // everything MW2 sent, for naming shots
uint32_t g_catalogSource = 0;          // MW2 catalog size the filtered list was built from
bool g_weaponMenu = false;
bool g_settingsMenu = false;
bool g_damageMenu = false;
int g_damageRow = 0;
int g_settingsRow = 0;
bool MenuOpen() { return g_weaponMenu || g_settingsMenu || g_damageMenu; }
size_t g_selected = 0;
std::vector<size_t> g_pickList; // Re4Inventory=1: the g_catalog guns the F8 picker offers
bool PickerStart();
void PickerApply(size_t entry);
std::string PickerTitle();
std::string PickerName(size_t entry);
bool PickerIsCurrent(size_t entry);
uint32_t g_request = 0, g_requestKind = 0, g_requestWeapon = 0;
int32_t g_requestClip = 0, g_requestStock = 0, g_requestSize = 0;
DWORD g_requestTime = 0;
std::string g_notice;
DWORD g_noticeUntil = 0;
bool g_prevKeys[256]{};
float g_mousePendingX = 0, g_mousePendingY = 0; // RE4 mouse movement collected in PadRead, consumed by Exchange
int g_lastHp = -1;
DWORD g_damageUntil = 0;
float g_damageStrength = 0;
volatile LONG g_hitCheckFrame = 0; // frame number when the over-the-shoulder camera last ran
LONG g_frame = 0;
DWORD g_lastTick = 0; // last frame RE4 ran gameplay (camera); stale = paused or in a menu

struct Lock
{
    HANDLE handle;
    bool acquired;
    explicit Lock(HANDLE h) : handle(h), acquired(false)
    {
        if (h) { DWORD s = WaitForSingleObject(h, 0); acquired = s == WAIT_OBJECT_0 || s == WAIT_ABANDONED; }
    }
    ~Lock() { if (acquired) ReleaseMutex(handle); }
};

std::wstring Wide(const std::string& s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n)
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool CreateBridge()
{
    std::wstring name = L"Local\\" + Wide(g_cfg.mappingName);
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, MappingSize, name.c_str());
    if (!g_mapping)
        return false;
    bool existing = GetLastError() == ERROR_ALREADY_EXISTS;
    g_shared = (uint8_t*)MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, MappingSize);
    g_controlMutex = CreateMutexW(nullptr, FALSE, (name + L"_control").c_str());
    g_frameMutex = CreateMutexW(nullptr, FALSE, (name + L"_frame").c_str());
    g_sceneMutex = CreateMutexW(nullptr, FALSE, (name + L"_scene").c_str());
    if (g_cfg.syncFrames && !g_tickEvent)
        g_tickEvent = CreateEventW(nullptr, FALSE, FALSE, (name + L"_tick").c_str()); // auto-reset
    if (!g_shared || !g_controlMutex || !g_frameMutex || !g_sceneMutex)
        return false;
    Lock lock(g_controlMutex);
    if (!lock.acquired)
        return false;
    if (!existing)
        ZeroMemory(g_shared, MappingSize);
    auto* c = (Control*)g_shared;
    c->magic = Magic;
    c->version = Version;
    c->host.tick = GetTickCount();
    c->host.shutdown = 0;
    ((Features*)(g_shared + FeaturesOffset))->reserved[4] = GetCurrentProcessId();
    return true;
}

int g_runtimeSkin = -1; // skin the running MW2 was started with
int g_runtimeResolution = -1; // resolution the running MW2 was started with

// MW2 reads its render size from the runtime settings file: rewrite only its resolution line, keeping the rest.
void WriteRuntimeResolution(const std::wstring& path)
{
    FILE* in = _wfopen(path.c_str(), L"rb");
    if (!in)
        return;
    std::string text;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
        text.append(buffer, n);
    fclose(in);
    const Resolution& r = kResolutions[g_cfg.resolution];
    std::string wanted = "resolution=" + std::to_string(r.w) + "x" + std::to_string(r.h);
    size_t at = text.find("resolution=");
    if (at == std::string::npos)
        return;
    size_t end = text.find_first_of("\r\n", at);
    if (end == std::string::npos)
        end = text.size();
    if (text.compare(at, end - at, wanted) == 0)
        return;
    text.replace(at, end - at, wanted);
    FILE* out = _wfopen(path.c_str(), L"wb");
    if (!out)
        return;
    fwrite(text.data(), 1, text.size(), out);
    fclose(out);
    Log("MW2 resolution set: %s", wanted.c_str());
}

bool StartRuntime()
{
    if (g_child.hProcess)
    {
        if (WaitForSingleObject(g_child.hProcess, 0) == WAIT_TIMEOUT)
            return true;
        CloseHandle(g_child.hProcess);
        CloseHandle(g_child.hThread);
        g_child = {};
    }
    std::wstring root = Wide(g_cfg.runtimeDir), exe = root + L"\\" + Wide(g_cfg.runtimeExe);
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        g_runtimeError = "MW2 runtime not found";
        Log("MW2 runtime not found: %s\\%s", g_cfg.runtimeDir.c_str(), g_cfg.runtimeExe.c_str());
        return false;
    }
    WriteRuntimeResolution(root + L"\\" + Wide(g_cfg.settingsFile));
    g_runtimeResolution = g_cfg.resolution;
    SetEnvironmentVariableW(L"IW4L_GTA_BRIDGE", Wide(g_cfg.mappingName).c_str());
    SetEnvironmentVariableW(L"IW4L_RE4_BRIDGE", L"1"); // runtime: god mode, RE4 owns guns and ammo
    SetEnvironmentVariableW(L"WGPU_BACKEND", L"dx12");
    SetEnvironmentVariableW(L"IW4L_PRESENT_MODE", L"AutoNoVsync");
    SetEnvironmentVariableW(L"IW4L_SETTINGS_PATH", (root + L"\\" + Wide(g_cfg.settingsFile)).c_str());
    SetEnvironmentVariableW(L"IW4L_PIPELINED_RENDERING", L"0");
    SetEnvironmentVariableW(L"IW4L_SCRIPT_DVARS", L"set scr_dm_timelimit 0; set scr_dm_scorelimit 0; set scr_war_timelimit 0; set scr_war_scorelimit 0");
    SetEnvironmentVariableW(L"IW4L_IMAGE_DECODE_BUDGET_MIB", L"128");
    SetEnvironmentVariableW(L"IW4L_RE4_LEON_STYLE", kSkins[g_cfg.skin].leon ? L"1" : nullptr);
    // the player's own paintings of the arms images (<image>.png), used instead of the automatic Leon paint
    SetEnvironmentVariableW(L"IW4L_RE4_LEON_PAINT", (root + L"\\leon-paint").c_str());
    std::wstring command = L"\"" + exe + L"\" --cmds \"wait progression; unlock all; map " + Wide(kSkins[g_cfg.skin].map) +
                           L"; wait world; spawn 0; force_match_start\" menu";
    g_runtimeSkin = g_cfg.skin;
    Log("MW2 skin: %s (map %s)", kSkins[g_cfg.skin].name, kSkins[g_cfg.skin].map);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    bool okay = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                               root.c_str(), &startup, &g_child) != 0;
    SetEnvironmentVariableW(L"IW4L_GTA_BRIDGE", nullptr);
    SetEnvironmentVariableW(L"IW4L_RE4_BRIDGE", nullptr);
    if (okay)
    {
        g_runtimeStarted = GetTickCount();
        g_unreadySince = 0;
        g_runtimeWasReady = false;
        g_runtimeError.clear();
        g_guest = {};
        g_lastShot = 0;
        Lock lock(g_controlMutex);
        if (lock.acquired)
        {
            ((Control*)g_shared)->guest = {};
            ZeroMemory(g_shared + FeaturesOffset, MappingSize - FeaturesOffset);
            ((Features*)(g_shared + FeaturesOffset))->reserved[4] = GetCurrentProcessId();
        }
        Log("Started MW2 runtime pid %lu", g_child.dwProcessId);
    }
    else
    {
        g_runtimeError = "MW2 start failed";
        Log("MW2 runtime start failed (%lu)", GetLastError());
    }
    return okay;
}

void Restore();

void WatchRuntime()
{
    if (!g_requested || !g_child.hProcess)
        return;
    DWORD code = 0;
    if (GetExitCodeProcess(g_child.hProcess, &code) && code != STILL_ACTIVE)
    {
        g_runtimeError = "MW2 runtime stopped";
        Log("MW2 runtime stopped (exit code %lu). F6 to retry", code);
        g_requested = false;
        Restore();
        return;
    }
    DWORD now = GetTickCount();
    if (g_guest.ready && now - g_guest.tick < 1500)
    {
        if (!g_runtimeWasReady)
            Log("MW2 ready after %.1f s", (now - g_runtimeStarted) / 1000.f);
        g_runtimeWasReady = true;
        g_unreadySince = 0;
        return;
    }
    if (!g_unreadySince)
        g_unreadySince = now;
    // The first start on a PC builds about 1 GB of texture cache and compiles the shaders, which takes minutes
    // on a slow CPU or disk. The cache survives a kill, but killing it early only makes the player wait longer.
    if (now - g_unreadySince < (g_runtimeWasReady ? 15000u : 900000u))
        return;
    g_runtimeError = g_runtimeWasReady ? "MW2 connection lost" : "MW2 startup timed out";
    Log("%s. F6 to retry", g_runtimeError.c_str());
    TerminateProcess(g_child.hProcess, 1);
    g_requested = false;
    Restore();
}

BOOL CALLBACK MoveGuestWindow(HWND window, LPARAM pid)
{
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == (DWORD)pid && GetWindow(window, GW_OWNER) == nullptr)
    {
        RECT bounds{};
        GetWindowRect(window, &bounds);
        if (bounds.left > -9000)
        {
            SetWindowPos(window, HWND_BOTTOM, -10000, 0, 800, 600, SWP_NOACTIVATE | SWP_NOSIZE);
            ShowWindow(window, SW_SHOWNOACTIVATE);
        }
        return FALSE;
    }
    return TRUE;
}

// ---------------------------------------------------------------- collision
// Room collision: RE4's loaded SAT polygons (floors, slopes, walls) near the player, in host space.
Triangle g_triangles[MaxTriangles];

// Whether the triangle w (RE4 coordinates) covers the point (x, z) seen from above.
bool OverPoint(const Vec3 w[3], float x, float z)
{
    auto side = [&](const Vec3& a, const Vec3& b) { return (b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x); };
    float d0 = side(w[0], w[1]), d1 = side(w[1], w[2]), d2 = side(w[2], w[0]);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}

// MW2 surface types (IW4 surfaceFlags bits 20-24) for footsteps.
constexpr uint32_t kSurfConcrete = 5, kSurfDirt = 6, kSurfRock = 17, kSurfWood = 21;

// The MW2 floor material for Leon's footsteps: village (rooms 1xx) wood indoors and dirt outdoors, castle (2xx)
// stone, island (3xx) concrete indoors and dirt outdoors; anything else concrete or dirt.
uint32_t FloorMaterial(bool roofed)
{
    uint8_t* g = Global();
    uint32_t room = g ? At<uint16_t>(g, re4::G_curRoomId) : 0, area = room >> 8;
    uint32_t material;
    if (area == 1)
        material = roofed ? kSurfWood : kSurfDirt;
    else if (area == 2)
        material = roofed ? kSurfConcrete : kSurfRock;
    else
        material = roofed ? kSurfConcrete : kSurfDirt;
    static uint32_t s_room = ~0u, s_material = ~0u;
    if (room != s_room || material != s_material)
    {
        static const char* names[32] = { nullptr };
        names[kSurfConcrete] = "concrete"; names[kSurfDirt] = "dirt"; names[kSurfRock] = "rock"; names[kSurfWood] = "wood";
        Log("footsteps: room %03X %s -> %s", room, roofed ? "indoors" : "outdoors", names[material]);
        s_room = room;
        s_material = material;
    }
    return material;
}

uint32_t GatherCollision(const Vec3& center)
{
    uint32_t n = 0;
    bool roofed = false;
    const float r = g_cfg.collisionRadius * g_cfg.unitsPerMeter, rv = 6.f * g_cfg.unitsPerMeter;
    const float head = 1.9f * g_cfg.unitsPerMeter; // anything over Leon's head counts as a ceiling
    __try
    {
        uint8_t* mgr = g_satMgr;
        uint8_t* array = At<uint8_t*>(mgr, re4::Mgr_array);
        uint32_t count = At<uint32_t>(mgr, re4::Mgr_count), stride = At<uint32_t>(mgr, re4::Mgr_stride);
        if (!array || count > 4096 || stride < 0xC0 || stride > 0x1000)
            return 0;
        for (uint32_t s = 0; s < count && n < MaxTriangles; ++s)
        {
            uint8_t* sat = array + s * stride;
            if ((At<uint32_t>(sat, re4::Unit_beFlag) & 0x201) != 1 || !(At<uint8_t>(sat, re4::Sat_flag) & 4))
                continue;
            auto* verts = At<Vec3*>(sat, re4::Sat_vert);
            auto* polys = At<uint16_t(*)[10]>(sat, re4::Sat_poly);
            uint32_t vertNum = At<uint16_t>(sat, re4::Sat_vertNum), polyNum = At<uint16_t>(sat, re4::Sat_polyNum);
            const float* m = &At<float>(sat, re4::Sat_mat);
            if (!verts || !polys)
                continue;
            for (uint32_t p = 0; p < polyNum && n < MaxTriangles; ++p)
            {
                Vec3 w[3];
                bool bad = false;
                for (int k = 0; k < 3; ++k)
                {
                    uint32_t i = polys[p][k];
                    if (i >= vertNum) { bad = true; break; }
                    const Vec3& v = verts[i];
                    w[k] = { m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3],
                             m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
                             m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11] };
                }
                if (bad)
                    continue;
                float minX = std::min({ w[0].x, w[1].x, w[2].x }), maxX = std::max({ w[0].x, w[1].x, w[2].x });
                float minY = std::min({ w[0].y, w[1].y, w[2].y }), maxY = std::max({ w[0].y, w[1].y, w[2].y });
                float minZ = std::min({ w[0].z, w[1].z, w[2].z }), maxZ = std::max({ w[0].z, w[1].z, w[2].z });
                if (minX > center.x + r || maxX < center.x - r || minZ > center.z + r || maxZ < center.z - r ||
                    minY > center.y + rv || maxY < center.y - rv)
                    continue;
                Vec3 e1{ w[1].x - w[0].x, w[1].y - w[0].y, w[1].z - w[0].z }, e2{ w[2].x - w[0].x, w[2].y - w[0].y, w[2].z - w[0].z };
                Vec3 c{ e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
                if (c.x * c.x + c.y * c.y + c.z * c.z < 1.f) // degenerate (area below ~0.5 mm^2)
                    continue;
                if (!roofed && minY > center.y + head && OverPoint(w, center.x, center.z))
                    roofed = true;
                Triangle& t = g_triangles[n++];
                for (int k = 0; k < 3; ++k)
                    ToHost(w[k], t.vertices[k]);
                t.surface = 0; // static world; the floor material is set below
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("collision read faulted; sending %u triangles", n);
    }
    // RE4's collision has no floor material, so MW2 (which plays no footstep on material 0) gets one from where
    // Leon is: the area (room number) and whether a ceiling is over his head.
    uint32_t material = FloorMaterial(roofed);
    for (uint32_t i = 0; i < n; ++i)
        g_triangles[i].surface = material;
    return n;
}

void PublishCollision(uint8_t* pl)
{
    if (!g_sceneMutex || !g_satMgr || GetTickCount() - g_lastScene < 80)
        return;
    const Vec3 center = At<Vec3>(pl, re4::Coord_pos);
    uint32_t n = GatherCollision(center);
    g_lastScene = GetTickCount();
    if (!n)
        return;
    Lock lock(g_sceneMutex);
    if (!lock.acquired)
        return;
    auto* scene = (Scene*)(g_shared + SceneOffset);
    memcpy(g_shared + TriangleOffset, g_triangles, n * sizeof(Triangle));
    scene->count = n;
    scene->tick = GetTickCount();
    scene->overflow = n == MaxTriangles;
    ToHost(center, scene->center);
    scene->sequence = ++g_sceneSequence;
    g_hostMovement.sceneSequence = g_sceneSequence;
    if (n != g_sceneCount || (n == MaxTriangles) != g_sceneOverflow)
    {
        // Height range of what was sent versus Leon's feet, in metres: the floor should sit at his feet.
        float lo = 1e9f, hi = -1e9f;
        for (uint32_t i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k)
            {
                lo = std::min(lo, g_triangles[i].vertices[k][2]);
                hi = std::max(hi, g_triangles[i].vertices[k][2]);
            }
        Log("collision: %u triangles near Leon%s, heights %.2f..%.2f m, Leon's feet at %.2f m", n,
            n == MaxTriangles ? " (limit reached)" : "", lo, hi, scene->center[2]);
    }
    g_sceneCount = n;
    g_sceneOverflow = n == MaxTriangles;
}

// ---------------------------------------------------------------- activation
bool Down(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }
bool Focused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void RestorePrompts();
void Restore()
{
    if (!g_active)
        return;
    if (uint8_t* g = Global())
    {
        uint32_t& disp = At<uint32_t>(g, re4::G_dispFlags);
        const uint32_t mine = re4::DPF_PL | re4::DPF_COCKPIT;
        disp = (disp & ~mine) | (g_savedDisp & mine);
    }
    RestorePrompts();
    g_active = false;
    g_inactiveSince = GetTickCount();
    Log("MW2 control off; RE4 has Leon and the camera again");
}

void Activate(uint8_t* g, uint8_t* pl)
{
    // Start facing where Leon faces, looking level. RE4's camera is unreliable here: after a cutscene
    // it can still be in front of Leon looking back at him.
    float t = At<Vec3>(pl, re4::Coord_ang).y; // Leon looks along (sin t, 0, cos t); host is (x, -z, y)
    float yaw = std::atan2(-std::cos(t), std::sin(t)) * 180 / Pi;
    // A short break (menu, a press of E that did nothing, a pause) with Leon still where MW2 left him: MW2 just
    // carries on, same movement and same view, instead of starting over (that restart was a visible hitch).
    bool resume = false;
    if (g_hostMovement.epoch && g_guestMovement.epoch == g_hostMovement.epoch && g_guestMovement.sequence &&
        GetTickCount() - g_inactiveSince < 10000)
    {
        Vec3 leon = At<Vec3>(pl, re4::Coord_pos), mw2 = ToRe4(g_guestMovement.foot);
        float dx = leon.x - mw2.x, dy = leon.y - mw2.y, dz = leon.z - mw2.z;
        float turn = std::fabs(std::remainder(yaw - g_yaw, 360.f));
        resume = dx * dx + dy * dy + dz * dz < 100.f * 100.f && turn < 15.f; // within 10 cm and 15 degrees
    }
    if (!resume)
    {
        g_yaw = yaw;
        g_pitch = 0;
        if (!++g_hostMovement.epoch)
            ++g_hostMovement.epoch;
        ToHost(At<Vec3>(pl, re4::Coord_pos), g_hostMovement.foot);
    }
    g_lastScene = 0;
    uint32_t& disp = At<uint32_t>(g, re4::G_dispFlags);
    g_savedDisp = disp;
    if (g_cfg.hidePlayer)
        disp |= re4::DPF_PL;  // Leon hidden: first person
    disp |= re4::DPF_COCKPIT; // RE4's HUD hidden: MW2's HUD and the health bar replace it
    g_active = true;
    Log("MW2 control on%s: epoch %u, Leon routine %u/%u/%u at (%.0f, %.0f, %.0f), yaw %.1f pitch %.1f, disp flags 0x%08X -> 0x%08X",
        resume ? " (resumed)" : "", g_hostMovement.epoch, At<uint8_t>(pl, re4::Model_rno0), At<uint8_t>(pl, re4::Model_rno0 + 1),
        At<uint8_t>(pl, re4::Model_rno0 + 2), At<Vec3>(pl, re4::Coord_pos).x, At<Vec3>(pl, re4::Coord_pos).y,
        At<Vec3>(pl, re4::Coord_pos).z, g_yaw, g_pitch, g_savedDisp, disp);
}

// Why MW2 may not have control right now, or null when it may.
// RE4 action (jump over a fence, jump down, climb a ladder, kick...): when E is pressed with RE4's action
// prompt on screen, RE4 plays the action and moves Leon itself. MW2 steps aside until Leon's routine is
// back to normal movement, then takes over again from where Leon landed.
DWORD g_actionStart = 0;
uint8_t g_actionRno[3] = {};
bool g_actionSeenChange = false;
const char* ActionBlocker(uint8_t* pl)
{
    if (!g_actionStart)
        return nullptr;
    DWORD age = GetTickCount() - g_actionStart;
    const uint8_t* r = &At<uint8_t>(pl, re4::Model_rno0);
    bool normal = r[0] == g_actionRno[0] && r[1] == g_actionRno[1];
    if (!normal && !g_actionSeenChange)
    {
        g_actionSeenChange = true;
        Log("RE4 action: Leon routine %u/%u/%u -> %u/%u/%u", g_actionRno[0], g_actionRno[1], g_actionRno[2], r[0], r[1], r[2]);
    }
    // Until RE4 actually starts an action, MW2 keeps Leon: a pickup or a press of E that did nothing never
    // shows Leon for a moment. Once his routine changes (jump, climb, kick...), MW2 steps aside.
    if (!g_actionSeenChange)
    {
        if (age > 500)
            g_actionStart = 0;
        return nullptr;
    }
    if (!normal)
    {
        if (age < 10000)
            return "RE4 action (jump, climb...)";
        Log("RE4 action: still not back to normal after 10 s, MW2 takes over");
    }
    else
        Log("RE4 action done after %.1f s", age / 1000.f);
    g_actionStart = 0;
    return nullptr;
}

// RE4 screens that stop gameplay (merchant, attache case, Hunnigan's calls, game over, pause): RE4's camera
// stops with them, so CameraSetHook cannot see them open. The pad read and the text keep running, so they
// check this and hand everything back to RE4 at once.
const char* Re4ScreenBlocker()
{
    uint8_t* g = Global();
    if (!g) return "no game";
    if (At<uint8_t>(g, re4::G_rno0) != 3) return "not in gameplay";
    if (At<int16_t>(g, re4::G_plHp) <= 0) return "Leon is down";
    if (At<uint32_t>(g, re4::G_statusFlags) & re4::STA_SUSPEND) return "RE4 screen (merchant, case, call, menu)";
    return nullptr;
}

// RE4's item pickup screen ("take it?"): it does not pause RE4 or set any screen flag above, so MW2 keeps Leon
// (hidden, no hand-over flash) but steps out of the way: no MW2 hand, RE4's text and windows in their own place,
// every key to RE4 and none to MW2.
bool ItemGetOpen()
{
    uint8_t* g = Global();
    return g && (At<uint32_t>(g, re4::G_statusFlags1) & re4::STA1_ITEM_GET);
}

// A pickup screen ("take it?"): RE4 stops (camera, player or enemy logic) within a moment of E. Its text and
// window must stay where RE4 puts them (bottom of the screen); MW2's hand is hidden anyway while RE4 stops.
// Only a stop that follows E counts, so every other caption keeps its place.
extern DWORD g_lastUse;
bool PickupScreen()
{
    uint8_t* g = Global();
    if (!g || !g_active)
        return false;
    DWORD now = GetTickCount();
    bool stopped = (At<uint32_t>(g, re4::G_stopFlags) & (re4::SPF_CAMERA | re4::SPF_PL | re4::SPF_EM)) || ItemGetOpen() ||
                   now - g_lastTick > 100;
    static bool was = false, fromUse = false;
    if (stopped && !was)
    {
        fromUse = now - g_lastUse < 1500;
        if (fromUse)
            Log("pickup screen: RE4 stopped %lu ms after E (stop flags 0x%08X, status 0x%08X 0x%08X)", now - g_lastUse,
                At<uint32_t>(g, re4::G_stopFlags), At<uint32_t>(g, re4::G_statusFlags), At<uint32_t>(g, re4::G_statusFlags1));
    }
    else if (!stopped && was && fromUse)
        Log("pickup screen closed");
    was = stopped;
    return stopped && fromUse;
}

const char* Blocker(uint8_t* g, uint8_t* pl)
{
    if (!g_requested) return "off";
    // How old MW2's state was when it was last read, not now: after RE4 was paused (attache case, map, menu)
    // the copy is old only because RE4 stopped running, and MW2 is still fine.
    if (!(g_guest.ready && g_lastExchange - g_guest.tick < 1500)) return "MW2 loading";
    if (!g || !pl) return "no player";
    if (At<uint8_t>(g, re4::G_rno0) != 3) return "not in gameplay";
    if (At<uint8_t>(g, re4::G_plType) != 0) return "not playing as Leon";
    if (At<int16_t>(g, re4::G_plHp) <= 0) return "Leon is down";
    if (At<uint8_t>(pl, re4::Model_rno0) >= 2) return "Leon is hurt or grabbed (RE4 handles it)";
    if (At<uint32_t>(g, re4::G_statusFlags) & re4::STA_SUSPEND) return "cutscene, event or menu";
    if (g_qteActive && *g_qteActive) return "quick-time event (RE4 handles it)";
    // Rides RE4 moves Leon on: MW2's movement cannot follow a moving floor, so RE4 has Leon until he is off
    if (At<uint32_t>(g, re4::G_statusFlags1) & re4::STA1_PL_BOAT) return "on the boat (RE4 has Leon until he is off)";
    if (At<uint32_t>(g, re4::G_statusFlags) & re4::STA_RIDE_GONDOLA) return "on a gondola (RE4 has Leon until he is off)";
    if (At<uint32_t>(g, re4::G_statusFlags3) & re4::STA3_ELEVATOR) return "on a lift (RE4 has Leon until he is off)";
    if (const char* action = ActionBlocker(pl)) return action;
    if (!g_active && g_frame - g_hitCheckFrame > 3) return "RE4 gameplay camera not active";
    return nullptr;
}

// ---------------------------------------------------------------- MW2 shots -> RE4 hits
Shot g_shotQueue[64];
uint32_t g_pendingShots = 0;
Explosion g_blastQueue[ExplosionCount];
uint32_t g_pendingBlasts = 0, g_lastExplosion = 0;
uint32_t g_hitsSent = 0, g_blastsSent = 0;

const char* WeaponName(uint32_t weapon)
{
    for (auto& e : g_catalogAll)
        if (e.weapon == weapon)
            return e.name;
    return "";
}

bool StartsWithAny(const char* name, std::initializer_list<const char*> prefixes)
{
    for (const char* x : prefixes)
    {
        size_t n = strlen(x);
        if (!strncmp(name, x, n) && (name[n] == 0 || name[n] == ' '))
            return true;
    }
    return false;
}

// MW2 gun classes (F11 sliders). Each class hits through the RE4 gun whose reactions fit (shotgun knockback,
// rifle hits); its damage is then scaled by the class's slider in LifeDownHook.
enum GunClass { GC_PISTOL, GC_TOUGH_PISTOL, GC_SMG, GC_RIFLE, GC_SHOTGUN, GC_SNIPER, GC_COUNT, GC_OTHER = GC_COUNT };
const char* kGunClassNames[GC_COUNT] = { "NORMAL PISTOLS", "TOUGH PISTOLS", "SMGS", "ASSAULT RIFLES", "12 GAUGE",
                                         "SNIPERS" };
const char* kGunClassHint[GC_COUNT] = { "USP, M9", "Desert Eagle, .44 Magnum", "UMP, P90, Vector, Glock...",
                                        "AK-47, M4, SCAR, AUG, RPD...", "SPAS-12, Striker, AA-12...",
                                        "Intervention, Barrett, WA2000, M21" };

struct GunFit { int weapon; int gunClass; };
GunFit Re4WeaponFor(const char* name, float rangeMetres)
{
    if (rangeMetres < 3.f)
        return { re4::WEP_KNIFE, GC_OTHER }; // MW2 melee
    if (StartsWithAny(name, { "deserteagle", "coltanaconda" }))
        return { re4::WEP_HANDGUN, GC_TOUGH_PISTOL };
    if (StartsWithAny(name, { "glock", "beretta393", "pp2000", "tmp", "mp5k", "ump45", "kriss", "p90", "uzi" }))
        return { re4::WEP_HANDGUN, GC_SMG };
    if (StartsWithAny(name, { "ak47", "m4", "famas", "fal", "scar", "tavor", "fn2000", "masada", "m16", "aug", "rpd",
                              "sa80", "mg4", "m240" }))
        return { re4::WEP_HANDGUN, GC_RIFLE };
    if (StartsWithAny(name, { "cheytac" }))
        return { re4::WEP_RIFLE_BOLT, GC_SNIPER };
    if (StartsWithAny(name, { "barrett", "wa2000", "m21" }))
        return { re4::WEP_RIFLE_SEMI, GC_SNIPER };
    if (StartsWithAny(name, { "striker", "aa12" }))
        return { re4::WEP_STRIKER, GC_SHOTGUN };
    if (StartsWithAny(name, { "spas12", "ranger", "model1887", "m1014" }))
        return { re4::WEP_SHOTGUN, GC_SHOTGUN };
    if (StartsWithAny(name, { "rpg", "at4", "m79" }))
        return { re4::WEP_RPG, GC_OTHER };
    return { re4::WEP_HANDGUN, GC_PISTOL }; // beretta, usp and anything unknown
}

// Damage scaling: an MW2 bullet that hit sets the scale; the enemy applies the hit within a few frames.
volatile float g_hitScale = 1.f;
volatile DWORD g_hitScaleUntil = 0;
void* g_lifeDownOrig = nullptr;
int __cdecl ScaleDamage(uint8_t* em, int damage)
{
    if (!g_active || GetTickCount() > g_hitScaleUntil || damage <= 0)
        return damage;
    __try
    {
        if (!em || At<uint8_t>(em, re4::Model_id) == 0) // Leon
            return damage;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return damage;
    }
    int scaled = std::max(1, (int)std::lround(damage * g_hitScale));
    static int logged = 0;
    if (logged < 30 && ++logged)
        Log("damage: enemy %p %d -> %d (x%.1f)", (void*)em, damage, scaled, (float)g_hitScale);
    return scaled;
}

__declspec(naked) void LifeDownHook()
{
    __asm {
        push dword ptr [esp + 8]   // damage
        push dword ptr [esp + 8]   // em (shifted by the push above)
        call ScaleDamage
        add esp, 8
        mov dword ptr [esp + 8], eax
        jmp dword ptr [g_lifeDownOrig]
    }
}

int CallBulletHit(uint8_t* pl, Vec3 start, Vec3 end, int weapon, int flags = 0)
{
    __try
    {
        return g_bulletHit(pl, &start, &end, weapon, flags, 6000.f);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("BulletHit faulted (weapon 0x%02X)", weapon);
        return -1;
    }
}

// Enemies (EmMgr) with a body within radius of a point, for explosions.
int EnemiesNear(uint8_t* pl, const Vec3& c, float radius, uint8_t** out, int max)
{
    int n = 0;
    __try
    {
        uint8_t* array = At<uint8_t*>(g_emMgr, re4::Mgr_array);
        uint32_t count = At<uint32_t>(g_emMgr, re4::Mgr_count), stride = At<uint32_t>(g_emMgr, re4::Mgr_stride);
        for (uint32_t i = 0; array && i < count && i < 512 && n < max; ++i)
        {
            uint8_t* em = array + i * stride;
            if (em == pl || (At<uint32_t>(em, re4::Unit_beFlag) & 0x601) == 0 || At<int16_t>(em, re4::Em_hp) <= 0)
                continue;
            const Vec3 p = At<Vec3>(em, re4::Coord_pos);
            float dx = p.x - c.x, dy = p.y + 900.f - c.y, dz = p.z - c.z;
            if (dx * dx + dy * dy + dz * dz < radius * radius)
                out[n++] = em;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return n;
}

// What RE4's own grenades do when they go off (Leon's: bio4 0x73A050 hand, 0x73A3B6 incendiary, 0x73A620 flash).
// Hand: explosion effects 0x0D + 0x1A, sound 0x14, then a blast hit (weapon 0x13). Flash: effect 0x0C, sound 0x13,
// blast hit 0x17. Incendiary: fire burst 0x0B and ground fire 0x26, sound 0x22, and no blast hit: a burning area
// (2.5 m) that sets enemies on fire. All three set the status flags and the last-blast position enemies react to.
enum class Blast { Hand, Flash, Fire };
bool g_fragFire = false; // MW2's frag (G) currently stands for RE4's incendiary grenades (else hand grenades)

int RE4Explosion(Vec3 pos, Blast kind)
{
    uint8_t* g = Global();
    __try
    {
        bool fire = kind == Blast::Fire && g_fireArea && g_fireList;
        if (g_effect)
        {
            if (fire)
            {
                g_effect(0, -1, &pos, 0, 0, 0x0B, 0, 0, 0, 0);
                g_effect(0, -1, &pos, 0, 0, 0x26, 0, 0, 0, 0);
            }
            else
            {
                g_effect(0, -1, &pos, 0, 0, kind == Blast::Flash ? 0x0C : 0x0D, 0, 0, 0, 0);
                if (kind != Blast::Flash)
                    g_effect(0, -1, &pos, 0, 0, 0x1A, 0, 0, 0, 0);
            }
        }
        if (g_sound)
            g_sound(1, fire ? 0x22 : kind == Blast::Flash ? 0x13 : 0x14, &pos, 0, 0, 0);
        if (g)
            At<uint32_t>(g, re4::G_statusFlags) |= re4::STA_PL_FIRE;
        int hits = 0;
        if (fire)
            hits = g_fireArea(g_fireList, nullptr, 1, 0x4B, &pos, 2500.f, 1500.f);
        else if (g_bulletHit)
        {
            Vec3 p = pos;
            if (kind == Blast::Flash)
                p.y += 500.f;
            // (an incendiary without the fire area: its weapon number, a blast like the hand grenade's)
            int weapon = kind == Blast::Flash ? re4::WEP_FLASH : kind == Blast::Fire ? re4::WEP_INCENDIARY : re4::WEP_GRENADE;
            hits = g_bulletHit(nullptr, &p, &p, weapon, 3, kind == Blast::Flash ? 15000.f : 6000.f);
        }
        if (g)
        {
            At<uint32_t>(g, re4::G_statusFlags + 4) |= re4::STA1_EXPLOSION;
            At<Vec3>(g, re4::G_blastPos) = pos;
            At<uint8_t>(g, re4::G_blastFlag) = 1;
        }
        return hits;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("RE4 explosion faulted");
        return -1;
    }
}

int ArsenalWeaponNo(uint32_t mw2Weapon);

// Which RE4 things a shot hit: every Em with a hit waiting (enemies, boxes 0x53, barrels 0x58, objects 0x50,
// doors 0x51, windows 0x56, torches 0x57...). Logged for the first shots only.
void LogShotTargets(uint32_t sequence)
{
    static int logged = 0;
    if (logged >= 80 || !g_emMgr)
        return;
    char ids[200] = "";
    int n = 0;
    __try
    {
        uint8_t* array = At<uint8_t*>(g_emMgr, re4::Mgr_array);
        uint32_t count = At<uint32_t>(g_emMgr, re4::Mgr_count), stride = At<uint32_t>(g_emMgr, re4::Mgr_stride);
        for (uint32_t i = 0; array && i < count && i < 512 && n < 12; ++i)
        {
            uint8_t* em = array + i * stride;
            if ((At<uint32_t>(em, re4::Unit_beFlag) & 1) && (At<uint8_t>(em, re4::Em_dmgFlag) & 1))
            {
                size_t len = strlen(ids);
                snprintf(ids + len, sizeof(ids) - len, " 0x%02X", At<uint8_t>(em, re4::Model_id));
                ++n;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    if (n)
    {
        ++logged;
        Log("shot %u hit RE4 Em id:%s", sequence, ids);
    }
}

void ResolveShots(uint8_t* pl)
{
    uint32_t shots = g_pendingShots, blasts = g_pendingBlasts;
    g_pendingShots = g_pendingBlasts = 0;
    if (!g_active || !pl || !g_bulletHit)
        return;
    DWORD now = GetTickCount();
    for (uint32_t i = 0; i < shots; ++i)
    {
        const Shot& sh = g_shotQueue[i];
        if (now - sh.hostTick > 500 || !std::isfinite(sh.damage) || sh.damage <= 0)
            continue;
        float d[3] = { sh.direction[0], sh.direction[1], sh.direction[2] };
        float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!std::isfinite(len) || len < .5f || len > 1.5f)
            continue;
        float range = std::clamp(sh.range, 1.f, 100.f);
        float endH[3] = { sh.origin[0] + d[0] / len * range, sh.origin[1] + d[1] / len * range, sh.origin[2] + d[2] / len * range };
        const char* name = WeaponName(sh.weapon);
        GunFit fit = Re4WeaponFor(name, sh.range);
        if (sh.range >= 3.f)
            if (int no = ArsenalWeaponNo(sh.weapon))
                fit.weapon = no; // exactly the RE4 gun, with its upgrades
        int weapon = fit.weapon;
        int hits = 0;
        if (weapon == re4::WEP_KNIFE)
        {
            // MW2's knife swing as RE4's knife (bio4 0x752F80): weapon 0x10 with flag 1, several hit lines swept
            // across the blade's arc (here straight ahead and 15 degrees to each side, in host space, Z up)
            for (float a : { 0.f, -0.26f, 0.26f })
            {
                float c = std::cos(a), s = std::sin(a);
                float dx = d[0] / len, dy = d[1] / len;
                float e[3] = { sh.origin[0] + (dx * c - dy * s) * range, sh.origin[1] + (dx * s + dy * c) * range, endH[2] };
                hits = std::max(hits, CallBulletHit(pl, ToRe4(sh.origin), ToRe4(e), weapon, 1));
            }
        }
        else
            hits = CallBulletHit(pl, ToRe4(sh.origin), ToRe4(endH), weapon);
        LogShotTargets(sh.sequence);
        if (hits > 0)
        {
            g_hitScale = fit.gunClass < GC_COUNT ? g_cfg.damage[fit.gunClass] : 1.f;
            g_hitScaleUntil = GetTickCount() + 400;
        }
        ++g_hitsSent;
        static uint32_t logged = 0;
        if (logged < 40 || hits > 0)
        {
            ++logged;
            Log("shot %u: MW2 %s -> RE4 weapon 0x%02X, range %.1f m, BulletHit returned %d", sh.sequence,
                name[0] ? name : "?", weapon, range, hits);
        }
    }
    for (uint32_t i = 0; i < blasts; ++i)
    {
        const Explosion& e = g_blastQueue[i];
        const char* name = WeaponName(e.weapon);
        bool flash = strstr(name, "flash") || strstr(name, "concussion") || e.kind == 2;
        if (strstr(name, "smoke"))
            continue;
        // Exactly what RE4's own hand grenade / flash grenade does when it goes off: effect, sound, status flags,
        // then the attack-hit routine in blast mode (no attacker, start == end, radius).
        Vec3 c = ToRe4(e.origin);
        g_hitScaleUntil = 0; // RE4's own grenade damage
        // MW2's frag is the RE4 grenade it was thrown as (hand or incendiary, see the grenade sync)
        bool fire = !flash && g_fragFire && strstr(name, "frag");
        int hits = RE4Explosion(c, flash ? Blast::Flash : fire ? Blast::Fire : Blast::Hand);
        ++g_blastsSent;
        Log("explosion %u: MW2 %s (kind %u, radius %.1f m) -> RE4 %s, %s %d", e.sequence, name[0] ? name : "?", e.kind,
            e.radius, flash ? "flash grenade" : fire ? "incendiary" : "hand grenade",
            fire ? "fire area" : "BulletHit returned", hits);
    }
}

void Notice(const std::string& text, DWORD ms);

// ---------------------------------------------------------------- RE4 captions
// Captions ("Will you use the emblem?", item messages) are placed by MesSet. While MW2 has control the
// caption layout is placed higher so MW2's gun does not cover it; its window (IDC_MSG_WINDOW) follows in
// RaisePrompts. Action-button prompts (moved separately) and the inventory are left alone.
using MesSetFn = bool(__fastcall*)(void* self, void* edx, uint32_t num, int px, int py, uint32_t attr, int wk,
                                   uint8_t col, int font);
MesSetFn g_mesSetOrig = nullptr;
bool __fastcall MesSetHook(void* self, void* edx, uint32_t num, int px, int py, uint32_t attr, int wk, uint8_t col,
                           int font)
{
    bool pickup = PickupScreen();
    if (pickup)
    {
        RestorePrompts(); // the pickup screen's window back in place too
        static int logged = 0;
        if (logged < 10 && ++logged)
            Log("pickup caption %u (layout %d, slot %d) kept at (%d, %d)", num, font, wk, px, py);
    }
    if (g_active && (font == 0 || font == 5) && g_cfg.messageRaise != 0.f && !Re4ScreenBlocker() && !pickup)
    {
        static int logged = 0;
        if (logged < 20 && ++logged)
            Log("caption %u (layout %d, slot %d) at (%d, %d) moved up %.0f", num, font, wk, px, py, g_cfg.messageRaise);
        py -= (int)g_cfg.messageRaise;
    }
    return g_mesSetOrig(self, edx, num, px, py, attr, wk, col, font);
}

// ---------------------------------------------------------------- RE4 prompts
// While MW2 has control, RE4's "press ... to" prompts (2D class IDC_ACT_BUTTON) move up the screen so MW2's
// gun does not cover them. Only top-level units move; their children follow. Restored when MW2 lets go.
struct PromptUnit { uint8_t* unit; float y; };
std::vector<PromptUnit> g_movedPrompts;

void RaisePrompts()
{
    if (!g_idSys)
        return;
    __try
    {
        int count = At<int32_t>(g_idSys, re4::IdSys_count);
        uint8_t* units = At<uint8_t*>(g_idSys, re4::IdSys_units);
        if (!units || count <= 0 || count > 4096)
            return;
        for (int i = 0; i < count; ++i)
        {
            uint8_t* u = units + i * re4::IdUnit_size;
            uint8_t cls = At<uint8_t>(u, re4::IdUnit_class);
            if ((cls != re4::IDC_ACT_BUTTON && cls != re4::IDC_MSG_WINDOW) || At<void*>(u, re4::IdUnit_parent))
                continue;
            float raise = cls == re4::IDC_MSG_WINDOW ? g_cfg.messageRaise : g_cfg.promptRaise;
            float& y = At<Vec3>(u, re4::IdUnit_pos0).y;
            bool known = false;
            for (auto& m : g_movedPrompts)
                if (m.unit == u)
                {
                    known = true;
                    if (y != m.y + raise) // RE4 reset it: take the new base
                        m.y = y;
                    y = m.y + raise;
                }
            if (!known)
            {
                g_movedPrompts.push_back({ u, y });
                if (cls == re4::IDC_MSG_WINDOW)
                    Log("caption window unit %d at y %.0f moved up %.0f", i, y, raise);
                y += raise;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void RestorePrompts()
{
    __try
    {
        for (auto& m : g_movedPrompts)
            At<Vec3>(m.unit, re4::IdUnit_pos0).y = m.y;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_movedPrompts.clear();
}

// ---------------------------------------------------------------- weapon menu
// MW2 weapons that have no counterpart in RE4 stay out of the menu: throwing knife, lock-on launchers,
// placed explosives, riot shield and tactical insertion. Killstreaks, UAV and airstrikes are already
// excluded by the runtime. Guns, launchers and frag/flash/stun/smoke grenades stay.
bool FitsRe4(const char* name)
{
    static const char* excluded[] = { "throwingknife", "throwing knife", "javelin", "stinger", "c4", "claymore",
                                      "riotshield", "riot shield", "tacticalinsertion", "tactical insertion",
                                      "flare", "scrambler", "portable radar" };
    for (const char* x : excluded)
    {
        size_t n = strlen(x);
        if (!strncmp(name, x, n) && (name[n] == 0 || name[n] == ' '))
            return false;
    }
    return name[0] != 0;
}

bool Edge(int key)
{
    bool d = Down(key), result = d && !g_prevKeys[key];
    g_prevKeys[key] = d;
    return result;
}

void Notice(const std::string& text, DWORD ms = 3000)
{
    g_notice = text;
    g_noticeUntil = GetTickCount() + ms;
}

// kind 1 = give/equip catalog weapon, 2 = refill ammo (MW2-GTA protocol); RE4 runtime mode adds
// 4 = hold only `weapon` with clip/stock, 5 = set `weapon`'s clip/stock (a grenade's count is its clip);
// size = the gun's clip size in MW2 (RE4 capacity; 0 = MW2's own)
bool Request(uint32_t kind, uint32_t weapon = 0, bool silent = false, int32_t clip = 0, int32_t stock = 0,
             int32_t size = 0)
{
    if (g_request && g_features.ack != g_request && GetTickCount() - g_requestTime < 3000)
    {
        if (!silent)
            Notice("Weapon request still pending");
        return false;
    }
    if (!++g_request)
        ++g_request;
    g_requestKind = kind;
    g_requestWeapon = weapon;
    g_requestClip = clip;
    g_requestStock = stock;
    g_requestSize = size;
    g_requestTime = GetTickCount();
    if (!silent)
        Notice(kind == 2 ? "Refilling ammo..." : "Equipping...");
    return true;
}

void SaveSetting(const char* key, const char* fmt, double value)
{
    char path[MAX_PATH], text[32];
    snprintf(path, sizeof(path), "%sMW2RE4.ini", g_dir);
    snprintf(text, sizeof(text), fmt, value);
    WritePrivateProfileStringA("MW2RE4", key, text, path);
}

// Infinite ammo: MW2's own resupply, asked for whenever the reserve drops below one magazine
// or a grenade slot is empty.
void InfiniteAmmo()
{
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (!g_cfg.infiniteAmmo || g_cfg.re4Inventory || !g_active || !g_guest.ready || now - last < 1500)
        return;
    bool low = g_guest.stock <= 0 || (g_guest.clip > 0 && g_guest.stock < g_guest.clip) ||
               g_features.lethalAmmo == 0 || g_features.tacticalAmmo == 0;
    if (low && Request(2, 0, true))
        last = now;
}

void Restore();
constexpr int kSettingsRows = 6;
// The skin or the resolution changed since MW2 started: the restart row applies them
bool RestartNeeded()
{
    return g_child.hProcess && (g_runtimeSkin != g_cfg.skin || g_runtimeResolution != g_cfg.resolution);
}
void SettingsInput(bool up, bool down, bool left, bool right, bool enter, bool back)
{
    if (up) g_settingsRow = (g_settingsRow + kSettingsRows - 1) % kSettingsRows;
    if (down) g_settingsRow = (g_settingsRow + 1) % kSettingsRows;
    if (back) g_settingsMenu = false;
    int dir = right ? 1 : left ? -1 : 0;
    switch (g_settingsRow)
    {
    case 0:
        if (dir)
        {
            g_cfg.fov = std::clamp(g_cfg.fov + dir * 5.f, 60.f, 120.f);
            SaveSetting("FOV", "%.0f", g_cfg.fov);
        }
        break;
    case 1:
        if (dir)
        {
            g_cfg.mouseScale = std::clamp(std::round((g_cfg.mouseScale + dir * .1f) * 10.f) / 10.f, .1f, 5.f);
            SaveSetting("MouseScale", "%.1f", g_cfg.mouseScale);
        }
        break;
    case 2:
        if (dir || enter)
        {
            g_cfg.infiniteAmmo = !g_cfg.infiniteAmmo;
            SaveSetting("InfiniteAmmo", "%.0f", g_cfg.infiniteAmmo);
        }
        break;
    case 3:
        if (dir)
        {
            g_cfg.skin = (g_cfg.skin + kSkinCount + dir) % kSkinCount;
            SaveSetting("Skin", "%.0f", g_cfg.skin);
        }
        break;
    case 4:
        if (dir)
        {
            g_cfg.resolution = std::clamp(g_cfg.resolution + dir, 0, kResolutionCount - 1);
            char text[32], path[MAX_PATH];
            snprintf(text, sizeof(text), "%ux%u", kResolutions[g_cfg.resolution].w, kResolutions[g_cfg.resolution].h);
            snprintf(path, sizeof(path), "%sMW2RE4.ini", g_dir);
            WritePrivateProfileStringA("MW2RE4", "Resolution", text, path);
        }
        break;
    case 5:
        if (enter && RestartNeeded())
        {
            // MW2 loads the new map's soldiers and render size: restart it. RE4 keeps running; MW2 mode returns
            // when ready.
            Log("restarting MW2 for skin %s, %ux%u", kSkins[g_cfg.skin].name, kResolutions[g_cfg.resolution].w,
                kResolutions[g_cfg.resolution].h);
            Restore();
            TerminateProcess(g_child.hProcess, 1);
            WaitForSingleObject(g_child.hProcess, 3000);
            g_settingsMenu = false;
            g_runtimeWasReady = false;
            g_unreadySince = 0;
            g_runtimeError.clear();
            g_guest = {};
            if (!StartRuntime())
                g_requested = false;
            Notice("Restarting MW2 with the new settings...", 4000);
        }
        break;
    }
}

std::string SettingsText()
{
    auto bar = [](float frac) {
        std::string b = "[";
        int n = (int)std::lround(std::clamp(frac, 0.f, 1.f) * 20);
        for (int i = 0; i < 20; ++i)
            b += i < n ? '|' : '.';
        return b + "]";
    };
    char rows[kSettingsRows][160];
    snprintf(rows[0], 160, "FIELD OF VIEW      %s  %.0f", bar((g_cfg.fov - 60.f) / 60.f).c_str(), g_cfg.fov);
    snprintf(rows[1], 160, "MOUSE SENSITIVITY  %s  %.1f", bar((g_cfg.mouseScale - .1f) / 4.9f).c_str(), g_cfg.mouseScale);
    snprintf(rows[2], 160, "INFINITE AMMO      %s", g_cfg.infiniteAmmo ? "ON" : "OFF");
    snprintf(rows[3], 160, "SKIN               < %s >", kSkins[g_cfg.skin].name);
    const Resolution& res = kResolutions[g_cfg.resolution];
    snprintf(rows[4], 160, "MW2 RESOLUTION     < %ux%u >%s", res.w, res.h,
             g_cfg.resolution == 2 ? "  (default)" : g_cfg.resolution > 2 ? "  (sharper, slower)" : "  (faster)");
    snprintf(rows[5], 160, "%s", !RestartNeeded() ? "RESTART MW2          (not needed)"
                                                  : "RESTART MW2 NOW      Enter: applies skin and resolution (MW2 loads again)");
    std::string t = "MW2 SETTINGS   Up/Down select   Left/Right change   F10 close\n\n";
    for (int i = 0; i < kSettingsRows; ++i)
    {
        t += i == g_settingsRow ? "> " : "   ";
        t += rows[i];
        t += "\n";
    }
    return t;
}

// ---------------------------------------------------------------- damage menu (F11)
// One slider per MW2 gun class: further right = more damage (x0.1 to x5.0 of the class's normal hit).
// Left/Right held down repeats.
void DamageInput(bool up, bool down, bool left, bool right, bool back)
{
    if (up) g_damageRow = (g_damageRow + GC_COUNT - 1) % GC_COUNT;
    if (down) g_damageRow = (g_damageRow + 1) % GC_COUNT;
    if (back) g_damageMenu = false;
    static DWORD heldSince = 0, lastRepeat = 0;
    DWORD now = GetTickCount();
    bool l = Down(VK_LEFT), r = Down(VK_RIGHT);
    int dir = right ? 1 : left ? -1 : 0;
    if (dir)
        heldSince = lastRepeat = now;
    else if ((l || r) && now - heldSince > 350 && now - lastRepeat > 50)
    {
        dir = r ? 1 : -1;
        lastRepeat = now;
    }
    if (!l && !r)
        heldSince = 0;
    if (!dir)
        return;
    float& v = g_cfg.damage[g_damageRow];
    v = std::clamp(std::round((v + dir * .1f) * 10.f) / 10.f, .1f, 5.f);
    char key[16];
    snprintf(key, sizeof(key), "Damage%d", g_damageRow);
    SaveSetting(key, "%.1f", v);
}

std::string DamageText()
{
    std::string t = "GUN DAMAGE   Up/Down select   Left/Right less/more   F11 close\n\n";
    for (int r = 0; r < GC_COUNT; ++r)
    {
        float v = g_cfg.damage[r];
        int n = (int)std::lround(v / 5.f * 25.f);
        std::string bar = "[";
        for (int k = 0; k < 25; ++k)
            bar += k < n ? '|' : '.';
        bar += "]";
        char line[256];
        snprintf(line, sizeof(line), "%s%s  %s  x%.1f\n      %s\n", r == g_damageRow ? "> " : "   ", kGunClassNames[r],
                 bar.c_str(), v, kGunClassHint[r]);
        t += line;
    }
    return t + "\nx1.0 = exactly the RE4 gun's damage, upgrades included";
}

void WeaponMenuInput(bool focused)
{
    bool f8 = Edge(VK_F8), f9 = Edge(VK_F9), up = Edge(VK_UP), down = Edge(VK_DOWN), left = Edge(VK_LEFT),
         right = Edge(VK_RIGHT), enter = Edge(VK_RETURN), esc = Edge(VK_BACK), f10 = Edge(VK_F10),
         f11 = Edge(VK_F11);
    InfiniteAmmo();
    if (!g_active || !focused)
    {
        g_weaponMenu = false;
        g_settingsMenu = false;
        g_damageMenu = false;
        return;
    }
    if (f10)
    {
        g_settingsMenu = !g_settingsMenu;
        g_weaponMenu = g_damageMenu = false;
    }
    if (f11)
    {
        g_damageMenu = !g_damageMenu;
        g_weaponMenu = g_settingsMenu = false;
    }
    if (g_damageMenu)
    {
        DamageInput(up, down, left, right, esc);
        f8 = false;
        up = down = left = right = enter = esc = false;
    }
    if (g_settingsMenu)
    {
        SettingsInput(up, down, left, right, enter, esc);
        f8 = false;
        up = down = left = right = enter = esc = false;
    }
    if (f8)
        g_damageMenu = false;
    if ((f8 || f9) && g_cfg.re4Inventory)
    {
        if (f9)
            Notice("Ammo comes from Leon's attache case", 3000);
        if (f8)
            g_weaponMenu = !g_weaponMenu && PickerStart();
        f8 = f9 = false;
    }
    if (f8)
    {
        g_weaponMenu = !g_weaponMenu && !g_catalog.empty();
        if (g_catalog.empty())
            Notice("MW2 weapon list not received yet");
        g_selected = std::min(g_selected, g_catalog.empty() ? size_t(0) : g_catalog.size() - 1);
    }
    if (f9)
        Request(2);
    if (g_weaponMenu && !g_catalog.empty())
    {
        size_t n = g_cfg.re4Inventory ? g_pickList.size() : g_catalog.size();
        if (!n)
        {
            g_weaponMenu = false;
            return;
        }
        if (up) g_selected = (g_selected + n - 1) % n;
        if (down) g_selected = (g_selected + 1) % n;
        if (left) g_selected = g_selected >= 12 ? g_selected - 12 : 0;
        if (right) g_selected = std::min(g_selected + 12, n - 1);
        if (esc) g_weaponMenu = false;
        if (enter)
        {
            if (g_cfg.re4Inventory)
                PickerApply(g_pickList[g_selected]);
            else
                Request(1, g_catalog[g_selected].weapon);
            g_weaponMenu = false;
        }
    }
    if (g_request && g_features.ack == g_request && g_noticeUntil &&
        (g_notice == "Equipping..." || g_notice == "Refilling ammo..."))
    {
        bool ok = g_features.outcome == 1;
        Notice(ok ? (g_requestKind == 2 ? "Ammo refilled - R to reload" : "Weapon equipped") : "MW2 refused the request");
        Log("weapon request %u kind %u weapon %u -> outcome %u", g_request, g_requestKind, g_requestWeapon, g_features.outcome);
    }
}

std::string WeaponMenuText()
{
    bool pick = g_cfg.re4Inventory != 0;
    size_t n = pick ? g_pickList.size() : g_catalog.size();
    std::string t = pick ? PickerTitle() + "\nUp/Down select   Left/Right page   Enter use it   F8 close\n\n"
                         : "WEAPONS   Up/Down select   Left/Right page   Enter equip   F8 close\n\n";
    size_t first = g_selected / 12 * 12;
    for (size_t i = first; i < std::min(first + 12, n); ++i)
    {
        std::string name = pick ? PickerName(g_pickList[i]) : std::string(g_catalog[i].name);
        t += i == g_selected ? "> " : "   ";
        for (auto& c : name)
            c = (char)toupper((unsigned char)c);
        t += name;
        if (pick && PickerIsCurrent(g_pickList[i]))
            t += "   (now)";
        t += "\n";
    }
    char page[64];
    snprintf(page, sizeof(page), "\npage %zu / %zu   (%zu %s)", first / 12 + 1, (n + 11) / 12, n,
             pick ? "choices" : "weapons");
    return t + page;
}

// ---------------------------------------------------------------- RE4 attache case -> MW2 gun
// RE4 owns the guns and the ammo. MW2 holds the MW2 gun matching the RE4 gun Leon has equipped; its clip is
// that RE4 gun's loaded ammo and its reserve the matching ammo boxes in the case. MW2 shots spend RE4 ammo,
// an MW2 reload takes boxes by RE4's rule (fill to the gun's capacity upgrade), and pickups, the merchant and
// RE4-mode reloads update MW2. Hits use the RE4 gun's own weapon number, so firepower upgrades count.
// MW2 grenades are RE4's hand + incendiary grenades (frag) and flash grenades (flash).
struct GunMap { uint16_t item; const char* key; const char* label; const char* mw2; uint16_t ammo; };
constexpr uint16_t kNoAmmo = 0xFFFF;
const GunMap kGuns[] = {
    { 35, "Handgun", "Handgun", "usp", 4 },
    { 36, "Handgun", "Handgun", "usp", 4 },
    { 33, "Punisher", "Punisher", "beretta", 4 },
    { 64, "Punisher", "Punisher", "beretta", 4 },
    { 37, "Red9", "Red9", "deserteagle", 4 },
    { 38, "Red9", "Red9 (stock)", "deserteagle", 4 },
    { 39, "Blacktail", "Blacktail", "glock", 4 },
    { 3, "Matilda", "Matilda", "beretta393", 4 },
    { 41, "BrokenButterfly", "Broken Butterfly", "coltanaconda", 0 },
    { 42, "Killer7", "Killer7", "deserteaglegold", 0 },
    { 55, "Handcannon", "Handcannon", "coltanaconda", 26 },
    { 44, "Shotgun", "Shotgun", "spas12", 24 },
    { 71, "Shotgun", "Shotgun", "spas12", 24 },
    { 148, "RiotGun", "Riot Gun", "m1014", 24 },
    { 45, "Striker", "Striker", "striker", 24 },
    { 46, "Rifle", "Rifle", "cheytac", 7 },
    { 107, "Rifle", "Rifle (scope)", "cheytac", 7 },
    { 153, "Rifle", "Rifle (thermal)", "cheytac", 7 },
    { 47, "SemiAutoRifle", "Rifle (semi-auto)", "barrett", 7 },
    { 108, "SemiAutoRifle", "Rifle (semi-auto, scope)", "barrett", 7 },
    { 81, "SemiAutoRifle", "Rifle (semi-auto, thermal)", "barrett", 7 },
    { 48, "TMP", "TMP", "tmp", 32 },
    { 50, "TMP", "TMP (stock)", "tmp", 32 },
    { 52, "ChicagoTypewriter", "Chicago Typewriter", "ak47", 106 },
    { 83, "ChicagoTypewriter", "Chicago Typewriter", "ak47", 106 },
    { 53, "RocketLauncher", "Rocket Launcher", "rpg", kNoAmmo },
    { 23, "RocketLauncher", "Rocket Launcher (special)", "rpg", kNoAmmo },
    { 109, "RocketLauncher", "Infinite Launcher", "rpg", kNoAmmo },
    // Not MW2 guns yet (equipping one keeps the last MW2 gun): the Mine Thrower (54, 171), whose mines stick and then
    // go off, which MW2's Thumper cannot do, and the P.R.L. 412.
};
constexpr int kGunCount = sizeof(kGuns) / sizeof(kGuns[0]);
std::string g_gunMw2[kGunCount]; // MW2 gun per entry, [Guns] in the INI can change it

uint8_t* g_itemMgr = nullptr;
using ItemArmFn = bool(__fastcall*)(uint8_t* mgr, void* edx, uint8_t* item);
using ItemEraseFn = void(__fastcall*)(uint8_t* mgr, void* edx, uint8_t* item);
using ChargeNumFn = uint16_t(__cdecl*)(uint16_t id, int level);
using WeaponNoFn = uint8_t(__cdecl*)(uint16_t id);
using WeaponChangeFn = void(__cdecl*)();
using SpecialTunedFn = bool(__fastcall*)(uint8_t* item, void* edx);
ItemArmFn g_itemArm = nullptr;
ItemEraseFn g_itemErase = nullptr;
ChargeNumFn g_chargeNum = nullptr;
WeaponNoFn g_weaponNo = nullptr;

WeaponChangeFn g_weaponChange = nullptr;
SpecialTunedFn g_specialTuned = nullptr;
bool ArsenalReady()
{
    return g_cfg.re4Inventory && g_itemMgr && g_itemArm && g_itemErase && g_chargeNum && g_weaponNo;
}

void LoadGunMap()
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sMW2RE4.ini", g_dir);
    for (int i = 0; i < kGunCount; ++i)
    {
        char buf[64];
        GetPrivateProfileStringA("Guns", kGuns[i].key, kGuns[i].mw2, buf, sizeof(buf), path);
        g_gunMw2[i] = buf;
    }
}

const GunMap* GunFor(uint16_t item, int* index = nullptr)
{
    for (int i = 0; i < kGunCount; ++i)
        if (kGuns[i].item == item)
        {
            if (index)
                *index = i;
            return &kGuns[i];
        }
    return nullptr;
}

// Every item in Leon's case (cItemMgr item array, as re4_tweaks walks it).
template <class F> void ForItems(F f)
{
    uint8_t* p = At<uint8_t*>(g_itemMgr, re4::ItemMgr_items);
    int n = At<int>(g_itemMgr, re4::ItemMgr_count);
    uint8_t chr = At<uint8_t>(g_itemMgr, re4::ItemMgr_char);
    for (int i = 0; p && i <= n && i < 512; ++i)
    {
        p += re4::Item_size;
        if ((At<uint8_t>(p, re4::Item_flag) & 1) && At<uint8_t>(p, re4::Item_chr) == chr)
            f(p);
    }
}

int CountItem(uint16_t id)
{
    int n = 0;
    ForItems([&](uint8_t* p) { if (At<uint16_t>(p, re4::Item_id) == id) n += At<uint16_t>(p, re4::Item_num); });
    return n;
}

void SafeErase(uint8_t* item);
// Removes up to n of an item (ammo boxes, grenades); returns how many were removed.
int TakeItem(uint16_t id, int n)
{
    std::vector<uint8_t*> stacks;
    ForItems([&](uint8_t* p) { if (At<uint16_t>(p, re4::Item_id) == id) stacks.push_back(p); });
    int taken = 0;
    for (uint8_t* p : stacks)
    {
        if (taken >= n)
            break;
        uint16_t& num = At<uint16_t>(p, re4::Item_num);
        int t = std::min<int>(num, n - taken);
        num = (uint16_t)(num - t);
        taken += t;
        if (!num)
            SafeErase(p);
    }
    return taken;
}

bool ItemValid(uint8_t* item, uint16_t id)
{
    bool found = false;
    ForItems([&](uint8_t* p) { if (p == item && At<uint16_t>(p, re4::Item_id) == id) found = true; });
    return found;
}

uint8_t* EquippedItem()
{
    uint8_t* item = At<uint8_t*>(g_itemMgr, re4::ItemMgr_wep);
    if (!item || !(At<uint8_t>(item, re4::Item_flag) & 1))
        return nullptr;
    return item;
}

int Loaded(uint8_t* item) { return At<uint16_t>(item, re4::Item_ammo) >> 3; }
void SetLoaded(uint8_t* item, int n)
{
    uint16_t& v = At<uint16_t>(item, re4::Item_ammo);
    v = (uint16_t)((v & 7) | (std::clamp(n, 0, 8191) << 3));
}
int Capacity(uint8_t* item)
{
    int level = ((At<uint16_t>(item, re4::Item_levels) >> 12) & 0xF) + 1;
    int n = g_chargeNum(At<uint16_t>(item, re4::Item_id), level);
    return n == 0x8000 ? 1 : n; // 0x8000: single shot (rocket launcher)
}
// RE4's firing speed and reload speed upgrades of the equipped gun, for MW2: time factors in thousandths,
// fire | reload << 16 (0 = unchanged). MW2 shortens that gun's fire / reload times and animations by them.
// RE4's tables differ per gun; these are close to its handgun / shotgun / rifle steps.
uint32_t SpeedUpgrades()
{
    static const uint16_t fire[] = { 1000, 850, 720, 610, 520, 450 };
    static const uint16_t reload[] = { 1000, 850, 600, 450, 400, 350 };
    if (!g_active || !ArsenalReady())
        return 0;
    __try
    {
        uint8_t* item = EquippedItem();
        if (!item)
            return 0;
        uint16_t levels = At<uint16_t>(item, re4::Item_levels);
        int f = std::min((levels >> 4) & 0xF, 5), r = std::min((levels >> 8) & 0xF, 5);
        static int logged = -1;
        if (logged != (f | r << 4))
        {
            logged = f | r << 4;
            Log("speed upgrades: firing speed level %d, reload speed level %d -> MW2 times x%.2f / x%.2f", f + 1, r + 1,
                fire[f] / 1000.f, reload[r] / 1000.f);
        }
        return fire[f] | (uint32_t)reload[r] << 16;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}
// Exclusive upgrades with unlimited ammo: Chicago Typewriter, Handcannon; and the Infinite Launcher.
bool Unlimited(uint8_t* item)
{
    uint16_t id = At<uint16_t>(item, re4::Item_id);
    if (id == 109)
        return true;
    if (id != 52 && id != 83 && id != 55)
        return false;
    if (((At<uint16_t>(item, re4::Item_levels) >> 12) & 0xF) + 1 >= 7)
        return true;
    return g_specialTuned && g_specialTuned(item, nullptr);
}
int Reserve(const GunMap* gun) { return gun->ammo == kNoAmmo ? 0 : CountItem(gun->ammo); }
// A rocket launcher in the case is loaded (one shot); others report their loaded ammo.
int LoadedFor(uint8_t* item, const GunMap* gun) { return gun->ammo == kNoAmmo ? 1 : Loaded(item); }

uint32_t CatalogWeapon(const std::string& name)
{
    for (auto& e : g_catalogAll)
        if (name == e.name)
            return e.weapon;
    return 0;
}

std::string Upper(std::string t)
{
    for (auto& c : t)
        c = (char)toupper((unsigned char)c);
    return t;
}

struct Arsenal
{
    uint8_t* item = nullptr;    // RE4 gun MW2 holds
    uint16_t itemId = 0;
    int gun = -1;               // kGuns index
    uint32_t weapon = 0;        // MW2 weapon id
    int clip = -1, stock = -1;  // what MW2 was told
    int size = -1;              // MW2 clip size sent (the RE4 gun's capacity)
    int frag = -1, flash = -1;
    uint32_t pending = 0;       // request waiting for its ack
    // after an ack: what MW2 should show before its counts mean anything again (it reports what it displays,
    // which can be a frame or two behind)
    int expect = 0;             // 0 nothing, 1 gun clip/stock, 2 frag count, 3 flash count
    uint32_t expectWeapon = 0;
    int expectClip = 0, expectStock = 0;
    DWORD expectSince = 0, expectFor = 0;
    DWORD equippedAt = 0;
    DWORD runtimeStarted = 0;
    DWORD gunMismatchSince = 0, fragMismatchSince = 0, flashMismatchSince = 0; // self-correction timers
    std::string missing;        // MW2 gun name not in MW2's catalog (logged once)
    bool remap = false;         // the F8 picker changed the MW2 gun for the equipped RE4 gun: arm it again
} g_arm;

// Picker entries: a g_catalog index (an MW2 gun for the equipped RE4 gun) or one of the two grenade rows, which
// choose what MW2's frag (G) throws: RE4's hand grenades or its incendiary grenades.
constexpr size_t kPickHandGrenade = SIZE_MAX, kPickIncendiary = SIZE_MAX - 1;

bool PickerIsCurrent(size_t entry)
{
    if (entry == kPickHandGrenade)
        return !g_cfg.incendiary;
    if (entry == kPickIncendiary)
        return g_cfg.incendiary != 0;
    return g_arm.gun >= 0 && entry < g_catalog.size() && g_gunMw2[g_arm.gun] == g_catalog[entry].name;
}

std::string PickerName(size_t entry)
{
    char line[96];
    if (entry == kPickHandGrenade || entry == kPickIncendiary)
    {
        bool fire = entry == kPickIncendiary;
        snprintf(line, sizeof(line), "G grenade: %s (%d in the case)", fire ? "incendiary" : "hand grenade",
                 CountItem(fire ? re4::ITEM_FLAME_GRENADE : re4::ITEM_GRENADE));
        return line;
    }
    return entry < g_catalog.size() ? g_catalog[entry].name : "?";
}

std::string PickerTitle()
{
    return g_arm.gun >= 0 && g_arm.item ? "MW2 GUN FOR: " + Upper(kGuns[g_arm.gun].label) : "GRENADE (G)";
}

// F8 with Re4Inventory=1: the two grenade rows, then MW2's guns (no grenades or equipment) for the RE4 gun Leon
// has equipped (guns only when one is equipped).
bool PickerStart()
{
    if (g_catalog.empty())
    {
        Notice("MW2 weapon list not received yet");
        return false;
    }
    g_pickList.assign({ kPickHandGrenade, kPickIncendiary });
    g_selected = g_cfg.incendiary ? 1 : 0;
    if (!g_arm.item || g_arm.gun < 0)
    {
        Notice("Equip an RE4 gun to pick its MW2 gun", 2500);
        return true;
    }
    static const char* notGuns[] = { "grenade", "smoke", "flash", "concussion", "semtex", "stun" };
    for (size_t i = 0; i < g_catalog.size(); ++i)
    {
        const char* name = g_catalog[i].name;
        bool skip = false;
        for (const char* x : notGuns)
            skip = skip || strstr(name, x);
        if (skip)
            continue;
        if (PickerIsCurrent(i))
            g_selected = g_pickList.size();
        g_pickList.push_back(i);
    }
    return true;
}

// A grenade row sets what G throws (saved as Incendiary= in the INI); a gun row makes that MW2 gun stand for the
// equipped RE4 gun (and its other versions), saved to [Guns].
void PickerApply(size_t entry)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sMW2RE4.ini", g_dir);
    if (entry == kPickHandGrenade || entry == kPickIncendiary)
    {
        g_cfg.incendiary = entry == kPickIncendiary;
        WritePrivateProfileStringA("MW2RE4", "Incendiary", g_cfg.incendiary ? "1" : "0", path);
        bool fire = g_cfg.incendiary != 0;
        if (CountItem(fire ? re4::ITEM_FLAME_GRENADE : re4::ITEM_GRENADE))
            Notice(fire ? "G throws incendiary grenades" : "G throws hand grenades", 2500);
        else
            Notice(fire ? "No incendiary grenades in the attache case yet" : "No hand grenades in the attache case yet",
                   3000);
        Log("picker: G grenade = %s", g_cfg.incendiary ? "incendiary" : "hand grenade");
        return;
    }
    if (g_arm.gun < 0 || entry >= g_catalog.size())
        return;
    const char* mw2 = g_catalog[entry].name;
    const char* key = kGuns[g_arm.gun].key;
    for (int i = 0; i < kGunCount; ++i)
        if (!strcmp(kGuns[i].key, key))
            g_gunMw2[i] = mw2;
    WritePrivateProfileStringA("Guns", key, mw2, path);
    g_arm.remap = true;
    g_arm.missing.clear();
    Log("picker: RE4 %s -> MW2 %s (saved to [Guns])", kGuns[g_arm.gun].label, mw2);
}

bool ArmRequest(uint32_t kind, uint32_t weapon, int clip, int stock, int expect = 1, int size = 0)
{
    if (!Request(kind, weapon, true, clip, stock, size))
        return false;
    g_arm.pending = g_request;
    g_arm.expect = expect;
    g_arm.expectWeapon = weapon;
    g_arm.expectClip = clip;
    g_arm.expectStock = stock;
    g_arm.expectFor = kind == 4 ? 2500 : 1000;
    return true;
}

void SafeArm(uint8_t* item)
{
    __try
    {
        if (g_itemArm(g_itemMgr, nullptr, item) && g_weaponChange)
            g_weaponChange();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("arsenal: equipping a gun faulted");
    }
}

void SafeErase(uint8_t* item)
{
    __try
    {
        g_itemErase(g_itemMgr, nullptr, item);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("arsenal: removing an item faulted");
    }
}

int SafeWeaponNo(uint16_t id)
{
    __try
    {
        return g_weaponNo(id);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// '1': Leon equips the next gun in his case (RE4 then hands it to MW2).
void NextGun()
{
    uint8_t* current = EquippedItem();
    std::vector<uint8_t*> guns;
    ForItems([&](uint8_t* p) { if (GunFor(At<uint16_t>(p, re4::Item_id))) guns.push_back(p); });
    std::sort(guns.begin(), guns.end(), [](uint8_t* a, uint8_t* b) {
        int ia = 0, ib = 0;
        GunFor(At<uint16_t>(a, re4::Item_id), &ia);
        GunFor(At<uint16_t>(b, re4::Item_id), &ib);
        return ia != ib ? ia < ib : a < b;
    });
    if (guns.empty())
        return;
    size_t i = 0;
    while (i < guns.size() && guns[i] != current)
        ++i;
    uint8_t* next = guns[i < guns.size() ? (i + 1) % guns.size() : 0];
    if (next != current)
        SafeArm(next);
}

// MW2 -> RE4: shots and reloads MW2 made with the gun it holds are spent from that RE4 gun and its boxes.
// Sends nothing, so it also runs while RE4 has control (MW2's counts are then frozen) and right before a
// gun change, so the old gun's last shots are never lost. Returns false when the gun left the case.
bool Account(bool active)
{
    if (!g_arm.item || g_guest.reserved[0] != g_arm.weapon || g_arm.gun < 0)
        return true;
    uint8_t* item = g_arm.item;
    if (!ItemValid(item, g_arm.itemId) || Unlimited(item) || g_cfg.infiniteAmmo)
        return true;
    const GunMap* gun = &kGuns[g_arm.gun];
    int c = g_guest.clip, s = g_guest.stock;
    if (s < g_arm.stock)
    {
        // MW2 reloaded (its clip size is the RE4 capacity): the rounds it moved come out of the boxes
        int moved = TakeItem(gun->ammo, g_arm.stock - s);
        if (gun->ammo != kNoAmmo)
            SetLoaded(item, c);
        Log("arsenal: reload %s %d -> %d, took %d from the case, %d left", gun->label, g_arm.clip, c, moved, Reserve(gun));
        g_arm.clip = c; // the boxes are spent: never count this reload twice
        g_arm.stock = s;
        return true;
    }
    if (c < g_arm.clip)
    {
        if (gun->ammo != kNoAmmo)
            SetLoaded(item, c); // shots
        g_arm.clip = c;
        if (c == 0 && gun->ammo == kNoAmmo && At<uint16_t>(item, re4::Item_id) != 109)
        {
            // a rocket launcher is used up, as in RE4
            Log("arsenal: %s fired, removed from the case", gun->label);
            SafeErase(item);
            g_arm.item = nullptr;
            if (active)
                NextGun();
            return false;
        }
    }
    return true;
}

void ArsenalUpdate(bool focused)
{
    static bool nextWas = false, nextQueued = false; // '1' is kept until the arsenal is free to act on it
    bool next = focused && g_active && !MenuOpen() && Down('1');
    if (next && !nextWas)
        nextQueued = true;
    nextWas = next;
    // G with no RE4 grenades: MW2 has nothing to throw, say why
    static bool throwWas = false;
    bool throwing = focused && g_active && !MenuOpen() && Down('G');
    if (throwing && !throwWas && ArsenalReady() && g_guest.ready && g_features.lethalAmmo <= 0 &&
        !CountItem(re4::ITEM_GRENADE) && !CountItem(re4::ITEM_FLAME_GRENADE))
        Notice("No hand or incendiary grenades in the attache case", 2000);
    throwWas = throwing;
    if (!ArsenalReady())
        return;
    if (g_arm.runtimeStarted != g_runtimeStarted)
    {
        Arsenal fresh;
        fresh.runtimeStarted = g_runtimeStarted;
        g_arm = fresh;
    }
    if (!g_guest.ready || g_catalogAll.empty())
        return;
    if (!g_active)
    {
        // RE4 has control: MW2 cannot shoot, but its last shots before the hand-over still count
        if (!g_arm.pending && !g_arm.expect)
            Account(false);
        g_arm.gunMismatchSince = g_arm.fragMismatchSince = g_arm.flashMismatchSince = 0;
        nextQueued = false;
        return;
    }
    DWORD now = GetTickCount();
    if (g_arm.pending)
    {
        if (g_features.ack == g_arm.pending)
        {
            if (g_features.outcome != 1)
                Log("arsenal: MW2 refused request %u (kind %u weapon %u clip %d stock %d)", g_arm.pending,
                    g_requestKind, g_requestWeapon, g_requestClip, g_requestStock);
            g_arm.pending = 0;
            g_arm.expectSince = now;
            if (g_features.outcome != 1)
                g_arm.expect = 0;
        }
        else if (now - g_requestTime < 3000)
            return;
        else
        {
            Log("arsenal: request %u got no answer, sending again", g_arm.pending);
            g_arm.pending = 0;
            g_arm.expect = 0;
            g_arm.weapon = 0;
        }
    }
    if (g_arm.expect)
    {
        bool shown = g_arm.expect == 1 ? g_guest.reserved[0] == g_arm.expectWeapon && g_guest.clip == g_arm.expectClip &&
                                             g_guest.stock == g_arm.expectStock
                   : g_arm.expect == 2 ? g_features.lethalAmmo == g_arm.expectClip
                                       : g_features.tacticalAmmo == g_arm.expectClip;
        if (!shown && now - g_arm.expectSince < g_arm.expectFor)
            return;
        if (!shown)
            Log("arsenal: MW2 shows weapon %u clip %d stock %d (frag %d flash %d), expected weapon %u %d/%d", g_guest.reserved[0],
                g_guest.clip, g_guest.stock, g_features.lethalAmmo, g_features.tacticalAmmo, g_arm.expectWeapon,
                g_arm.expectClip, g_arm.expectStock);
        g_arm.expect = 0;
    }
    // MW2's shots and reloads first, so a gun change below never loses the old gun's last shots
    if (!Account(true))
        return;
    if (nextQueued)
    {
        nextQueued = false;
        NextGun();
        return;
    }

    // 1. The gun: Leon's equipped RE4 gun (a knife or grenade in his hands keeps the last gun).
    uint8_t* eq = EquippedItem();
    int index = -1;
    const GunMap* want = eq ? GunFor(At<uint16_t>(eq, re4::Item_id), &index) : nullptr;
    if (want && (eq != g_arm.item || !g_arm.weapon || g_arm.remap))
    {
        uint32_t weapon = CatalogWeapon(g_gunMw2[index]);
        if (!weapon && g_gunMw2[index] == "deserteaglegold")
            weapon = CatalogWeapon("deserteagle");
        if (!weapon)
        {
            if (g_arm.missing != g_gunMw2[index])
            {
                g_arm.missing = g_gunMw2[index];
                Log("arsenal: MW2 has no gun '%s' (for RE4 %s); check [Guns] in MW2RE4.ini", g_gunMw2[index].c_str(),
                    want->label);
                Notice("MW2 has no gun called " + g_gunMw2[index]);
            }
            return;
        }
        bool unlimited = Unlimited(eq) || g_cfg.infiniteAmmo;
        int clip = unlimited ? 999 : LoadedFor(eq, want), stock = unlimited ? 0 : Reserve(want);
        int size = unlimited ? 0 : std::max(1, Capacity(eq));
        if (!ArmRequest(4, weapon, clip, stock, 1, size))
            return;
        g_arm.remap = false;
        bool first = !g_arm.weapon;
        g_arm.item = eq;
        g_arm.itemId = At<uint16_t>(eq, re4::Item_id);
        g_arm.gun = index;
        g_arm.weapon = weapon;
        g_arm.clip = clip;
        g_arm.stock = stock;
        g_arm.size = size;
        g_arm.equippedAt = now;
        Log("arsenal: RE4 %s -> MW2 %s (weapon %u) clip %d of %d, reserve %d%s", want->label, g_gunMw2[index].c_str(),
            weapon, clip, size, stock, unlimited ? " (unlimited)" : "");
        if (!first)
            Notice(std::string(want->label) + "  =  " + Upper(g_gunMw2[index]), 2500);
        return;
    }
    if (g_arm.item && !ItemValid(g_arm.item, g_arm.itemId))
    {
        // sold, or a rocket launcher that was fired: MW2's copy is empty until Leon equips another gun
        Log("arsenal: RE4 gun %u left the case", g_arm.itemId);
        g_arm.item = nullptr;
        ArmRequest(5, g_arm.weapon, 0, 0, 0);
        g_arm.clip = g_arm.stock = 0;
        return;
    }

    // 2. Its ammo.
    if (g_arm.item && g_guest.reserved[0] == g_arm.weapon)
    {
        const GunMap* gun = &kGuns[g_arm.gun];
        uint8_t* item = g_arm.item;
        int c = g_guest.clip, s = g_guest.stock;
        if (Unlimited(item) || g_cfg.infiniteAmmo)
        {
            if (c < 900 && ArmRequest(5, g_arm.weapon, 999, 0))
                g_arm.clip = 999, g_arm.stock = 0;
            return;
        }
        int loaded = LoadedFor(item, gun), reserve = Reserve(gun), cap = std::max(1, Capacity(item));
        bool firing = Down(VK_LBUTTON);
        // Self-correction: what MW2 shows must be the case's counts. A difference that lasts 1.5 s (not while
        // firing) is fixed from the case.
        bool off = c != loaded || s != reserve;
        if (!off || firing)
            g_arm.gunMismatchSince = 0;
        else if (!g_arm.gunMismatchSince)
            g_arm.gunMismatchSince = now;
        if (g_arm.gunMismatchSince && now - g_arm.gunMismatchSince > 1500)
        {
            Log("arsenal: MW2 shows %s %d / %d but the case has %d / %d: fixing", gun->label, c, s, loaded, reserve);
            g_arm.gunMismatchSince = 0;
            if (ArmRequest(5, g_arm.weapon, loaded, reserve, 1, cap))
            {
                g_arm.clip = loaded;
                g_arm.stock = reserve;
                g_arm.size = cap;
            }
            return;
        }
        // (not while firing: setting the clip then would hand back the shots fired meanwhile)
        if ((loaded != g_arm.clip || reserve != g_arm.stock || cap != g_arm.size) && !firing)
        {
            // pickups, the merchant (capacity upgrades too), an RE4-mode reload
            if (ArmRequest(5, g_arm.weapon, loaded, reserve, 1, cap))
            {
                g_arm.clip = loaded;
                g_arm.stock = reserve;
                g_arm.size = cap;
            }
            return;
        }
    }
    else if (g_arm.weapon && now - g_arm.equippedAt > 4000)
    {
        Log("arsenal: MW2 holds weapon %u instead of %u, equipping again", g_guest.reserved[0], g_arm.weapon);
        g_arm.weapon = 0;
        return;
    }

    // 3. Grenades.
    // MW2's frag is the grenade type chosen in the F8 picker (hand grenades or incendiaries); when the case has
    // none of that type but has the other, G throws the other (the pick comes back once the case has it again).
    // Not switched while a throw is still to be taken from the case, so it is taken from the type it was thrown as.
    if (g_arm.frag < 0 || g_features.lethalAmmo >= g_arm.frag)
    {
        bool fire = g_cfg.incendiary != 0;
        if (!CountItem(fire ? re4::ITEM_FLAME_GRENADE : re4::ITEM_GRENADE) &&
            CountItem(fire ? re4::ITEM_GRENADE : re4::ITEM_FLAME_GRENADE))
            fire = !fire;
        if (fire != g_fragFire)
        {
            g_fragFire = fire;
            g_arm.frag = -1; // send the new type's count
            Log("arsenal: G throws %s grenades%s", fire ? "incendiary" : "hand", (fire != (g_cfg.incendiary != 0)) ?
                " (the case has none of the picked type)" : "");
        }
    }
    const uint16_t fragItem = g_fragFire ? re4::ITEM_FLAME_GRENADE : re4::ITEM_GRENADE;
    uint32_t fragWeapon = CatalogWeapon("frag grenade"), flashWeapon = CatalogWeapon("flash grenade");
    int grenades = CountItem(fragItem);
    int flashes = CountItem(re4::ITEM_FLASH_GRENADE);
    if (fragWeapon)
    {
        if (g_arm.frag > 0 && g_features.lethalAmmo < g_arm.frag)
        {
            int thrown = g_arm.frag - std::max(0, g_features.lethalAmmo);
            TakeItem(fragItem, thrown);
            grenades = std::max(0, grenades - thrown);
            g_arm.frag = std::max(0, g_features.lethalAmmo);
        }
        if (g_features.lethalAmmo == grenades)
            g_arm.fragMismatchSince = 0;
        else if (!g_arm.fragMismatchSince)
            g_arm.fragMismatchSince = now;
        bool fix = g_arm.fragMismatchSince && now - g_arm.fragMismatchSince > 1500;
        if ((grenades != g_arm.frag || fix) && ArmRequest(5, fragWeapon, grenades, 0, 2))
        {
            if (fix)
                Log("arsenal: MW2 shows %d frags but the case has %d grenades: fixing", g_features.lethalAmmo, grenades);
            g_arm.frag = grenades;
            g_arm.fragMismatchSince = 0;
            return;
        }
    }
    if (flashWeapon)
    {
        if (g_arm.flash > 0 && g_features.tacticalAmmo < g_arm.flash)
        {
            int thrown = g_arm.flash - std::max(0, g_features.tacticalAmmo);
            TakeItem(re4::ITEM_FLASH_GRENADE, thrown);
            flashes = std::max(0, flashes - thrown);
            g_arm.flash = std::max(0, g_features.tacticalAmmo);
        }
        if (g_features.tacticalAmmo == flashes)
            g_arm.flashMismatchSince = 0;
        else if (!g_arm.flashMismatchSince)
            g_arm.flashMismatchSince = now;
        bool fix = g_arm.flashMismatchSince && now - g_arm.flashMismatchSince > 1500;
        if ((flashes != g_arm.flash || fix) && ArmRequest(5, flashWeapon, flashes, 0, 3))
        {
            if (fix)
                Log("arsenal: MW2 shows %d flashes but the case has %d: fixing", g_features.tacticalAmmo, flashes);
            g_arm.flash = flashes;
            g_arm.flashMismatchSince = 0;
        }
    }
}

// The RE4 weapon number of the gun MW2 is firing, or 0 when MW2's gun is not one of RE4's.
int ArsenalWeaponNo(uint32_t mw2Weapon)
{
    if (!ArsenalReady() || !g_arm.item || mw2Weapon != g_arm.weapon)
        return 0;
    return SafeWeaponNo(g_arm.itemId);
}

void SafeArsenalUpdate(bool focused)
{
    __try
    {
        ArsenalUpdate(focused);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int logged = 0;
        if (logged++ < 5)
            Log("arsenal: faulted");
    }
}

// RE4 gun -> MW2 gun list, shown while the attache case or the merchant is open.
std::string GunListText()
{
    std::string t = "MW2 GUNS IN RE4\n";
    for (int i = 0; i < kGunCount; ++i)
    {
        if (i && !strcmp(kGuns[i].key, kGuns[i - 1].key))
            continue;
        char line[96];
        std::string label = kGuns[i].label;
        size_t paren = label.find(" (");
        if (paren != std::string::npos)
            label.resize(paren);
        snprintf(line, sizeof(line), "%-19s %s\n", label.c_str(), Upper(g_gunMw2[i]).c_str());
        t += line;
    }
    return t;
}

// ---------------------------------------------------------------- damage trace (research)
// Hardware watchpoints on the two Ganados nearest Leon: their pending-hit flag and their HP. When RE4
// writes either, the writer's address and its callers are logged. Used once, in RE4 mode, to find the
// exact code Leon's own weapons use to damage enemies, so MW2's shots can go through the same path.
DWORD g_mainThreadId = 0;
uint8_t* g_traceEms[2] = {};
volatile LONG g_traceWanted = 0; // 1 = helper thread should (re)arm the debug registers
uint32_t g_traceSeen[96];
volatile LONG g_traceSeenCount = 0;

bool InText(uint32_t a) { return a >= (uint32_t)g_textBegin && a < (uint32_t)g_textEnd; }

LONG CALLBACK TraceHandler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD hit = c->Dr6 & 0xF;
    if (!hit)
        return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    uint32_t eip = c->Eip;
    LONG n = g_traceSeenCount;
    for (LONG i = 0; i < n; ++i)
        if (g_traceSeen[i] == eip)
            return EXCEPTION_CONTINUE_EXECUTION;
    if (n < 96)
    {
        g_traceSeen[n] = eip;
        g_traceSeenCount = n + 1;
        uint32_t calls[8] = {};
        int k = 0;
        __try
        {
            uint32_t* sp = (uint32_t*)c->Esp;
            for (int i = 0; i < 160 && k < 8; ++i)
                if (InText(sp[i]) && ((uint8_t*)sp[i])[-5] == 0xE8)
                    calls[k++] = sp[i];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
        int slot = hit & 1 ? 0 : hit & 2 ? 1 : hit & 4 ? 2 : 3;
        const char* what[4] = { "Ganado A hit flag", "Ganado A HP", "Ganado B hit flag", "Ganado B HP" };
        Log("TRACE %s written by bio4+0x%X (exe VA 0x%X) eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
            "callers: %X %X %X %X %X %X %X %X",
            what[slot], eip - (uint32_t)base, eip - (uint32_t)base + 0x400000, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi,
            c->Edi, calls[0] ? calls[0] - (uint32_t)base + 0x400000 : 0, calls[1] ? calls[1] - (uint32_t)base + 0x400000 : 0,
            calls[2] ? calls[2] - (uint32_t)base + 0x400000 : 0, calls[3] ? calls[3] - (uint32_t)base + 0x400000 : 0,
            calls[4] ? calls[4] - (uint32_t)base + 0x400000 : 0, calls[5] ? calls[5] - (uint32_t)base + 0x400000 : 0,
            calls[6] ? calls[6] - (uint32_t)base + 0x400000 : 0, calls[7] ? calls[7] - (uint32_t)base + 0x400000 : 0);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI TraceArmer(void*)
{
    for (;;)
    {
        Sleep(100);
        if (!InterlockedExchange(&g_traceWanted, 0) || !g_mainThreadId)
            continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_mainThreadId);
        if (!t)
            continue;
        SuspendThread(t);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c))
        {
            uintptr_t regs[4] = {};
            DWORD dr7 = 0;
            for (int e = 0; e < 2; ++e)
            {
                if (!g_traceEms[e])
                    continue;
                regs[e * 2] = (uintptr_t)(g_traceEms[e] + re4::Em_dmgFlag);   // 1 byte, write
                regs[e * 2 + 1] = (uintptr_t)(g_traceEms[e] + re4::Em_hp);    // 2 bytes, write
                int a = e * 2, b = e * 2 + 1;
                dr7 |= (1u << (a * 2)) | (1u << (b * 2));
                dr7 |= (1u << (16 + a * 4)) | (0u << (18 + a * 4));   // write, len 1
                dr7 |= (1u << (16 + b * 4)) | (1u << (18 + b * 4));   // write, len 2
            }
            c.Dr0 = regs[0]; c.Dr1 = regs[1]; c.Dr2 = regs[2]; c.Dr3 = regs[3]; c.Dr7 = dr7;
            SetThreadContext(t, &c);
        }
        ResumeThread(t);
        CloseHandle(t);
    }
}

// GrenadeTrace=1 (research): RE4's own effect, sound and blast-hit calls are logged with their arguments and the
// calling address while MW2 is off, to copy what RE4 does (e.g. its incendiary grenade). Repeats are dropped.
using TraceFn = int(__cdecl*)(int, int, int, int, int, int, int, int, int, int, int, int);
TraceFn g_traceEffectOrig = nullptr, g_traceSoundOrig = nullptr, g_traceHitOrig = nullptr;

void TraceCall(const char* what, void* from, const int* a)
{
    static int lines = 0;
    static std::vector<std::pair<void*, int>> seen;
    if (g_active || lines >= 600)
        return;
    int key = a[0] * 31 + a[1] * 17 + a[4] * 7 + a[5] * 3 + a[3];
    int repeats = 0;
    for (auto& e : seen)
        if (e.first == from && e.second == key)
            ++repeats;
    if (repeats >= 2)
        return;
    seen.emplace_back(from, key);
    ++lines;
    float f5;
    memcpy(&f5, &a[5], 4);
    Log("TRACE %s from bio4+0x%X: %d %d 0x%X %d %d %d %d %d %d %d (arg5 as float %.1f)", what,
        (unsigned)((uint8_t*)from - (uint8_t*)GetModuleHandleA(nullptr) + 0x400000), a[0], a[1], a[2], a[3], a[4], a[5],
        a[6], a[7], a[8], a[9], f5);
}

int __cdecl TraceEffect(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10, int a11)
{
    int a[10] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9 };
    TraceCall("effect", _ReturnAddress(), a);
    return g_traceEffectOrig(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}

int __cdecl TraceSound(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10, int a11)
{
    int a[10] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9 };
    TraceCall("sound", _ReturnAddress(), a);
    return g_traceSoundOrig(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}

int __cdecl TraceHit(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10, int a11)
{
    int a[10] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9 };
    TraceCall("blast/bullet hit", _ReturnAddress(), a);
    return g_traceHitOrig(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}

void InstallGrenadeTrace(uint8_t* base)
{
    // the JMP thunks every caller uses (bio4.exe 1.1.0 UHD; HookThunk checks the E9)
    struct { uint32_t va; void* hook; TraceFn* orig; const char* name; } hooks[] = {
        { 0x4014C4, (void*)&TraceEffect, &g_traceEffectOrig, "effect" },
        { 0x40ADC1, (void*)&TraceSound, &g_traceSoundOrig, "sound" },
        { 0x407865, (void*)&TraceHit, &g_traceHitOrig, "blast/bullet hit" },
    };
    for (auto& h : hooks)
    {
        bool ok = HookThunk(base + (h.va - 0x400000), h.hook, (void**)h.orig);
        Log("grenade trace: %s hook %s", h.name, ok ? "on" : "FAILED");
    }
}

void UpdateTrace(uint8_t* pl)
{
    static DWORD last = 0;
    if (!g_cfg.damageTrace || !g_emMgr || !pl || GetTickCount() - last < 500)
        return;
    last = GetTickCount();
    g_mainThreadId = GetCurrentThreadId();
    uint8_t* best[2] = {};
    float bestD[2] = { 1e30f, 1e30f };
    __try
    {
        uint8_t* array = At<uint8_t*>(g_emMgr, re4::Mgr_array);
        uint32_t count = At<uint32_t>(g_emMgr, re4::Mgr_count), stride = At<uint32_t>(g_emMgr, re4::Mgr_stride);
        const Vec3 lp = At<Vec3>(pl, re4::Coord_pos);
        for (uint32_t i = 0; array && i < count && i < 512; ++i)
        {
            uint8_t* em = array + i * stride;
            if (em == pl || (At<uint32_t>(em, re4::Unit_beFlag) & 0x601) == 0 || At<int16_t>(em, re4::Em_hp) <= 0)
                continue;
            const Vec3 p = At<Vec3>(em, re4::Coord_pos);
            float d = (p.x - lp.x) * (p.x - lp.x) + (p.z - lp.z) * (p.z - lp.z);
            if (d > 15000.f * 15000.f)
                continue;
            if (d < bestD[0]) { best[1] = best[0]; bestD[1] = bestD[0]; best[0] = em; bestD[0] = d; }
            else if (d < bestD[1]) { best[1] = em; bestD[1] = d; }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    if (best[0] != g_traceEms[0] || best[1] != g_traceEms[1])
    {
        g_traceEms[0] = best[0];
        g_traceEms[1] = best[1];
        for (int e = 0; e < 2; ++e)
            if (best[e])
                Log("TRACE watching Ganado %c: %p type %u hp %d at %.1f m", 'A' + e, (void*)best[e],
                    At<uint8_t>(best[e], re4::Model_id), At<int16_t>(best[e], re4::Em_hp), std::sqrt(bestD[e]) / 1000.f);
        g_traceWanted = 1;
    }
}

// ---------------------------------------------------------------- per-frame exchange
void Exchange(uint8_t* g, uint8_t* pl)
{
    if (!g_shared)
        return;
    if (g_requested && pl)
        PublishCollision(pl);
    Lock lock(g_controlMutex);
    if (!lock.acquired)
        return;
    auto* c = (Control*)g_shared;
    g_guest = c->guest;
    g_lastExchange = GetTickCount();
    g_guestMovement = c->guestMovement;
    g_guestCamera = *(GuestCamera*)(g_shared + CameraOffset);
    auto* feature = (Features*)(g_shared + FeaturesOffset);
    g_features = *feature;
    uint32_t catalogSize = std::min(feature->catalogCount, CatalogCount);
    if (catalogSize && catalogSize != g_catalogSource)
    {
        auto* first = (CatalogEntry*)(g_shared + CatalogOffset);
        g_catalog.clear();
        g_catalogAll.assign(first, first + catalogSize);
        for (auto& e : g_catalogAll)
            e.name[55] = 0;
        for (uint32_t i = 0; i < catalogSize; ++i)
        {
            CatalogEntry e = first[i];
            e.name[55] = 0;
            if (FitsRe4(e.name))
                g_catalog.push_back(e);
        }
        g_catalogSource = catalogSize;
        Log("weapon menu: %zu of %u MW2 weapons fit RE4", g_catalog.size(), catalogSize);
    }
    feature->request = g_request;
    feature->kind = g_requestKind;
    feature->weapon = g_requestWeapon;
    feature->reserved[1] = (uint32_t)g_requestClip;
    feature->reserved[2] = (uint32_t)g_requestStock;
    feature->reserved[3] = (uint32_t)g_requestSize; // clip size = the RE4 gun's capacity
    // MW2's accepted shots and explosions, resolved in RE4 after the lock (ResolveShots).
    uint32_t available = g_guest.shots - g_lastShot;
    if (available > ShotCount)
    {
        g_lastShot = g_guest.shots - ShotCount;
        available = ShotCount;
    }
    while (available--)
    {
        ++g_lastShot;
        const auto& sh = c->shots[g_lastShot % ShotCount];
        if (sh.sequence == g_lastShot && g_pendingShots < 64)
            g_shotQueue[g_pendingShots++] = sh;
    }
    uint32_t blasts = feature->explosions - g_lastExplosion;
    if (blasts > ExplosionCount)
    {
        g_lastExplosion = feature->explosions - ExplosionCount;
        blasts = ExplosionCount;
    }
    while (blasts--)
    {
        ++g_lastExplosion;
        const auto& e = ((Explosion*)(g_shared + ExplosionsOffset))[g_lastExplosion % ExplosionCount];
        if (e.sequence == g_lastExplosion && g_pendingBlasts < ExplosionCount)
            g_blastQueue[g_pendingBlasts++] = e;
    }

    DWORD now = GetTickCount();
    g_host.tick = now;
    g_host.enabled = g_active && !MenuOpen() && !ItemGetOpen() && !PickupScreen() ? 1 : 0;
    g_host.yaw = g_yaw;
    g_host.pitch = g_pitch;
    g_host.fov = g_cfg.fov;
    g_host.dt = std::clamp((now - g_lastTime) / 1000.f, 0.001f, 0.1f);
    g_lastTime = now;
    if (g && pl)
    {
        int hpMax = std::max<int>(1, At<int16_t>(g, re4::G_plHpMax));
        g_host.health = 100.f * std::max<int>(0, At<int16_t>(g, re4::G_plHp)) / hpMax;
        float foot[3];
        ToHost(At<Vec3>(pl, re4::Coord_pos), foot);
        g_host.eye[0] = foot[0];
        g_host.eye[1] = foot[1];
        g_host.eye[2] = foot[2] + 1.6f;
    }
    g_host.velocity[0] = g_host.velocity[1] = g_host.velocity[2] = 0;
    g_host.buttons = 0;
    if (g_active && !MenuOpen() && Focused())
    {
        if (Down(VK_LBUTTON)) g_host.buttons |= 1;
        if (Down(VK_RBUTTON)) g_host.buttons |= 2;
        if (Down('R')) g_host.buttons |= 4;
        if (Down('2')) g_host.buttons |= 8;
        if (Down('1')) g_host.buttons |= 65536;
        if (Down('W')) g_host.buttons |= 16;
        if (Down('S')) g_host.buttons |= 32;
        if (Down('A')) g_host.buttons |= 64;
        if (Down('D')) g_host.buttons |= 128;
        if (Down(VK_SHIFT)) g_host.buttons |= 256;
        if (Down(VK_SPACE)) g_host.buttons |= 512;
        if (Down('C')) g_host.buttons |= 1024;
        if (Down(VK_CONTROL)) g_host.buttons |= 2048;
        if (Down('G')) g_host.buttons |= 4096;
        if (Down('H')) g_host.buttons |= 8192;
        if (Down('V')) g_host.buttons |= 16384;
        if (Down('E')) g_host.buttons |= 32768;
        g_mouseTotalX += g_mousePendingX * g_cfg.mouseScale;
        g_mouseTotalY -= g_mousePendingY * g_cfg.mouseScale * (g_cfg.invertMouseY ? -1.f : 1.f);
    }
    g_mousePendingX = g_mousePendingY = 0;
    g_hostMovement.tick = now;
    g_hostMovement.flags = g_active ? 1 : 0;
    memcpy(&g_hostMovement.reserved[0], &g_mouseTotalX, sizeof(float));
    memcpy(&g_hostMovement.reserved[1], &g_mouseTotalY, sizeof(float));
    g_hostMovement.reserved[2] = 0;
    g_hostMovement.reserved[3] = LeonLook();
    g_hostMovement.reserved[4] = SpeedUpgrades();
    c->host = g_host;
    c->hostMovement = g_hostMovement;
    c->lock = {};
}

// Leon follows MW2's predicted player: position and facing. His own logic is stopped meanwhile.
void ApplyMovement(uint8_t* pl)
{
    if (!g_active || g_guestMovement.epoch != g_hostMovement.epoch || !g_guestMovement.sequence)
        return;
    const float* f = g_guestMovement.foot;
    if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2]))
        return;
    At<Vec3>(pl, re4::Coord_pos) = ToRe4(f);
    if (g_guestCamera.epoch == g_hostMovement.epoch && g_guestCamera.sequence)
    {
        Vec3 d = DirToRe4(g_guestCamera.forward);
        if (d.x * d.x + d.z * d.z > 1e-6f)
            At<Vec3>(pl, re4::Coord_ang).y = std::atan2(d.x, d.z);
    }
}

// RE4's camera shows exactly MW2's camera: no smoothing, no shoulder offset.
void ApplyCamera(uint8_t* camera)
{
    if (!g_active || g_guestCamera.epoch != g_hostMovement.epoch || !g_guestCamera.sequence)
        return;
    const auto& gc = g_guestCamera;
    for (float v : gc.eye) if (!std::isfinite(v)) return;
    for (float v : gc.forward) if (!std::isfinite(v)) return;
    auto& cp = At<CameraPoint>(camera, re4::Camera_point);
    Vec3 eye = ToRe4(gc.eye), dir = DirToRe4(gc.forward);
    cp.campos = eye;
    cp.target = { eye.x + dir.x * 1000.f, eye.y + dir.y * 1000.f, eye.z + dir.z * 1000.f };
    cp.roll = 0.f;
    // MW2 reports a horizontal FOV for its own image aspect; RE4 takes a vertical one.
    uint32_t w = 16, h = 9;
    {
        auto* frame = (Frame*)(g_shared + FrameOffset);
        if (frame->width && frame->height) { w = frame->width; h = frame->height; }
    }
    float hfov = std::clamp(gc.fov, 20.f, 150.f) * Pi / 180;
    cp.fovy = 2 * std::atan(std::tan(hfov / 2) * h / w) * 180 / Pi;
}

// ---------------------------------------------------------------- game hooks
using HitCheckFn = void(__fastcall*)(void*, void*, float*, void*, CameraPoint*);
HitCheckFn g_hitCheckOrig = nullptr;
void __fastcall HitCheckHook(void* self, void* edx, float* plMat, void* offset, CameraPoint* aim)
{
    g_hitCheckOrig(self, edx, plMat, offset, aim);
    g_hitCheckFrame = g_frame;
}

DWORD g_lastStatus = 0;
void Status(uint8_t* g, uint8_t* pl, const char* blocker)
{
    DWORD now = GetTickCount();
    if (now - g_lastStatus < 1000)
        return;
    g_lastStatus = now;
    if (g_child.hProcess)
    {
        WatchRuntime();
        EnumWindows(MoveGuestWindow, g_child.dwProcessId);
    }
    if (!g_requested && !g_active)
        return;
    Vec3 pos{};
    if (pl)
        pos = At<Vec3>(pl, re4::Coord_pos);
    auto* frame = (Frame*)(g_shared + FrameOffset);
    Log("status: %s | guest ready=%u frame=%u %ux%u | movement seq=%u age=%lums grounded=%u | camera seq=%u | "
        "triangles=%u | room=r%03X Leon=(%.0f, %.0f, %.0f) | mouse total=(%.0f, %.0f) | shots to RE4=%u blasts=%u",
        blocker ? blocker : "MW2 ACTIVE", g_guest.ready, frame->sequence, frame->width, frame->height,
        g_guestMovement.sequence, now - g_guestMovement.tick, g_guestMovement.grounded, g_guestCamera.sequence,
        g_sceneCount, g ? At<uint16_t>(g, re4::G_curRoomId) : 0, pos.x, pos.y, pos.z, g_mouseTotalX, g_mouseTotalY,
        g_hitsSent, g_blastsSent);
}

// RE4 input while MW2 has control: Leon's own logic keeps running (damage, grabs, doors, items),
// but only RE4's action/menu buttons reach him; movement, aim, fire and knife are MW2's.
using PadReadFn = void(__cdecl*)();
PadReadFn g_padReadOrig = nullptr;
bool g_useWasDown = false;
DWORD g_lastUse = 0; // when E was last pressed in MW2 mode
void __cdecl PadReadHook()
{
    g_padReadOrig();
    if (!g_active || !g_keyOn || !g_keyTrg)
        return;
    // A merchant, the case, a call or the game-over screen opened: RE4's camera stopped, so only here can
    // it be seen. RE4 gets its keys, mouse and text back now (MW2 resumes when gameplay does).
    static LONG padFrame = 0;
    static int padsWithoutCamera = 0;
    padsWithoutCamera = g_frame != padFrame ? 0 : padsWithoutCamera + 1;
    padFrame = g_frame;
    if (const char* screen = Re4ScreenBlocker())
    {
        Log("MW2 suspended: %s", screen);
        Restore();
        return;
    }
    if (PickupScreen())
    {
        RestorePrompts();
        return; // every key to RE4: accept or leave the item
    }
    // Paused with no screen of its own (an item pickup freezes RE4 for about half a second, the pause menu):
    // MW2 keeps Leon, hidden, and carries on when RE4 does; meanwhile RE4 gets all its keys.
    if (padsWithoutCamera >= 4 && GetTickCount() - g_lastTick > 200)
        return;
    // RE4's mouse deltas share their address with Joy[0]'s left stick: read them before zeroing it.
    if (g_mouseX && g_mouseY)
    {
        g_mousePendingX += (float)*g_mouseX;
        g_mousePendingY += (float)*g_mouseY;
    }
    uint64_t allowed = MenuOpen() ? 0 : re4::KEY_ALLOWED; // the menu owns arrows/Enter
    *g_keyOn &= allowed;
    *g_keyTrg &= allowed;
    bool use = Focused() && Down('E');
    if (use)
    {
        *g_keyOn |= re4::KEY_USE;
        if (!g_useWasDown)
        {
            *g_keyTrg |= re4::KEY_USE;
            g_lastUse = GetTickCount();
            uint8_t* pl = Player();
            if (pl && g_actBtnActive && *g_actBtnActive && !g_actionStart)
            {
                g_actionStart = GetTickCount();
                g_actionSeenChange = false;
                memcpy(g_actionRno, &At<uint8_t>(pl, re4::Model_rno0), 3);
            }
        }
    }
    g_useWasDown = use;
    if (g_joy)
    {
        At<int32_t>(g_joy, re4::Joy_leftX) = At<int32_t>(g_joy, re4::Joy_leftY) = 0;
        At<int32_t>(g_joy, re4::Joy_pcLeftX) = At<int32_t>(g_joy, re4::Joy_pcLeftY) = 0;
    }
}

// Runs once per frame inside CameraControl::move, right before RE4 builds its view matrices.
using CameraSetFn = void(__cdecl*)(uint8_t* camera);
CameraSetFn g_cameraSetOrig = nullptr;
void __cdecl CameraSetHook(uint8_t* camera)
{
    ++g_frame;
    uint8_t* g = Global();
    uint8_t* pl = Player();
    bool focused = Focused();
    bool key = focused && Down(g_cfg.toggleKey);
    if (key && !g_keyWasDown)
    {
        g_requested = !g_requested;
        if (g_requested && !StartRuntime())
            g_requested = false;
        Log(g_requested ? "F6: MW2 requested" : "F6: MW2 off");
        if (!g_requested)
            Restore();
    }
    g_keyWasDown = key;
    bool helpKey = focused && Down(VK_F7);
    if (helpKey && !g_helpKeyWasDown)
    {
        g_cfg.showControls = !g_cfg.showControls;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%sMW2RE4.ini", g_dir);
        WritePrivateProfileStringA("MW2RE4", "ShowControls", g_cfg.showControls ? "1" : "0", path);
    }
    g_helpKeyWasDown = helpKey;
    WeaponMenuInput(focused);
    if (!g_active)
        UpdateTrace(pl);

    const char* blocker = Blocker(g, pl);
    if (blocker && g_active)
    {
        Log("MW2 suspended: %s", blocker);
        Restore();
    }
    else if (!blocker && !g_active)
        Activate(g, pl);

    Exchange(g, pl);
    if (g_tickEvent)
        SetEvent(g_tickEvent); // MW2's next frame is this RE4 frame's (its view was just written)
    SafeArsenalUpdate(focused);
    g_lastTick = GetTickCount();
    if (g_active && g)
    {
        // scripts can reset display flags; keep Leon and RE4's HUD hidden while MW2 has control
        uint32_t& disp = At<uint32_t>(g, re4::G_dispFlags);
        if (g_cfg.hidePlayer)
            disp |= re4::DPF_PL;
        disp |= re4::DPF_COCKPIT;
    }
    if (g && pl)
    {
        int hp = At<int16_t>(g, re4::G_plHp);
        if (g_lastHp >= 0 && hp < g_lastHp && g_active)
        {
            int hpMax = std::max<int>(1, At<int16_t>(g, re4::G_plHpMax));
            g_damageUntil = GetTickCount() + 850;
            g_damageStrength = std::clamp((g_lastHp - hp) * 4.f / hpMax, .35f, 1.f);
            Log("Leon hurt: %d -> %d of %d", g_lastHp, hp, hpMax);
        }
        g_lastHp = hp;
    }
    ResolveShots(pl);
    bool itemGet = ItemGetOpen();
    static bool itemGetWas = false;
    if (itemGet != itemGetWas)
        Log("RE4 item pickup screen %s (%lu ms after E)", itemGet ? "open" : "closed", GetTickCount() - g_lastUse);
    itemGetWas = itemGet;
    if (g_active && (itemGet || PickupScreen()))
        RestorePrompts();
    else if (g_active)
        RaisePrompts();
    if (g_active && pl)
    {
        ApplyMovement(pl);
        ApplyCamera(camera);
    }
    Status(g, pl, blocker);
    g_cameraSetOrig(camera);
}

// ---------------------------------------------------------------- overlay (D3D9)
IDirect3DTexture9* g_texture = nullptr;
IDirect3DPixelShader9* g_chroma = nullptr;
IDirect3DDevice9* g_chromaDevice = nullptr;
std::vector<uint8_t> g_pixels;
uint32_t g_imageW = 0, g_imageH = 0, g_lastFrame = 0;
bool g_drawnThisPresent = false;

void ReleaseTexture()
{
    if (g_texture) { g_texture->Release(); g_texture = nullptr; }
    g_lastFrame = 0;
}

void CreateChromaShader(IDirect3DDevice9* device)
{
    if (g_chromaDevice == device)
        return;
    g_chromaDevice = device;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                       UINT, UINT, ID3DBlob**, ID3DBlob**);
    static HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? (CompileFn)GetProcAddress(compiler, "D3DCompile") : nullptr;
    if (!compile)
    {
        Log("Chroma shader compiler unavailable; using CPU fallback");
        return;
    }
    // MW2 draws its weapon and HUD over magenta; this turns magenta into transparency (from MW2-GTA).
    const char* source = R"(
 sampler2D image : register(s0);
 float4 main(float2 uv:TEXCOORD0,float4 tint:COLOR0):COLOR0 {
   float3 rgb=tex2D(image,uv).rgb;
   float matte=max(0,min(rgb.r,rgb.b)-rgb.g);
   float alpha=1-matte;
   float3 foreground=saturate((rgb-float3(matte,0,matte))/max(alpha,0.00392157));
   return float4(foreground,alpha)*tint;
 }
 )";
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(source, strlen(source), "MW2RE4Chroma", nullptr, nullptr, "main", "ps_3_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (SUCCEEDED(hr) && code)
        hr = device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &g_chroma);
    if (errors) { Log("%s", (const char*)errors->GetBufferPointer()); errors->Release(); }
    if (code) code->Release();
    Log(SUCCEEDED(hr) ? "GPU chroma shader ready" : "GPU chroma shader failed; using CPU fallback");
}

void CpuChroma(uint8_t* target, const uint8_t* source, uint32_t width)
{
    for (uint32_t x = 0; x < width; x++)
    {
        auto* p = source + x * 4;
        auto* q = target + x * 4;
        unsigned matte = std::min(p[0], p[2]) > p[1] ? std::min(p[0], p[2]) - p[1] : 0;
        unsigned alpha = 255 - matte;
        q[3] = (uint8_t)alpha;
        for (int ch = 0; ch < 3; ch++)
        {
            unsigned contribution = ch == 1 ? p[ch] : (p[ch] > matte ? p[ch] - matte : 0);
            q[ch] = alpha ? (uint8_t)std::min(255u, (contribution * 255 + alpha / 2) / alpha) : 0;
        }
    }
}

struct Vertex { float x, y, z, rhw; DWORD color; float u, v; };
void Quad(IDirect3DDevice9* device, float x, float y, float w, float h, DWORD color, IDirect3DTexture9* tex = nullptr)
{
    Vertex v[4] = { { x - .5f, y - .5f, 0, 1, color, 0, 0 }, { x + w - .5f, y - .5f, 0, 1, color, 1, 0 },
                    { x - .5f, y + h - .5f, 0, 1, color, 0, 1 }, { x + w - .5f, y + h - .5f, 0, 1, color, 1, 1 } };
    device->SetTexture(0, tex);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
}

// Text drawn with GDI into a texture once, then reused until the text or size changes.
// White glyphs, alpha from coverage; the colour comes from the quad's vertex colour.
struct TextTexture
{
    std::string text;
    int pixelHeight = 0;
    IDirect3DTexture9* texture = nullptr;
    UINT width = 0, height = 0;
};
TextTexture g_helpText, g_statusText, g_menuText, g_noticeText, g_hpText, g_settingsText, g_damageText, g_gunListText, g_partnerText;

void ReleaseText(TextTexture& t)
{
    if (t.texture) { t.texture->Release(); t.texture = nullptr; }
    t.text.clear();
}

bool UpdateText(IDirect3DDevice9* device, TextTexture& t, const std::string& text, int pixelHeight)
{
    if (t.texture && t.text == text && t.pixelHeight == pixelHeight)
        return true;
    ReleaseText(t);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontA(-pixelHeight, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    std::vector<std::string> lines;
    for (size_t start = 0;;)
    {
        size_t end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    int lineH = pixelHeight * 5 / 4, w = 1;
    for (auto& l : lines)
    {
        SIZE sz{};
        GetTextExtentPoint32A(dc, l.c_str(), (int)l.size(), &sz);
        w = std::max(w, (int)sz.cx);
    }
    int h = lineH * (int)lines.size();
    w += 4;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits)
    {
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)w * h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        for (size_t i = 0; i < lines.size(); ++i)
            TextOutA(dc, 2, (int)i * lineH, lines[i].c_str(), (int)lines[i].size());
        GdiFlush();
        if (SUCCEEDED(device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.texture, nullptr)))
        {
            D3DLOCKED_RECT rect{};
            if (SUCCEEDED(t.texture->LockRect(0, &rect, nullptr, 0)))
            {
                for (int y = 0; y < h; ++y)
                {
                    auto* src = (uint8_t*)bits + (size_t)y * w * 4;
                    auto* dst = (uint8_t*)rect.pBits + (size_t)y * rect.Pitch;
                    for (int x = 0; x < w; ++x)
                    {
                        uint8_t a = std::max({ src[x * 4], src[x * 4 + 1], src[x * 4 + 2] });
                        dst[x * 4] = dst[x * 4 + 1] = dst[x * 4 + 2] = 255;
                        dst[x * 4 + 3] = a;
                    }
                }
                t.texture->UnlockRect(0);
                ok = true;
            }
        }
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    if (!ok) { ReleaseText(t); return false; }
    t.text = text;
    t.pixelHeight = pixelHeight;
    t.width = w;
    t.height = h;
    return true;
}

// Panel with a dark background and the text on top, with a 1-pixel shadow for legibility.
void DrawPanel(IDirect3DDevice9* device, TextTexture& t, float x, float y, DWORD color)
{
    float pad = t.pixelHeight * .5f;
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    Quad(device, x - pad, y - pad, t.width + 2 * pad, t.height + 2 * pad, 0xa0101010);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    Quad(device, x + 1, y + 1, (float)t.width, (float)t.height, 0xff000000, t.texture);
    Quad(device, x, y, (float)t.width, (float)t.height, color, t.texture);
}

const char* kHelp =
    "MW2 CONTROLS   (F7 hide)\n"
    "WASD  move        Mouse  look\n"
    "Shift  sprint     Space  jump\n"
    "C  crouch         Ctrl  prone\n"
    "Left click  fire  Right click  aim\n"
    "R  reload         1  next gun in the case\n"
    "G  grenade        H  tactical\n"
    "V  melee          E  use: doors, items\n"
    "Guns and ammo: Leon's attache case\n"
    "F5  gun list in the case (on / off)\n"
    "F8  pick the MW2 gun for the equipped RE4 gun\n"
    "F10  settings: FOV, mouse, ammo, skin, resolution\n"
    "F11  gun damage\n"
    "RE4 inventory, map and menu keys work\n"
    "F6  MW2 on / off";

std::string StatusText()
{
    if (g_active) return "";
    if (!g_runtimeError.empty() && !g_requested) return "MW2: " + g_runtimeError + " - press F6 to retry";
    if (!g_requested) return "";
    if (g_guest.ready && GetTickCount() - g_guest.tick < 1500) return "MW2 ready - waiting for RE4 gameplay";
    char text[128];
    DWORD seconds = (GetTickCount() - g_runtimeStarted) / 1000;
    if (seconds < 60)
        snprintf(text, sizeof(text), "MW2 loading... %lus  (F6 cancels)", seconds);
    else
        snprintf(text, sizeof(text), "MW2 loading... %lus  (the first start can take several minutes, F6 cancels)",
                 seconds);
    return text;
}

// Small square, top left: yellow = MW2 loading, white = ready but waiting, red = error. Hidden while active.
DWORD IndicatorColor()
{
    if (g_active) return 0;
    if (!g_runtimeError.empty() && !g_requested) return 0xffd03030;
    if (!g_requested) return 0;
    if (g_guest.ready && GetTickCount() - g_guest.tick < 1500) return 0xffffffff;
    return 0xffe0c020;
}

void DrawOverlay(IDirect3DDevice9* device)
{
    if (!g_shared)
        return;
    DWORD indicator = IndicatorColor();
    // Menus and pause stop RE4's gameplay camera: hide MW2 after two presented frames without one.
    static LONG lastFrameSeen = 0;
    static int missed = 0;
    missed = g_frame != lastFrameSeen ? 0 : missed + 1;
    lastFrameSeen = g_frame;
    bool ticking = missed < 2 && GetTickCount() - g_lastTick < 200;
    if (!ticking)
    {
        indicator = 0;
        Lock lock(g_controlMutex);
        if (lock.acquired)
            ((Control*)g_shared)->host.enabled = 0; // MW2 ignores input while RE4 is paused
    }
    // RE4's item pickup screen: MW2's hand goes the moment it opens
    bool drawMw2 = g_active && ticking && !ItemGetOpen();
    uint8_t* gl = Global();
    // F5 shows or hides the gun list (checked here: the case and the merchant stop RE4's camera hook)
    static bool f5WasDown = false, shopHidden = false;
    bool f5 = Focused() && Down(VK_F5);
    bool shop = gl && (At<uint32_t>(gl, re4::G_statusFlags + 8) & re4::STA2_INTO_SHOP);
    if (!shop)
        shopHidden = false;
    if (f5 && !f5WasDown && g_requested && ArsenalReady() && shop && g_cfg.shopGunNames && !g_cfg.showGunList)
        shopHidden = !shopHidden; // the merchant's own list: F5 hides it for this visit
    else if (f5 && !f5WasDown && g_requested && ArsenalReady())
    {
        g_cfg.showGunList = !g_cfg.showGunList;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%sMW2RE4.ini", g_dir);
        WritePrivateProfileStringA("MW2RE4", "ShowGunList", g_cfg.showGunList ? "1" : "0", path);
        Log("F5: gun list %s", g_cfg.showGunList ? "on" : "off");
    }
    f5WasDown = f5;
    bool gunList = (g_cfg.showGunList || (shop && g_cfg.shopGunNames && !shopHidden)) && g_requested &&
                   ArsenalReady() && gl && (shop || (At<uint32_t>(gl, re4::G_statusFlags) & re4::STA_SUB_SCRN));
    if (!drawMw2 && !indicator && !gunList)
        return;
    CreateChromaShader(device);
    uint32_t sequence = 0;
    if (g_active)
    {
        Lock lock(g_frameMutex);
        if (lock.acquired)
        {
            auto* frame = (Frame*)(g_shared + FrameOffset);
            if (frame->sequence != g_lastFrame && frame->width && frame->height && frame->width <= MaxWidth &&
                frame->height <= MaxHeight)
            {
                g_imageW = frame->width;
                g_imageH = frame->height;
                sequence = frame->sequence;
                g_pixels.assign(g_shared + PixelOffset, g_shared + PixelOffset + g_imageW * g_imageH * 4);
            }
        }
    }
    if (sequence)
    {
        D3DSURFACE_DESC desc{};
        if (g_texture)
            g_texture->GetLevelDesc(0, &desc);
        if (g_texture && (desc.Width != g_imageW || desc.Height != g_imageH))
            ReleaseTexture();
        if (!g_texture)
            device->CreateTexture(g_imageW, g_imageH, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_texture, nullptr);
        D3DLOCKED_RECT rect{};
        if (g_texture && SUCCEEDED(g_texture->LockRect(0, &rect, nullptr, D3DLOCK_DISCARD)))
        {
            for (uint32_t y = 0; y < g_imageH; y++)
            {
                auto* row = (uint8_t*)rect.pBits + y * rect.Pitch;
                auto* src = g_pixels.data() + y * g_imageW * 4;
                if (g_chroma) memcpy(row, src, g_imageW * 4);
                else CpuChroma(row, src, g_imageW);
            }
            g_texture->UnlockRect(0);
            g_lastFrame = sequence;
        }
    }

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
        return;
    D3DSURFACE_DESC screen{};
    backBuffer->GetDesc(&screen);
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)))
    {
        backBuffer->Release();
        return;
    }
    saved->Capture();
    IDirect3DSurface9* previousTarget = nullptr;
    device->GetRenderTarget(0, &previousTarget);
    device->SetRenderTarget(0, backBuffer);
    if (SUCCEEDED(device->BeginScene()))
    {
        D3DVIEWPORT9 full{ 0, 0, screen.Width, screen.Height, 0.f, 1.f };
        device->SetViewport(&full);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        float w = (float)screen.Width, h = (float)screen.Height;
        if (drawMw2 && g_texture)
        {
            device->SetPixelShader(g_chroma);
            Quad(device, 0, 0, w, h, 0xffffffff, g_texture);
            device->SetPixelShader(nullptr);
        }
        int textPx = std::max(12, (int)(h * .022f));
        if (indicator)
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            float s = std::max(10.f, h * .02f);
            Quad(device, s, s, s, s, 0xff000000);
            Quad(device, s + 2, s + 2, s - 4, s - 4, indicator);
            std::string status = StatusText();
            if (!status.empty() && UpdateText(device, g_statusText, status, textPx))
                DrawPanel(device, g_statusText, s * 2.6f, s - textPx * .1f, indicator);
        }
        if (drawMw2 && g_cfg.showControls && !MenuOpen() && UpdateText(device, g_helpText, kHelp, textPx))
        {
            float m = h * .03f;
            DrawPanel(device, g_helpText, m, m, 0xffffffff);
        }
        if (drawMw2)
        {
            // Leon's health (RE4 owns health): bar bottom left, red screen edges when hurt.
            uint8_t* g = Global();
            int hp = g ? At<int16_t>(g, re4::G_plHp) : 0, hpMax = g ? std::max<int>(1, At<int16_t>(g, re4::G_plHpMax)) : 1;
            float frac = std::clamp((float)hp / hpMax, 0.f, 1.f);
            float bx = w * .025f, by = h * .93f, bw = w * .16f, bh = std::max(8.f, h * .014f);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            Quad(device, bx - 3, by - 3, bw + 6, bh + 6, 0xc0181818);
            Quad(device, bx, by, bw, bh, 0xff505050);
            if (frac > 0)
                Quad(device, bx, by, bw * frac, bh, frac < .3f ? 0xffdf4545 : (frac < .6f ? 0xffe0c040 : 0xff7ed091));
            DWORD now = GetTickCount();
            if (now < g_damageUntil)
            {
                DWORD a = (DWORD)(150 * g_damageStrength * (g_damageUntil - now) / 850.f);
                DWORD c = (a << 24) | 0x00b00000;
                Quad(device, 0, 0, w, h * .045f, c);
                Quad(device, 0, h * .955f, w, h * .045f, c);
                Quad(device, 0, h * .045f, w * .025f, h * .91f, c);
                Quad(device, w * .975f, h * .045f, w * .025f, h * .91f, c);
            }
            if (uint8_t* partner = g ? Partner() : nullptr)
            {
                // Ashley's (or Luis's) health, next to Leon's: RE4's own HUD is hidden while MW2 has control
                int ahp = At<int16_t>(g, re4::G_subHp), ahpMax = std::max<int>(1, At<int16_t>(g, re4::G_subHpMax));
                float af = std::clamp((float)ahp / ahpMax, 0.f, 1.f);
                float ax = bx + bw + w * .03f, aw = bw * .75f;
                device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
                Quad(device, ax - 3, by - 3, aw + 6, bh + 6, 0xc0181818);
                Quad(device, ax, by, aw, bh, 0xff505050);
                if (af > 0)
                    Quad(device, ax, by, aw * af, bh, af < .3f ? 0xffdf4545 : 0xffe8a0c8);
                uint8_t id = At<uint8_t>(partner, re4::Model_id);
                char aText[48];
                snprintf(aText, sizeof(aText), "%s %d / %d", id == 0x04 ? "LUIS" : "ASHLEY", std::max(0, ahp), ahpMax);
                if (UpdateText(device, g_partnerText, aText, std::max(10, textPx * 4 / 5)))
                {
                    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                    Quad(device, ax + 1, by - g_partnerText.height - 2.f, (float)g_partnerText.width, (float)g_partnerText.height, 0xff000000, g_partnerText.texture);
                    Quad(device, ax, by - g_partnerText.height - 3.f, (float)g_partnerText.width, (float)g_partnerText.height, 0xffffffff, g_partnerText.texture);
                }
            }
            char hpText[32];
            snprintf(hpText, sizeof(hpText), "HP %d / %d", std::max(0, hp), hpMax);
            if (UpdateText(device, g_hpText, hpText, std::max(10, textPx * 4 / 5)))
            {
                device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                Quad(device, bx + 1, by - g_hpText.height - 2.f, (float)g_hpText.width, (float)g_hpText.height, 0xff000000, g_hpText.texture);
                Quad(device, bx, by - g_hpText.height - 3.f, (float)g_hpText.width, (float)g_hpText.height, 0xffffffff, g_hpText.texture);
            }
        }
        if (drawMw2 && g_damageMenu && UpdateText(device, g_damageText, DamageText(), (int)(textPx * 1.05f)))
            DrawPanel(device, g_damageText, (w - g_damageText.width) / 2, h * .12f, 0xfff0f0f0);
        if (drawMw2 && g_settingsMenu && UpdateText(device, g_settingsText, SettingsText(), (int)(textPx * 1.15f)))
            DrawPanel(device, g_settingsText, (w - g_settingsText.width) / 2, h * .22f, 0xfff0f0f0);
        if (drawMw2 && g_weaponMenu && UpdateText(device, g_menuText, WeaponMenuText(), (int)(textPx * 1.15f)))
            DrawPanel(device, g_menuText, (w - g_menuText.width) / 2, h * .16f, 0xfff0f0f0);
        if (gunList && UpdateText(device, g_gunListText, GunListText(), std::max(10, textPx * 4 / 5)))
            DrawPanel(device, g_gunListText, w - g_gunListText.width - h * .02f, h * .98f - g_gunListText.height, 0xffffd746);
        if (drawMw2 && GetTickCount() < g_noticeUntil && UpdateText(device, g_noticeText, g_notice, textPx))
            DrawPanel(device, g_noticeText, h * .03f, h * .80f, 0xffffd746);
        device->EndScene();
    }
    if (previousTarget)
    {
        device->SetRenderTarget(0, previousTarget);
        previousTarget->Release();
    }
    backBuffer->Release();
    saved->Apply();
    saved->Release();
}

using PresentFn = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SwapPresentFn = HRESULT(WINAPI*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
PresentFn g_presentOrig = nullptr;
ResetFn g_resetOrig = nullptr;
SwapPresentFn g_swapPresentOrig = nullptr;
bool g_loggedPresent = false;

void SafeDrawOverlay(IDirect3DDevice9* device)
{
    __try
    {
        DrawOverlay(device);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("overlay draw faulted");
    }
}

void BeforePresent(IDirect3DDevice9* device, const char* via)
{
    if (!g_loggedPresent)
    {
        g_loggedPresent = true;
        Log("first frame presented via %s; overlay hook working", via);
        InstallCrashFilter();
    }
    SafeDrawOverlay(device);
    if (g_shared)
    {
        Lock lock(g_controlMutex);
        if (lock.acquired)
            ((Control*)g_shared)->host.tick = GetTickCount();
    }
}

HRESULT WINAPI PresentHook(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty)
{
    BeforePresent(device, "device Present");
    g_drawnThisPresent = true;
    HRESULT hr = g_presentOrig(device, src, dst, window, dirty);
    g_drawnThisPresent = false;
    return hr;
}

HRESULT WINAPI SwapPresentHook(IDirect3DSwapChain9* chain, const RECT* src, const RECT* dst, HWND window,
                               const RGNDATA* dirty, DWORD flags)
{
    if (!g_drawnThisPresent)
    {
        IDirect3DDevice9* device = nullptr;
        if (SUCCEEDED(chain->GetDevice(&device)) && device)
        {
            BeforePresent(device, "swap chain Present");
            device->Release();
        }
    }
    return g_swapPresentOrig(chain, src, dst, window, dirty, flags);
}

HRESULT WINAPI ResetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
{
    ReleaseTexture();
    Log("D3D9 device reset");
    return g_resetOrig(device, params);
}

void PatchSlot(void** table, int slot, void* hook, void** orig)
{
    DWORD old;
    if (table[slot] == hook)
        return;
    if (VirtualProtect(&table[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
    {
        *orig = table[slot];
        table[slot] = hook;
        VirtualProtect(&table[slot], sizeof(void*), old, &old);
    }
}

void HookDevice(IDirect3DDevice9* device)
{
    void** table = *(void***)device;
    PatchSlot(table, 16, (void*)&ResetHook, (void**)&g_resetOrig);
    PatchSlot(table, 17, (void*)&PresentHook, (void**)&g_presentOrig);
    IDirect3DSwapChain9* chain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &chain)) && chain)
    {
        PatchSlot(*(void***)chain, 3, (void*)&SwapPresentHook, (void**)&g_swapPresentOrig);
        chain->Release();
    }
    Log("overlay: hooked RE4's D3D9 device (Present/Reset)");
}

// RE4 creates its Direct3D object through one call; wrap it so the overlay attaches to the exact
// device RE4 creates (a generic probe device can have a different vtable, e.g. a pure device).
using CreateDeviceFn = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
CreateDeviceFn g_createDeviceOrig = nullptr;
HRESULT WINAPI CreateDeviceHook(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out)
{
    HRESULT hr = g_createDeviceOrig(d3d, adapter, type, window, flags, params, out);
    Log("RE4 created its D3D9 device: hr=0x%08lX flags=0x%08lX %ux%u windowed=%d", hr, flags,
        params ? params->BackBufferWidth : 0, params ? params->BackBufferHeight : 0, params ? params->Windowed : -1);
    if (SUCCEEDED(hr) && out && *out)
        HookDevice(*out);
    return hr;
}

using D3DCreateFn = IDirect3D9*(WINAPI*)(UINT);
D3DCreateFn g_d3dCreateOrig = nullptr;
IDirect3D9* WINAPI D3DCreateHook(UINT version)
{
    IDirect3D9* d3d = g_d3dCreateOrig(version);
    if (d3d)
        PatchSlot(*(void***)d3d, 16, (void*)&CreateDeviceHook, (void**)&g_createDeviceOrig);
    Log("RE4 created its Direct3D9 object (%p)", (void*)d3d);
    return d3d;
}

// ---------------------------------------------------------------- startup
bool g_ready = false;
DWORD WINAPI StartBridge(void*);

bool Init()
{
    Module m = ReadModule();
    uint8_t* a = Find(m, re4::kGlobalPtr);
    uint8_t* b = Find(m, re4::kPlayerPtr);
    uint8_t* c = Find(m, re4::kHitCheckCall);
    uint8_t* d = Find(m, re4::kCameraSetCall);
    uint8_t* e = Find(m, re4::kMouseX);
    uint8_t* f = Find(m, re4::kMouseY);
    uint8_t* s = Find(m, re4::kSatMgr);
    uint8_t* x = Find(m, re4::kD3DCreateCall);
    uint8_t* ko = Find(m, re4::kKeyOn);
    uint8_t* kt = Find(m, re4::kKeyTrg);
    uint8_t* jy = Find(m, re4::kJoy);
    uint8_t* pr = Find(m, re4::kPadReadCall);
    uint8_t* em = Find(m, re4::kEmMgr);
    if (uint8_t* bh = Find(m, re4::kBulletHit))
        g_bulletHit = (BulletHitFn)bh;
    if (uint8_t* fa = Find(m, re4::kFireArea))
    {
        g_fireList = *(uint8_t**)(fa + 1);
        g_fireArea = (FireAreaFn)(fa + 24 + *(int32_t*)(fa + 20));
    }
    if (uint8_t* ab = Find(m, re4::kActBtn))
        g_actBtnActive = *(uint8_t**)ab;
    if (uint8_t* fx = Find(m, re4::kExplosionFx))
    {
        g_effect = (EffectFn)(fx + 0x17 + 5 + *(int32_t*)(fx + 0x18));
        g_sound = (SoundFn)(fx + 0x27 + 5 + *(int32_t*)(fx + 0x28));
    }
    if (uint8_t* q = Find(m, re4::kQteActive))
        g_qteActive = *(uint8_t**)q;
    if (g_cfg.grenadeTrace)
        InstallGrenadeTrace((uint8_t*)GetModuleHandleA(nullptr));
    if (uint8_t* ld = Find(m, re4::kLifeDown))
    {
        // the JMP thunk all callers use
        uint8_t* text = m.code.empty() ? nullptr : m.code[0].begin;
        uint8_t* thunk = nullptr;
        for (size_t i = 0; text && i + 5 < 0x10000; ++i)
            if (text[i] == 0xE9 && text + i + 5 + *(int32_t*)(text + i + 1) == ld)
            {
                thunk = text + i;
                break;
            }
        if (thunk && HookThunk(thunk, (void*)&LifeDownHook, &g_lifeDownOrig))
            Log("damage sliders: LifeDownSet2 hooked at thunk 0x%p", (void*)thunk);
        else
            Log("damage sliders: LifeDownSet2 thunk not found; sliders have no effect");
    }
    if (uint8_t* ms = Find(m, re4::kMesSetCall))
    {
        uint8_t* thunk = ms[0] == 0xE8 ? ms + 5 + *(int32_t*)(ms + 1) : nullptr;
        if (thunk && HookThunk(thunk, (void*)&MesSetHook, (void**)&g_mesSetOrig))
            Log("captions: MesSet hooked at thunk 0x%p", (void*)thunk);
    }
    auto callTarget = [](uint8_t* site) -> uint8_t* {
        return site && site[0] == 0xE8 ? site + 5 + *(int32_t*)(site + 1) : nullptr;
    };
    if (uint8_t* im = Find(m, re4::kItemMgr))
        g_itemMgr = *(uint8_t**)im;
    g_itemArm = (ItemArmFn)callTarget(Find(m, re4::kItemArm));
    g_itemErase = (ItemEraseFn)callTarget(Find(m, re4::kItemErase));
    g_chargeNum = (ChargeNumFn)callTarget(Find(m, re4::kChargeNum));
    g_weaponNo = (WeaponNoFn)callTarget(Find(m, re4::kWeaponNo));
    g_weaponChange = (WeaponChangeFn)callTarget(Find(m, re4::kWeaponChange));
    g_specialTuned = (SpecialTunedFn)callTarget(Find(m, re4::kSpecialTuned));
    LoadGunMap();
    Log("attache case: ItemMgr 0x%p arm %p erase %p chargeNum %p weaponNo %p weaponChange %p specialTuned %p -> %s",
        (void*)g_itemMgr, (void*)g_itemArm, (void*)g_itemErase, (void*)g_chargeNum, (void*)g_weaponNo,
        (void*)g_weaponChange, (void*)g_specialTuned,
        ArsenalReady() ? "MW2 uses RE4's guns and ammo" : "MW2 keeps its own guns (Re4Inventory=0 or a pattern is missing)");
    if (uint8_t* as = Find(m, re4::kPartnerPtr))
        g_pAS = *(uint8_t***)as;
    if (uint8_t* ids = Find(m, re4::kIdSys))
        g_idSys = *(uint8_t**)ids;
    if (em)
        g_emMgr = *(uint8_t**)em;
    if (!m.code.empty())
    {
        g_textBegin = m.code[0].begin;
        g_textEnd = m.code[0].begin + m.code[0].size;
    }
    if (!a || !b || !c || !d || !e || !f || !s || !x || !ko || !kt || !jy || !pr)
        return false;
    g_keyOn = *(uint64_t**)ko;
    g_keyTrg = *(uint64_t**)kt;
    g_joy = *(uint8_t**)jy - 9;
    g_pG = *(uint8_t***)a;
    g_pPL = *(uint8_t***)b;
    g_mouseX = *(int32_t**)e;
    g_mouseY = *(int32_t**)f;
    g_satMgr = *(uint8_t**)s;
    Log("pG=0x%p pPL=0x%p mouse=0x%p/0x%p SatMgr=0x%p", (void*)g_pG, (void*)g_pPL, (void*)g_mouseX, (void*)g_mouseY,
        (void*)g_satMgr);
    if (!HookCall(c, (void*)&HitCheckHook, (void**)&g_hitCheckOrig))
        return false;
    if (!HookCall(d, (void*)&CameraSetHook, (void**)&g_cameraSetOrig))
        return false;
    if (!HookCall(x, (void*)&D3DCreateHook, (void**)&g_d3dCreateOrig))
        return false;
    if (!HookCall(pr, (void*)&PadReadHook, (void**)&g_padReadOrig))
        return false;
    Log("input filter on PadRead: keys 0x%p/0x%p joy 0x%p", (void*)g_keyOn, (void*)g_keyTrg, (void*)g_joy);
    if (g_cfg.damageTrace && g_emMgr)
    {
        AddVectoredExceptionHandler(1, TraceHandler);
        CreateThread(nullptr, 0, TraceArmer, nullptr, 0, nullptr);
        Log("damage trace on: shoot a Ganado with Leon's own gun (F6 off); writers are logged as TRACE lines");
    }
    Log("hooked CameraQuasiFPS::hitCheck and the camera set call in CameraControl::move");
    return true;
}

DWORD WINAPI Startup(void*)
{
    // The exe may still be unpacking when we load; retry for up to 30 s.
    for (int i = 0; i < 60 && !g_ready; ++i)
    {
        Sleep(500);
        g_ready = Init();
    }
    if (!g_ready)
    {
        Log("startup failed: nothing hooked, game unchanged");
        return 0;
    }
    return StartBridge(nullptr);
}

DWORD WINAPI StartBridge(void*)
{
    if (!CreateBridge())
        Log("shared memory failed; MW2 cannot start");
    Log("ready: load a save, then press F6 to start MW2 (about a minute the first time)");
    return 0;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        GetModuleFileNameA(module, g_dir, MAX_PATH);
        if (char* slash = strrchr(g_dir, '\\'))
            slash[1] = 0;
        OpenLog();
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExA(exe, GetFileExInfoStandard, &fa);
        Log("%s loaded into %s (%lu bytes)", kVersion, exe, fa.nFileSizeLow);
        InstallCrashFilter();
        LoadSettings();
        g_ready = Init(); // patch the game before it runs, as in phase 1
        CreateThread(nullptr, 0, g_ready ? StartBridge : Startup, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_shared)
        {
            ((Control*)g_shared)->host.shutdown = 1; // MW2 runtime exits on its own
        }
    }
    return TRUE;
}
