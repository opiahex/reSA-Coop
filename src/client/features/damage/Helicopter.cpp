#include <array>
#include <deque>
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/lib/sync/EventWindow.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
namespace coop {namespace {
using namespace game;
CVehicle*processing=nullptr;uint32_t sequence=0;EventWindow seen[3];
struct Shot{uint8_t from=0;uint16_t id=0;uint32_t seq=0;CVector offset,end;int damage=0;uint64_t received=0;std::vector<EntityRef> refs;};
std::deque<Shot> pending;
using ProcessFn=void(__fastcall*)(CVehicle*,void*);ProcessFn originalProcess=nullptr;
void __fastcall Process(CVehicle*v,void*edx){auto*previous=processing;processing=v;originalProcess(v,edx);processing=previous;}
using RoundFn=void(__cdecl*)(const CVector*,const CVector*,int);RoundFn originalRound=nullptr;
CVector World(const CMatrix&m,const CVector&p){return m.pos+m.right*p.x+m.forward*p.y+m.up*p.z;}
using AudioFn=void(__fastcall*)(void*,void*,int,int,CEntity*);AudioFn originalAudio=nullptr;
void __fastcall Audio(void*self,void*edx,int event,int weapon,CEntity*entity){
    auto*n=processing?Vehicles::Instance().Find(processing):nullptr;
    if(Net().Online()&&n&&Vehicles::Instance().IsMirror(*n))return;
    originalAudio(self,edx,event,weapon,entity);
}
bool Replay(const Shot&s){
    auto*n=Vehicles::Instance().Find(s.id);
    if(!n||!Vehicles::Alive(*n))return false;
    if(n->owner!=s.from||!Vehicles::Instance().IsMirror(*n)||n->veh->VehicleSubClass()!=VEHICLE_TYPE_HELI)return true;
    if(n->veh->AreaCode()!=13&&n->veh->AreaCode()!=FindPlayerPed(0)->AreaCode())return true;
    if(!n->veh->Matrix())return false;
    auto start=World(*n->veh->Matrix(),s.offset);if(!((s.end-start).Length()<300))return true;
    Call<0x73AF00>(&start,&s.end,s.damage);CallMethod<0x506F40>((void*)0xB6BC90,0x91,0x1F,n->veh);
    LOGD("Helicopter round replayed vehicle=%04X seq=%u source=%u",s.id,s.seq,s.from);return true;
}
void __cdecl Round(const CVector*start,const CVector*end,int damage){
    auto*n=processing?Vehicles::Instance().Find(processing):nullptr;
    if(Net().Online()&&n&&Vehicles::Instance().IsMirror(*n))return;
    originalRound(start,end,damage);
    if(!Net().Online()||!core::IsPlayerReady()||!processing||!start||!end||!processing->Matrix())return;
    if(!n)n=Vehicles::Instance().EnsureNetworked(processing);if(!n||Vehicles::Instance().IsMirror(*n))return;
    auto&m=*processing->Matrix();auto delta=*start-m.pos;CVector offset{delta.Dot(m.right),delta.Dot(m.forward),delta.Dot(m.up)};
    Writer w(proto::G_HELI_SHOT);w.Entity(ENTITY_VEHICLE,n->netId);w.U32(++sequence);w.Vec({offset.x,offset.y,offset.z});w.Vec({end->x,end->y,end->z});w.U8((uint8_t)std::clamp(damage,0,255));Net().Send(proto::CH_RELIABLE,w);
    LOGD("Helicopter round sent vehicle=%04X seq=%u",n->netId,sequence);
}
class Helicopter:public Module{
    const char*Name()const override{return "HelicopterRounds";}
    void OnInstallHooks()override{
        // Capture the exact vehicle executing this native call, rather than
        // guessing a helicopter near the muzzle when two aircraft overlap.
        mem::HookFn(0x6C7050,&Process,&originalProcess,"CHeli::ProcessControl gun authority");
        mem::RedirectCallFn(0x6C7773,&Round,&originalRound);
        mem::RedirectCallFn(0x6C7788,&Audio,&originalAudio);
        Net().SetHandler(proto::G_HELI_SHOT,[](uint8_t from,Reader&r){Shot s;s.from=from;s.id=r.U16();s.seq=r.U32();auto a=r.Vec(),b=r.Vec();s.damage=r.U8();s.offset={a.x,a.y,a.z};s.end={b.x,b.y,b.z};
            if(!r.Ok()||from>=3||!core::IsPlayerReady()||!(s.offset.Length()<10)||!(s.end.Length()<25000)||!seen[from].Accept(s.seq))return;
            s.received=NowMs();s.refs=Net().EntityContext();
            if(!Replay(s)){if(pending.size()>=64)pending.pop_front();pending.push_back(std::move(s));}
        });
    }
    void OnPostProcess()override{if(!core::IsPlayerReady())return;for(auto i=pending.begin();i!=pending.end();){
        if(NowMs()-i->received>1000||!Net().EntityContextValid(i->refs)||Replay(*i))i=pending.erase(i);else ++i;
    }}
    void OnPeerLeave(uint8_t id)override{if(id<3)seen[id]={};for(auto i=pending.begin();i!=pending.end();)i=i->from==id?pending.erase(i):std::next(i);}
    void OnGameShutdown()override{pending.clear();processing=nullptr;for(auto&w:seen)w={};}
    void OnSessionEnd()override{OnGameShutdown();sequence=0;}
}feature;struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}}
