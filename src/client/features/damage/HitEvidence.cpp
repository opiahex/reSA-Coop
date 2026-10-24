#include <map>
#include <set>
#include "features/damage/HitEvidence.h"
#include "features/damage/HitContext.h"
#include "shared/lib/sync/HitHistory.h"
#include "entities/player/Players.h"
#include "entities/ped/Peds.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/lib/core/Core.h"
#include "shared/api/net-session/Session.h"
#include "common/lib/util/Util.h"
#include "common/lib/log/Log.h"
namespace coop { namespace {
using namespace game;
struct Track {int handle=-1;uint16_t model=0;uint64_t seen=0;HitHistory history;};
std::map<CEntity*,Track> tracks;uint64_t sampled=0;
int Handle(CEntity*e){return e->IsPed()?GetPedRef((CPed*)e):e->IsVehicle()?GetVehicleRef((CVehicle*)e):-1;}
int8_t EffectiveArea(CEntity*e){return e->IsPed()&&!((CPed*)e)->IsPlayer()&&!Players::Instance().FindByPed((CPed*)e)?AreaOf((CPed*)e):e->AreaCode();}
class Evidence:public Module{
 const char*Name()const override{return "HitEvidence";}
 void OnPostProcess()override{
    if(!Net().Online()||!core::IsPlayerReady()||NowMs()-sampled<25)return;
    sampled=NowMs();
    auto add=[&](CEntity*e,bool alive){
        if(!e)return;auto&t=tracks[e];int h=Handle(e);
        if(t.handle!=h||t.model!=e->ModelIndex()){t={};t.handle=h;t.model=e->ModelIndex();}
        t.seen=sampled;t.history.Push((uint32_t)sampled,e->GetPosition(),EffectiveArea(e),alive);
    };
    auto*me=FindPlayerPed(0);add(me,me&&me->Health()>0);
    for(auto&[id,n]:Peds::Instance().All())if(n.owner==Net().MyId()&&n.ped&&GetPed(n.handle)==n.ped)add(n.ped,n.ped->Health()>0);
    for(auto&[id,n]:Vehicles::Instance().All())if(!Vehicles::Instance().IsMirror(n)&&Vehicles::Alive(n))add(n.veh,n.veh->Health()>0);
    for(auto i=tracks.begin();i!=tracks.end();)i=i->second.seen!=sampled?tracks.erase(i):std::next(i);
 }
 void OnGameShutdown()override{tracks.clear();sampled=0;}
 void OnSessionEnd()override{OnGameShutdown();}
} evidence;
struct Reg{Reg(){core::RegisterModule(&evidence);}}reg;
}
void WriteHitEvidence(Writer&w,game::CEntity*target){
    bool present=target&&(!g_bulletHit.target||target==g_bulletHit.target);uint32_t tick=0;
    if(present&&target->IsPed()){
        if(auto*p=Players::Instance().FindByPed((game::CPed*)target))tick=p->history.Size()?p->history.PlayoutTick(NowMs()):p->state.time;
        else if(auto*p=Peds::Instance().Find((game::CPed*)target))tick=p->state.time;
    }else if(present&&target->IsVehicle()){
        if(auto*v=Vehicles::Instance().Find((game::CVehicle*)target))tick=v->history.Size()?v->history.PlayoutTick(NowMs()):v->state.time;
    }
    w.Bool(present&&tick);
    if(!present||!tick)return;
    w.U32(tick);w.U16(target->ModelIndex());w.I8(EffectiveArea(target));
    uint8_t clockOwner=Net().MyId();uint32_t repair=0;
    if(target->IsPed()){
        if(auto*p=Players::Instance().FindByPed((game::CPed*)target))clockOwner=p->id;
        else if(auto*n=Peds::Instance().Find((game::CPed*)target))clockOwner=n->owner;
    }else if(target->IsVehicle())if(auto*n=Vehicles::Instance().Find((game::CVehicle*)target)){clockOwner=n->owner;repair=n->repairRevision;}
    w.U8(clockOwner);w.U32(repair);
    auto p=target->GetPosition(),h=target==g_bulletHit.target?g_bulletHit.end:p;
    w.Vec({p.x,p.y,p.z});w.Vec({h.x,h.y,h.z});
}
bool ReadHitEvidence(Reader&r,game::CEntity*target){
    bool present=r.Bool();if(!present)return r.Ok();
    uint32_t tick=r.U32();uint16_t model=r.U16();int8_t area=r.I8();uint8_t clockOwner=r.U8();uint32_t repair=r.U32();auto p=r.Vec(),h=r.Vec();
    if(!r.Ok()||!target||target->ModelIndex()!=model||EffectiveArea(target)!=area)return false;
    uint64_t handover=0;
    if(target->IsVehicle())if(auto*n=Vehicles::Instance().Find((game::CVehicle*)target)){if(n->repairRevision!=repair)return false;handover=n->handoverUntil;}
    if(target->IsPed())if(auto*n=Peds::Instance().Find((game::CPed*)target))handover=n->handoverUntil;
    if(clockOwner!=Net().MyId()&&(clockOwner>=3||NowMs()>=handover))return false;
    auto it=tracks.find(target);
    game::CVector seen{p.x,p.y,p.z},hit{h.x,h.y,h.z};bool vehicle=target->IsVehicle();
    bool ok=it!=tracks.end()&&it->second.handle==Handle(target)&&
        (clockOwner==Net().MyId()?it->second.history.Accept((uint32_t)NowMs(),tick,seen,area,vehicle?8.f:3.f):(target->GetPosition()-seen).Length()<(vehicle?8.f:3.f))&&
        (hit-seen).Length()< (vehicle?18.f:3.5f);
    LOGD("Historical hit %s tick %u age %d model %u",ok?"accepted":"rejected",tick,(int32_t)((uint32_t)NowMs()-tick),model);
    return ok;
}
}
