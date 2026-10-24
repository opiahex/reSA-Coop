#include <map>
#include "shared/api/vehicle-effects/VehicleEffects.h"
#include "shared/api/net-session/Session.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/lib/core/Core.h"
#include "common/lib/util/Util.h"
#include "common/lib/log/Log.h"
namespace coop {namespace {
struct Horn {uint64_t generation=0,received=0,holdUntil=0,sent=0;uint32_t sequence=0;uint8_t owner=255;bool on=false;};
std::map<uint16_t,Horn> horns;uint32_t sequence=0;
class Effects:public Module {
 const char*Name()const override{return "VehicleEffects";}
 void OnInstallHooks()override{Net().SetHandler(proto::G_VEH_HORN,[](uint8_t from,Reader&r){
    auto id=r.U16();auto seq=r.U32();bool on=r.Bool();auto*n=Vehicles::Instance().Find(id);
    if(!r.Ok()||!seq||!n||!Vehicles::Alive(*n)||n->owner!=from||!Vehicles::Instance().IsMirror(*n))return;
    auto&h=horns[id];auto generation=Net().EntityGeneration(ENTITY_VEHICLE,id);
    if(h.generation!=generation||h.owner!=from)h={};
    if(h.received&&(int32_t)(seq-h.sequence)<=0)return;
    h.generation=generation;h.owner=from;h.sequence=seq;h.received=NowMs();h.on=on;
    if(on)h.holdUntil=NowMs()+80;
    LOGD("Horn received vehicle=%04X owner=%u on=%d seq=%u",id,from,on,seq);
 });}
 void OnPostProcess()override{for(auto i=horns.begin();i!=horns.end();){auto*n=Vehicles::Instance().Find(i->first);i=(!n||i->second.generation!=Net().EntityGeneration(ENTITY_VEHICLE,i->first)||i->second.owner!=n->owner)?horns.erase(i):std::next(i);}}
 void OnSessionEnd()override{horns.clear();sequence=0;}
 void OnGameShutdown()override{horns.clear();}
 void OnEntityReplaced(uint8_t kind,uint8_t,uint16_t id)override{if(kind==ENTITY_VEHICLE)horns.erase(id);}
}feature;struct Register{Register(){core::RegisterModule(&feature);}}registration;
}
void CaptureVehicleHorn(game::CVehicle*vehicle,bool sounding){
    if(!Net().Online()||!core::IsPlayerReady())return;
    auto*n=Vehicles::Instance().Find(vehicle);if(!n||!Vehicles::Alive(*n)||Vehicles::Instance().IsMirror(*n))return;
    auto&h=horns[n->netId];auto generation=Net().EntityGeneration(ENTITY_VEHICLE,n->netId);
    if(h.generation!=generation||h.owner!=Net().MyId())h={};
    if(h.sent&&h.on==sounding&&(!sounding||NowMs()-h.sent<250))return;
    // Silence is sent too: a short tap is retained even between state snapshots.
    h.generation=generation;h.owner=Net().MyId();h.on=sounding;h.sent=NowMs();
    Writer w(proto::G_VEH_HORN);w.Entity(ENTITY_VEHICLE,n->netId);w.U32(++sequence);w.Bool(sounding);Net().Send(proto::CH_RELIABLE,w);
    LOGD("Horn sent vehicle=%04X on=%d seq=%u",n->netId,sounding,sequence);
}
bool RemoteVehicleHorn(game::CVehicle*vehicle,bool fallback){
    auto*n=Vehicles::Instance().Find(vehicle);if(!n||!Vehicles::Instance().IsMirror(*n))return fallback;
    auto i=horns.find(n->netId);
    if(i!=horns.end()){auto&h=i->second;if(h.owner==n->owner&&h.generation==Net().EntityGeneration(ENTITY_VEHICLE,n->netId)&&h.received)
        return NowMs()-h.received<1000&&(h.on||NowMs()<h.holdUntil);}
    return n->hasState&&NowMs()-n->recvMs<500&&(n->state.flags&VehicleState::F_HORN);
}
}
