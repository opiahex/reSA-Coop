#include <algorithm>
#include "features/damage/HitContext.h"
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/sync/EventWindow.h"

namespace coop { namespace {
using namespace game;
using BurstFn=bool(__fastcall*)(CVehicle*,void*,uint8_t,bool);
BurstFn originalCar=nullptr,originalBike=nullptr;
using FixFn=void(__fastcall*)(CVehicle*,void*);FixFn originalCarFix=nullptr,originalBikeFix=nullptr;
EventWindow received[3];
uint32_t lastBullet=0;uint16_t lastVehicle=0;uint8_t lastPiece=0;
bool replay=false;
bool Burst(CVehicle*v,void*edx,uint8_t piece,bool effect,BurstFn original){
    // Native CBike accepts only 0/1 or wheel components 13/15; passing 14/16
    // indexes beyond its two tyre bytes.
    if(WheelState::Count(v)==2&&piece!=13&&piece!=15&&piece>1)return false;
    auto*n=Net().Online()&&core::IsPlayerReady()?Vehicles::Instance().Find(v):nullptr;
    if(n&&Vehicles::Instance().IsMirror(*n)&&!replay){
        if(g_bulletHit.target==v&&g_bulletHit.sequence&&piece>=13&&piece<=16&&
           !(lastBullet==g_bulletHit.sequence&&lastVehicle==n->netId&&lastPiece==piece)){
            lastBullet=g_bulletHit.sequence;lastVehicle=n->netId;lastPiece=piece;
            Writer w(proto::G_TYRE_HIT);w.U32(lastBullet);w.Entity(ENTITY_VEHICLE,n->netId);w.U16(n->model);
            w.U8(piece);w.U8(g_bulletHit.weapon);w.U32(n->state.time);
            w.U8(n->owner);w.U32(n->repairRevision);
            auto p=v->GetPosition();w.Vec({p.x,p.y,p.z});
            auto h=g_bulletHit.end;w.Vec({h.x,h.y,h.z});
            Net().Send(proto::CH_RELIABLE,w);
            LOGD("Tyre hit sent: car %04X piece %u shot %u",n->netId,piece,lastBullet);
        }
        // The owner sends the real damage manager. Do not let this client's
        // tyre status be rolled back by its next authoritative appearance.
        return true;
    }
    bool changed=original(v,edx,piece,effect);
    if(changed&&n){n->lastChangeMs=NowMs();n->lastSendMs=0;n->lookCheckMs=0;}
    return changed;
}
bool __fastcall Car(CVehicle*v,void*e,uint8_t p,bool f){return Burst(v,e,p,f,originalCar);}
bool __fastcall Bike(CVehicle*v,void*e,uint8_t p,bool f){return Burst(v,e,p,f,originalBike);}
void Fixed(CVehicle*v){
    if(!Net().Online())return;auto*n=Vehicles::Instance().Find(v);
    if(n&&!n->applyingLook&&!Vehicles::Instance().IsMirror(*n)){
        ++n->repairRevision;n->hasLook=false;n->lookCheckMs=0;n->lastSendMs=0;
        LOGD("Vehicle %04X repaired revision %u",n->netId,n->repairRevision);
    }
}
void __fastcall CarFix(CVehicle*v,void*e){originalCarFix(v,e);Fixed(v);}
void __fastcall BikeFix(CVehicle*v,void*e){originalBikeFix(v,e);Fixed(v);}
class Tyres:public Module {
public:
 const char*Name()const override{return "Tyres";}
 void OnInstallHooks()override{
    mem::HookFn(0x6A32B0,&Car,&originalCar,"CAutomobile::BurstTyre");
    mem::HookFn(0x6BEB20,&Bike,&originalBike,"CBike::BurstTyre");
    mem::HookFn(0x6A3440,&CarFix,&originalCarFix,"CAutomobile::Fix");
    mem::HookFn(0x6B7050,&BikeFix,&originalBikeFix,"CBike::Fix");
    Net().SetHandler(proto::G_TYRE_HIT,[](uint8_t from,Reader&r){
        uint32_t shot=r.U32();uint16_t id=r.U16(),model=r.U16();
        uint8_t piece=r.U8(),weapon=r.U8();uint32_t tick=r.U32();uint8_t clockOwner=r.U8();uint32_t repair=r.U32();auto p=r.Vec(),h=r.Vec();
        if(!r.Ok()||from>=3||!shot||piece<13||piece>16||!((weapon>=22&&weapon<=34)||weapon==38||weapon==52)||!core::IsPlayerReady())return;
        auto*n=Vehicles::Instance().Find(id);auto*shooter=Players::Instance().Get(from);
        if(!n||!Vehicles::Alive(*n)||Vehicles::Instance().IsMirror(*n)||n->model!=model||!shooter||!shooter->hasState)return;
        // This is the target owner's clock, not the shooter's system clock.
        int32_t age=(int32_t)((uint32_t)NowMs()-tick);
        CVector seen{p.x,p.y,p.z},hit{h.x,h.y,h.z};
        bool transfer=clockOwner!=Net().MyId()&&clockOwner<3&&NowMs()<n->handoverUntil;
        bool freshSpawn=!tick&&NowMs()-n->firstAnnouncedMs<3000;
        if(repair!=n->repairRevision){LOGD("Tyre hit predates repair: car %04X received %u current %u",id,repair,n->repairRevision);return;}
        if((!transfer&&!freshSpawn&&(!tick||age < -100||age>1500))||(seen-n->veh->GetPosition()).Length()>45||
           (hit-seen).Length()>15||(hit-shooter->state.pos).Length()>450||
           (shooter->state.area!=n->veh->AreaCode()&&n->veh->AreaCode()!=13)||!received[from].Accept(shot))return;
        replay=true;int count=WheelState::Count(n->veh);
        // Native BurstTyre retains tyre-proof/Rhino/wreck checks and applies
        // the wheel impulse and burst sound once in the simulator's game.
        bool changed=count==2?Bike(n->veh,nullptr,piece,true):count==4?Car(n->veh,nullptr,piece,true):false;
        replay=false;
        LOGI("Tyre hit applied: from %u car %04X piece %u age %d changed %d",from,id,piece,age,changed);
    });
 }
 void OnPeerLeave(uint8_t id)override{if(id<3)received[id]={};}
 void OnPeerResync(uint8_t id)override{OnPeerLeave(id);}
 void OnGameShutdown()override{for(auto&w:received)w={};lastBullet=0;}
 void OnSessionEnd()override{OnGameShutdown();}
};
Tyres tyres;struct Reg{Reg(){core::RegisterModule(&tyres);}}reg;
}}
