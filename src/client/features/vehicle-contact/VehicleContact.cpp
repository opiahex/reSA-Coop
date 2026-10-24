// Forward contact that an owner could not observe through a delayed proxy.
// Retain native collision response and discard a report already resolved here.
#include <algorithm>
#include <cmath>
#include <map>
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/vehicle/Vehicles.h"
#include "entities/activity/Activities.h"
#include "shared/api/net-session/Session.h"
#include "shared/lib/memory/Memory.h"
#include "shared/api/mission-objects/SharedObject.h"
namespace coop { namespace {
using namespace game;
struct Contact {uint16_t source=0,target=0;uint8_t owner=255;CVector move,turn;};
std::map<uint32_t,Contact> pending;
std::map<uint64_t,uint32_t> received;
std::map<uint32_t,uint64_t> nativeContacts;
uint32_t sequence=0;uint64_t lastSend=0,lastLog=0,lastPrune=0;
CVector Limit(CVector v,float max){auto length=v.Length();return length>max?v*(max/length):v;}
using ContactFn=bool(__fastcall*)(CPhysical*,void*,CPhysical*,void*,float*,float*);
ContactFn originalHard=nullptr,originalSoft=nullptr;
bool ProcessContact(ContactFn original,CPhysical*a,void*edx,CPhysical*b,void*point,float*damageA,float*damageB){
    if(!Net().Online()||!core::IsPlayerReady()||!a||!b)return original(a,edx,b,point,damageA,damageB);
    if(TrainingIgnoresContact(a,b)){
        if(damageA)*damageA=0;
        if(damageB)*damageB=0;
        return false;
    }
    bool cargo=false;bool cargoResult=ProcessSharedCargoContact(original,a,edx,b,point,damageA,damageB,cargo);
    if(cargo)return cargoResult;
    if(!a->IsVehicle()||!b->IsVehicle())return original(a,edx,b,point,damageA,damageB);
    auto*na=Vehicles::Instance().Find((CVehicle*)a);auto*nb=Vehicles::Instance().Find((CVehicle*)b);
    if(!na||!nb||na->owner==nb->owner||(na->owner!=Net().MyId()&&nb->owner!=Net().MyId())||
       (a->PhysicalFlags()&0x2000u)||(b->PhysicalFlags()&0x2000u))return original(a,edx,b,point,damageA,damageB);
    CVector am=a->MoveSpeed(),at=a->TurnSpeed(),bm=b->MoveSpeed(),bt=b->TurnSpeed();
    bool result=original(a,edx,b,point,damageA,damageB);
    {
        auto*remote=na->owner==Net().MyId()?nb:na;auto*source=remote==na?nb:na;
        nativeContacts[((uint32_t)remote->netId<<16)|source->netId]=NowMs();
        auto*v=remote->veh;CVector move=(v->MoveSpeed()-(remote==na?am:bm))*50.f;
        CVector turn=(v->TurnSpeed()-(remote==na?at:bt))*50.f;
        if(move.Length()>0.0001f||turn.Length()>0.0001f){
            auto&c=pending[((uint32_t)source->netId<<16)|remote->netId];c.source=source->netId;c.target=remote->netId;c.owner=remote->owner;
            c.move=c.move+move;c.turn=c.turn+turn;
        }
    }
    return result;
}
bool __fastcall Hard(CPhysical*a,void*e,CPhysical*b,void*p,float*x,float*y){return ProcessContact(originalHard,a,e,b,p,x,y);}
bool __fastcall Soft(CPhysical*a,void*e,CPhysical*b,void*p,float*x,float*y){return ProcessContact(originalSoft,a,e,b,p,x,y);}
class VehicleContact:public Module {
public:
 const char*Name()const override{return "VehicleContact";}
 void OnInstallHooks()override{
    // HOODLUM's integrity code reads the original soft-collision prologue.
    // Redirect verified call sites, leaving both function entries intact.
    for(uintptr_t call:{0x54BDB2u,0x54BF78u,0x54C23Au,0x54C435u,0x54D17Eu,0x54D27Eu,0x54D3FEu,0x54D4D2u})
        if(mem::CallTarget(call)==0x548680)mem::RedirectCallFn(call,&Hard,&originalHard);
    for(uintptr_t call:{0x54BE1Cu,0x54C098u,0x54C35Au,0x54C54Du})
        if(mem::CallTarget(call)==0x54A2C0)mem::RedirectCallFn(call,&Soft,&originalSoft);
    Net().SetHandler(proto::G_VEH_CONTACT,[](uint8_t from,Reader&r){
        auto source=r.U16(),target=r.U16();auto serial=r.U32(),stamp=r.U32();auto m=r.VecQ(30.f),t=r.VecQ(15.f);
        if(!r.Ok()||!core::IsPlayerReady())return;
        auto*a=Vehicles::Instance().Find(source);auto*b=Vehicles::Instance().Find(target);
        if(!a||!b||a->owner!=from||b->owner!=Net().MyId()||from==b->owner||
           a->veh->AreaCode()!=b->veh->AreaCode()||(a->veh->GetPosition()-b->veh->GetPosition()).Length()>25||
           (b->veh->PhysicalFlags()&0x2000u)||TrainingIgnoresContact(a->veh,b->veh))return;
        if(a->hasState&&(int32_t)(a->lastTime-stamp)>500)return;
        uint32_t key=((uint32_t)source<<16)|target;
        // A vehicle retains its ID when another peer takes the wheel; that
        // peer's collision sequence is independent of the previous driver's.
        auto&last=received[((uint64_t)from<<32)|key];
        if(last&&(int32_t)(serial-last)<=0)return;last=serial;
        if(auto it=nativeContacts.find(key);it!=nativeContacts.end()&&NowMs()-it->second<120)return;
        b->veh->MoveSpeed()=b->veh->MoveSpeed()+Limit({m.x,m.y,m.z},20.f)*(1.f/50.f);
        b->veh->TurnSpeed()=b->veh->TurnSpeed()+Limit({t.x,t.y,t.z},12.f)*(1.f/50.f);
        if(b->veh->Status()==4)b->veh->SetStatus(3);
        b->lastChangeMs=NowMs();b->lastSendMs=0;
        if(NowMs()-lastLog>1000){lastLog=NowMs();LOGD("Vehicle contact applied: %04X -> %04X delta=%.3f m/s",source,target,CVector{m.x,m.y,m.z}.Length());}
    });
 }
 void OnPostProcess()override{
    if(!Net().Online()||!core::IsPlayerReady()||NowMs()-lastSend<33)return;lastSend=NowMs();
    for(auto&[key,c]:pending){auto*a=Vehicles::Instance().Find(c.source);auto*b=Vehicles::Instance().Find(c.target);
        if(!a||!b||a->owner!=Net().MyId()||b->owner!=c.owner||c.owner==Net().MyId()||TrainingIgnoresContact(a->veh,b->veh))continue;
        auto m=Limit(c.move,20.f),t=Limit(c.turn,12.f);Writer w(proto::G_VEH_CONTACT);w.Entity(ENTITY_VEHICLE,c.source);w.Entity(ENTITY_VEHICLE,c.target);w.U32(++sequence);w.U32((uint32_t)NowMs());w.VecQ({m.x,m.y,m.z},30.f);w.VecQ({t.x,t.y,t.z},15.f);Net().Send(proto::CH_RELIABLE,w);
    }pending.clear();
    if(NowMs()-lastPrune>5000){lastPrune=NowMs();for(auto it=received.begin();it!=received.end();)it=(!Vehicles::Instance().Find((uint16_t)(it->first>>16))||!Vehicles::Instance().Find((uint16_t)it->first))?received.erase(it):std::next(it);for(auto it=nativeContacts.begin();it!=nativeContacts.end();)it=NowMs()-it->second>10000?nativeContacts.erase(it):std::next(it);}
 }
 void OnGameShutdown()override{pending.clear();received.clear();nativeContacts.clear();}
 void OnSessionEnd()override{OnGameShutdown();lastSend=0;}
}feature;
struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}}
