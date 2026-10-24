// Fires, explosions, projectiles and water cannons both games share. Fires on the ground from explosions and molotovs start in each
// game by themselves (those are synced, see below), so only these travel: a fire on a player, a ped or a vehicle this game owns (shot with a molotov,
// a flamethrower, a burning car), and the host's mission fires (START_SCRIPT_FIRE). The other game lights the same
// fire on its copy of the entity, or at the spot: such a fire never spreads, never burns the copy (copies are
// fireproof, the proof is lifted only to light it) and goes out when the owner's fire does. A copy this game's player
// puts out (water, an extinguisher) puts the owner's fire out too: the partner helps with the host's firefighting.
#include <cmath>
#include <map>
#include <deque>
#include <algorithm>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Transform.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/sync/EventWindow.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kFireManager = 0xB71F80;  // gFireManager: CFire m_aFires[60], 0x28 bytes each
constexpr int kFires = 60;
constexpr uint32_t kFireProof = 1u << 19;     // CPhysical::physicalFlags.bFireProof
enum : uint8_t { FIRE_START = 1, FIRE_STOP = 2, FIRE_OUT = 3, FIRE_STATE = 4, FIRE_DOUSE = 5 };  // OUT: a copy was put out, to the owner
enum : uint8_t { ON_SPOT = 0, ON_VEHICLE = 1, ON_PED = 2, ON_PLAYER = 3 };

uint8_t SourceKind(uint8_t kind){return kind==ON_PED?ENTITY_PED:kind==ON_VEHICLE?ENTITY_VEHICLE:0;}
void WriteSource(Writer&w,uint8_t kind,uint16_t id,bool optional=false,uint64_t generation=0){if(auto type=SourceKind(kind))w.Entity(type,id,optional?ENTITY_OPTIONAL:ENTITY_USE,255,generation);else w.U16(id);}
bool SourceValid(uint8_t kind,uint16_t id,const std::vector<EntityRef>&refs){auto type=SourceKind(kind);if(!type)return true;for(auto ref:refs)if(ref.kind==type&&ref.id==id)return Net().EntityContextValid({ref});return false;}

uintptr_t Fire(int i) { return kFireManager + (uintptr_t)i * 0x28; }
bool Active(uintptr_t f) { return Ref<uint8_t>(f) & 1; }
CEntity* Target(uintptr_t f) { return Ref<CEntity*>(f + 0x10); }

struct Sent {
    uint16_t id;
    CEntity* target;
    int16_t generation=0;
    float strength=0;
    uint64_t sent=0;
    int8_t area=0;
};
std::map<uintptr_t, Sent> g_sent;         // our fires the other game copied
struct Copy {
    uintptr_t fire;
    uint8_t from;  // whose fire it copies
    CVector position;
    float strength=1;
    uint8_t kind=0;uint16_t entity=0;
    int8_t area=0;
    int16_t generation=0;
};
std::map<uint16_t, Copy> g_copies;    // fires lit here for the other game, by its id
std::map<uint16_t,Copy> g_pendingFires;
uint16_t g_nextId = 1;
uint64_t g_scanMs = 0;

bool IsCopy(uintptr_t f) {
    for (auto& [id, c] : g_copies)
        if (c.fire == f && Ref<int16_t>(f+2)==c.generation) return true;
    return false;
}

// this game's player could have put it out: close, with water or an extinguisher (a fire truck's cannon, weapon 42)
bool PlayerFighting(const CVector& at) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || (me->GetPosition() - at).Length() > 30.0f) return false;
    if (me->InVehicle()) return me->Vehicle() && (me->Vehicle()->ModelIndex() == 407||me->Vehicle()->ModelIndex()==601);  // FIRETRUK
    return me->ActiveWeapon().type == 42;
}

// what the other game knows this burning entity as; false when it does not know it
bool Describe(CEntity* e, uint8_t& kind, uint16_t& id) {
    if (!e) return false;
    if (e->IsPed()) {
        if (e == (CEntity*)FindPlayerPed(0)) return kind = ON_PLAYER, id = Net().MyId(), true;
        NetPed* np = Peds::Instance().Find((CPed*)e);
        return np && np->owner == Net().MyId() && (kind = ON_PED, id = np->netId, true);
    }
    if (e->Type() == ENTITY_TYPE_VEHICLE) {
        NetVehicle* nv = Vehicles::Instance().Find((CVehicle*)e);
        return nv && !Vehicles::Instance().IsMirror(*nv) && (kind = ON_VEHICLE, id = nv->netId, true);
    }
    return false;
}

// the copy of what burns in the other game
CEntity* Resolve(uint8_t kind, uint16_t id) {
    if (kind == ON_VEHICLE) {
        NetVehicle* nv = Vehicles::Instance().Find(id);
        return nv && nv->veh && IsVehicleValid(nv->veh) ? (CEntity*)nv->veh : nullptr;
    }
    if (kind == ON_PED) return (CEntity*)GetPed(Peds::Instance().HandleOf(id));
    if (kind == ON_PLAYER) {
        RemotePlayer* rp = Players::Instance().Get((uint8_t)id);
        return rp ? (CEntity*)rp->ped : nullptr;
    }
    return nullptr;
}

void SendStop(uint16_t id) {
    Writer w(proto::G_FIRE);
    w.U8(FIRE_STOP);
    w.Entity(ENTITY_FIRE,id,ENTITY_REMOVE,(id>>12)-1);
    Net().Send(proto::CH_RELIABLE, w);
}

bool SendCurrentFire(uintptr_t f,const Sent&s){
    if(!Active(f)||Ref<int16_t>(f+2)!=s.generation||Target(f)!=s.target)return false;
    uint8_t kind=ON_SPOT;uint16_t entity=0;
    if(s.target&&!Describe(s.target,kind,entity))return false;
    auto p=Ref<CVector>(f+4);Writer w(proto::G_FIRE);w.U8(FIRE_START);w.Entity(ENTITY_FIRE,s.id,ENTITY_BIRTH|ENTITY_SNAPSHOT,(s.id>>12)-1);
    w.U8(kind);WriteSource(w,kind,entity);w.Vec({p.x,p.y,p.z});w.F32(Ref<float>(f+0x1C));w.I8(s.area);
    Net().Send(proto::CH_RELIABLE,w);return true;
}

void Scan() {
    for (int i = 0; i < kFires; ++i) {
        uintptr_t f = Fire(i);
        auto sent = g_sent.find(f);
        if (!Active(f) || IsCopy(f)) {
            if (sent != g_sent.end()) {
                SendStop(sent->second.id);
                g_sent.erase(sent);
            }
            continue;
        }
        CEntity* target = Target(f);
        if (sent != g_sent.end()) {
            if (sent->second.target == target && sent->second.generation==Ref<int16_t>(f+2)) {
                float strength=Ref<float>(f+0x1C);
                if(std::fabs(strength-sent->second.strength)>.02f||NowMs()-sent->second.sent>1000){
                    Writer w(proto::G_FIRE);w.U8(FIRE_STATE);w.Entity(ENTITY_FIRE,sent->second.id,ENTITY_USE,(sent->second.id>>12)-1);w.F32(strength);
                    Net().Send(proto::CH_RELIABLE,w);sent->second.strength=strength;sent->second.sent=NowMs();
                }
                continue;
            }
            SendStop(sent->second.id);  // the slot holds another fire now
            g_sent.erase(sent);
        }
        uint8_t kind = ON_SPOT;
        uint16_t entityId = 0;
        bool script = Ref<uint8_t>(f) & 2;  // bCreatedByScript
        if (target ? !Describe(target, kind, entityId) : !(script && Net().IsHost() && HostReplicating())) continue;
        uint16_t id;bool used;do{id=(uint16_t)(((Net().MyId()+1)<<12)|(g_nextId++&0xFFF));used=false;for(auto&[other,record]:g_sent)if(record.id==id)used=true;}while(used);
        Net().BeginEntity(ENTITY_FIRE,id,Net().MyId());g_sent[f] = {id, target,Ref<int16_t>(f+2),Ref<float>(f+0x1C),NowMs(),target?target->AreaCode():FindPlayerPed(0)->AreaCode()};
        CVector pos = Ref<CVector>(f + 0x04);
        Writer w(proto::G_FIRE);
        w.U8(FIRE_START);
        w.Entity(ENTITY_FIRE,id,ENTITY_BIRTH|ENTITY_SNAPSHOT,(id>>12)-1);
        w.U8(kind);
        WriteSource(w,kind,entityId);
        w.Vec({pos.x, pos.y, pos.z});
        w.F32(Ref<float>(f + 0x1C));  // m_fStrength
        w.I8(target?target->AreaCode():FindPlayerPed(0)->AreaCode());
        Net().Send(proto::CH_RELIABLE, w);
        LOGD("Fire %04X sent (on %d %04X)", id, kind, entityId);
    }
}

void Light(uint8_t from, uint16_t id, uint8_t kind, uint16_t entityId, const CVector& pos, float strength, int8_t area) {
    uintptr_t fire = 0;
    if (kind == ON_SPOT) {
        // CFireManager::StartFire(CVector, size, unused, creator, lifetime, generations, unused): no spreading
        fire = CallMethod<0x539F00, uintptr_t>((void*)kFireManager, pos.x, pos.y, pos.z, strength, (uint8_t)1,
                                                (CEntity*)nullptr, (uint32_t)600000, (int8_t)0, (uint8_t)1);
        if (fire && strength > 1.0f) {  // it lights every fire at strength 1 (CFire::Start): a mission's big fire too
            Ref<float>(fire + 0x1C) = strength;
            CVector at = Ref<CVector>(fire + 0x04);
            CallMethod<0x539360>((void*)fire, &at, (void*)nullptr);  // CFire::CreateFxSysForStrength
        }
    } else {
        CEntity* e = Resolve(kind, entityId);
        if (!e){g_pendingFires[id]={0,from,pos,strength,kind,entityId,area,0};return;}
        auto* p = (CPhysical*)e;
        uint32_t flags = p->PhysicalFlags();
        p->PhysicalFlags() = flags & ~kFireProof;
        // CFireManager::StartFire(target, creator, size, unused, lifetime, generations)
        CallMethod<0x53A050, void*>((void*)kFireManager, e, (CEntity*)nullptr, 0.8f, (uint8_t)1, (uint32_t)600000, (int8_t)0);
        p->PhysicalFlags() = flags;
        fire = Field<uintptr_t>(e, e->IsPed() ? 0x730 : 0x490);  // CPed::m_pFire, CVehicle::m_pFire
    }
    if (!fire){g_pendingFires[id]={0,from,pos,strength,kind,entityId,area,0};return;}
    g_pendingFires.erase(id);
    // Script copies never expire because a guest camera is outside the room.
    Ref<uint8_t>(fire)|=2;Ref<uint8_t>(fire+0x20)=0;
    g_copies[id] = {fire, from,pos,strength,kind,entityId,area,Ref<int16_t>(fire+2)};
    LOGD("Fire %04X lit (on %d %04X)", id, kind, entityId);
}

void PutOut(uint16_t id) {
    g_pendingFires.erase(id);
    auto it = g_copies.find(id);
    if (it == g_copies.end()) return;
    if (Active(it->second.fire)&&Ref<int16_t>(it->second.fire+2)==it->second.generation){CallMethod<0x5393F0>((void*)it->second.fire);Ref<uint8_t>(it->second.fire)&=~2u;}
    g_copies.erase(it);
}

// the owner's side of FIRE_OUT: its fire goes out as the copy did
void PutOutOwn(uint16_t id) {
    for (auto it = g_sent.begin(); it != g_sent.end(); ++it)
        if (it->second.id == id) {
            if (Active(it->first)&&Ref<int16_t>(it->first+2)==it->second.generation&&Target(it->first)==it->second.target)
                CallMethod<0x5393F0>((void*)it->first);  // CFire::Extinguish
            SendStop(id); // both guests must observe the authoritative script result
            g_sent.erase(it);
            LOGI("Fire %04X put out by the other player", id);
            return;
        }
}

// ---------------------------------------------------------------- explosions
// A blast (a grenade, a rocket, a Rhino's shell, a bomb, a mission's explosion) goes off in the game of whoever set it
// off, and the other game sets off the same one in the name of its copy of the creator: its damage there falls on
// what that game owns (Damage.cpp). A blast here of the other side's copy (a rocket or a grenade its copy of the other
// player fired) is left out, the real one comes. Cars, boats, planes and objects blow up in each game by their own
// sync, and a molotov's burst lights the fires each game has.
bool IsBlast(int type) {  // eExplosionType: GRENADE, ROCKET, WEAK_ROCKET, MINE, TANK_FIRE, SMALL
    return type == 0 || type == 2 || type == 3 || type == 8 || type == 10 || type == 11;
}

// the other player, its car, or a ped or a car of the other game (a copy here)
bool OtherSide(CEntity* e) {
    if (!e) return false;
    if (e->IsPed()) {
        NetPed* np = Peds::Instance().Find((CPed*)e);
        return Players::Instance().FindByPed((CPed*)e) || (np && np->owner != Net().MyId());
    }
    NetVehicle* nv = e->Type() == ENTITY_TYPE_VEHICLE ? Vehicles::Instance().Find((CVehicle*)e) : nullptr;
    return nv && Vehicles::Instance().IsMirror(*nv);
}

using AddExplosionFn = bool(__cdecl*)(CEntity*, CEntity*, int, CVector, uint32_t, uint8_t, float, uint8_t);
AddExplosionFn g_origAddExplosion = nullptr;
uint32_t g_blastSequence=0;EventWindow g_explosionsSeen[3],g_projectilesSeen[3];
struct Flight {
    CPhysical* object=nullptr;CEntity* creator=nullptr;
    uint8_t kind=0;uint16_t id=0;bool authority=false;
    uint32_t token=0;uint64_t created=0;
    CVector lastPos;
    int handle=-1;uint8_t from=255;uint64_t creatorGeneration=0;int weapon=0;
};
void RetireProjectile(uint8_t from,uint32_t token);
void ClearReplicaProjectiles(uint8_t from);
std::deque<Flight> g_flights;
using RemoveProjectileFn=void(__cdecl*)(void*,CPhysical*);
RemoveProjectileFn g_origRemoveProjectile=nullptr;
CPhysical*g_detonatingProjectile=nullptr;
void __cdecl HookRemoveProjectile(void*info,CPhysical*object){
    struct Restore{CPhysical*saved=g_detonatingProjectile;~Restore(){g_detonatingProjectile=saved;}} restore;
    g_detonatingProjectile=object;g_origRemoveProjectile(info,object);
}
// Copy authority belongs to this projectile, never to its creator's current owner.
using DetonateFn=void(__cdecl*)();DetonateFn originalDetonate=nullptr;
void __cdecl HookDetonate(){
    struct Mask{int slot;CPhysical*object;uint8_t active;};std::vector<Mask> hidden;
    if(Net().Online())for(auto&f:g_flights)if(f.weapon==39&&!f.authority)
        for(int i=0;i<32;++i)if(Ref<CPhysical*>(0xC89110+i*4)==f.object&&Ref<uint8_t>(0xC891A8+i*0x24+0x10)){
            auto&on=Ref<uint8_t>(0xC891A8+i*0x24+0x10);hidden.push_back({i,f.object,on});on=0;break;}
    originalDetonate();
    for(auto&m:hidden)if(Ref<CPhysical*>(0xC89110+m.slot*4)==m.object)Ref<uint8_t>(0xC891A8+m.slot*0x24+0x10)=m.active;
}
Flight* BlastFlight(CEntity*victim,CEntity*creator,const CVector&pos){
    // RemoveProjectile is the native detonation path. Scope its exact object
    // so two overlapping grenades whose thrower changes owner cannot borrow
    // each other's authority by a nearest-position guess.
    if(g_detonatingProjectile)for(auto&f:g_flights)if(f.object==g_detonatingProjectile)return &f;
    Flight*best=nullptr;float distance=6.f;
    for(auto&f:g_flights){
        if((f.weapon!=39&&NowMs()-f.created>30000)||f.creator!=creator||!f.object)continue;
        bool active=false;
        for(int i=0;i<32;++i)if(Ref<CPhysical*>(0xC89110+i*4)==f.object&&Ref<uint8_t>(0xC891A8+i*0x24+0x10))active=true;
        // Update may clear m_bActive before it calls AddExplosion. The last
        // sampled point is safe even after the native object was deleted.
        bool alive=f.handle>=0&&GetObject(f.handle)==f.object;
        float d=(((active||alive)?f.object->GetPosition():f.lastPos)-pos).Length();
        if(victim==f.object||d<distance){best=&f;distance=d;if(victim==f.object)break;}
    }
    return best;
}
bool g_settingOff = false;  // a blast of the other game

// CExplosion::AddExplosion(victim, creator, type, position, lifetime, sound, camera shake, visible)
bool __cdecl HookAddExplosion(CEntity* victim, CEntity* creator, int type, CVector pos, uint32_t lifetime, uint8_t sound,
                              float shake, uint8_t visible) {
    if (Net().Online() && core::IsPlayerReady() && IsBlast(type) && !g_settingOff) {
        auto*flight=BlastFlight(victim,creator,pos);
        if(flight?!flight->authority:OtherSide(creator)){if(flight)LOGD("Projectile shadow blast suppressed token %u source %u",flight->token,flight->from);return true;}
        uint8_t kind = ON_SPOT;
        uint16_t id = 0;
        if(flight){kind=flight->kind;id=flight->id;}
        else if (!Describe(creator, kind, id)) kind = ON_SPOT, id = 0;
        // Original simulator may now hold only a mirror of the thrower.
        Writer w(proto::G_EXPLOSION);
        if(++g_blastSequence==0)++g_blastSequence;w.U32(g_blastSequence);
        w.U8(kind);
        WriteSource(w,kind,id,true,flight?flight->creatorGeneration:0);
        w.U8((uint8_t)type);
        w.Vec({pos.x, pos.y, pos.z});
        w.U32(lifetime);
        w.U8(sound);
        w.F32(shake);
        w.U8(visible);w.U32(flight?flight->token:0);
        Net().Send(proto::CH_RELIABLE, w);LOGD("Explosion authority token %u creator %04X peer %u",flight?flight->token:0,id,Net().MyId());
    }
    return g_origAddExplosion(victim, creator, type, pos, lifetime, sound, shake, visible);
}

struct PendingBlast {uint8_t from,kind;uint16_t id;int type;CVector pos;uint32_t lifetime;uint8_t sound;float shake;uint8_t visible;uint64_t received;uint32_t projectile=0;std::vector<EntityRef> refs;};
std::deque<PendingBlast> g_pendingBlasts;
void ReplayBlast(const PendingBlast&b){
    // A missing/deleted thrower cannot suppress damage to locally owned entities.
    // Null is an ordinary native world explosion, with the same origin and type.
    RetireProjectile(b.from,b.projectile);
    auto*creator=SourceValid(b.kind,b.id,b.refs)?Resolve(b.kind,b.id):nullptr;
    g_settingOff=true;g_origAddExplosion(nullptr,creator,b.type,b.pos,b.lifetime,b.sound,b.shake,b.visible);g_settingOff=false;
    LOGD("Explosion replay from %u creator %04X wait %llu",b.from,b.id,(unsigned long long)(NowMs()-b.received));
}
void HandleExplosion(uint8_t from, Reader& r) {
    uint32_t sequence=r.U32();
    uint8_t kind = r.U8();
    uint16_t id = r.U16();
    int type = r.U8();
    Vec3 p = r.Vec();
    uint32_t lifetime = r.U32();
    uint8_t sound = r.U8();
    float shake = r.F32();
    uint8_t visible = r.U8();uint32_t projectile=r.U32();
    if (!r.Ok() || from>=3 || !core::IsPlayerReady() || !IsBlast(type) || kind>ON_PLAYER ||
        lifetime>600000 || sound>1 || visible>1 || std::fabs(shake)>100 || !g_explosionsSeen[from].Accept(sequence)) return;
    PendingBlast blast{from,kind,id,type,{p.x,p.y,p.z},lifetime,sound,shake,visible,NowMs(),projectile,Net().EntityContext()};
    if(kind!=ON_SPOT&&!Resolve(kind,id)){
        if(g_pendingBlasts.size()>=64){ReplayBlast(g_pendingBlasts.front());g_pendingBlasts.pop_front();}
        g_pendingBlasts.push_back(blast);
    }else ReplayBlast(blast);
}

// ---------------------------------------------------------------- projectiles
// A grenade, a molotov, tear gas, a rocket, a bomb or a flare flies in the game that threw or fired it, and the other
// game throws the same one from the same spot at the same speed in the name of its copy of the thrower, to be seen:
// it goes off there without harm (a blast of the other side is left out, above) and the real blast comes. A projectile
// here of the other side's copy (its copy of the other player throwing) is left out the same way. Satchel charges
// stay in the game that planted them, the other one gets their blasts.
constexpr uintptr_t kProjectileInfos = 0xC891A8;  // gaProjectileInfo[32], 0x24 bytes each, m_bActive at +0x10
constexpr uintptr_t kProjectiles = 0xC89110;      // CProjectileInfo::ms_apProjectile[32]
bool Thrown(int weapon) { return (weapon >= 16 && weapon <= 21) || weapon == 39 || weapon == 58; }  // GRENADE..FREEFALL_BOMB, FLARE

uint32_t ActiveProjectiles() {
    uint32_t mask = 0;
    for (int i = 0; i < 32; ++i)
        if (Ref<uint8_t>(kProjectileInfos + (uintptr_t)i * 0x24 + 0x10)) mask |= 1u << i;
    return mask;
}
CPhysical* NewProjectile(uint32_t before) {
    uint32_t added = ActiveProjectiles() & ~before;
    for (int i = 0; i < 32; ++i)
        if (added & (1u << i)) return Ref<CPhysical*>(kProjectiles + (uintptr_t)i * 4);
    return nullptr;
}

// a target both games know (the other player, a copy), unlike Describe: a heat-seeking rocket's
bool DescribeAny(CEntity* e, uint8_t& kind, uint16_t& id) {
    if (!e || Describe(e, kind, id)) return e != nullptr;
    if (e->IsPed()) {
        if (RemotePlayer* rp = Players::Instance().FindByPed((CPed*)e)) return kind = ON_PLAYER, id = rp->id, true;
        NetPed* np = Peds::Instance().Find((CPed*)e);
        return np && (kind = ON_PED, id = np->netId, true);
    }
    NetVehicle* nv = e->Type() == ENTITY_TYPE_VEHICLE ? Vehicles::Instance().Find((CVehicle*)e) : nullptr;
    return nv && (kind = ON_VEHICLE, id = nv->netId, true);
}

using AddProjectileFn = bool(__cdecl*)(CEntity*, int, CVector, float, const CVector*, CEntity*);
AddProjectileFn g_origAddProjectile = nullptr;
bool g_throwing = false;  // a projectile of the other game

// CProjectileInfo::AddProjectile(creator, weapon, position, force, direction, target)
bool __cdecl HookAddProjectile(CEntity* creator, int weapon, CVector pos, float force, const CVector* dir, CEntity* target) {
    bool shared = Net().Online() && core::IsPlayerReady() && !g_throwing;
    if (shared && OtherSide(creator)) return false;
    uint32_t before = ActiveProjectiles();
    bool ok = g_origAddProjectile(creator, weapon, pos, force, dir, target);
    CPhysical* obj = ok && shared && Thrown(weapon) ? NewProjectile(before) : nullptr;
    if (!obj) return ok;
    uint8_t kind = ON_SPOT, targetKind = ON_SPOT;
    uint16_t id = 0, targetId = 0;
    if (!Describe(creator, kind, id)) kind = ON_SPOT, id = 0;
    if (!DescribeAny(target, targetKind, targetId)) targetKind = ON_SPOT, targetId = 0;
    CVector at = obj->GetPosition(), speed = obj->MoveSpeed();
    Writer w(proto::G_PROJECTILE);
    if(++g_blastSequence==0)++g_blastSequence;w.U32(g_blastSequence);
    w.U8(kind);
    WriteSource(w,kind,id,true);
    w.U8((uint8_t)weapon);
    w.F32(force);
    w.Vec({at.x, at.y, at.z});
    w.Vec({speed.x, speed.y, speed.z});
    w.U8(targetKind);
    WriteSource(w,targetKind,targetId,true);
    w.U32((uint32_t)NowMs());
    for(auto i=g_flights.begin();i!=g_flights.end();)i=i->object==obj?g_flights.erase(i):std::next(i);
    g_flights.push_back({obj,creator,kind,id,true,g_blastSequence,NowMs(),at,GetObjectRef((CObject*)obj),Net().MyId(),Net().EntityGeneration(SourceKind(kind),id)});
    g_flights.back().weapon=weapon;
    while(g_flights.size()>64)g_flights.pop_front();
    Net().Send(proto::CH_RELIABLE, w);
    return ok;
}

struct PendingProjectile {
    uint8_t from=0,kind=0,targetKind=0;uint16_t id=0,targetId=0;
    uint32_t sequence=0,tick=0;int weapon=0;float force=0;CVector pos,speed;uint64_t received=0;std::vector<EntityRef> refs;
};
std::deque<PendingProjectile> g_pendingProjectiles;
void RemoveFlightCopy(Flight&f){
    if(f.authority)return;
    for(int i=0;i<32;++i)if(Ref<CPhysical*>(kProjectiles+i*4)==f.object&&Ref<uint8_t>(kProjectileInfos+i*0x24+0x10)){
        Call<0x7388F0>((void*)(kProjectileInfos+i*0x24),f.object);break;
    }
}
void RetireProjectile(uint8_t from,uint32_t token){
    if(!token)return;
    for(auto i=g_pendingProjectiles.begin();i!=g_pendingProjectiles.end();)i=i->from==from&&i->sequence==token?g_pendingProjectiles.erase(i):std::next(i);
    for(auto i=g_flights.begin();i!=g_flights.end();)if(i->from==from&&i->token==token){RemoveFlightCopy(*i);i=g_flights.erase(i);}else ++i;
}
void ClearReplicaProjectiles(uint8_t from){
    for(auto i=g_flights.begin();i!=g_flights.end();)if(i->from==from&&!i->authority){RemoveFlightCopy(*i);i=g_flights.erase(i);}else ++i;
}
bool ReplayProjectile(const PendingProjectile&p){
    auto*creator=SourceValid(p.kind,p.id,p.refs)?Resolve(p.kind,p.id):nullptr;
    if(p.kind==ON_SPOT){auto*rp=Players::Instance().Get(p.from);creator=rp?rp->ped:nullptr;}
    if(!creator)return false;
    auto*target=p.targetKind==ON_PLAYER&&p.targetId==Net().MyId()?(CEntity*)FindPlayerPed(0):p.targetKind==ON_SPOT?nullptr:SourceValid(p.targetKind,p.targetId,p.refs)?Resolve(p.targetKind,p.targetId):nullptr;
    if(p.targetKind!=ON_SPOT&&!target&&NowMs()-p.received<300)return false;
    auto at=p.pos,speed=p.speed,dir=speed.Normalized();
    uint32_t before=ActiveProjectiles();g_throwing=true;
    bool ok=g_origAddProjectile(creator,p.weapon,at,p.force,&dir,target);g_throwing=false;
    auto*obj=ok?NewProjectile(before):nullptr;if(!obj)return true;
    // Keep the fuse age. Do not launch an old grenade with a new full lifetime.
    const auto*sourcePeer=Net().Peer(p.from);
    uint64_t estimatedTransit=(Net().Rtt()+(sourcePeer?sourcePeer->ping:0))/2;
    uint32_t age=(uint32_t)std::min<uint64_t>(1500,NowMs()-p.received+estimatedTransit);
    auto*rp=Players::Instance().Get(p.from);
    // Sender-clock progress already includes time spent waiting for a creator.
    // Adding it to the local wait counted the same 450ms dependency twice.
    if(rp&&rp->hasState){int32_t progress=(int32_t)(rp->state.time-p.tick);if(progress>0&&progress<1500)age=std::max(age,(uint32_t)progress);}
    float seconds=age/1000.f;
    // GTA speed is distance per 50 Hz step. Avoid advancing through walls:
    // visual position starts at the transmitted point; the fuse stays correct.
    (void)seconds;obj->SetPositionRaw(at);obj->MoveSpeed()=speed;
    for(int i=0;i<32;++i)if(Ref<CPhysical*>(kProjectiles+i*4)==obj){
        auto&expires=Ref<uint32_t>(kProjectileInfos+i*0x24+0xC);
        uint32_t gameNow=Ref<uint32_t>(0xB7CB84);int32_t left=(int32_t)(expires-gameNow);
        if(left>0)expires=gameNow+(uint32_t)std::max(1,left-(int)age);
    }
    for(auto i=g_flights.begin();i!=g_flights.end();)i=i->object==obj?g_flights.erase(i):std::next(i);
    g_flights.push_back({obj,creator,p.kind,p.id,false,p.sequence,NowMs(),at,GetObjectRef((CObject*)obj),p.from});
    g_flights.back().weapon=p.weapon;
    while(g_flights.size()>64)g_flights.pop_front();
    LOGI("Projectile replay from %u token %u wait %u creator %04X",p.from,p.sequence,age,p.id);
    return true;
}
void HandleProjectile(uint8_t from, Reader&r){
    PendingProjectile p;p.from=from;p.sequence=r.U32();p.kind=r.U8();p.id=r.U16();p.weapon=r.U8();p.force=r.F32();
    auto v=r.Vec(),speed=r.Vec();p.pos={v.x,v.y,v.z};p.speed={speed.x,speed.y,speed.z};p.targetKind=r.U8();p.targetId=r.U16();p.tick=r.U32();p.received=NowMs();p.refs=Net().EntityContext();
    if(!r.Ok()||from>=3||!core::IsPlayerReady()||!Thrown(p.weapon)||p.kind>ON_PLAYER||p.targetKind>ON_PLAYER||
       std::fabs(p.force)>100||p.speed.Length()>150||!g_projectilesSeen[from].Accept(p.sequence))return;
    if(!ReplayProjectile(p)){
        if(g_pendingProjectiles.size()>=64)g_pendingProjectiles.pop_front();
        g_pendingProjectiles.push_back(p);LOGD("Projectile queued dependency from %u token %u creator %04X",from,p.sequence,p.id);
    }
}
void ReannounceCharges(){
    for(auto&f:g_flights)if(f.authority&&f.weapon==39&&f.handle>=0&&GetObject(f.handle)==f.object){
        auto position=f.object->GetPosition(),speed=f.object->MoveSpeed();Writer w(proto::G_PROJECTILE);w.U32(f.token);w.U8(f.kind);WriteSource(w,f.kind,f.id,true,f.creatorGeneration);
        w.U8(39);w.F32(0);w.Vec({position.x,position.y,position.z});w.Vec({speed.x,speed.y,speed.z});w.U8(ON_SPOT);w.U16(0);w.U32((uint32_t)NowMs());Net().Send(proto::CH_RELIABLE,w);
    }
}
void RetryCombat(){
    for(auto i=g_pendingBlasts.begin();i!=g_pendingBlasts.end();){
        if(Resolve(i->kind,i->id)||NowMs()-i->received>=300){ReplayBlast(*i);i=g_pendingBlasts.erase(i);}else ++i;
    }
    for(auto i=g_pendingProjectiles.begin();i!=g_pendingProjectiles.end();){
        if(NowMs()-i->received>1500){LOGD("Projectile dependency expired token %u",i->sequence);i=g_pendingProjectiles.erase(i);}
        else if(ReplayProjectile(*i))i=g_pendingProjectiles.erase(i);else ++i;
    }
    // A creator reference can legitimately become null while its grenade is
    // still flying. The projectile handle identifies this flight; the creator
    // pointer must not cancel its authority when that NPC is deleted.
    // Native pool addresses are reused. Retire a record before a new projectile
    // can be associated with its old creator/authority.
    for(auto i=g_flights.begin();i!=g_flights.end();){
        bool active=false;
        for(int n=0;n<32;++n)if(Ref<CPhysical*>(kProjectiles+n*4)==i->object&&Ref<uint8_t>(kProjectileInfos+n*0x24+0x10)&&i->handle>=0&&GetObject(i->handle)==i->object)active=true;
        if(!active||(i->weapon!=39&&NowMs()-i->created>30000))i=g_flights.erase(i);else {i->lastPos=i->object->GetPosition();++i;}
    }
}

// ---------------------------------------------------------------- water cannons
// A fire truck's or a SWAT van's water jet (CWaterCannons::UpdateOne each frame it shoots) shoots from the other game's
// copy of the vehicle too, and puts out and knocks down there what it reaches.
struct Jet {
    CVector offset,speed;uint64_t ms=0,generation=0;uint32_t sequence=0;uint8_t owner=255;bool active=false;
};
struct SentJet {uint64_t input=0,sent=0,generation=0;CVector offset,speed;bool active=false;};
std::map<uint16_t,Jet>g_jets;std::map<uint16_t,SentJet>g_jetSent;uint32_t g_jetSequence=0;
using WaterCannonFn=void(__cdecl*)(uint32_t,CVector*,CVector*);
WaterCannonFn g_origWaterCannon=nullptr;
using PushWaterFn=void(__fastcall*)(void*,void*);PushWaterFn g_origPushWater=nullptr;
void __fastcall PushWater(void*cannon,void*edx){
    if(!Net().Online()){g_origPushWater(cannon,edx);return;}
    // Native PushPeds applies force even when the damage event is rejected.
    // Its existing exclusion bit suppresses physics on replicas, while each
    // participant and NPC authority still receives the same real water impact.
    std::vector<std::pair<CPed*,bool>> held;
    auto hold=[&](CPed*p){if(p&&IsPedValid(p)){held.push_back({p,(p->PhysicalFlags()&1u)!=0});p->PhysicalFlags()|=1u;}};
    for(auto&[id,rp]:Players::Instance().All())hold(rp.ped);
    for(auto&[id,np]:Peds::Instance().All())if(np.owner!=Net().MyId())hold(np.ped);
    g_origPushWater(cannon,edx);
    for(auto&[p,was]:held)if(IsPedValid(p))p->PhysicalFlags()=(p->PhysicalFlags()&~1u)|(was?1u:0u);
}
CVector CannonLocal(const CMatrix&m,const CVector&v){return {Dot(v,m.right),Dot(v,m.forward),Dot(v,m.up)};}
CVector CannonWorld(const CMatrix&m,const CVector&v){return m.right*v.x+m.forward*v.y+m.up*v.z;}
void SendJet(uint16_t id,SentJet&jet,bool active,bool reliable){
    Writer w(proto::G_WATER_CANNON);w.Entity(ENTITY_VEHICLE,id);w.U32(++g_jetSequence);w.Bool(active);
    if(active){w.Vec({jet.offset.x,jet.offset.y,jet.offset.z});w.Vec({jet.speed.x,jet.speed.y,jet.speed.z});}
    Net().Send(reliable?proto::CH_RELIABLE:proto::CH_STATE,w);jet.sent=NowMs();jet.active=active;
    if(reliable)LOGD("Water cannon edge sent vehicle=%04X on=%d seq=%u",id,active,g_jetSequence);
}
void __cdecl HookWaterCannon(uint32_t vehicle,CVector*start,CVector*speed){
    auto*car=(CVehicle*)(uintptr_t)vehicle;
    auto*known=Net().Online()&&IsVehicleValid(car)?Vehicles::Instance().Find(car):nullptr;
    if(known&&Vehicles::Instance().IsMirror(*known))return; // replicas replay only the owner's nozzle
    g_origWaterCannon(vehicle,start,speed);
    if(!Net().Online()||!core::IsPlayerReady()||!start||!speed||!IsVehicleValid(car)||!car->Matrix())return;
    auto*n=Vehicles::Instance().EnsureNetworked(car);if(!n||Vehicles::Instance().IsMirror(*n))return;
    auto&jet=g_jetSent[n->netId];auto generation=Net().EntityGeneration(ENTITY_VEHICLE,n->netId);if(jet.generation!=generation)jet={};
    jet.generation=generation;jet.input=NowMs();jet.offset=CannonLocal(*car->Matrix(),*start-car->GetPosition());jet.speed=CannonLocal(*car->Matrix(),*speed-car->MoveSpeed());
    if(!jet.active||NowMs()-jet.sent>=50)SendJet(n->netId,jet,true,!jet.active);
}
void ShootJets(){
    for(auto i=g_jetSent.begin();i!=g_jetSent.end();){auto*n=Vehicles::Instance().Find(i->first);auto&j=i->second;
        if(!n||!Vehicles::Alive(*n)||Vehicles::Instance().IsMirror(*n)||Net().EntityGeneration(ENTITY_VEHICLE,i->first)!=j.generation){i=g_jetSent.erase(i);continue;}
        if(j.active&&NowMs()-j.input>100)SendJet(i->first,j,false,true);++i;}
    for(auto i=g_jets.begin();i!=g_jets.end();){auto*n=Vehicles::Instance().Find(i->first);auto&j=i->second;
        if(NowMs()-j.ms>750||!n||!Vehicles::Alive(*n)||n->owner!=j.owner||!Vehicles::Instance().IsMirror(*n)||Net().EntityGeneration(ENTITY_VEHICLE,i->first)!=j.generation){i=g_jets.erase(i);continue;}
        if(j.active&&n->veh->Matrix()&&(n->veh->AreaCode()==FindPlayerPed(0)->AreaCode()||n->veh->AreaCode()==13)){
            auto start=n->veh->GetPosition()+CannonWorld(*n->veh->Matrix(),j.offset);auto speed=CannonWorld(*n->veh->Matrix(),j.speed)+n->veh->MoveSpeed();
            g_origWaterCannon((uint32_t)(uintptr_t)n->veh,&start,&speed);
        }++i;
    }
}

class Fires : public Module {
    void OnEntityReplaced(uint8_t kind,uint8_t,uint16_t id)override{if(kind==ENTITY_VEHICLE){g_jets.erase(id);g_jetSent.erase(id);return;}if(kind!=ENTITY_FIRE)return;g_pendingFires.erase(id);PutOut(id);}

public:
    const char* Name() const override { return "Fires"; }
    void CollectSyncAudit(std::vector<SyncAuditEntry>&out)override{
        for(auto&[f,s]:g_sent)if(Active(f)&&Ref<int16_t>(f+2)==s.generation){
            SyncAuditEntry e;e.kind=AUDIT_FIRE;e.id=s.id;e.owner=Net().MyId();e.area=s.area;
            e.pos=Ref<CVector>(f+4);e.alive=true;out.push_back(e);
        }
        for(auto&[id,c]:g_copies){SyncAuditEntry e;e.kind=AUDIT_FIRE;e.id=id;e.owner=c.from;e.area=c.area;e.pos=c.position;
            e.alive=Active(c.fire)&&Ref<int16_t>(c.fire+2)==c.generation;out.push_back(e);}
        for(auto&[id,c]:g_pendingFires)if(!g_copies.count(id)){SyncAuditEntry e;e.kind=AUDIT_FIRE;e.id=id;e.owner=c.from;e.area=c.area;e.pos=c.position;out.push_back(e);}
    }
    bool RepairSyncEntity(uint8_t kind,uint16_t id)override{
        if(kind!=AUDIT_FIRE||(id>>12)!=Net().MyId()+1)return false;
        for(auto&[f,s]:g_sent)if(s.id==id&&SendCurrentFire(f,s))return true;
        SendStop(id);return true;
    }

    void OnInstallHooks() override {
        mem::HookFn(0x738860,&HookDetonate,&originalDetonate,"CProjectileInfo::RemoveDetonatorProjectiles (local authority)");
        mem::HookFn(0x7388F0,&HookRemoveProjectile,&g_origRemoveProjectile,"CProjectileInfo::RemoveProjectile");
        mem::HookFn(0x736A50, &HookAddExplosion, &g_origAddExplosion, "CExplosion::AddExplosion");
        Net().SetHandler(proto::G_EXPLOSION, &HandleExplosion);
        mem::HookFn(0x737C80, &HookAddProjectile, &g_origAddProjectile, "CProjectileInfo::AddProjectile");
        Net().SetHandler(proto::G_PROJECTILE, &HandleProjectile);
        mem::HookFn(0x728CB0, &HookWaterCannon, &g_origWaterCannon, "CWaterCannons::UpdateOne");
        mem::HookFn(0x7295E0, &PushWater, &g_origPushWater, "CWaterCannon::PushPeds authority");
        Net().SetHandler(proto::G_WATER_CANNON, [](uint8_t from, Reader& r) {
            auto id=r.U16();auto sequence=r.U32();bool active=r.Bool();Vec3 at{},v{};if(active){at=r.Vec();v=r.Vec();}
            if(!r.Ok()||!sequence||!core::IsPlayerReady())return;auto*n=Vehicles::Instance().Find(id);
            auto finite=[](Vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);};
            if(!n||!Vehicles::Alive(*n)||n->owner!=from||!Vehicles::Instance().IsMirror(*n)||!finite(at)||!finite(v)||CVector{at.x,at.y,at.z}.Length()>20||CVector{v.x,v.y,v.z}.Length()>10)return;
            auto generation=Net().EntityGeneration(ENTITY_VEHICLE,id);auto&j=g_jets[id];
            if(j.generation==generation&&j.owner==from&&j.sequence&&(int32_t)(sequence-j.sequence)<=0)return;
            bool edge=j.active!=active||!j.ms;j={{at.x,at.y,at.z},{v.x,v.y,v.z},NowMs(),generation,sequence,from,active};
            if(edge)LOGD("Water cannon edge received vehicle=%04X owner=%u on=%d seq=%u",id,from,active,sequence);
        });
        Net().SetHandler(proto::G_FIRE, [](uint8_t from, Reader& r) {
            uint8_t op = r.U8();
            uint16_t id = r.U16();
            if (!r.Ok() || !core::IsPlayerReady()) return;
            if (op == FIRE_STOP) {
                if(auto p=g_pendingFires.find(id);p!=g_pendingFires.end()&&p->second.from==from)g_pendingFires.erase(p);
                auto c=g_copies.find(id);if(c!=g_copies.end()&&c->second.from==from)PutOut(id);return;
            }
            if (op == FIRE_OUT || op==FIRE_DOUSE) {
                float amount=op==FIRE_DOUSE?r.F32():0;
                if(!r.Ok()||!std::isfinite(amount)||amount<0||amount>100)return;
                if((id>>12)!=Net().MyId()+1)return;
                auto*rp=Players::Instance().Get(from);
                if(!rp||!rp->hasState||NowMs()-rp->stateRecvMs>1500)return;
                for(auto&[f,sent]:g_sent)if(sent.id==id){
                    if(!Active(f)||Ref<int16_t>(f+2)!=sent.generation||Target(f)!=sent.target)break;
                    CVector pos=Ref<CVector>(f+4);
                    if((!Target(f)||rp->state.area==Target(f)->AreaCode())&&(rp->state.pos-pos).Length()<35){
                        if(op==FIRE_OUT||amount>=Ref<float>(f+0x1C))PutOutOwn(id);
                        else {Ref<float>(f+0x1C)-=amount;CallMethod<0x539360>((void*)f,&pos,(void*)nullptr);}
                    }
                    break;
                }
                return;
            }
            if(op==FIRE_STATE){
                float strength=r.F32();
                if(!r.Ok()||!std::isfinite(strength)||strength<0||strength>100)return;
                if(auto p=g_pendingFires.find(id);p!=g_pendingFires.end()&&p->second.from==from)p->second.strength=strength;
                auto it=g_copies.find(id);
                if(!r.Ok()||!std::isfinite(strength)||strength<0||strength>100||it==g_copies.end()||it->second.from!=from)return;
                auto&c=it->second;
                if(Active(c.fire)&&Ref<int16_t>(c.fire+2)==c.generation){
                    float previous=Ref<float>(c.fire+0x1C);strength=std::min(previous,strength);Ref<float>(c.fire+0x1C)=strength;
                    if((int)previous!=(int)strength){auto pos=Ref<CVector>(c.fire+4);CallMethod<0x539360>((void*)c.fire,&pos,(void*)nullptr);}
                }
                c.strength=strength;return;
            }
            if(op!=FIRE_START||(id>>12)!=from+1)return;
            uint8_t kind = r.U8();
            uint16_t entityId = r.U16();
            Vec3 p = r.Vec();
            float strength = r.F32();int8_t area=r.I8();
            if (!r.Ok()||!std::isfinite(strength)||strength<0||strength>100||kind>ON_PLAYER) return;
            if(auto it=g_copies.find(id);it!=g_copies.end()){
                auto&c=it->second;
                if(c.from==from&&c.kind==kind&&c.entity==entityId&&c.area==area&&
                   Active(c.fire)&&Ref<int16_t>(c.fire+2)==c.generation){
                    c.position={p.x,p.y,p.z};c.strength=std::min(c.strength,strength);return;
                }
            }
            PutOut(id);
            Light(from, id, kind, entityId, {p.x, p.y, p.z}, strength,area);
        });
    }
    void OnSessionEnd() override { while(!g_copies.empty())PutOut(g_copies.begin()->first);Forget(); }
    void OnPeerLeave(uint8_t peer)override{
        if(peer<3){g_explosionsSeen[peer]={};g_projectilesSeen[peer]={};ClearReplicaProjectiles(peer);}
        for(auto i=g_pendingBlasts.begin();i!=g_pendingBlasts.end();)i=i->from==peer?g_pendingBlasts.erase(i):std::next(i);
        for(auto i=g_pendingProjectiles.begin();i!=g_pendingProjectiles.end();)i=i->from==peer?g_pendingProjectiles.erase(i):std::next(i);
        for(auto it=g_pendingFires.begin();it!=g_pendingFires.end();)it=it->second.from==peer?g_pendingFires.erase(it):std::next(it);
        for(auto it=g_copies.begin();it!=g_copies.end();){auto id=it->first;++it;if(g_copies[id].from==peer)PutOut(id);}
    }
    void OnGameStarted() override { Forget(); }
    void OnPreProcess() override {
        if (Net().Online() && core::IsPlayerReady()){RetryCombat();ShootJets();}
    }
    void OnPeerResync(uint8_t id) override { OnPeerLeave(id);ReannounceCharges();for(auto&[f,s]:g_sent)SendCurrentFire(f,s); }

    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady() || NowMs() - g_scanMs < 200) return;
        g_scanMs = NowMs();
        int budget=4;for(auto it=g_pendingFires.begin();it!=g_pendingFires.end()&&budget-->0;){auto id=it->first;auto c=it->second;++it;Light(c.from,id,c.kind,c.entity,c.position,c.strength,c.area);}
        // copies whose fire went out here by itself (water, the entity died) stay out; put out by this player, the
        // owner's goes out too
        for (auto it = g_copies.begin(); it != g_copies.end();) {
            if (Active(it->second.fire)&&Ref<int16_t>(it->second.fire+2)==it->second.generation) {
                auto&c=it->second;float strength=Ref<float>(c.fire+0x1C);
                if(strength<c.strength-.005f&&PlayerFighting(c.position)){
                    Writer w(proto::G_FIRE);w.U8(FIRE_DOUSE);w.Entity(ENTITY_FIRE,it->first,ENTITY_USE,it->second.from);w.F32(c.strength-strength);
                    Net().Send(proto::CH_RELIABLE,w);
                }
                c.strength=strength;
                ++it;
                continue;
            }
            if (Ref<int16_t>(it->second.fire+2)==it->second.generation && PlayerFighting(it->second.position)) {
                Writer w(proto::G_FIRE);
                w.U8(FIRE_OUT);
                w.Entity(ENTITY_FIRE,it->first,ENTITY_USE,it->second.from);
                Net().Send(proto::CH_RELIABLE, w);
                LOGI("Fire %04X of player %d put out here", it->first, it->second.from);
            }
            if(Ref<int16_t>(it->second.fire+2)==it->second.generation)Ref<uint8_t>(it->second.fire)&=~2u;
            it = g_copies.erase(it);
        }
        Scan();
    }

    static Fires& Instance() {
        static Fires m;
        return m;
    }

private:
    static void Forget() {
        g_blastSequence=0;for(auto&w:g_explosionsSeen)w={};for(auto&w:g_projectilesSeen)w={};
        g_pendingBlasts.clear();g_pendingProjectiles.clear();g_flights.clear();
        g_sent.clear();
        g_copies.clear();g_pendingFires.clear();
        g_jets.clear();
        g_jetSent.clear();g_jetSequence=0;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Fires::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
