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
#include "shared/api/story-actions/StoryActions.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-stage/StoryStage.h"
#include "shared/lib/memory/Memory.h"
#include "shared/api/mission-objects/SharedObject.h"
#include "shared/game/Script.h"
namespace coop { namespace {
using namespace game;
using game::script::Cmd;
struct Contact {
    uint16_t source=0,target=0;uint8_t owner=255;
    uint64_t sourceGeneration=0,targetGeneration=0;
    CVector move,turn,damageOffset,damageNormal;
    float damage=0;uint8_t damagePiece=0;
};
std::map<uint32_t,Contact> pending;
std::map<uint64_t,uint32_t> received;
std::map<uint32_t,uint64_t> nativeContacts;
uint32_t sequence=0;uint64_t lastSend=0,lastLog=0,lastPrune=0;
struct DamageSource {
    uint64_t generation=0;
    uint64_t acquiredAt=0,handoverUntil=0;
    int handle=-1;
    CVehicle*vehicle=nullptr;
    CEntity*source=nullptr;
};
std::map<uint16_t,DamageSource> damageSources;
uint64_t damageRun=0;
uint32_t damagePhase=0;
// CVehicle::m_pLastDamageEntity is persistent, unlike CPhysical's contact
// entity at 0xDC which is cleared every physics step. The native 051D
// predicate uses this exact source, not proximity or a loss of health alone.
CEntity* LastDamageSource(CVehicle*vehicle){return Field<CEntity*>(vehicle,0x50C);}
void ObserveDamageSource(CVehicle*vehicle,bool freshImpact=false){
    if(!Net().Online()||!core::IsPlayerReady()||!StoryRunId()||!IsVehicleValid(vehicle))return;
    auto*n=Vehicles::Instance().Find(vehicle);
    if(!n||!Vehicles::Alive(*n)||n->owner!=Net().MyId())return;
    auto generation=Net().EntityGeneration(ENTITY_VEHICLE,n->netId);
    if(!generation||!Net().EntityLive(ENTITY_VEHICLE,n->netId))return;
    auto*source=LastDamageSource(vehicle);
    auto i=damageSources.find(n->netId);
    bool baseline=i==damageSources.end()||i->second.generation!=generation||
        i->second.handle!=n->handle||i->second.vehicle!=vehicle||
        i->second.acquiredAt!=n->firstAnnouncedMs||i->second.handoverUntil!=n->handoverUntil;
    bool changed=!baseline&&i->second.source!=source;
    damageSources[n->netId]={generation,n->firstAnnouncedMs,n->handoverUntil,n->handle,vehicle,source};
    // A new owner usually receives a replica with a NULL last-damager field.
    // That absence must not erase a receipt accepted from the previous owner.
    if(!changed&&!freshImpact)return;
    auto*car=(CVehicle*)source;
    if(source&&IsVehicleValid(car)&&car->IsVehicle()&&car!=vehicle){
        int sourceHandle=GetVehicleRef(car);
        auto*sourceRecord=Vehicles::Instance().Find(car);
        if(sourceRecord&&Vehicles::Alive(*sourceRecord)&&Net().EntityLive(ENTITY_VEHICLE,sourceRecord->netId)&&
           sourceHandle>=0&&Cmd(0x051D).I(n->handle).I(sourceHandle).Run()){
            RecordStoryVehicleDamage(vehicle,car);
            return;
        }
    }
    // A subsequent NPC/ped hit, destruction of the source, or a native clear
    // replaces the one last-damager fact; old guest impacts are not a history.
    RecordStoryVehicleDamage(vehicle,nullptr);
}
using RegisterReferenceFn=void(__fastcall*)(CEntity*,void*,CEntity**);
RegisterReferenceFn originalRegisterReference=nullptr;
void __fastcall RegisterReference(CEntity*source,void*edx,CEntity**destination){
    originalRegisterReference(source,edx,destination);
    if(!destination||!Net().Online()||!core::IsPlayerReady()||!StoryRunId())return;
    // Native vehicle damage registers its permanent last-damager slot after
    // accepting the hit. Match that exact slot in a live, aligned vehicle-pool
    // entry; unrelated entity references cannot produce a damage receipt.
    auto*vehicle=(CVehicle*)((uintptr_t)destination-0x50C);
    if(IsVehicleValid(vehicle)&&LastDamageSource(vehicle)==source)
        ObserveDamageSource(vehicle,true);
}
void UpdateDamageSources(){
    auto run=StoryRunId();auto phase=CooperativeStoryPhase();
    if(!Net().Online()||!core::IsPlayerReady()||!run){damageSources.clear();damageRun=run;damagePhase=phase;return;}
    if(run!=damageRun||phase!=damagePhase){damageSources.clear();damageRun=run;damagePhase=phase;}
    for(const auto&[id,n]:Vehicles::Instance().All())
        if(n.owner==Net().MyId()&&Vehicles::Alive(n))ObserveDamageSource(n.veh);
    for(auto i=damageSources.begin();i!=damageSources.end();){
        auto*n=Vehicles::Instance().Find(i->first);
        i=(!n||n->owner!=Net().MyId()||!Vehicles::Alive(*n)||
            Net().EntityGeneration(ENTITY_VEHICLE,i->first)!=i->second.generation)?damageSources.erase(i):std::next(i);
    }
}
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
        if(result)nativeContacts[((uint32_t)remote->netId<<16)|source->netId]=NowMs();
        auto*v=remote->veh;CVector move=(v->MoveSpeed()-(remote==na?am:bm))*50.f;
        CVector turn=(v->TurnSpeed()-(remote==na?at:bt))*50.f;
        // The caller records these exact hard-collision outputs with
        // SetDamagedPieceRecord. A mirror is collision-proof here, so only
        // its owner can feed that native witness into vehicle damage.
        float damage=original==originalHard&&result&&point?(remote==na?damageA?*damageA:0:damageB?*damageB:0):0;
        if(!std::isfinite(damage)||damage<0)damage=0;
        if(move.Length()>0.0001f||turn.Length()>0.0001f||damage>0){
            auto sourceGeneration=Net().EntityGeneration(ENTITY_VEHICLE,source->netId),targetGeneration=Net().EntityGeneration(ENTITY_VEHICLE,remote->netId);
            auto&c=pending[((uint32_t)source->netId<<16)|remote->netId];
            if(c.sourceGeneration!=sourceGeneration||c.targetGeneration!=targetGeneration||c.owner!=remote->owner)c={};
            c.source=source->netId;c.target=remote->netId;c.owner=remote->owner;
            c.sourceGeneration=sourceGeneration;c.targetGeneration=targetGeneration;
            c.move=c.move+move;c.turn=c.turn+turn;
            if(damage>c.damage){
                c.damage=damage;c.damageOffset=Field<CVector>(point,0)-v->GetPosition();
                c.damageNormal=Field<CVector>(point,0x10)*(remote==na?1.f:-1.f);
                c.damagePiece=Field<uint8_t>(point,0x21);
            }
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
    mem::HookFn(0x571B70,&RegisterReference,&originalRegisterReference,"Vehicle last-damager native reference");
    // HOODLUM's integrity code reads the original soft-collision prologue.
    // Redirect verified call sites, leaving both function entries intact.
    for(uintptr_t call:{0x54BDB2u,0x54BF78u,0x54C23Au,0x54C435u,0x54D17Eu,0x54D27Eu,0x54D3FEu,0x54D4D2u})
        if(mem::CallTarget(call)==0x548680)mem::RedirectCallFn(call,&Hard,&originalHard);
    for(uintptr_t call:{0x54BE1Cu,0x54C098u,0x54C35Au,0x54C54Du})
        if(mem::CallTarget(call)==0x54A2C0)mem::RedirectCallFn(call,&Soft,&originalSoft);
    Net().SetHandler(proto::G_VEH_CONTACT,[](uint8_t from,Reader&r){
        auto source=r.U16(),target=r.U16();auto serial=r.U32(),stamp=r.U32();auto m=r.VecQ(30.f),t=r.VecQ(15.f);
        float damage=r.F32();auto offset=r.Vec(),normal=r.Vec();auto piece=r.U8();
        if(!r.Ok()||r.Remaining()||!core::IsPlayerReady()||!Net().EntityContextValid(Net().EntityContext())||
           damage<0||CVector{offset.x,offset.y,offset.z}.Length()>20.f||
           (damage>0&&(CVector{normal.x,normal.y,normal.z}.Length()<.9f||CVector{normal.x,normal.y,normal.z}.Length()>1.1f)))return;
        auto*a=Vehicles::Instance().Find(source);auto*b=Vehicles::Instance().Find(target);
        bool sourceRef=false,targetRef=false;
        for(auto&ref:Net().EntityContext())if(ref.kind==ENTITY_VEHICLE&&ref.flags==ENTITY_USE){sourceRef|=ref.id==source;targetRef|=ref.id==target;}
        if(!sourceRef||!targetRef||!a||!b||!Vehicles::Alive(*a)||!Vehicles::Alive(*b)||a->owner!=from||b->owner!=Net().MyId()||from==b->owner||
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
        if(damage>0){
            // Preserve the native maximum-force record; the next stock
            // VehicleDamage pass checks proofs and the model's threshold.
            // No health or persistent last-damager field is written here.
            alignas(4)uint8_t point[0x2C]{};
            Field<CVector>(point,0)=b->veh->GetPosition()+CVector{offset.x,offset.y,offset.z};
            Field<CVector>(point,0x10)={normal.x,normal.y,normal.z};Field<uint8_t>(point,0x21)=piece;
            CallMethod<0x5428C0>(b->veh,damage,(CEntity*)a->veh,point,1.f);
            LOGD("Vehicle native collision witness: %04X -> %04X force=%.3f piece=%u",source,target,damage,piece);
        }
        if(b->veh->Status()==4)b->veh->SetStatus(3);
        b->lastChangeMs=NowMs();b->lastSendMs=0;
        if(NowMs()-lastLog>1000){lastLog=NowMs();LOGD("Vehicle contact applied: %04X -> %04X delta=%.3f m/s",source,target,CVector{m.x,m.y,m.z}.Length());}
    });
 }
 void OnPostProcess()override{
    UpdateDamageSources();
    if(!Net().Online()||!core::IsPlayerReady()||NowMs()-lastSend<33)return;lastSend=NowMs();
    for(auto&[key,c]:pending){auto*a=Vehicles::Instance().Find(c.source);auto*b=Vehicles::Instance().Find(c.target);
        if(!a||!b||a->owner!=Net().MyId()||b->owner!=c.owner||c.owner==Net().MyId()||
           Net().EntityGeneration(ENTITY_VEHICLE,c.source)!=c.sourceGeneration||Net().EntityGeneration(ENTITY_VEHICLE,c.target)!=c.targetGeneration||
           TrainingIgnoresContact(a->veh,b->veh))continue;
        auto m=Limit(c.move,20.f),t=Limit(c.turn,12.f);Writer w(proto::G_VEH_CONTACT);
        w.Entity(ENTITY_VEHICLE,c.source,ENTITY_USE,255,c.sourceGeneration);w.Entity(ENTITY_VEHICLE,c.target,ENTITY_USE,255,c.targetGeneration);
        w.U32(++sequence);w.U32((uint32_t)NowMs());w.VecQ({m.x,m.y,m.z},30.f);w.VecQ({t.x,t.y,t.z},15.f);
        w.F32(c.damage);w.Vec({c.damageOffset.x,c.damageOffset.y,c.damageOffset.z});w.Vec({c.damageNormal.x,c.damageNormal.y,c.damageNormal.z});w.U8(c.damagePiece);
        Net().Send(proto::CH_RELIABLE,w);
    }pending.clear();
    if(NowMs()-lastPrune>5000){lastPrune=NowMs();for(auto it=received.begin();it!=received.end();)it=(!Vehicles::Instance().Find((uint16_t)(it->first>>16))||!Vehicles::Instance().Find((uint16_t)it->first))?received.erase(it):std::next(it);for(auto it=nativeContacts.begin();it!=nativeContacts.end();)it=NowMs()-it->second>10000?nativeContacts.erase(it):std::next(it);}
 }
 void OnGameShutdown()override{pending.clear();received.clear();nativeContacts.clear();damageSources.clear();damageRun=0;damagePhase=0;}
 void OnSessionEnd()override{OnGameShutdown();lastSend=0;}
}feature;
struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}}
