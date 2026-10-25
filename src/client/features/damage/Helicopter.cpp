#include <array>
#include <deque>
#include <map>
#include <tuple>
#include "entities/vehicle/Vehicles.h"
#include "entities/player/Players.h"
#include "entities/ped/Peds.h"
#include "shared/api/net-session/Session.h"
#include "shared/lib/sync/EventWindow.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
namespace coop {namespace {
using namespace game;
CVehicle*processing=nullptr;uint32_t sequence=0;EventWindow seen[3];
struct Shot{
    uint8_t from=0,kind=0,piece=0,surface=0,targetOwner=255;
    uint16_t id=0,target=0;uint32_t seq=0,repair=0,incarnation=0;uint64_t generation=0;
    CVector start,end,normal;int damage=0;int8_t area=0;
    bool visual=false,authorised=false;uint64_t received=0;std::vector<EntityRef>refs;
};
std::map<std::tuple<uint16_t,uint64_t,uint8_t>,uint64_t>knownSources;
void RememberSources(){auto now=NowMs();for(auto&[id,n]:Vehicles::Instance().All())if(Vehicles::Alive(n)&&n.veh->VehicleSubClass()==VEHICLE_TYPE_HELI)knownSources[{id,Net().EntityGeneration(ENTITY_VEHICLE,id),n.owner}]=now;
    for(auto i=knownSources.begin();i!=knownSources.end();)i=now-i->second>5000?knownSources.erase(i):std::next(i);
}
Shot*capturing=nullptr;CEntity*hitEntity=nullptr;std::deque<Shot>pending;
using ProcessFn=void(__fastcall*)(CVehicle*,void*);ProcessFn originalProcess=nullptr;
void __fastcall Process(CVehicle*v,void*edx){auto*previous=processing;processing=v;originalProcess(v,edx);processing=previous;}
using RoundFn=void(__cdecl*)(const CVector*,const CVector*,int);RoundFn originalRound=nullptr;
using AudioFn=void(__fastcall*)(void*,void*,int,int,CEntity*);AudioFn originalAudio=nullptr;
void __fastcall Audio(void*self,void*edx,int event,int weapon,CEntity*entity){
    auto*n=processing?Vehicles::Instance().Find(processing):nullptr;
    if(Net().Online()&&n&&Vehicles::Instance().IsMirror(*n))return;
    originalAudio(self,edx,event,weapon,entity);
}
using RayFn=bool(__cdecl*)(const CVector*,const CVector*,void*,CEntity**,bool,bool,bool,bool,bool,bool,bool,bool);
RayFn originalRay=nullptr;
bool __cdecl Ray(const CVector*a,const CVector*b,void*col,CEntity**entity,bool buildings,bool vehicles,bool peds,bool objects,bool dummies,bool see,bool camera,bool shoot){
    bool result=originalRay(a,b,col,entity,buildings,vehicles,peds,objects,dummies,see,camera,shoot);
    if(capturing&&entity&&*entity&&col){hitEntity=*entity;capturing->end=Field<CVector>(col,0);capturing->normal=Field<CVector>(col,0x10);capturing->piece=Field<uint8_t>(col,0x24);capturing->surface=Field<uint8_t>(col,0x23);}
    return result;
}
CEntity*Target(const Shot&s){
    if(s.kind==1){if(s.target==Net().MyId())return FindPlayerPed(0);auto*p=Players::Instance().Get((uint8_t)s.target);return p?p->ped:nullptr;}
    if(s.kind==2)return GetPed(Peds::Instance().HandleOf(s.target));
    if(s.kind==3){auto*v=Vehicles::Instance().Find(s.target);return v&&Vehicles::Alive(*v)?v->veh:nullptr;}
    return nullptr;
}
bool Replay(Shot&s){
    auto*n=Vehicles::Instance().Find(s.id);
    bool live=n&&Vehicles::Alive(*n)&&Net().EntityGeneration(ENTITY_VEHICLE,s.id)==s.generation;
    if(!s.authorised){
        if(live&&n->owner==s.from&&n->veh->VehicleSubClass()==VEHICLE_TYPE_HELI)s.authorised=true;
        else if(knownSources.count({s.id,s.generation,s.from}))s.authorised=true;
        else return false;
    }
    if(!s.visual){
        if(s.area==13||s.area==FindPlayerPed(0)->AreaCode()){
            Call<0x723750>(&s.start,&s.end,.02f,750u,150u);
            if(s.normal.Length()>.1f)CallMethod<0x49F3D0>((void*)0xA9AE00,&s.end,&s.normal,(int)s.surface,8,1.f);
            if(live)CallMethod<0x506F40>((void*)0xB6BC90,0x91,0x1F,n->veh);
        }
        s.visual=true;
    }
    if(!s.kind)return true;
    auto*target=Target(s);if(!target)return false;
    if(s.kind==1){auto*peer=Net().Peer((uint8_t)s.target);if(s.target!=Net().MyId()||!peer||peer->incarnation!=s.incarnation)return true;}
    if(s.kind==2){auto*p=Peds::Instance().Find((CPed*)target);if(!p||p->owner!=Net().MyId())return true;}
    if(s.kind==3){auto*v=Vehicles::Instance().Find((CVehicle*)target);if(!v||Vehicles::Instance().IsMirror(*v))return true;if(v->repairRevision!=s.repair)return true;}
    if(target->AreaCode()!=s.area&&target->AreaCode()!=13&&s.area!=13)return true;
    if(target->IsPed()){
        auto*p=(CPed*)target;if(p->Health()<=0)return true;
        int dir=0;auto d=s.start-p->GetPosition();if(p->Matrix()){float side=d.Dot(p->Matrix()->right),front=d.Dot(p->Matrix()->forward);dir=std::fabs(front)>std::fabs(side)?(front>=0?0:2):(side>=0?3:1);}
        Call<0x73A530,bool>(p,(CEntity*)nullptr,52,s.damage,(int)s.piece,dir);
    }else CallMethod<0x6D7C90>((CVehicle*)target,(CEntity*)nullptr,28,(float)s.damage,CVector{});
    LOGD("Helicopter resolved hit applied source=%u seq=%u target=%u:%04X damage=%d",s.from,s.seq,s.kind,s.target,s.damage);
    return true;
}
void __cdecl Round(const CVector*start,const CVector*end,int damage){
    auto*n=processing?Vehicles::Instance().Find(processing):nullptr;
    if(Net().Online()&&n&&Vehicles::Instance().IsMirror(*n))return;
    if(!Net().Online()||!core::IsPlayerReady()||!processing||!start||!end){originalRound(start,end,damage);return;}
    if(!n)n=Vehicles::Instance().EnsureNetworked(processing);
    if(!n){originalRound(start,end,damage);return;}
    Shot shot;shot.id=n->netId;shot.start=*start;shot.end=*end;shot.damage=std::clamp(damage,0,255);shot.area=processing->AreaCode();
    auto*previous=capturing;auto*previousHit=hitEntity;capturing=&shot;hitEntity=nullptr;
    originalRound(start,end,damage);auto*hit=hitEntity;capturing=previous;hitEntity=previousHit;
    if(hit&&hit->IsPed()){
        if(hit==FindPlayerPed(0)){shot.kind=1;shot.target=Net().MyId();shot.targetOwner=Net().MyId();}
        else if(auto*rp=Players::Instance().FindByPed((CPed*)hit)){shot.kind=1;shot.target=rp->id;shot.targetOwner=rp->id;}
        else if(auto*p=Peds::Instance().EnsureNetworked((CPed*)hit)){shot.kind=2;shot.target=p->netId;shot.targetOwner=p->owner;}
    }else if(hit&&hit->IsVehicle())if(auto*v=Vehicles::Instance().EnsureNetworked((CVehicle*)hit)){shot.kind=3;shot.target=v->netId;shot.targetOwner=v->owner;shot.repair=v->repairRevision;}
    Writer w(proto::G_HELI_SHOT);w.Entity(ENTITY_VEHICLE,shot.id,ENTITY_OPTIONAL);if(++sequence==0)++sequence;w.U32(sequence);
    w.Vec({shot.start.x,shot.start.y,shot.start.z});w.Vec({shot.end.x,shot.end.y,shot.end.z});w.Vec({shot.normal.x,shot.normal.y,shot.normal.z});
    w.U8((uint8_t)shot.damage);w.I8(shot.area);w.U8(shot.kind);w.U8(shot.targetOwner);
    if(shot.kind==2)w.Entity(ENTITY_PED,shot.target);else if(shot.kind==3)w.Entity(ENTITY_VEHICLE,shot.target);else w.U16(shot.target);
    auto*targetPeer=shot.kind==1?Net().Peer((uint8_t)shot.target):nullptr;w.U32(targetPeer?targetPeer->incarnation:0);w.U32(shot.repair);w.U8(shot.piece);w.U8(shot.surface);Net().Send(proto::CH_RELIABLE,w);
    LOGD("Helicopter resolved round sent vehicle=%04X seq=%u target=%u:%04X",shot.id,sequence,shot.kind,shot.target);
}
class Helicopter:public Module{
    const char*Name()const override{return "HelicopterRounds";}
    void OnInstallHooks()override{
        mem::HookFn(0x6C7050,&Process,&originalProcess,"CHeli::ProcessControl gun authority");
        mem::RedirectCallFn(0x6C7773,&Round,&originalRound);mem::RedirectCallFn(0x6C7788,&Audio,&originalAudio);
        mem::RedirectCallFn(0x73AF9F,&Ray,&originalRay);
        Net().SetHandler(proto::G_HELI_SHOT,[](uint8_t from,Reader&r){
            Shot s;s.from=from;s.id=r.U16();s.seq=r.U32();auto a=r.Vec(),b=r.Vec(),normal=r.Vec();s.start={a.x,a.y,a.z};s.end={b.x,b.y,b.z};s.normal={normal.x,normal.y,normal.z};s.damage=r.U8();s.area=r.I8();s.kind=r.U8();s.targetOwner=r.U8();s.target=r.U16();s.incarnation=r.U32();s.repair=r.U32();s.piece=r.U8();s.surface=r.U8();
            if(!r.Ok()||from>=3||!core::IsPlayerReady()||s.kind>3||(s.kind==1&&s.target>=3)||!(s.start.Length()<25000)||!(s.end.Length()<25000)||!((s.end-s.start).Length()<300)||!(s.normal.Length()<2)||!seen[from].Accept(s.seq))return;
            s.received=NowMs();s.generation=Net().ExpectedEntityGeneration(ENTITY_VEHICLE,s.id);s.refs=Net().EntityContext();
            s.refs.erase(std::remove_if(s.refs.begin(),s.refs.end(),[](const EntityRef&ref){return ref.flags&ENTITY_OPTIONAL;}),s.refs.end());
            // The source already applied the native hit to entities it owns.
            if(s.targetOwner==from)s.kind=0;
            if(!Replay(s)){if(pending.size()>=256){LOGW("Helicopter hit queue full seq=%u",s.seq);return;}pending.push_back(std::move(s));}
        });
    }
    void OnPostProcess()override{if(!core::IsPlayerReady())return;RememberSources();for(auto i=pending.begin();i!=pending.end();){
        if(!Net().EntityContextValid(i->refs)){i=pending.erase(i);continue;}
        if(Replay(*i)){i=pending.erase(i);continue;}
        if(NowMs()-i->received>5000){LOGW("Helicopter hit dependency expired source=%u seq=%u target=%u:%04X",i->from,i->seq,i->kind,i->target);i=pending.erase(i);}else ++i;
    }}
    void OnPeerLeave(uint8_t id)override{if(id<3)seen[id]={};for(auto i=pending.begin();i!=pending.end();)i=i->from==id?pending.erase(i):std::next(i);}
    void OnGameShutdown()override{pending.clear();knownSources.clear();processing=nullptr;capturing=nullptr;for(auto&w:seen)w={};}
    void OnSessionEnd()override{OnGameShutdown();sequence=0;}
}feature;struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}}
