#pragma once
#include <cstdint>
#include <cstddef>
namespace mw2gta {
constexpr uint32_t Magic=0x4154474D, Version=4, ShotCount=64;
constexpr uint32_t FrameOffset=8192, MaxWidth=1280, MaxHeight=720;
constexpr uint32_t SceneOffset=2097152, TriangleOffset=SceneOffset+64, MaxTriangles=8192;
constexpr uint32_t FeaturesOffset=TriangleOffset+MaxTriangles*40;
constexpr uint32_t CatalogOffset=FeaturesOffset+64, CatalogCount=128;
constexpr uint32_t ProjectilesOffset=CatalogOffset+CatalogCount*64, ProjectileCount=32;
constexpr uint32_t ExplosionsOffset=ProjectilesOffset+ProjectileCount*48, ExplosionCount=16;
// Keep control, scene, inventory and native profiling offsets stable. The
// larger image follows all metadata instead of overlapping the scene buffer.
constexpr uint32_t PixelOffset=ExplosionsOffset+ExplosionCount*48;
constexpr uint32_t MappingSize=PixelOffset+MaxWidth*MaxHeight*4;
struct Features {
 uint32_t request,kind,weapon,ack,outcome,catalogCount,projectileCount,explosions,tick;
 int32_t lethalAmmo,tacticalAmmo;
 uint32_t reserved[5];
};
struct CatalogEntry {uint32_t weapon,kind;char name[56];};
struct Projectile {uint32_t id,weapon;float origin[3],angles[3];uint32_t kind,tick,reserved[2];};
struct Explosion {uint32_t sequence,id,weapon,kind;float origin[3],radius,innerDamage,outerDamage;uint32_t tick,reserved;};
static_assert(sizeof(Features)==64 && sizeof(CatalogEntry)==64 && sizeof(Projectile)==48 && sizeof(Explosion)==48);
struct Host {
 uint32_t tick, enabled;
 float yaw,pitch,fov,dt;
 float velocity[3],eye[3];
 uint32_t buttons;
 float health;
 uint32_t shutdown;
};
struct Guest {
 uint32_t tick,ready;
 int32_t clip,stock;
 uint32_t shots,reserved[3];
 float ads,kickPitch,kickYaw,dt;
};
struct HostMovement {
 uint32_t epoch,tick,sceneSequence,flags;
 float foot[3];
 uint32_t reserved[5];
};
struct GuestMovement {
 uint32_t tick,epoch;
 float foot[3],velocity[3],viewHeight;
 uint32_t pmFlags,bobCycle,grounded;
 float dt;
 uint32_t sequence,reserved[2];
};
// Lock-on target from the host's aim, in host coordinates. flags: 1 aim valid,
// 2 aim is a vehicle, 8 track valid (last locked vehicle, for missiles in flight).
struct HostLock {uint32_t flags;float aim[3],track[3];};
static_assert(sizeof(HostLock)==28);
constexpr uint32_t CameraOffset=4352;
struct GuestCamera {
 uint32_t tick,epoch;
 float eye[3],forward[3],right[3],up[3],fov;
 uint32_t sequence;
};
static_assert(sizeof(GuestCamera)==64);
// MW2-RE4 balancing menu, right after the guest camera (free space before the image). Percentages: 100 = MW2's
// own; sprintMs 0xFFFF = MW2's own sprint, 0 = none; fireMs/reloadMs = the RE4 gun's times (0 = unknown).
constexpr uint32_t BalanceOffset=CameraOffset+64, BalanceMagic=0x314C4142;
struct Balance {
 uint32_t magic;
 uint16_t walk,adsWalk,sprintMs,spread,recoil,fireBlend,reloadBlend,fireMs,reloadMs,padding;
 uint32_t reserved[10];
};
static_assert(sizeof(Balance)==64 && BalanceOffset+64<=FrameOffset);
struct Shot {
 uint32_t sequence,weapon;
 float origin[3],direction[3];
 float range,damage,minDamage,maxDamageRange,minDamageRange;
 uint32_t hostTick,reserved[2];
};
struct Control {
 uint32_t magic,version;
 Host host;
 Guest guest;
 HostMovement hostMovement;
 GuestMovement guestMovement;
 HostLock lock;
 Shot shots[ShotCount];
};
struct Frame {uint32_t sequence,width,height,opaque; uint8_t padding[48];};
struct Scene {uint32_t sequence,count,tick,overflow; float center[3]; uint32_t reserved[9];};
struct Triangle {float vertices[3][3]; uint32_t surface;};
static_assert(sizeof(Host)==60 && sizeof(Guest)==48 && sizeof(Shot)==64);
static_assert(sizeof(HostMovement)==48 && sizeof(GuestMovement)==64);
static_assert(sizeof(Control)==4352 && sizeof(Frame)==64 && sizeof(Scene)==64 && sizeof(Triangle)==40);
static_assert(offsetof(Control,hostMovement)==116 && offsetof(Control,guestMovement)==164 && offsetof(Control,lock)==228 && offsetof(Control,shots)==256);
}
