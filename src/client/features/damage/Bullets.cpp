#include <deque>
#include <iterator>
#include <map>
#include "common/api/protocol/Protocol.h"
#include "common/api/protocol/ShotVisual.h"
#include "common/lib/util/Util.h"
#include "common/lib/log/Log.h"
#include "entities/player/Players.h"
#include "entities/ped/Peds.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "features/damage/HitContext.h"

namespace coop {
namespace {
using namespace game;
using ImpactFn = void(__fastcall*)(CWeapon*, void*, CEntity*, CEntity*, const CVector&, const CVector&, const void*, int);
ImpactFn g_originalImpact = nullptr;
uint32_t g_sequence = 0;
struct PendingShot { uint8_t from; ShotVisual shot; uint64_t received; };
std::deque<PendingShot> g_pending;
std::map<uint8_t, uint32_t> g_lastSequence;
CPed* ShootingPed(CEntity* entity) {
    if (!entity) return nullptr;
    return entity->IsPed() ? (CPed*)entity : entity->IsVehicle() ? (((CVehicle*)entity)->Driver()?((CVehicle*)entity)->Driver():CooperativeRemoteController((CVehicle*)entity)) : nullptr;
}
void __fastcall HookBulletImpact(CWeapon* weapon, void* edx, CEntity* shooter, CEntity* victim,
                                const CVector& start, const CVector& end, const void* collision, int incremental) {
    struct Restore { BulletHitContext saved=g_bulletHit; ~Restore(){g_bulletHit=saved;} } restore;
    g_bulletHit={};
    if (Net().Online()) {
        CPed* ped = ShootingPed(shooter);
        if (ped && Players::Instance().FindByPed(ped)) return;  // no second, independently calculated ray/impact
        if (ped && ped == FindPlayerPed(0)) {
            ShotVisual shot;
            shot.sequence = ++g_sequence;
            if(!shot.sequence)shot.sequence=++g_sequence;
            shot.weapon = (uint8_t)weapon->type;
            shot.start = {start.x, start.y, start.z};
            CVector hit = victim && collision ? Field<CVector>((void*)collision, 0) : end;
            shot.end = {hit.x, hit.y, hit.z};
            if (victim && collision) {
                CVector normal = Field<CVector>((void*)collision, 0x10);
                shot.normal = {normal.x, normal.y, normal.z};
                // Verified in the supported 1.0 US binary: DoBulletImpact
                // reads piece B at +24 and surface B at +23. Some SDK headers
                // add padding here which this executable does not have.
                shot.surface = Field<uint8_t>((void*)collision, 0x23);
                bool blood = victim->IsPed() && (!((CPed*)victim)->IsPlayer() || Net().FriendlyFire());
                shot.flags = blood ? ShotVisual::BLOOD : victim->IsPed() ? 0 : ShotVisual::IMPACT;
            }
            Writer w(proto::G_BULLET); shot.Write(w);
            Net().Send(proto::CH_RELIABLE, w);
            if(LocalStoryRoleMode()==2)LOGD("Story gunner bullet sent seq=%u weapon=%u hit=%d",shot.sequence,shot.weapon,victim!=nullptr);
            if(ped->Intelligence()&&CallMethod<0x601110,void*>(ped->Intelligence()))LOGD("Jetpack bullet sent seq=%u weapon=%u hit=%d",shot.sequence,shot.weapon,victim!=nullptr);
            g_bulletHit={victim,start,hit,shot.sequence,shot.weapon};
        }
    }
    g_originalImpact(weapon, edx, shooter, victim, start, end, collision, incremental);
}
// Off-screen network peds may have a fresh entity transform but a previous
// skinned hierarchy. Refresh only actors intersecting the local shooter's actual ray
// before native collision, so hitboxes follow their current passenger/pose state.
void PrepareLocalRay(CEntity*owner,const CVector&start,const CVector&end){
    if(!Net().Online()||!core::IsPlayerReady()||owner!=FindPlayerPed(0))return;
    auto delta=end-start;float length2=delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;if(length2<.01f)return;
    int budget=24;
    auto refresh=[&](CPed*p){if(!p||p==owner||!IsPedValid(p)||!p->RwObject()||p->Health()<=0||budget<=0)return;
        if(AreaOf(p)!=AreaOf((CPed*)owner)&&AreaOf(p)!=13)return;auto relative=p->GetPosition()-start;
        float t=(relative.x*delta.x+relative.y*delta.y+relative.z*delta.z)/length2;
        if(t<0||t>1.05f||(p->GetPosition()-(start+delta*t)).Length()>3.f)return;
        --budget;CallMethod<0x446F90>(p);CallMethod<0x532B00>(p);CallMethod<0x532B20>(p);
    };
    for(auto&[id,rp]:Players::Instance().All())refresh(rp.ped);
    for(auto&[id,np]:Peds::Instance().All())refresh(np.ped);
}
using FireFn=bool(__fastcall*)(CWeapon*,void*,CEntity*,CVector*,CVector*,CEntity*,CVector*,CVector*);
FireFn originalFire=nullptr;
bool __fastcall NativeFire(CWeapon*w,void*edx,CEntity*owner,CVector*start,CVector*barrel,CEntity*target,CVector*point,CVector*alternate){
    if(owner==FindPlayerPed(0)){auto a=start?*start:owner->GetPosition();auto b=target?target->GetPosition():point?*point:a+cam::Front()*1000.f;PrepareLocalRay(owner,a,b);}
    return originalFire(w,edx,owner,start,barrel,target,point,alternate);
}
using CarRayFn=void(__fastcall*)(CWeapon*,void*,CVector,CVector,CVehicle*,CEntity*);
CarRayFn originalCarRay=nullptr;
void __fastcall NativeCarRay(CWeapon*w,void*edx,CVector start,CVector end,CVehicle*vehicle,CEntity*owner){
    PrepareLocalRay(owner,start,end);originalCarRay(w,edx,start,end,vehicle,owner);
}
using InstantFn=bool(__fastcall*)(CWeapon*,void*,CEntity*,CVector*,CVector*,CEntity*,CVector*,CVector*,bool,bool);
InstantFn originalInstant=nullptr;
bool __fastcall NativeInstant(CWeapon*w,void*edx,CEntity*owner,CVector*origin,CVector*muzzle,CEntity*target,CVector*point,CVector*driveby,bool a,bool b){
    if(owner==FindPlayerPed(0)){auto start=origin?*origin:owner->GetPosition();auto end=target?target->GetPosition():point?*point:start+cam::Front()*1000.f;PrepareLocalRay(owner,start,end);}
    return originalInstant(w,edx,owner,origin,muzzle,target,point,driveby,a,b);
}
class BulletVisuals : public Module {
public:
    const char* Name() const override { return "BulletVisuals"; }
    void OnInstallHooks() override {
        mem::HookFn(0x73FB10,&NativeInstant,&originalInstant,"CWeapon::FireInstantHit network collision");
        mem::HookFn(0x742300,&NativeFire,&originalFire,"CWeapon::Fire network collision");
        mem::HookFn(0x73CBA0,&NativeCarRay,&originalCarRay,"CWeapon::FireInstantHitFromCar2 network collision");
        mem::HookFn(0x73B550, &HookBulletImpact, &g_originalImpact, "CWeapon::DoBulletImpact");
        Net().SetHandler(proto::G_BULLET, [](uint8_t from, Reader& r) {
            ShotVisual shot;
            if (!shot.Read(r)) return;
            auto it = g_lastSequence.find(from);
            if (it != g_lastSequence.end() && (int32_t)(shot.sequence - it->second) <= 0) return;
            g_lastSequence[from] = shot.sequence;
            if (g_pending.size() >= 128) g_pending.pop_front();
            g_pending.push_back({from, shot, NowMs()});
        });
    }
    void OnPreProcess() override {
        if (!core::IsPlayerReady()) return;
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            auto* rp = Players::Instance().Get(it->from);
            if (NowMs() - it->received > 1000) { it = g_pending.erase(it); continue; }
            if (!rp || !rp->ped) { ++it; continue; }
            const auto& s = it->shot;
            CVector start{s.start.x, s.start.y, s.start.z}, end{s.end.x, s.end.y, s.end.z};
            CVector normal{s.normal.x, s.normal.y, s.normal.z};
            CooperativeStoryShot(it->from,s.weapon,start,end);
            if(IsCooperativeRoleVehicle(rp->ped->VehicleIfIn()))LOGD("Story gunner bullet played source=%u seq=%u weapon=%u",it->from,s.sequence,s.weapon);
            Call<0x726AF0>(&start, &end, (int)s.weapon, (CEntity*)rp->ped);  // native bullet trace / whizz
            if (s.flags & ShotVisual::BLOOD)
                CallMethod<0x49EB00>((void*)0xA9AE00, &end, &normal, 8, 1.0f);  // visual FX only
            else if (s.flags & ShotVisual::IMPACT)
                CallMethod<0x49F3D0>((void*)0xA9AE00, &end, &normal, (int)s.surface, 8, 1.0f);
            it = g_pending.erase(it);
        }
    }
    void OnPeerLeave(uint8_t id) override {
        g_lastSequence.erase(id);
        for (auto it = g_pending.begin(); it != g_pending.end();)
            it = it->from == id ? g_pending.erase(it) : std::next(it);
    }
    void OnGameShutdown() override { g_pending.clear(); }
    void OnSessionEnd() override { g_pending.clear(); g_lastSequence.clear(); }
};
BulletVisuals g_bullets;
struct Reg { Reg() { core::RegisterModule(&g_bullets); } } g_reg;
}  // namespace
}  // namespace coop
