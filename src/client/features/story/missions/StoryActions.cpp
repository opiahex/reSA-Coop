#include <map>
#include "shared/lib/sync/StoryContinuation.h"
#include <set>
#include <tuple>
#include <deque>
#include "shared/api/story-actions/StoryActions.h"
#include "shared/api/story-stage/StoryStage.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/lib/sync/StoryCoopPolicy.h"
#include "shared/api/mission-objects/SharedObject.h"
#include "common/api/protocol/Protocol.h"
#include "common/lib/util/Util.h"
#include "common/lib/log/Log.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/ped/Peds.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/sync/EventWindow.h"
#include "shared/game/Script.h"
using namespace game;using game::script::Cmd;
namespace coop {namespace {
struct StoryParticipant{uint8_t player=255;uint16_t vehicle=0;uint64_t generation=0,at=0;void*base=nullptr;uint64_t name=0;uint8_t reason=0;uint64_t run=0;uint16_t object=0;uint64_t objectGeneration=0;uint32_t phase=0;};
std::map<void*,StoryParticipant> participants;
struct Proof{int handle=-1;uint16_t model=0;uint64_t at=0,generation=0;int weapon=54;uint8_t player=255;uint64_t run=0;uint32_t phase=0;};
std::map<uint32_t,Proof> photos;std::map<uint16_t,Proof> guestDamage;
EventWindow seen[3];uint32_t sequence=0;uint64_t photoTaken=0;bool wasMission=false;uint64_t actionRun=0;uint32_t proofPhase=0,photoPhase=0;
uint64_t photoViewAt=0;float photoViewFov=70;uint8_t photoViewPlayer=255;
std::map<uint32_t,Proof> entityDamage;
std::map<int,int> guestKills;
std::set<std::tuple<uint16_t,uint64_t>> killed;
struct StoryHit {uint8_t from=0,player=0;uint16_t id=0,model=0;uint64_t birth=0,received=0;bool lethal=false;std::vector<EntityRef> refs;int weapon=54;uint64_t run=0;uint32_t phase=0;};
std::deque<StoryHit> pendingHits;
// Native touching is transient. Preserve its exact identities long enough for
// the host's next SCM pass, without turning a remote impact into generic damage.
struct VehicleObjectContact {
    uint16_t vehicle=0,object=0,vehicleModel=0,objectModel=0;
    uint64_t vehicleGeneration=0,objectGeneration=0,at=0,run=0;
    uint32_t phase=0,stamp=0;uint8_t player=255;float speed=0;
    int vehicleHandle=-1,objectHandle=-1;
};
struct ContactTargets {
    void*base=nullptr;uint64_t name=0,run=0;uint32_t phase=0;
    std::map<uint16_t,uint64_t> objects;
};
std::map<uint32_t,VehicleObjectContact> vehicleContacts,pendingContacts;
std::map<void*,ContactTargets> contactTargets;
std::map<uint32_t,uint64_t> contactSent;
uint32_t ContactKey(uint16_t car,uint16_t object){return (uint32_t(car)<<16)|object;}
bool LiveContact(const VehicleObjectContact&c){
    if(!c.at||NowMs()<c.at||NowMs()-c.at>750||c.run!=StoryRunId()||c.phase!=CooperativeStoryPhase()||c.player>=3)return false;
    auto*n=Vehicles::Instance().Find(c.vehicle);auto*o=SharedMissionObjectById(c.object);auto*p=Players::Instance().Get(c.player);
    if(!n||!Vehicles::Alive(*n)||n->owner!=c.player||n->handle!=c.vehicleHandle||n->model!=c.vehicleModel||
       !o||!IsObjectValid(o)||GetObjectRef(o)!=c.objectHandle||o->ModelIndex()!=c.objectModel||
       !Net().EntityLive(ENTITY_VEHICLE,c.vehicle)||!Net().EntityLive(ENTITY_OBJECT,c.object)||
       Net().EntityGeneration(ENTITY_VEHICLE,c.vehicle)!=c.vehicleGeneration||Net().EntityGeneration(ENTITY_OBJECT,c.object)!=c.objectGeneration)return false;
    // Sender time is compared only with that sender's own state clock. This
    // also rejects contacts held in a dependency queue behind a missing car.
    return p&&p->ped&&p->hasState&&NowMs()-p->stateRecvMs<1500&&p->ped->Health()>0&&
        p->mode==RemotePlayer::Mode::Driver&&p->vehNetId==c.vehicle&&(p->state.area==o->AreaCode()||o->AreaCode()==13)&&
        (int32_t)(p->lastStateTime-c.stamp)<=750&&(int32_t)(c.stamp-p->lastStateTime)<=1500;
}
ContactTargets* ScriptContactTargets(void*script,bool create=false){
    if(!script)return nullptr;auto i=contactTargets.find(script);
    if(i!=contactTargets.end()&&(i->second.base!=Field<void*>(script,0x10)||i->second.name!=Field<uint64_t>(script,8)||
        i->second.run!=StoryRunId()||i->second.phase!=CooperativeStoryPhase())){contactTargets.erase(i);i=contactTargets.end();}
    if(i==contactTargets.end()&&create){
        if(contactTargets.size()>=64)return nullptr;
        i=contactTargets.emplace(script,ContactTargets{Field<void*>(script,0x10),Field<uint64_t>(script,8),StoryRunId(),CooperativeStoryPhase()}).first;
    }
    return i==contactTargets.end()?nullptr:&i->second;
}
const VehicleObjectContact* BoundContact(void*script){
    auto b=participants.find(script);if(b==participants.end()||b->second.reason!=6)return nullptr;auto&actor=b->second;
    if(actor.run!=StoryRunId()||actor.phase!=CooperativeStoryPhase()||actor.base!=Field<void*>(script,0x10)||actor.name!=Field<uint64_t>(script,8))return nullptr;
    auto i=vehicleContacts.find(ContactKey(actor.vehicle,actor.object));
    return i!=vehicleContacts.end()&&i->second.vehicleGeneration==actor.generation&&i->second.objectGeneration==actor.objectGeneration&&LiveContact(i->second)?&i->second:nullptr;
}
bool BindContactParticipant(void*script,CPed*me){
    auto*targets=ScriptContactTargets(script);if(!targets)return false;
    const VehicleObjectContact*latest=nullptr;
    for(auto&[key,c]:vehicleContacts){
        auto t=targets->objects.find(c.object);if(t==targets->objects.end()||t->second!=c.objectGeneration||!LiveContact(c))continue;
        // Native CJ contact takes precedence when he is actually touching the
        // same target; simply sitting in a different car must not steal it.
        auto*own=me->InVehicle()?me->Vehicle():nullptr;
        if(own&&IsVehicleValid(own)&&Cmd(0x0897).I(GetVehicleRef(own)).I(c.objectHandle).Run())continue;
        if(!latest||c.at>latest->at)latest=&c;
    }
    if(!latest)return false;
    auto&c=*latest;
    participants[script]={c.player,c.vehicle,c.vehicleGeneration,c.at,Field<void*>(script,0x10),Field<uint64_t>(script,8),6,c.run,c.object,c.objectGeneration,c.phase};
    return true;
}
void ClearContacts(){vehicleContacts.clear();pendingContacts.clear();contactTargets.clear();contactSent.clear();}
bool AcceptStoryHit(const StoryHit&h){
    if(h.run!=StoryRunId()||h.phase!=CooperativeStoryPhase())return true;
    if(!h.refs.empty()&&!Net().EntityContextValid(h.refs))return true;
    int handle=Peds::Instance().HandleOf(h.id);auto*ped=GetPed(handle);auto*n=ped?Peds::Instance().Find(ped):nullptr;
    if(!n||n->owner!=h.from)return false;
    if(n->model!=h.model||h.player>=3)return true;
    guestDamage[h.id]={handle,h.model,NowMs(),Net().EntityGeneration(ENTITY_PED,h.id),h.weapon,h.player,h.run,h.phase};
    RecordStoryAggressor(ped,h.player);
    if(h.lethal&&h.player!=Net().MyId()&&killed.insert({h.id,n->firstAnnouncedMs}).second){
        guestKills[h.model]=std::min(1000000,guestKills[h.model]+1);
        LOGI("Cooperative story kill: player=%u ped=%04X model=%u count=%d",h.player,h.id,h.model,guestKills[h.model]);
    }
    return true;
}
void ReportStoryHit(CPed*victim,uint8_t player,bool lethal,int weapon=54){
    if(!Net().Online()||!core::IsPlayerReady()||player>=3||!victim||!IsPedValid(victim))return;
    auto*n=Peds::Instance().Find(victim);if(!n||n->owner!=Net().MyId())return;
    if(Net().IsHost()){
        if(HostReplicating()){StoryHit h{Net().MyId(),player,n->netId,n->model,n->firstAnnouncedMs,NowMs(),lethal};h.weapon=weapon;h.run=StoryRunId();h.phase=CooperativeStoryPhase();AcceptStoryHit(h);}
    }else if(HostMissionRecent(0)){
        Writer w(proto::G_STORY_ACTION);w.U64(StoryRunId());w.U32(CooperativeStoryPhase());if(++sequence==0)++sequence;w.U32(sequence);w.U8(lethal?2:3);
        w.Entity(ENTITY_PED,n->netId);w.U16(n->model);w.U8(player);w.U64(n->firstAnnouncedMs);w.U8((uint8_t)weapon);w.U64(StoryRunId());Net().Send(proto::CH_RELIABLE,w);
    }
}
using PhotoFn=bool(__fastcall*)(CWeapon*,void*,CEntity*,const CVector*);PhotoFn originalPhoto=nullptr;
bool __fastcall Photo(CWeapon*w,void*edx,CEntity*owner,const CVector*point){
    struct Target{uint8_t kind;uint16_t id,model;int handle;};std::vector<Target> targets;
    bool share=Net().Online()&&!Net().IsHost()&&core::IsPlayerReady()&&HostMissionRecent(0)&&owner==FindPlayerPed(0);
    if(share){
        for(auto&[id,n]:Peds::Instance().All())if(n.ped&&GetPed(n.handle)==n.ped)targets.push_back({1,id,n.model,n.handle});
        std::vector<SyncAuditEntry> audit;core::CollectSyncAudit(audit);
        for(auto&e:audit)if(e.kind==AUDIT_OBJECT)if(auto*o=SharedMissionObjectById(e.id)){int h=GetObjectRef(o);targets.push_back({2,e.id,o->ModelIndex(),h});}
    }
    // 04C5/0833 consume their flags. A guest's photograph is an event of
    // this exposure, so clear old replica flags first and consume only after
    // the native camera has marked the visible targets of this exposure.
    if(share)for(auto&t:targets)Cmd(t.kind==1?0x04C5:0x0833).I(t.handle).Run();
    bool ok=originalPhoto(w,edx,owner,point);
    if(ok&&Net().IsHost()&&owner==FindPlayerPed(0)){photoViewAt=NowMs();photoViewPlayer=Net().MyId();photoViewFov=cam::Fov();}
    if(ok&&share){
        Writer packet(proto::G_STORY_ACTION);packet.U64(StoryRunId());packet.U32(CooperativeStoryPhase());if(++sequence==0)++sequence;packet.U32(sequence);packet.U8(1);
        std::vector<Target> hit;for(auto&t:targets)if(Cmd(t.kind==1?0x04C5:0x0833).I(t.handle).Run()&&hit.size()<64)hit.push_back(t);
        packet.U8((uint8_t)hit.size());packet.U64(StoryRunId());packet.F32(cam::Fov());for(auto&t:hit){packet.U8(t.kind);packet.Entity(t.kind==1?ENTITY_PED:ENTITY_OBJECT,t.id);packet.U16(t.model);}Net().Send(proto::CH_RELIABLE,packet);
        LOGI("Story photo sent token=%u targets=%u",sequence,(unsigned)hit.size());
    }
    return ok;
}
class StoryActions:public Module{
 const char*Name()const override{return "StoryActions";}
 void OnInstallHooks()override{
    mem::HookFn(0x73C1F0,&Photo,&originalPhoto,"CWeapon::TakePhotograph");
    Net().SetHandler(proto::G_STORY_ACTION,[](uint8_t from,Reader&r){
        auto packetRun=r.U64();auto packetPhase=r.U32();
        if(!r.Ok()||packetRun!=StoryRunId()||packetPhase!=CooperativeStoryPhase()){LOGD("Obsolete story action ignored from=%u",from);return;}
        uint32_t token=r.U32();uint8_t action=r.U8();
        if(action==2||action==3){
            StoryHit h;h.from=from;h.lethal=action==2;h.id=r.U16();h.model=r.U16();h.player=r.U8();h.birth=r.U64();h.received=NowMs();h.refs=Net().EntityContext();h.weapon=r.U8();h.run=r.U64();h.phase=packetPhase;
            if(!r.Ok()||from>=3||!Net().IsHost()||!HostReplicating()||!core::IsPlayerReady()||h.player>=3||!h.birth||!seen[from].Accept(token))return;
            if(!AcceptStoryHit(h)){if(pendingHits.size()>=64)pendingHits.pop_front();pendingHits.push_back(h);}return;
        }
        if(action==5){
            VehicleObjectContact c;c.vehicle=r.U16();c.object=r.U16();c.vehicleModel=r.U16();c.objectModel=r.U16();c.speed=r.F32();c.stamp=r.U32();
            c.player=from;c.at=NowMs();c.run=packetRun;c.phase=packetPhase;
            if(!r.Ok()||from>=3||!Net().IsHost()||!HostReplicating()||!core::IsPlayerReady()||!std::isfinite(c.speed)||c.speed<0||c.speed>500)return;
            auto*n=Vehicles::Instance().Find(c.vehicle);auto*o=SharedMissionObjectById(c.object);if(!n||!Vehicles::Alive(*n)||!o||!IsObjectValid(o))return;
            bool carRef=false,objectRef=false;
            for(auto&ref:Net().EntityContext()){
                if(ref.kind==ENTITY_VEHICLE&&ref.id==c.vehicle&&ref.flags==ENTITY_USE){c.vehicleGeneration=ref.generation;carRef=true;}
                if(ref.kind==ENTITY_OBJECT&&ref.id==c.object&&ref.flags==ENTITY_USE){c.objectGeneration=ref.generation;objectRef=true;}
            }
            if(!carRef||!objectRef||!Net().EntityContextValid(Net().EntityContext()))return;
            c.vehicleHandle=n->handle;c.objectHandle=GetObjectRef(o);
            if(!LiveContact(c)||!seen[from].Accept(token))return;
            if(vehicleContacts.size()>=64&&!vehicleContacts.count(ContactKey(c.vehicle,c.object)))return;
            vehicleContacts[ContactKey(c.vehicle,c.object)]=c;
            LOGD("Story vehicle contact player=%u car=%04X object=%04X speed=%.2f",from,c.vehicle,c.object,c.speed);return;
        }
        if(action==4){
            auto kind=r.U8();auto id=r.U16();auto player=r.U8();auto weapon=r.U8();auto actionRun=r.U64();
            if(!r.Ok()||!Net().IsHost()||!HostReplicating()||actionRun!=StoryRunId()||player>=3||weapon>58||kind!=ENTITY_VEHICLE)return;
            auto*n=Vehicles::Instance().Find(id);if(!n||!Vehicles::Alive(*n)||n->owner!=from||!seen[from].Accept(token))return;
            entityDamage[(uint32_t(kind)<<16)|id]={n->handle,n->model,NowMs(),Net().EntityGeneration(kind,id),weapon,player,actionRun,packetPhase};return;
        }
        if(action!=1)return;
        uint8_t count=r.U8();auto actionRun=r.U64();auto fov=r.F32();if(actionRun!=StoryRunId()||count>64||!std::isfinite(fov)||fov<5||fov>120)return;std::vector<std::tuple<uint8_t,uint16_t,uint16_t>> entries;
        for(int i=0;i<count;++i){uint8_t kind=r.U8();uint16_t id=r.U16(),model=r.U16();entries.emplace_back(kind,id,model);}
        if(!r.Ok()||from>=3||!Net().IsHost()||!HostReplicating()||!core::IsPlayerReady()||!seen[from].Accept(token))return;
        auto*p=Players::Instance().Get(from);if(!p||!p->hasState||NowMs()-p->stateRecvMs>1500)return;
        photoTaken=NowMs();photoPhase=packetPhase;
        for(auto[kind,id,model]:entries){
            CEntity*e=kind==1?(CEntity*)GetPed(Peds::Instance().HandleOf(id)):kind==2?(CEntity*)SharedMissionObjectById(id):nullptr;
            int8_t area=e?(kind==1?AreaOf((CPed*)e):e->AreaCode()):0;
            if(!e||e->ModelIndex()!=model||(area!=p->state.area&&area!=13)||(e->GetPosition()-p->state.pos).Length()>135)continue;
            int handle=kind==1?GetPedRef((CPed*)e):GetObjectRef((CObject*)e);photos[((uint32_t)kind<<16)|id]={handle,model,NowMs(),Net().EntityGeneration(kind==1?ENTITY_PED:ENTITY_OBJECT,id),54,from,actionRun,packetPhase};
            photoViewAt=NowMs();photoViewFov=fov;photoViewPlayer=from;
            LOGI("Story photo accepted from %u kind=%u id=%u model=%u",from,kind,id,model);
        }
    });
 }
 void OnPostProcess()override{
    bool mission=Net().IsHost()?HostReplicating():HostMissionRecent(0);
    if((wasMission&&!mission)||actionRun!=StoryRunId()){participants.clear();photos.clear();guestDamage.clear();entityDamage.clear();guestKills.clear();killed.clear();pendingHits.clear();ClearContacts();photoTaken=photoViewAt=0;photoViewPlayer=255;}wasMission=mission;actionRun=StoryRunId();
    if(proofPhase!=CooperativeStoryPhase()){
        auto eraseOld=[](auto&proofs){for(auto i=proofs.begin();i!=proofs.end();)i=(i->second.run!=StoryRunId()||i->second.phase!=CooperativeStoryPhase())?proofs.erase(i):std::next(i);};
        eraseOld(photos);eraseOld(guestDamage);eraseOld(entityDamage);eraseOld(vehicleContacts);eraseOld(pendingContacts);eraseOld(contactTargets);contactSent.clear();if(photoPhase!=CooperativeStoryPhase()){photoTaken=photoViewAt=0;photoViewPlayer=255;}proofPhase=CooperativeStoryPhase();}
    for(auto i=participants.begin();i!=participants.end();){auto*p=Players::Instance().Get(i->second.player);auto*n=Vehicles::Instance().Find(i->second.vehicle);
        i=(!p||!p->ped||p->ped->Health()<=0||(i->second.vehicle&&(!n||!Vehicles::Alive(*n)||Net().EntityGeneration(ENTITY_VEHICLE,i->second.vehicle)!=i->second.generation)))?participants.erase(i):std::next(i);
    }
    for(auto i=pendingHits.begin();i!=pendingHits.end();)i=(NowMs()-i->received>1500||AcceptStoryHit(*i))?pendingHits.erase(i):std::next(i);
    for(auto i=guestDamage.begin();i!=guestDamage.end();)i=NowMs()-i->second.at>600000?guestDamage.erase(i):std::next(i);
    for(auto i=vehicleContacts.begin();i!=vehicleContacts.end();)i=!LiveContact(i->second)?vehicleContacts.erase(i):std::next(i);
    // Static objects use another native response overload. Read the car's
    // bounded collision records after physics, rather than depending on which
    // impulse overload ran or scanning nearby objects. Native 0x543580 checks
    // bCanBeCollidedWith/count; 1.0 US keeps count at 0xB9 and six refs at 0xBC.
    if(mission&&!Net().IsHost()&&StoryRunId()&&core::IsPlayerReady()){
        auto*me=FindPlayerPed(0);auto*car=me?me->VehicleIfIn():nullptr;
        auto*n=car&&IsVehicleValid(car)?Vehicles::Instance().Find(car):nullptr;
        if(n&&Vehicles::Alive(*n)&&n->owner==Net().MyId()&&car->Driver()==me&&CallMethod<0x543580,bool>(car)){
            unsigned count=std::min<unsigned>(Field<uint8_t>(car,0xB9),6);
            for(unsigned i=0;i<count;++i){
                auto*object=(CObject*)Field<CEntity*>(car,0xBC+(int)i*4);
                if(object&&IsObjectValid(object)&&object->IsObject()&&SharedMissionObjectId(object))RecordStoryVehicleObjectContact(car,object);
            }
        }
    }
    for(auto i=pendingContacts.begin();i!=pendingContacts.end();){
        auto c=i->second;auto key=i->first;
        if(!mission||!c.run||c.run!=StoryRunId()||c.phase!=CooperativeStoryPhase()||NowMs()-c.at>250){i=pendingContacts.erase(i);continue;}
        if(NowMs()-contactSent[key]<100){++i;continue;}
        auto*n=Vehicles::Instance().Find(c.vehicle);auto*o=SharedMissionObjectById(c.object);auto*me=FindPlayerPed(0);
        if(n&&Vehicles::Alive(*n)&&n->owner==Net().MyId()&&me&&me->InVehicle()&&me->Vehicle()==n->veh&&n->veh->Driver()==me&&
           o&&IsObjectValid(o)&&GetObjectRef(o)==c.objectHandle&&Net().EntityGeneration(ENTITY_VEHICLE,c.vehicle)==c.vehicleGeneration&&Net().EntityGeneration(ENTITY_OBJECT,c.object)==c.objectGeneration&&
           Cmd(0x0897).I(n->handle).I(c.objectHandle).Run()){
            Writer w(proto::G_STORY_ACTION);w.U64(c.run);w.U32(c.phase);if(++sequence==0)++sequence;w.U32(sequence);w.U8(5);
            w.Entity(ENTITY_VEHICLE,c.vehicle);w.Entity(ENTITY_OBJECT,c.object);w.U16(c.vehicleModel);w.U16(c.objectModel);w.F32(c.speed);w.U32(c.stamp);Net().Send(proto::CH_RELIABLE,w);contactSent[key]=NowMs();
        }
        i=pendingContacts.erase(i);
    }
    for(auto i=contactSent.begin();i!=contactSent.end();)i=NowMs()-i->second>1000?contactSent.erase(i):std::next(i);
 }
 void OnPeerLeave(uint8_t id)override{if(id<3)seen[id]={};for(auto i=vehicleContacts.begin();i!=vehicleContacts.end();)i=i->second.player==id?vehicleContacts.erase(i):std::next(i);for(auto i=participants.begin();i!=participants.end();)i=i->second.player==id?participants.erase(i):std::next(i);}
 void OnPeerResync(uint8_t id)override{OnPeerLeave(id);}
 void OnGameShutdown()override{ClearContacts();participants.clear();guestKills.clear();killed.clear();pendingHits.clear();photos.clear();guestDamage.clear();entityDamage.clear();photoTaken=photoViewAt=0;photoViewPlayer=255;wasMission=false;for(auto&w:seen)w={};}
 void OnSessionEnd()override{OnGameShutdown();sequence=0;}
}actions;
struct Reg{Reg(){core::RegisterModule(&actions);}}reg;
}
void RecordStoryDamage(CPed*victim,uint8_t guest,int weapon){ReportStoryHit(victim,guest,false,weapon);RecordStoryAggressor(victim,guest);}
void RecordStoryKill(CPed*victim,uint8_t player){ReportStoryHit(victim,player,true);}
void ResetCooperativeKills(){guestKills.clear();killed.clear();}
int CooperativeGuestKills(int model){auto i=guestKills.find(model);return i==guestKills.end()?0:i->second;}
void RecordStoryEntityDamage(CEntity*entity,uint8_t player,int weapon){
    if(!Net().Online()||!entity||player>=3||weapon<0||weapon>58||!StoryRunId())return;
    uint8_t kind=entity->IsVehicle()?ENTITY_VEHICLE:entity->IsObject()?ENTITY_OBJECT:0;uint16_t id=0;uint8_t owner=255;int handle=-1;
    if(kind==ENTITY_VEHICLE){auto*n=Vehicles::Instance().Find((CVehicle*)entity);if(!n||!Vehicles::Alive(*n))return;id=n->netId;owner=n->owner;handle=n->handle;}
    else if(kind==ENTITY_OBJECT){id=SharedMissionObjectId((CObject*)entity);owner=Net().HostId();handle=GetObjectRef((CObject*)entity);}else return;
    if(!id||owner!=Net().MyId())return;
    if(Net().IsHost())entityDamage[(uint32_t(kind)<<16)|id]={handle,entity->ModelIndex(),NowMs(),Net().EntityGeneration(kind,id),weapon,player,StoryRunId(),CooperativeStoryPhase()};
    else {Writer w(proto::G_STORY_ACTION);w.U64(StoryRunId());w.U32(CooperativeStoryPhase());if(++sequence==0)++sequence;w.U32(sequence);w.U8(4);w.U8(kind);w.Entity(kind,id);w.U8(player);w.U8((uint8_t)weapon);w.U64(StoryRunId());Net().Send(proto::CH_RELIABLE,w);}
}
void RecordStoryVehicleObjectContact(CVehicle*vehicle,CObject*object){
    if(!Net().Online()||Net().IsHost()||!StoryRunId()||!HostMissionRecent(0)||!core::IsPlayerReady()||!vehicle||!object||!IsVehicleValid(vehicle)||!IsObjectValid(object))return;
    auto*n=Vehicles::Instance().Find(vehicle);auto*me=FindPlayerPed(0);auto id=SharedMissionObjectId(object);
    if(!n||!Vehicles::Alive(*n)||n->owner!=Net().MyId()||!id||!me||!me->InVehicle()||me->Vehicle()!=vehicle||vehicle->Driver()!=me)return;
    auto key=ContactKey(n->netId,id);if(NowMs()-contactSent[key]<100)return;
    if(pendingContacts.size()>=32&&!pendingContacts.count(key))return;
    VehicleObjectContact c;c.vehicle=n->netId;c.object=id;c.vehicleModel=n->model;c.objectModel=object->ModelIndex();
    c.vehicleGeneration=Net().EntityGeneration(ENTITY_VEHICLE,c.vehicle);c.objectGeneration=Net().EntityGeneration(ENTITY_OBJECT,c.object);
    if(!c.vehicleGeneration||!c.objectGeneration||!Net().EntityLive(ENTITY_VEHICLE,c.vehicle)||!Net().EntityLive(ENTITY_OBJECT,c.object))return;
    c.at=NowMs();c.stamp=(uint32_t)c.at;c.run=StoryRunId();c.phase=CooperativeStoryPhase();c.player=Net().MyId();c.vehicleHandle=n->handle;c.objectHandle=GetObjectRef(object);
    Cmd(0x02E3).I(n->handle).Out(&c.speed).Run();if(!std::isfinite(c.speed)||c.speed<0||c.speed>500)return;
    pendingContacts[key]=c;
}
bool CooperativeStoryContactSpeed(void*script,int carHandle,float&speed){
    if(!Net().IsHost()||!HostReplicating())return false;auto*c=BoundContact(script);if(!c||c->vehicleHandle!=carHandle)return false;speed=c->speed;return true;
}
void ClearStoryEntityDamage(uint8_t kind,int handle){
    uint16_t id=0;if(kind==ENTITY_VEHICLE){if(auto*n=Vehicles::Instance().Find(GetVehicle(handle)))id=n->netId;}
    else if(kind==ENTITY_OBJECT)id=SharedMissionObjectId(GetObject(handle));if(id)entityDamage.erase((uint32_t(kind)<<16)|id);
}
void ClearStoryDamage(int handle){if(auto*p=GetPed(handle))if(auto*n=Peds::Instance().Find(p))guestDamage.erase(n->netId);}
int CooperativeStoryActor(void*script,int opcode,int cjHandle){
    auto*me=FindPlayerPed(0);if(!me||cjHandle!=GetPedRef(me)||!Net().IsHost()||!HostReplicating()||!StoryContinuationQuery(opcode).inputs)return -1;
    auto i=participants.find(script);if(i==participants.end())return -1;auto&b=i->second;
    if(b.run!=StoryRunId()||b.base!=Field<void*>(script,0x10)||b.name!=Field<uint64_t>(script,8)||(b.vehicle&&Net().EntityGeneration(ENTITY_VEHICLE,b.vehicle)!=b.generation)){participants.erase(i);return -1;}
    if(b.reason==6&&!BoundContact(script))return -1;
    auto*p=Players::Instance().Get(b.player);auto*n=Vehicles::Instance().Find(b.vehicle);
    if(!p||!p->ped||!p->hasState||!FreshStoryContinuation(NowMs(),b.at,p->stateRecvMs)||p->ped->Health()<=0||p->state.area!=me->AreaCode()||(b.vehicle&&(!n||!Vehicles::Alive(*n))))return -1;
    if(StoryContinuationQuery(opcode).vehicle&&!b.vehicle)return -1;


    return p->pedHandle;
}
game::CVehicle* CooperativeStoryVehicle(void*script){
    auto i=participants.find(script);if(i==participants.end()||i->second.run!=StoryRunId()||(i->second.reason==6&&!BoundContact(script)))return nullptr;auto*n=Vehicles::Instance().Find(i->second.vehicle);
    return n&&Vehicles::Alive(*n)&&Net().EntityGeneration(ENTITY_VEHICLE,n->netId)==i->second.generation?n->veh:nullptr;
}
bool CooperativeStoryCondition(int opcode,const int32_t*p,void*script){
    if(!Net().IsHost()||!HostReplicating())return false;auto*me=FindPlayerPed(0);if(!me)return false;
    if(opcode==0x0897){
        auto*car=GetVehicle(p[0]);auto*object=GetObject(p[1]);auto*n=car?Vehicles::Instance().Find(car):nullptr;auto id=object?SharedMissionObjectId(object):0;
        if(!n||!Vehicles::Alive(*n)||!object||!id)return false;
        auto generation=Net().EntityGeneration(ENTITY_OBJECT,id);
        if(auto*targets=ScriptContactTargets(script,true);targets&&(targets->objects.size()<64||targets->objects.count(id)))targets->objects[id]=generation;
        auto i=vehicleContacts.find(ContactKey(n->netId,id));
        return i!=vehicleContacts.end()&&i->second.vehicleHandle==p[0]&&i->second.objectHandle==p[1]&&LiveContact(i->second);
    }
    if(opcode==0x00DF&&p[0]==GetPedRef(me)&&BindContactParticipant(script,me))return true;
    int nativeCount=(opcode==0x00DB||opcode==0x00DD||opcode==0x0179||opcode==0x023B||opcode==0x02D8||opcode==0x0737)?2:opcode==0x00DF?1:
        (opcode==0x00A3||opcode==0x02D6||(opcode>=0x00EC&&opcode<=0x00EE))?6:(opcode==0x00A4||(opcode>=0x00FE&&opcode<=0x0100))?8:0;
    if(nativeCount&&p[0]==GetPedRef(me)){
        Cmd local((uint16_t)opcode);for(int i=0;i<nativeCount;++i)local.I(p[i]);
        if(local.Run()){if(script)participants.erase(script);return false;}
    }
    if(opcode==0x00E1){
        if(p[0]!=0||!script)return false;auto i=participants.find(script);if(i==participants.end())return false;auto&b=i->second;
        if(b.reason!=4||b.run!=StoryRunId()||NowMs()-b.at>1000||b.base!=Field<void*>(script,0x10)||b.name!=Field<uint64_t>(script,8))return false;
        auto*rp=Players::Instance().Get(b.player);if(!rp||!rp->ped||!rp->hasState||NowMs()-rp->stateRecvMs>500)return false;
        auto*pad=GetPad(0);auto savedNew=pad->NewState,savedOld=pad->OldState;auto disabled=pad->DisablePlayerControls;
        rp->storyPad.To(pad->NewState);rp->previousStoryPad.To(pad->OldState);pad->DisablePlayerControls=0;bool result=Cmd(0x00E1).I(0).I(p[1]).Run();
        pad->NewState=savedNew;pad->OldState=savedOld;pad->DisablePlayerControls=disabled;return result;
    }else if(opcode==0x051C||opcode==0x031E||opcode==0x0366||opcode==0x08FF){
        uint8_t kind=(opcode==0x051C||opcode==0x031E)?ENTITY_VEHICLE:ENTITY_OBJECT;CEntity*entity=kind==ENTITY_VEHICLE?(CEntity*)GetVehicle(p[0]):(CEntity*)GetObject(p[0]);if(!entity)return false;
        uint16_t id=kind==ENTITY_OBJECT?SharedMissionObjectId((CObject*)entity):Vehicles::Instance().Find((CVehicle*)entity)?Vehicles::Instance().Find((CVehicle*)entity)->netId:0;
        auto i=entityDamage.find((uint32_t(kind)<<16)|id);if(i==entityDamage.end())return false;auto&proof=i->second;
        if(proof.run!=StoryRunId()||proof.phase!=CooperativeStoryPhase()||proof.handle!=p[0]||proof.model!=entity->ModelIndex()||proof.generation!=Net().EntityGeneration(kind,id))return false;
        if(opcode==0x051C&&p[1]!=GetPedRef(me))return false;if((opcode==0x031E||opcode==0x08FF)&&!SharedWeaponCondition(proof.weapon,p[1]))return false;
        if(script&&proof.player<3)participants[script]={proof.player,0,0,NowMs(),Field<void*>(script,0x10),Field<uint64_t>(script,8),5,StoryRunId()};return true;
    }else if(opcode==0x00DB||opcode==0x00DD||opcode==0x00DF){
        if(p[0]!=GetPedRef(me))return false;
        auto test=[&](int h){auto cmd=Cmd((uint16_t)opcode);cmd.I(h);if(opcode!=0x00DF)cmd.I(p[1]);return cmd.Run();};
        if(test(p[0])){if(script)participants.erase(script);return false;}
        for(auto&[id,rp]:Players::Instance().All())if(rp.ped&&rp.hasState&&NowMs()-rp.stateRecvMs<1500&&rp.ped->Health()>0&&rp.state.area==me->AreaCode())
            if(test(rp.pedHandle)){
                auto*car=rp.ped->Vehicle();auto*n=car&&IsVehicleValid(car)?Vehicles::Instance().Find(car):nullptr;
                if(script&&n)participants[script]={id,n->netId,Net().EntityGeneration(ENTITY_VEHICLE,n->netId),NowMs(),Field<void*>(script,0x10),Field<uint64_t>(script,8),0,StoryRunId()};
                return true;
            }
    }else if(opcode==0x0179||opcode==0x023B||opcode==0x02D6||opcode==0x02D8||opcode==0x0737||opcode==0x00A3||opcode==0x00A4||
             (opcode>=0x00EC&&opcode<=0x00EE)||(opcode>=0x00FE&&opcode<=0x0100)){
        if(p[0]!=GetPedRef(me))return false;
        int count=(opcode==0x00A3||opcode==0x02D6)?6:opcode==0x00A4?8:(opcode>=0x00EC&&opcode<=0x00EE)?6:(opcode>=0x00FE&&opcode<=0x0100)?8:2;
        auto test=[&](int actor){auto cmd=Cmd((uint16_t)opcode);cmd.I(actor);for(int i=1;i<count;++i)cmd.I(p[i]);return cmd.Run();};
        if(test(p[0])){if(script)participants.erase(script);return false;}
        for(auto&[id,rp]:Players::Instance().All())if(rp.ped&&rp.hasState&&NowMs()-rp.stateRecvMs<1500&&rp.ped->Health()>0&&rp.state.area==me->AreaCode()){
            bool accepted=opcode==0x0737?SharedStoryObjectHolder(GetObject(p[1]))==id:test(rp.pedHandle);
            if(accepted&&opcode!=0x02D6&&count>2&&p[count-1]){auto f=[&](int i){float value;memcpy(&value,p+i,4);return value;};
                if(opcode==0x00A3)accepted=StoryGroupReady((f(1)+f(3))*.5f,(f(2)+f(4))*.5f,me->GetPosition().z,me->AreaCode(),std::max(std::fabs(f(3)-f(1)),std::fabs(f(4)-f(2)))*.5f+1,100000);
                else if(opcode==0x00A4)accepted=StoryGroupReady((f(1)+f(4))*.5f,(f(2)+f(5))*.5f,(f(3)+f(6))*.5f,me->AreaCode(),std::max(std::fabs(f(4)-f(1)),std::fabs(f(5)-f(2)))*.5f+1,std::fabs(f(6)-f(3))*.5f+1);
                else accepted=StoryGroupReady(f(1),f(2),count==8?f(3):me->GetPosition().z,me->AreaCode(),std::max(std::fabs(f(count==8?4:3)),std::fabs(f(count==8?5:4)))+1,count==8?std::fabs(f(6))+1:100000);}
            if(!accepted)continue;auto*n=rp.ped->InVehicle()?Vehicles::Instance().Find(rp.ped->Vehicle()):nullptr;
            if(script)participants[script]={id,(uint16_t)(n?n->netId:0),n?Net().EntityGeneration(ENTITY_VEHICLE,n->netId):0,NowMs(),Field<void*>(script,0x10),Field<uint64_t>(script,8),(uint8_t)(opcode==0x02D8?1:opcode==0x0737?2:(opcode==0x0179||opcode==0x023B)?4:3),StoryRunId()};
            return true;
        }
    }else if(opcode==0x0480){
        auto*guard=GetPed(p[0]);auto*observer=guard?Peds::Instance().Find(guard):nullptr;
        if(!guard||!observer||observer->owner!=Net().MyId()||guard->CreatedBy()!=PED_MISSION||guard->Health()<=0)return false;
        // A replica body can be absent from this frame's native ped scanner.
        // Use the same native direction/occlusion test as the stock corpse
        // query, restricted to live network identities in this guard's area.
        int budget=8;
        for(auto&[id,n]:Peds::Instance().All())if(n.ped&&GetPed(n.handle)==n.ped&&n.ped->Health()<=0&&n.ped->IsVisible()&&n.pedType==p[1]&&
            (AreaOf(n.ped)==AreaOf(guard)||AreaOf(n.ped)==13)&&(n.ped->GetPosition()-guard->GetPosition()).Length()<40){
            if(budget--<=0)break;if(Call<0x5F1C40,bool>(guard,n.ped,true))return true;
        }
    }else if(opcode==0x0364||opcode==0x09ED||opcode==0x06B7){
        if(p[1]!=GetPedRef(me)||(opcode!=0x06B7&&!GetPed(p[0])))return false;
        // Native visibility decides cone, occlusion and detection. This only
        // widens an explicit story guard query to the live mission participants.
        for(auto&[id,rp]:Players::Instance().All())if(rp.ped&&rp.hasState&&NowMs()-rp.stateRecvMs<1500&&rp.ped->Health()>0&&rp.state.area==me->AreaCode()&&
            (rp.ped->GetPosition()-me->GetPosition()).Length()<120&&Cmd((uint16_t)opcode).I(p[0]).I(rp.pedHandle).Run())return true;
    }else if(opcode==0x031D){
        auto*n=Peds::Instance().Find(GetPed(p[0]));auto i=guestDamage.find(n?n->netId:0);
        bool accepted=n&&i!=guestDamage.end()&&i->second.run==StoryRunId()&&i->second.phase==CooperativeStoryPhase()&&i->second.handle==p[0]&&i->second.generation==Net().EntityGeneration(ENTITY_PED,n->netId)&&SharedWeaponCondition(i->second.weapon,p[1]);
        if(accepted&&script)participants[script]={i->second.player,0,0,NowMs(),Field<void*>(script,0x10),Field<uint64_t>(script,8),5,StoryRunId()};return accepted;
    }else if(opcode==0x051A){
        if(p[1]!=GetPedRef(me)||!GetPed(p[0]))return false;
        auto*n=Peds::Instance().Find(GetPed(p[0]));auto i=guestDamage.find(n?n->netId:0);if(n&&i!=guestDamage.end()&&i->second.run==StoryRunId()&&i->second.phase==CooperativeStoryPhase()&&i->second.handle==p[0]&&i->second.model==n->model&&i->second.generation==Net().EntityGeneration(ENTITY_PED,n->netId)&&NowMs()-i->second.at<600000){if(script)participants[script]={i->second.player,0,0,NowMs(),Field<void*>(script,0x10),Field<uint64_t>(script,8),5,StoryRunId()};return true;}
        for(auto&[id,rp]:Players::Instance().All())if(rp.ped&&Cmd(0x051A).I(p[0]).I(rp.pedHandle).Run())return true;
    }else if(opcode==0x04C5||opcode==0x0833){
        uint8_t kind=opcode==0x04C5?1:2;CEntity*e=kind==1?(CEntity*)GetPed(p[0]):(CEntity*)GetObject(p[0]);if(!e)return false;
        uint16_t id=0;if(kind==1){if(auto*n=Peds::Instance().Find((CPed*)e))id=n->netId;}else id=SharedMissionObjectId((CObject*)e);
        auto i=photos.find(((uint32_t)kind<<16)|id);bool accepted=id&&i!=photos.end()&&i->second.run==StoryRunId()&&i->second.phase==CooperativeStoryPhase()&&i->second.handle==p[0]&&i->second.model==e->ModelIndex()&&i->second.generation==Net().EntityGeneration(kind==1?ENTITY_PED:ENTITY_OBJECT,id);
        if(accepted){if(script)participants[script]={i->second.player,0,0,NowMs(),Field<void*>(script,0x10),Field<uint64_t>(script,8),5,StoryRunId()};photos.erase(i);}return accepted;
    }else if(opcode==0x04DC&&photoTaken){photoTaken=0;return true;}
    return false;
}
void ClearCooperativeStoryScript(void*script){participants.erase(script);contactTargets.erase(script);}
bool CooperativePhotoFov(float&fov){
    if(!Net().IsHost()||!HostReplicating()||photoViewPlayer>=3||photoViewPlayer==Net().MyId()||!photoViewAt||NowMs()-photoViewAt>1000)return false;
    fov=photoViewFov;return true;
}

}
