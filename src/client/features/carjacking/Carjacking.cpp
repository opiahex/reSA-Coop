// A native NPC owns its entry task; the player owns the resulting drag-out.
// Replaying the NPC's animation alone cannot release a remote driver's seat.
#include <map>
#include <cmath>
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Anim.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/sync/EventWindow.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"

namespace coop { namespace {
using namespace game;
struct Sent { uint64_t at=0,birth=0;int actor=-1;uint8_t victim=255; };
std::map<void*,Sent> sent;
EventWindow received[proto::kMaxPlayers];uint32_t sequence=0;
struct Transfer{uint16_t npc=0,vehicle=0;uint8_t victim=255;uint64_t birth=0,until=0;};
std::map<uint16_t,Transfer> transfers;
struct DoorSent{uint64_t at=0;uint32_t group=0,anim=0;};
std::map<uint32_t,DoorSent> doors;
EventWindow doorReceived[proto::kMaxPlayers];
using DoorFn=void(__fastcall*)(CVehicle*,void*,CPed*,uint32_t,uint32_t,uint32_t,float);
DoorFn originalDoor=nullptr;
void __fastcall ProcessDoor(CVehicle*car,void*edx,CPed*actor,uint32_t door,uint32_t group,uint32_t anim,float time){
    originalDoor(car,edx,actor,door,group,anim,time);
    if(!Net().Online()||!core::IsPlayerReady()||!actor||door<8||door>11)return;
    auto*np=Peds::Instance().Find(actor);if(!np||np->owner!=Net().MyId())return;
    auto*nv=Vehicles::Instance().Find(car);if(!nv)return;
    auto key=((uint32_t)nv->netId<<8)|door;auto&last=doors[key];
    if(last.group==group&&last.anim==anim&&NowMs()-last.at<100)return;
    last={NowMs(),group,anim};
    Writer w(proto::G_NPC_CAR_DOOR);w.U32(++sequence);w.Entity(ENTITY_PED,np->netId);
    w.Entity(ENTITY_VEHICLE,nv->netId);w.U8((uint8_t)door);w.U16((uint16_t)group);w.U16((uint16_t)anim);w.F32(time);
    Net().Send(proto::CH_RELIABLE,w);
}
void ReceiveDoor(uint8_t from,Reader&r){
    auto token=r.U32();auto npc=r.U16(),vehicle=r.U16();auto door=r.U8();auto group=r.U16(),anim=r.U16();float time=r.F32();
    if(!r.Ok()||from>=proto::kMaxPlayers||from==Net().MyId()||door<8||door>11||group>511||anim>1000||
       !std::isfinite(time)||time<0||time>120||!core::IsPlayerReady())return;
    auto*actor=GetPed(Peds::Instance().HandleOf(npc));auto*np=actor?Peds::Instance().Find(actor):nullptr;
    auto*nv=Vehicles::Instance().Find(vehicle);
    if(!np||np->owner!=from||!nv||!Vehicles::Alive(*nv)||actor->Health()<=0||
       (actor->GetPosition()-nv->veh->GetPosition()).Length()>10||!doorReceived[from].Accept(token))return;
    originalDoor(nv->veh,nullptr,actor,door,group,anim,time);
}

void HoldSeat(RemotePlayer&rp,uint16_t vehicle){
    rp.enterPending=false;
    rp.seatTransition.Begin(vehicle,rp.lastStateTime,NowMs(),true);
    rp.exitTaskMs=NowMs();
}
void Report(void*task,CPed*actor,CPed*victim,CVehicle*car,int door){
    if(!Net().Online()||!core::IsPlayerReady()||!actor||!victim||!IsPedValid(actor)||
       !IsVehicleValid(car)||door<8||door>11)return;
    auto*rp=Players::Instance().FindByPed(victim);auto*np=Peds::Instance().Find(actor);
    if(!rp||!np||np->owner!=Net().MyId()||actor->Health()<=0||!victim->InVehicle()||
       victim->Vehicle()!=car||(actor->GetPosition()-car->GetPosition()).Length()>10)return;
    auto*nv=Vehicles::Instance().EnsureNetworked(car);if(!nv)return;
    auto birth=Net().EntityGeneration(ENTITY_PED,np->netId);auto i=sent.find(task);
    if(i!=sent.end()&&i->second.actor==np->handle&&i->second.birth==birth&&
       i->second.victim==rp->id&&NowMs()-i->second.at<8000)return;
    if(sent.size()>=64)sent.erase(sent.begin());sent[task]={NowMs(),birth,np->handle,rp->id};
    HoldSeat(*rp,nv->netId);
    if(car->Driver()==victim){
        Vehicles::Instance().ExpectCarjack(nv->netId,Net().MyId());
        transfers[nv->netId]={np->netId,nv->netId,rp->id,Net().EntityGeneration(ENTITY_VEHICLE,nv->netId),NowMs()+8000};
    }
    Writer w(proto::G_NPC_CARJACK);w.U32(++sequence);w.Entity(ENTITY_PED,np->netId);
    w.Entity(ENTITY_VEHICLE,nv->netId);w.U8(rp->id);w.U8((uint8_t)door);w.U32(rp->lastStateTime);
    Net().Send(proto::CH_RELIABLE,w);
    LOGI("NPC carjack sent ped=%04X victim=%u vehicle=%04X door=%d",np->netId,rp->id,nv->netId,door);
}
using DragFn=bool(__fastcall*)(void*,void*,CPed*);DragFn originalDrag=nullptr;
bool __fastcall DragProcess(void*task,void*edx,CPed*actor){
    // Actual pulling, after opening the door: approaching a locked car must
    // not eject its occupant before the native task can reach the victim.
    auto*car=Field<CVehicle*>(task,0x10);int door=Field<int>(task,0x14);
    CPed*victim=nullptr;
    if(IsVehicleValid(car)){
        int seat=door==10?-1:SeatOfTargetDoor(door);
        victim=seat==-1?car->Driver():seat>=0&&seat<car->MaxPassengers()?car->Passengers()[seat]:nullptr;
    }
    bool done=originalDrag(task,edx,actor);
    if(!done&&Field<void*>(task,0xC))Report(task,actor,victim,car,door);
    return done;
}
using BikeFn=bool(__fastcall*)(void*,void*,CPed*);BikeFn originalBike=nullptr;
bool __fastcall BikeJacked(void*task,void*edx,CPed*victim){
    Report(task,Field<CPed*>(task,0x20),victim,Field<CVehicle*>(task,0x14),Field<int>(task,0x18));
    return originalBike(task,edx,victim);
}
void Receive(uint8_t from,Reader&r){
    uint32_t token=r.U32();uint16_t npc=r.U16(),vehicle=r.U16();uint8_t victim=r.U8(),door=r.U8();uint32_t time=r.U32();
    if(!r.Ok()||from>=proto::kMaxPlayers||victim>=proto::kMaxPlayers||from==Net().MyId()||
       door<8||door>11||!core::IsPlayerReady())return;
    auto*np=GetPed(Peds::Instance().HandleOf(npc));auto*n=np?Peds::Instance().Find(np):nullptr;
    auto*nv=Vehicles::Instance().Find(vehicle);auto*rp=Players::Instance().Get(victim);
    CPed*ped=victim==Net().MyId()?FindPlayerPed(0):rp?rp->ped:nullptr;
    if(!n||n->owner!=from||np->Health()<=0||!nv||!Vehicles::Alive(*nv)||!ped||
       !ped->InVehicle()||ped->Vehicle()!=nv->veh||ped->Health()<=0||AreaOf(np)!=AreaOf(ped)||
       (np->GetPosition()-nv->veh->GetPosition()).Length()>10||
       (rp&&(int32_t)(rp->lastStateTime-time)>4000)||!received[from].Accept(token))return;
    if(nv->veh->Driver()==ped)Vehicles::Instance().ExpectCarjack(vehicle,from);
    if(rp)HoldSeat(*rp,vehicle);
    if(nv->veh->VehicleSubClass()==VEHICLE_TYPE_BIKE){
        if(!anim::FindTask(ped,826))if(void*t=Call<0x61A5A0,void*>((size_t)0x2C)){
            CallMethod<0x648B90>(t,nv->veh,(uint32_t)door,(uint32_t)TimeMs(),np,nv->veh->Driver()==ped);
            CallMethod<0x681AF0>((uint8_t*)ped->Intelligence()+4,t,0,false);
        }
    }else if(!anim::FindTask(ped,823)&&!anim::FindTask(ped,824)&&!anim::FindTask(ped,825)){
        if(void*t=Call<0x61A5A0,void*>((size_t)0x14)){
            CallMethod<0x648620>(t,nv->veh,(int)door);
            CallMethod<0x681AF0>((uint8_t*)ped->Intelligence()+4,t,0,false);
        }
    }
    LOGI("NPC carjack accepted ped=%04X victim=%u vehicle=%04X",npc,victim,vehicle);
}
class Carjacking:public Module{
    const char*Name()const override{return "Carjacking";}
    void OnInstallHooks()override{
        mem::HookFn(0x64E060,&DragProcess,&originalDrag,"CTaskSimpleCarSlowDragPedOut::ProcessPed");
        mem::HookFn(0x64C970,&BikeJacked,&originalBike,"CTaskSimpleBikeJacked::ProcessPed");
        Net().SetHandler(proto::G_NPC_CARJACK,&Receive);
        mem::HookFn(0x6D56C0,&ProcessDoor,&originalDoor,"CVehicle::ProcessOpenDoor");
        Net().SetHandler(proto::G_NPC_CAR_DOOR,&ReceiveDoor);
    }
    void OnPostProcess()override{
        for(auto i=sent.begin();i!=sent.end();)i=NowMs()-i->second.at>10000?sent.erase(i):std::next(i);
        for(auto i=doors.begin();i!=doors.end();)i=NowMs()-i->second.at>10000?doors.erase(i):std::next(i);
        for(auto i=transfers.begin();i!=transfers.end();){
            const auto&t=i->second;auto*nv=Vehicles::Instance().Find(t.vehicle);
            auto*actor=Peds::Instance().FindOwned(t.npc);auto*victim=Players::Instance().Get(t.victim);
            bool expired=NowMs()>t.until||!nv||!Vehicles::Alive(*nv)||!actor||
                Net().EntityGeneration(ENTITY_VEHICLE,t.vehicle)!=t.birth;
            bool complete=!expired&&actor->InVehicle()&&actor->Vehicle()==nv->veh&&nv->veh->Driver()==actor&&
                (!victim||victim->mode==RemotePlayer::Mode::OnFoot);
            if(complete){Vehicles::Instance().TakeOwnership(*nv);LOGI("NPC carjack completed vehicle=%04X owner=%u",t.vehicle,Net().MyId());}
            i=expired||complete?transfers.erase(i):std::next(i);
        }
    }
    void OnPeerLeave(uint8_t id)override{if(id<proto::kMaxPlayers){received[id]={};doorReceived[id]={};}}
    void OnPeerResync(uint8_t id)override{OnPeerLeave(id);}
    void OnGameShutdown()override{sent.clear();transfers.clear();doors.clear();for(auto&w:received)w={};for(auto&w:doorReceived)w={};}
    void OnSessionEnd()override{OnGameShutdown();sequence=0;}
} feature;struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}}
