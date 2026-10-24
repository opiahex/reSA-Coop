#include <vector>
#include "entities/player/Players.h"
#include "entities/ped/Peds.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "common/lib/util/Util.h"
#include "common/lib/log/Log.h"

namespace coop {namespace {
using namespace game;
using ClearFn=void(__cdecl*)(const CVector&,float,uint8_t);
ClearFn original[5]{};
bool Needed(CEntity*e){
    for(auto&[id,rp]:Players::Instance().All())
        if(rp.ped&&rp.hasState&&rp.state.health>0&&NowMs()-rp.stateRecvMs<1500&&
           rp.state.area==e->AreaCode()&&(rp.state.pos-e->GetPosition()).Length()<180)return true;
    return false;
}
template<int N>void __cdecl Clear(const CVector&at,float radius,uint8_t projectiles){
    std::vector<std::pair<int,uint8_t>> cars,peds;
    if(Net().Online()&&core::IsPlayerReady()){
        for(auto&[id,n]:Vehicles::Instance().All())if(Vehicles::Alive(n)&&!Vehicles::Instance().IsMirror(n)&&
            n.veh->CreatedBy()!=2&&n.veh->Health()>0&&n.veh->Status()!=5&&Needed(n.veh)){
            cars.push_back({n.handle,n.veh->CreatedBy()});CallMethod<0x6D5D70>(n.veh,2);
        }
        for(auto&[id,n]:Peds::Instance().All())if(n.ped&&IsPedValid(n.ped)&&n.owner==Net().MyId()&&
            n.ped->CreatedBy()==1&&n.ped->Health()>0&&Needed(n.ped)){
            peds.push_back({GetPedRef(n.ped),n.ped->CreatedBy()});n.ped->CreatedBy()=2;
        }
    }
    // Only the five native resurrection cleanup calls are redirected. Scripted
    // CLEAR_AREA/deletion still works, and entity IDs are never resurrected.
    original[N](at,radius,projectiles);
    for(auto&[h,by]:peds)if(auto*p=GetPed(h))p->CreatedBy()=by;
    for(auto&[h,by]:cars)if(auto*v=GetVehicle(h))CallMethod<0x6D5D70>(v,(int)by);
    if(!cars.empty()||!peds.empty())LOGI("Resurrection retained nearby partner population cars=%u peds=%u",(unsigned)cars.size(),(unsigned)peds.size());
}
class Resurrection:public Module{
    const char*Name()const override{return "SharedResurrectionPopulation";}
    void OnInstallHooks()override{
        mem::RedirectCallFn(0x4422B7,&Clear<0>,&original[0]);
        mem::RedirectCallFn(0x4429BB,&Clear<1>,&original[1]);
        mem::RedirectCallFn(0x442F08,&Clear<2>,&original[2]);
        mem::RedirectCallFn(0x4432B3,&Clear<3>,&original[3]);
        mem::RedirectCallFn(0x44355E,&Clear<4>,&original[4]);
    }
}feature;struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}}
