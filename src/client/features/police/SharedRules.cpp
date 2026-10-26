// Police rules apply to nearby participants of the host's story/war, not to
// distant players or their private school activities. Based on upstream 95d336a.
#include <cmath>
#include <vector>
#include "entities/activity/Activities.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/ped/Peds.h"
#include "entities/vehicle/Vehicles.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/api/gangwar/GangWarCoop.h"
#include "shared/game/Anim.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "common/lib/util/Util.h"
#include "common/lib/log/Log.h"
namespace coop {namespace {
using namespace game;
bool ReplicaOffender(CEntity*e){
    if(!Net().Online()||!e)return false;
    auto*p=e->IsPed()?(CPed*)e:e->IsVehicle()?((CVehicle*)e)->Driver():nullptr;
    return p&&Players::Instance().FindByPed(p);
}
using CrimeFn=void(__cdecl*)(int,CEntity*,CPed*);CrimeFn originalCrime=nullptr;
void __cdecl Crime(int type,CEntity*victim,CPed*by){if(!ReplicaOffender(by))originalCrime(type,victim,by);}
using EventFn=void(__fastcall*)(void*,void*,CPed*);EventFn originalRunOver=nullptr,originalCarDamage=nullptr,originalKnockOff=nullptr;
void __fastcall RunOver(void*e,void*edx,CPed*p){if(!ReplicaOffender(Field<CEntity*>(e,0x10)))originalRunOver(e,edx,p);}
void __fastcall CarDamage(void*e,void*edx,CPed*p){if(!ReplicaOffender(Field<CEntity*>(e,0x18)))originalCarDamage(e,edx,p);}
void __fastcall KnockOff(void*e,void*edx,CPed*p){if(!ReplicaOffender(Field<CEntity*>(e,0x30)))originalKnockOff(e,edx,p);}
using DiveFn=void(__fastcall*)(void*,void*,CPed*);DiveFn originalDive=nullptr;
void __fastcall Dive(void*t,void*edx,CPed*p){
    auto*me=core::IsPlayerReady()?FindPlayerPed(0):nullptr;auto*w=me&&me->PlayerData()?me->Wanted():nullptr;
    if(!w||!ReplicaOffender(Field<CEntity*>(t,8)))return originalDive(t,edx,p);
    uint8_t saved[0x21C];memcpy(saved,w,sizeof saved);originalDive(t,edx,p);memcpy(w,saved,sizeof saved);
}
// An empty replica is STATUS_PHYSICS for interpolation, while an empty native
// police car is ABANDONED and doesn't hold a wanted star. Scope that difference
// to the presence query; a live police crew must still prevent evasion.
using PresenceFn=int(__cdecl*)(CVector,float);PresenceFn originalPresence=nullptr;
int __cdecl Presence(CVector at,float radius){
    std::vector<std::pair<CVehicle*,uint8_t>> empty;
    if(Net().Online())for(auto&[id,n]:Vehicles::Instance().All())if(Vehicles::Alive(n)&&Vehicles::Instance().IsMirror(n)&&(n.veh->VehicleFlagsLo()&1)&&n.veh->Status()==3){
        auto crew=[](CPed*p){if(!p||!IsPedValid(p)||p->Health()<=0)return false;auto*np=Peds::Instance().Find(p);return p->PedType()==PED_TYPE_COP||(np&&np->pedType==PED_TYPE_COP);};
        bool occupied=crew(n.veh->Driver());for(int i=0;i<8;++i)occupied|=crew(n.veh->Passengers()[i]);
        if(!occupied){empty.push_back({n.veh,n.veh->Status()});n.veh->SetStatus(4);}
    }
    int count=originalPresence(at,radius);for(auto&[v,status]:empty)v->SetStatus(status);return count;
}
struct Rules{bool active=false,war=false;uint8_t max=6,ignore=0;float multiplier=1;};Rules remote,own;
uint64_t received=0,sent=0;bool applied=false;uint32_t sequence=0,latest=0;
bool Nearby(){
    auto*rp=Players::Instance().Get(Net().HostId());auto*me=core::IsPlayerReady()?FindPlayerPed(0):nullptr;
    return Net().Online()&&!Net().IsHost()&&me&&flow::InHostWorld()&&!PlayerTraining(Net().MyId())&&rp&&rp->hasState&&NowMs()-rp->stateRecvMs<1500&&rp->state.area==AreaOf(me)&&(rp->state.pos-me->GetPosition()).Length()<(applied?340.f:280.f)&&received&&NowMs()-received<3500;
}
using WarFn=bool(__cdecl*)();WarFn originalWar[2]{};
bool LocalCooperativeWar(){
    // A native police callback may currently borrow the offender's slot.
    // The global battle flag alone would exempt an unrelated distant player.
    auto*me=core::IsPlayerReady()?FindPlayerPed():nullptr;
    return me&&CooperativeGangWarFightingAt(me->GetPosition(),AreaOf(me));
}
bool __cdecl War0(){return Net().Online()&&core::IsPlayerReady()?LocalCooperativeWar():originalWar[0]();}
bool __cdecl War1(){return Net().Online()&&core::IsPlayerReady()?LocalCooperativeWar():originalWar[1]();}
Rules Capture(){Rules r;auto*w=FindPlayerPed(0)->Wanted();r.max=(uint8_t)Ref<int32_t>(0x8CDEE4);r.ignore=Field<uint8_t>(w,0x1E)&5;r.multiplier=Field<float>(w,0x14);return r;}
void Apply(const Rules&r){auto*w=FindPlayerPed(0)->Wanted();Call<0x561E70>((int)r.max);Field<uint8_t>(w,0x1E)=(Field<uint8_t>(w,0x1E)&~5u)|(r.ignore&5);Field<float>(w,0x14)=r.multiplier;}
void Restore(){if(applied&&core::IsPlayerReady()){Apply(own);applied=false;LOGD("Shared police rules released");}}
class SharedRules:public Module{
    const char*Name()const override{return "SharedPoliceRules";}
    void OnInstallHooks()override{
        mem::HookFn(0x532010,&Crime,&originalCrime,"CCrime::ReportCrime actual offender");
        mem::HookFn(0x4B27D0,&RunOver,&originalRunOver,"CEventVehicleHitAndRun actual offender");
        mem::HookFn(0x4B50B0,&CarDamage,&originalCarDamage,"CEventVehicleDamage actual offender");
        mem::HookFn(0x4B4E80,&KnockOff,&originalKnockOff,"CEventKnockOffBike actual offender");
        mem::HookFn(0x655F20,&Dive,&originalDive,"CTaskSimpleEvasiveDive actual offender");
        mem::HookFn(0x5625F0,&Presence,&originalPresence,"CWanted::WorkOutPolicePresence live police crews");
        mem::RedirectCallFn(0x562167,&War0,&originalWar[0]);mem::RedirectCallFn(0x56218E,&War1,&originalWar[1]);
        Net().SetHandler(proto::G_POLICE_RULES,[](uint8_t from,Reader&r){auto seq=r.U32();Rules next;next.active=r.Bool();next.war=r.Bool();next.max=r.U8();next.ignore=r.U8();next.multiplier=r.F32();
            if(!r.Ok()||from!=Net().HostId()||Net().IsHost()||(latest&&(int32_t)(seq-latest)<=0)||next.max>6||next.ignore>5||!std::isfinite(next.multiplier)||next.multiplier<0||next.multiplier>10)return;
            remote=next;latest=seq;received=NowMs();});
    }
    void OnPostProcess()override{
        if(!Net().Online()||!core::IsPlayerReady())return;
        if(Net().IsHost()){
            if(NowMs()-sent<750)return;sent=NowMs();Rules r=Capture();r.war=Call<0x443AC0,bool>();r.active=r.war||HostReplicating();
            Writer w(proto::G_POLICE_RULES);w.U32(++sequence);w.Bool(r.active);w.Bool(r.war);w.U8(r.max);w.U8(r.ignore);w.F32(r.multiplier);Net().Send(proto::CH_RELIABLE,w);return;
        }
        if(remote.active&&Nearby()){if(!applied){own=Capture();applied=true;LOGD("Shared police rules applied war=%d",remote.war);}Apply(remote);}else Restore();
    }
    void OnPeerJoin(uint8_t)override{sent=0;}
    void OnPeerResync(uint8_t)override{sent=0;}
    void OnPeerLeave(uint8_t id)override{if(id==Net().HostId()){remote={};received=0;Restore();}}
    void OnGameShutdown()override{applied=false;remote={};received=0;latest=0;}
    void OnSessionEnd()override{Restore();OnGameShutdown();sequence=0;}
}feature;struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}}
