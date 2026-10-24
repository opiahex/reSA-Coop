#include "entities/activity/Activities.h"
#include <array>
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/lib/memory/Memory.h"

namespace coop { namespace {
using namespace game;
enum : uint8_t { READY=1, POPULATION=2, TRAINING=4 };
struct State { uint8_t flags=0; uint64_t received=0; };
std::array<State,3> states{};
uint8_t lastFlags=255;uint64_t lastSend=0;
uint8_t LocalFlags() {
    if(!core::IsPlayerReady()||core::IsRestarting())return 0;
    bool school=LocalSchoolActivity();
    // A school, shop or scripted camera may disable population just in its
    // own game. Such a peer cannot supply the nearby group's street traffic.
    bool available=!school&&Ref<float>(0x8D2530)>.001f&&Ref<float>(0x8A5B20)>.001f;
    return READY|(available?POPULATION:0)|(school?TRAINING:0);
}
uint8_t Flags(uint8_t id) {
    if(!Net().Online())return 0;
    if(id==Net().MyId())return LocalFlags();
    return id<states.size()&&Net().Peer(id)&&NowMs()-states[id].received<4000?states[id].flags:0;
}
uint8_t Owner(CEntity* entity) {
    if(!entity)return 255;
    if(entity==FindPlayerPed(0))return Net().MyId();
    if(entity->IsPed()){
        if(auto*rp=Players::Instance().FindByPed((CPed*)entity))return rp->id;
        if(auto*np=Peds::Instance().Find((CPed*)entity))return np->owner;
    }
    if(entity->IsVehicle()){
        auto*v=(CVehicle*)entity;
        // A school can spawn/seat a car before the next entity scan or
        // ownership handover. The player at the wheel identifies it now.
        if(auto*driver=v->Driver()){
            if(driver==FindPlayerPed(0))return Net().MyId();
            if(auto*rp=Players::Instance().FindByPed(driver))return rp->id;
        }
        if(auto*nv=Vehicles::Instance().Find(v))return nv->owner;
        if(Vehicles::ScriptControlled(v))return Net().MyId();
    }
    return 255;
}
uint8_t Trainee(CEntity* entity) {
    auto owner=Owner(entity);
    if(owner==255||!PlayerTraining(owner))return 255;
    if(entity->IsPed()) {
        if(entity==FindPlayerPed(0)||Players::Instance().FindByPed((CPed*)entity))return owner;
        return 255;
    }
    if(entity->IsVehicle()) {
        auto*v=(CVehicle*)entity;auto*nv=Vehicles::Instance().Find(v);
        auto driver=Owner(v->Driver());
        if((driver!=255&&PlayerTraining(driver))||(nv&&(nv->script||(owner==Net().MyId()&&Vehicles::ScriptControlled(v)))))return owner;
    }
    return 255;
}
using PreCollisionFn=void(__fastcall*)(CPhysical*,void*,CPhysical*,bool,bool*,bool*,bool*,bool*);
PreCollisionFn originalPed=nullptr,originalVehicle=nullptr;
void FilterCollision(PreCollisionFn original,CPhysical*a,void*e,CPhysical*b,bool ignore,bool*disabled,bool*ignored,bool*unable,bool*stuck) {
    original(a,e,b,ignore,disabled,ignored,unable,stuck);
    if(disabled&&TrainingIgnoresContact(a,b))*disabled=true;
}
void __fastcall PedCollision(CPhysical*a,void*e,CPhysical*b,bool i,bool*d,bool*g,bool*u,bool*s){FilterCollision(originalPed,a,e,b,i,d,g,u,s);}
void __fastcall VehicleCollision(CPhysical*a,void*e,CPhysical*b,bool i,bool*d,bool*g,bool*u,bool*s){FilterCollision(originalVehicle,a,e,b,i,d,g,u,s);}
using DamageRecordFn=void(__fastcall*)(CPhysical*,void*,float,CEntity*,const void*,float);
DamageRecordFn originalDamageRecord=nullptr;
void __fastcall DamageRecord(CPhysical*a,void*edx,float force,CEntity*b,const void*point,float distance){
    // Direct/native collision paths can record damage outside the pair
    // precheck. Do not leave a deferred impact for VehicleDamage next frame.
    if(!TrainingIgnoresContact(a,b))originalDamageRecord(a,edx,force,b,point,distance);
}
class Activities:public Module {
public:
    const char*Name()const override{return "Activities";}
    void OnInstallHooks()override{
        Net().SetHandler(proto::G_ACTIVITY_STATE,[](uint8_t from,Reader&r){auto flags=r.U8();if(!r.Ok()||from>=states.size()||!Net().Peer(from)||(flags&~7))return;states[from]={flags,NowMs()};});
        // Native pair filters preserve collision with roads, walls, cones and
        // exercise targets. Turning off bUsesCollision would also lose those.
        mem::HookFn(0x5E3C30,&PedCollision,&originalPed,"CPed::SpecialEntityPreCollisionStuff");
        mem::HookFn(0x6D6640,&VehicleCollision,&originalVehicle,"CVehicle::SpecialEntityPreCollisionStuff");
        mem::HookFn(0x5428C0,&DamageRecord,&originalDamageRecord,"CPhysical::SetDamagedPieceRecord");
    }
    void OnPreProcess()override{
        if(!Net().Online())return;
        auto flags=LocalFlags();auto now=NowMs();
        if(flags!=lastFlags||now-lastSend>1000){Writer w(proto::G_ACTIVITY_STATE);w.U8(flags);Net().Send(proto::CH_RELIABLE,w);if(flags!=lastFlags)LOGI("Local activity: school=%d population_available=%d",(flags&TRAINING)!=0,(flags&POPULATION)!=0);lastFlags=flags;lastSend=now;}
    }
    void OnSessionStart()override{states={};lastFlags=255;lastSend=0;}
    void OnSessionEnd()override{OnSessionStart();}
    void OnPeerLeave(uint8_t id)override{if(id<states.size())states[id]={};}
    void OnPeerResync(uint8_t id)override{OnPeerLeave(id);lastSend=0;lastFlags=255;}
    void OnGameShutdown()override{if(Net().Online()){Writer w(proto::G_ACTIVITY_STATE);w.U8(0);Net().Send(proto::CH_RELIABLE,w);}lastFlags=255;}
} activities;
struct Reg{Reg(){core::RegisterModule(&activities);}}reg;
}
bool PlayerTraining(uint8_t id){return (Flags(id)&TRAINING)!=0;}
bool PopulationAvailable(uint8_t id){return (Flags(id)&(READY|POPULATION))==(READY|POPULATION);}
bool TrainingIgnoresContact(game::CEntity*a,game::CEntity*b){
    if(!Net().Online()||!core::IsPlayerReady()||!a||!b)return false;
    // Ordinary street collisions need no entity-map lookups when nobody is
    // taking a course. This filter also runs for native ambient traffic.
    if(!PlayerTraining(0)&&!PlayerTraining(1)&&!PlayerTraining(2))return false;
    auto x=Owner(a),y=Owner(b);
    return x!=255&&y!=255&&x!=y&&(Trainee(a)!=255||Trainee(b)!=255);
}
bool TrainingVehicleRestricted(game::CVehicle*v){
    auto*nv=v?Vehicles::Instance().Find(v):nullptr;
    return nv&&nv->owner!=Net().MyId()&&PlayerTraining(nv->owner)&&nv->script;
}
}
