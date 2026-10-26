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
#include "shared/game/Anim.h"
using namespace game;using game::script::Cmd;
namespace coop {namespace {
struct DamageSource {
    uint16_t vehicle=0,model=0,roleSource=0;
    uint64_t generation=0,roleSourceGeneration=0;
    uint32_t assignment=0;
};
struct StoryParticipant{uint8_t player=255;uint16_t vehicle=0;uint64_t generation=0,at=0;void*base=nullptr;uint64_t name=0;uint8_t reason=0;uint64_t run=0;uint16_t object=0;uint64_t objectGeneration=0;uint32_t phase=0;uint8_t targetKind=0;uint16_t targetId=0,contextVehicle=0;uint64_t targetGeneration=0,contextGeneration=0;int targetHandle=-1;uint32_t proofToken=0;DamageSource acceptedSource;uint64_t inputAt=0,predicateTarget=0;uint16_t predicate=0;};
std::map<void*,StoryParticipant> participants;
struct Proof{int handle=-1;uint16_t model=0;uint64_t at=0,generation=0;int weapon=54;uint8_t player=255;uint64_t run=0;uint32_t phase=0;DamageSource source;uint32_t token=0;bool weaponValid=true,actorValid=true;};
std::map<uint32_t,Proof> photos;std::map<uint16_t,Proof> guestDamage;
EventWindow seen[3];uint32_t sequence=0;uint64_t photoTaken=0;uint64_t actionRun=0;uint32_t proofPhase=0,photoPhase=0;
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
struct VehicleDamageFact {
    uint16_t victim=0,model=0;int handle=-1;
    uint64_t generation=0,run=0,at=0;uint32_t phase=0,token=0,stamp=0;
    uint8_t owner=255,player=255;DamageSource source;
};
std::map<uint16_t,VehicleDamageFact> vehicleDamage;
std::map<uint16_t,VehicleDamageFact> pendingVehicleDamage,lastReportedVehicleDamage;
struct GuestAnimation {
    uint64_t run=0,generation=0,deadline=0;uint32_t phase=0,incarnation=0;
    uint16_t object=0,model=0;uint8_t player=255;int8_t area=0;
    std::string name,block;float blend=4;int loop=0,lockX=0,lockY=0,freeze=0,duration=0;
};
std::deque<GuestAnimation> guestAnimations;
bool SnatchAnimation(const GuestAnimation&a){
    if(!std::isfinite(a.blend)||std::fabs(a.blend-4.f)>.01f||a.loop||a.lockX||a.lockY||a.freeze)return false;
    if(a.block=="BIKES"&&(a.name=="BIKES_SNATCH_L"||a.name=="BIKES_SNATCH_R"))return a.duration==220;
    return a.block=="MISC"&&(a.name=="PICKUP_BOX"||a.name=="GRAB_L"||a.name=="GRAB_R")&&a.duration==180;
}
void ApplyGuestAnimations(){
    if(!core::IsPlayerReady())return;
    auto*me=FindPlayerPed(0);auto self=Net().MyIncarnation();
    for(auto i=guestAnimations.begin();i!=guestAnimations.end();){
        auto&a=*i;
        if(!Net().Online()||!me||me->Health()<=0||!self||self!=a.incarnation||NowMs()>a.deadline||
           a.run!=StoryRunId()||a.phase!=CooperativeStoryPhase()||me->AreaCode()!=a.area||Net().EntityGeneration(ENTITY_OBJECT,a.object)!=a.generation){i=guestAnimations.erase(i);continue;}
        int block=Call<0x4D3990,int>(a.block.c_str());
        if(block<0||block>=Ref<int32_t>(0xB4EA30)){i=guestAnimations.erase(i);continue;}
        if(!anim::BlockLoaded(block)){RequestModel(anim::kIfpBase+block,2|0x10);++i;continue;}
        Cmd(0x0605).I(GetPedRef(me)).S(a.name.c_str()).S(a.block.c_str()).F(a.blend).I(a.loop).I(a.lockX).I(a.lockY).I(a.freeze).I(a.duration).Run();
        LOGI("Story guest animation played player=%u object=%04X name=%s",Net().MyId(),a.object,a.name.c_str());
        i=guestAnimations.erase(i);
    }
}
uint8_t VehiclePlayer(CVehicle*vehicle){
    if(!vehicle||!IsVehicleValid(vehicle))return 255;
    auto*ped=vehicle->Driver();if(!ped)ped=CooperativeRemoteController(vehicle);
    if(ped==FindPlayerPed(0))return Net().MyId();
    auto*p=ped?Players::Instance().FindByPed(ped):nullptr;return p?p->id:255;
}
DamageSource CaptureDamageSource(CVehicle*vehicle,uint8_t player,bool requirePlayer=true){
    DamageSource source;auto*n=vehicle&&IsVehicleValid(vehicle)?Vehicles::Instance().Find(vehicle):nullptr;
    auto*actor=player==Net().MyId()?(CPed*)FindPlayerPed(0):Players::Instance().Get(player)?(CPed*)Players::Instance().Get(player)->ped:nullptr;
    bool actualPlayer=actor&&(actor->VehicleIfIn()==vehicle||CooperativeRemoteController(vehicle)==actor);
    if(!n||!Vehicles::Alive(*n)||!Net().EntityLive(ENTITY_VEHICLE,n->netId)||(requirePlayer&&(player>=3||!actualPlayer)))return source;
    source.vehicle=n->netId;source.model=n->model;source.generation=Net().EntityGeneration(ENTITY_VEHICLE,n->netId);
    StoryRemoteRole role;
    // The caller supplied the real native creator vehicle. A role seen later
    // must never manufacture an RC origin for an ordinary on-foot shot.
    if(player<3&&CooperativeRemoteRole(player,role)&&role.run==StoryRunId()&&role.phase==CooperativeStoryPhase()&&
       role.car==vehicle&&role.vehicle==source.vehicle&&role.vehicleGeneration==source.generation){
        source.roleSource=role.source;source.roleSourceGeneration=role.sourceGeneration;source.assignment=role.assignment;
    }
    return source;
}
bool LiveDamageSource(const DamageSource&source,uint8_t player){
    if(!source.vehicle||!source.generation)return false;
    auto*n=Vehicles::Instance().Find(source.vehicle);
    if(!n||!Vehicles::Alive(*n)||n->model!=source.model||!Net().EntityLive(ENTITY_VEHICLE,source.vehicle)||Net().EntityGeneration(ENTITY_VEHICLE,source.vehicle)!=source.generation)return false;
    if(source.roleSource){
        StoryRemoteRole role;
        if(player>=3||!CooperativeRemoteRole(player,role)||role.run!=StoryRunId()||role.phase!=CooperativeStoryPhase()||role.assignment!=source.assignment||
           role.vehicle!=source.vehicle||role.vehicleGeneration!=source.generation||role.source!=source.roleSource||role.sourceGeneration!=source.roleSourceGeneration)return false;
    }
    return true;
}
void WriteDamageSource(Writer&w,const DamageSource&source,bool optional=false){
    w.Bool(source.vehicle!=0);if(!source.vehicle)return;
    auto reference=[&](uint16_t id){
        uint8_t flags=optional?ENTITY_OPTIONAL:ENTITY_USE;
        // Metadata must never weaken an existing mandatory victim reference.
        for(auto&ref:w.Entities())if(ref.kind==ENTITY_VEHICLE&&ref.id==id&&ref.flags==ENTITY_USE)flags=ENTITY_USE;
        w.Entity(ENTITY_VEHICLE,id,flags);
    };
    reference(source.vehicle);w.U16(source.model);w.Bool(source.roleSource!=0);
    if(source.roleSource){reference(source.roleSource);w.U32(source.assignment);}
}
bool ReadDamageSource(Reader&r,DamageSource&source,bool mandatory=true){
    if(!r.Bool())return r.Ok();
    source.vehicle=r.U16();source.model=r.U16();bool role=r.Bool();
    if(role){source.roleSource=r.U16();source.assignment=r.U32();}
    bool carRef=false,roleRef=!role;
    for(auto&ref:Net().EntityContext())if(ref.kind==ENTITY_VEHICLE&&(mandatory?ref.flags==ENTITY_USE:(ref.flags&~ENTITY_OPTIONAL)==ENTITY_USE)){
        if(ref.id==source.vehicle){source.generation=ref.generation;carRef=true;}
        if(role&&ref.id==source.roleSource){source.roleSourceGeneration=ref.generation;roleRef=true;}
    }
    return r.Ok()&&source.vehicle&&source.generation&&carRef&&roleRef&&(!role||(source.roleSource&&source.roleSourceGeneration&&source.assignment));
}
bool MandatoryStoryContext(){
    std::vector<EntityRef> required;
    for(auto&ref:Net().EntityContext())if(!(ref.flags&ENTITY_OPTIONAL))required.push_back(ref);
    return Net().EntityContextValid(required);
}
bool LiveVehicleDamage(const VehicleDamageFact&proof){
    auto*n=Vehicles::Instance().Find(proof.victim);
    return proof.run==StoryRunId()&&proof.phase==CooperativeStoryPhase()&&n&&Vehicles::Alive(*n)&&n->handle==proof.handle&&n->model==proof.model&&
        Net().EntityLive(ENTITY_VEHICLE,proof.victim)&&Net().EntityGeneration(ENTITY_VEHICLE,proof.victim)==proof.generation&&LiveDamageSource(proof.source,proof.player);
}
uint64_t StoryPredicateTarget(int opcode,const int32_t*parameters,int count){
    uint64_t hash=1469598103934665603ull;hash=(hash^(uint16_t)opcode)*1099511628211ull;
    for(int i=1;i<count;++i)hash=(hash^(uint32_t)parameters[i])*1099511628211ull;
    return hash?hash:1;
}
void SetStageParticipant(void*script,uint8_t player,uint16_t vehicle,uint8_t reason,uint8_t targetKind=0,uint16_t targetId=0,int targetHandle=-1){
    if(!script||player>=3)return;
    if(player==Net().MyId()){participants.erase(script);return;}
    auto generation=vehicle?Net().EntityGeneration(ENTITY_VEHICLE,vehicle):0;
    StoryParticipant binding{player,vehicle,generation,NowMs(),Field<void*>(script,0x10),Field<uint64_t>(script,8),reason,StoryRunId()};
    binding.phase=CooperativeStoryPhase();binding.targetKind=targetKind;binding.targetId=targetId;binding.targetHandle=targetHandle;
    binding.targetGeneration=targetKind&&targetId?Net().EntityGeneration(targetKind,targetId):0;
    participants[script]=binding;
}
bool StageParticipantLive(void*script,const StoryParticipant&b){
    if(!script||b.run!=StoryRunId()||b.phase!=CooperativeStoryPhase()||b.base!=Field<void*>(script,0x10)||b.name!=Field<uint64_t>(script,8))return false;
    if(b.vehicle&&(!Net().EntityLive(ENTITY_VEHICLE,b.vehicle)||Net().EntityGeneration(ENTITY_VEHICLE,b.vehicle)!=b.generation))return false;
    if(!b.targetKind)return b.reason==6||b.predicateTarget||(b.at&&NowMs()>=b.at&&NowMs()-b.at<=5000);
    if(!b.targetId||!b.targetGeneration||!Net().EntityLive(b.targetKind,b.targetId)||Net().EntityGeneration(b.targetKind,b.targetId)!=b.targetGeneration)return false;
    if(b.targetKind==ENTITY_VEHICLE){auto*n=Vehicles::Instance().Find(b.targetId);if(!n||!Vehicles::Alive(*n)||n->handle!=b.targetHandle)return false;}
    else if(b.targetKind==ENTITY_OBJECT){auto*o=SharedMissionObjectById(b.targetId);if(!o||GetObjectRef(o)!=b.targetHandle)return false;}
    else if(b.targetKind==ENTITY_PED){if(Peds::Instance().HandleOf(b.targetId)!=b.targetHandle||!GetPed(b.targetHandle))return false;}
    else return false;
    // A native CLEAR_LAST_DAMAGE consumes the receipt, not the follow-up
    // action it already selected. Keep that accepted action's exact source
    // until this stage/target ends; do not look up a later player's damage.
    if(b.reason==7||b.reason==8)return b.proofToken&&b.acceptedSource.vehicle==b.vehicle&&b.acceptedSource.generation==b.generation&&LiveDamageSource(b.acceptedSource,b.player);
    return true;
}
void BindVehicleDamage(void*script,const VehicleDamageFact&proof,CVehicle*original){
    if(!script)return;
    if(proof.player>=3||proof.player==Net().MyId()){participants.erase(script);return;}
    SetStageParticipant(script,proof.player,proof.source.vehicle,7,ENTITY_VEHICLE,proof.victim,proof.handle);
    auto&b=participants[script];b.proofToken=proof.token;b.acceptedSource=proof.source;
    if(auto*n=original?Vehicles::Instance().Find(original):nullptr){b.contextVehicle=n->netId;b.contextGeneration=Net().EntityGeneration(ENTITY_VEHICLE,n->netId);}
}
bool HostVehicleContext(CVehicle*source){
    if(!source||!IsVehicleValid(source))return false;auto*me=FindPlayerPed(0);
    if((me&&me->VehicleIfIn()==source)||Players::Instance().LastCar()==source)return true;
    StoryRemoteRole role;return CooperativeRemoteRole(Net().MyId(),role)&&role.car==source&&role.run==StoryRunId()&&role.phase==CooperativeStoryPhase();
}
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
void ClearContacts(){vehicleContacts.clear();pendingContacts.clear();contactTargets.clear();contactSent.clear();vehicleDamage.clear();pendingVehicleDamage.clear();lastReportedVehicleDamage.clear();guestAnimations.clear();}
void PrepareActionRun(){
    auto run=StoryRunId();if(actionRun==run)return;
    // Producers and validated handlers can run before OnPostProcess in the
    // first mission frame. Reset before recording, not after the new evidence.
    participants.clear();photos.clear();guestDamage.clear();entityDamage.clear();guestKills.clear();killed.clear();pendingHits.clear();ClearContacts();
    photoTaken=photoViewAt=0;photoViewPlayer=255;proofPhase=photoPhase=CooperativeStoryPhase();actionRun=run;
}
bool GuestButton(const RemotePlayer&player,int button){
    auto*pad=GetPad(0);auto savedNew=pad->NewState,savedOld=pad->OldState;auto disabled=pad->DisablePlayerControls;
    player.storyPad.To(pad->NewState);player.previousStoryPad.To(pad->OldState);pad->DisablePlayerControls=0;
    bool result=Cmd(0x00E1).I(0).I(button).Run();
    pad->NewState=savedNew;pad->OldState=savedOld;pad->DisablePlayerControls=disabled;return result;
}
bool AcceptStoryHit(const StoryHit&h){
    PrepareActionRun();
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
    PrepareActionRun();
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
    if(Net().Online()&&StoryRunId())PrepareActionRun();
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
        if(!r.Ok()||!packetRun||packetRun!=StoryRunId()||packetPhase!=CooperativeStoryPhase()){LOGD("Obsolete story action ignored from=%u",from);return;}
        PrepareActionRun();
        uint32_t token=r.U32();uint8_t action=r.U8();
        if(action==2||action==3){
            StoryHit h;h.from=from;h.lethal=action==2;h.id=r.U16();h.model=r.U16();h.player=r.U8();h.birth=r.U64();h.received=NowMs();h.refs=Net().EntityContext();h.weapon=r.U8();h.run=r.U64();h.phase=packetPhase;
            if(!r.Ok()||from>=3||!Net().IsHost()||!HostReplicating()||!core::IsPlayerReady()||h.player>=3||!h.birth||!seen[from].Accept(token))return;
            if(!AcceptStoryHit(h)){if(pendingHits.size()>=64)pendingHits.pop_front();pendingHits.push_back(h);}return;
        }
        if(action==7){
            GuestAnimation a;a.player=r.U8();a.incarnation=r.U32();auto stamp=r.U32();a.area=r.I8();a.object=r.U16();a.model=r.U16();
            a.name=r.Str(23);a.block=r.Str(16);a.blend=r.F32();a.loop=r.U8();a.lockX=r.U8();a.lockY=r.U8();a.freeze=r.U8();a.duration=r.I16();
            a.run=packetRun;a.phase=packetPhase;a.deadline=NowMs()+1500;
            if(!r.Ok()||Net().IsHost()||from!=Net().HostId()||a.player!=Net().MyId()||!core::IsPlayerReady()||!SnatchAnimation(a))return;
            auto*host=Players::Instance().Get(from);auto self=Net().MyIncarnation();
            if(!host||!host->hasState||NowMs()-host->stateRecvMs>1500||!self||self!=a.incarnation||
               (int32_t)(host->lastStateTime-stamp)>1500||(int32_t)(stamp-host->lastStateTime)>1500)return;
            for(auto&ref:Net().EntityContext())if(ref.kind==ENTITY_OBJECT&&ref.id==a.object&&(ref.flags&ENTITY_OPTIONAL))a.generation=ref.generation;
            // DELETE_OBJECT follows the snatch in the same native SCM pass.
            // That legitimate removal may arrive while its IFP is loading;
            // retain the captured generation, but never accept a reused ID.
            if(!a.object||!a.generation||Net().EntityGeneration(ENTITY_OBJECT,a.object)!=a.generation||!seen[from].Accept(token))return;
            if(auto*object=SharedMissionObjectById(a.object);object&&object->ModelIndex()!=a.model)return;
            if(guestAnimations.size()>=8)guestAnimations.pop_front();guestAnimations.push_back(std::move(a));return;
        }
        if(action==6){
            VehicleDamageFact proof;proof.victim=r.U16();proof.model=r.U16();proof.player=r.U8();proof.stamp=r.U32();
            if(!ReadDamageSource(r,proof.source)||!r.Ok()||from>=3||!Net().IsHost()||!HostReplicating()||!core::IsPlayerReady()||
               (proof.player>=3&&proof.player!=255)||!Net().EntityContextValid(Net().EntityContext()))return;
            auto*n=Vehicles::Instance().Find(proof.victim);if(!n||!Vehicles::Alive(*n)||n->owner!=from||n->model!=proof.model)return;
            bool targetRef=false;for(auto&ref:Net().EntityContext())if(ref.kind==ENTITY_VEHICLE&&ref.id==proof.victim&&ref.flags==ENTITY_USE){proof.generation=ref.generation;targetRef=true;}
            auto*reporter=Players::Instance().Get(from);
            if(!targetRef||!reporter||!reporter->hasState||NowMs()-reporter->stateRecvMs>1500||(int32_t)(reporter->lastStateTime-proof.stamp)>1500||
               (int32_t)(proof.stamp-reporter->lastStateTime)>1500||!seen[from].Accept(token))return;
            proof.handle=n->handle;proof.owner=from;proof.run=packetRun;proof.phase=packetPhase;proof.token=token;proof.at=NowMs();
            if(!proof.source.vehicle){vehicleDamage.erase(proof.victim);return;}
            if(proof.source.vehicle==proof.victim)return;
            if(!LiveVehicleDamage(proof)||(vehicleDamage.size()>=128&&!vehicleDamage.count(proof.victim)))return;
            vehicleDamage[proof.victim]=proof;
            LOGD("Story vehicle damage owner=%u player=%u target=%04X source=%04X token=%u",from,proof.player,proof.victim,proof.source.vehicle,token);return;
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
            auto kind=r.U8();auto id=r.U16();auto player=r.U8();auto weapon=r.U8();auto actionRun=r.U64();DamageSource source;
            if(!ReadDamageSource(r,source,false)||!r.Ok()||!Net().IsHost()||!HostReplicating()||actionRun!=StoryRunId()||player>=3||weapon>58||kind!=ENTITY_VEHICLE)return;
            auto*n=Vehicles::Instance().Find(id);uint64_t generation=0;
            for(auto&ref:Net().EntityContext())if(ref.kind==kind&&ref.id==id&&ref.flags==ENTITY_USE)generation=ref.generation;
            if(!n||!Vehicles::Alive(*n)||n->owner!=from||!generation||!MandatoryStoryContext()||!seen[from].Accept(token))return;
            // A destroyed/retired shooting vehicle may remove retaliation
            // context, but cannot erase the victim owner's accepted hit.
            if(source.vehicle&&!LiveDamageSource(source,player))source={};
            Proof proof{n->handle,n->model,NowMs(),generation,weapon,player,actionRun,packetPhase};proof.source=source;proof.token=token;
            entityDamage[(uint32_t(kind)<<16)|id]=proof;return;
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
    PrepareActionRun();
    if(proofPhase!=CooperativeStoryPhase()){
        auto eraseOld=[](auto&proofs){for(auto i=proofs.begin();i!=proofs.end();)i=(i->second.run!=StoryRunId()||i->second.phase!=CooperativeStoryPhase())?proofs.erase(i):std::next(i);};
        eraseOld(photos);eraseOld(guestDamage);eraseOld(entityDamage);eraseOld(vehicleContacts);eraseOld(pendingContacts);eraseOld(contactTargets);eraseOld(vehicleDamage);eraseOld(pendingVehicleDamage);contactSent.clear();if(photoPhase!=CooperativeStoryPhase()){photoTaken=photoViewAt=0;photoViewPlayer=255;}proofPhase=CooperativeStoryPhase();}
    for(auto i=participants.begin();i!=participants.end();){auto*p=Players::Instance().Get(i->second.player);auto*n=Vehicles::Instance().Find(i->second.vehicle);
        i=(!StageParticipantLive(i->first,i->second)||!p||!p->ped||p->ped->Health()<=0||(i->second.vehicle&&(!n||!Vehicles::Alive(*n)||Net().EntityGeneration(ENTITY_VEHICLE,i->second.vehicle)!=i->second.generation)))?participants.erase(i):std::next(i);
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
    for(auto i=pendingVehicleDamage.begin();i!=pendingVehicleDamage.end();){
        auto proof=i->second;i=pendingVehicleDamage.erase(i);auto*n=Vehicles::Instance().Find(proof.victim);
        if(!mission||proof.run!=StoryRunId()||proof.phase!=CooperativeStoryPhase()||NowMs()-proof.at>250||!n||!Vehicles::Alive(*n)||n->owner!=Net().MyId()||
           n->handle!=proof.handle||n->model!=proof.model||Net().EntityGeneration(ENTITY_VEHICLE,proof.victim)!=proof.generation)continue;
        if(proof.source.vehicle&&!LiveVehicleDamage(proof))continue;
        Writer w(proto::G_STORY_ACTION);w.U64(proof.run);w.U32(proof.phase);if(++sequence==0)++sequence;proof.token=sequence;w.U32(sequence);w.U8(6);
        w.Entity(ENTITY_VEHICLE,proof.victim);w.U16(proof.model);w.U8(proof.player);w.U32(proof.stamp);WriteDamageSource(w,proof.source);
        if(Net().IsHost()){if(proof.source.vehicle)vehicleDamage[proof.victim]=proof;else vehicleDamage.erase(proof.victim);}
        else Net().Send(proto::CH_RELIABLE,w);
    }
    for(auto i=vehicleDamage.begin();i!=vehicleDamage.end();)i=!LiveVehicleDamage(i->second)?vehicleDamage.erase(i):std::next(i);
    for(auto i=lastReportedVehicleDamage.begin();i!=lastReportedVehicleDamage.end();)i=NowMs()-i->second.at>1500?lastReportedVehicleDamage.erase(i):std::next(i);
    ApplyGuestAnimations();
 }
 void OnPeerLeave(uint8_t id)override{if(id<3)seen[id]={};for(auto i=vehicleDamage.begin();i!=vehicleDamage.end();)i=i->second.player==id?vehicleDamage.erase(i):std::next(i);for(auto i=vehicleContacts.begin();i!=vehicleContacts.end();)i=i->second.player==id?vehicleContacts.erase(i):std::next(i);for(auto i=participants.begin();i!=participants.end();)i=i->second.player==id?participants.erase(i):std::next(i);}
 void OnPeerResync(uint8_t id)override{OnPeerLeave(id);}
 void OnGameShutdown()override{ClearContacts();participants.clear();guestKills.clear();killed.clear();pendingHits.clear();photos.clear();guestDamage.clear();entityDamage.clear();photoTaken=photoViewAt=0;photoViewPlayer=255;actionRun=0;proofPhase=photoPhase=0;for(auto&w:seen)w={};}
 void OnSessionEnd()override{OnGameShutdown();sequence=0;}
}actions;
struct Reg{Reg(){core::RegisterModule(&actions);}}reg;
}
void RecordStoryDamage(CPed*victim,uint8_t guest,int weapon){ReportStoryHit(victim,guest,false,weapon);RecordStoryAggressor(victim,guest);}
void RecordStoryKill(CPed*victim,uint8_t player){ReportStoryHit(victim,player,true);}
void ResetCooperativeKills(){guestKills.clear();killed.clear();}
int CooperativeGuestKills(int model){auto i=guestKills.find(model);return i==guestKills.end()?0:i->second;}
void RecordStoryEntityDamage(CEntity*entity,uint8_t player,int weapon,CVehicle*sourceVehicle){
    if(!Net().Online()||!entity||player>=3||weapon<0||weapon>58||!StoryRunId())return;
    PrepareActionRun();
    uint8_t kind=entity->IsVehicle()?ENTITY_VEHICLE:entity->IsObject()?ENTITY_OBJECT:0;uint16_t id=0;uint8_t owner=255;int handle=-1;
    if(kind==ENTITY_VEHICLE){auto*n=Vehicles::Instance().Find((CVehicle*)entity);if(!n||!Vehicles::Alive(*n))return;id=n->netId;owner=n->owner;handle=n->handle;}
    else if(kind==ENTITY_OBJECT){id=SharedMissionObjectId((CObject*)entity);owner=Net().HostId();handle=GetObjectRef((CObject*)entity);}else return;
    if(!id||owner!=Net().MyId())return;auto source=CaptureDamageSource(sourceVehicle,player);
    if(++sequence==0)++sequence;
    if(Net().IsHost()){Proof proof{handle,entity->ModelIndex(),NowMs(),Net().EntityGeneration(kind,id),weapon,player,StoryRunId(),CooperativeStoryPhase()};proof.source=source;proof.token=sequence;entityDamage[(uint32_t(kind)<<16)|id]=proof;}
    else {Writer w(proto::G_STORY_ACTION);w.U64(StoryRunId());w.U32(CooperativeStoryPhase());w.U32(sequence);w.U8(4);w.U8(kind);w.Entity(kind,id);w.U8(player);w.U8((uint8_t)weapon);w.U64(StoryRunId());WriteDamageSource(w,source,true);Net().Send(proto::CH_RELIABLE,w);}
}
void RecordStoryVehicleDamage(CVehicle*victim,CVehicle*source){
    if(!Net().Online()||!core::IsPlayerReady()||!StoryRunId()||!(Net().IsHost()?HostReplicating():HostMissionRecent(0))||!victim||!IsVehicleValid(victim))return;
    PrepareActionRun();
    auto*n=Vehicles::Instance().Find(victim);if(!n||!Vehicles::Alive(*n)||n->owner!=Net().MyId()||!Net().EntityLive(ENTITY_VEHICLE,n->netId))return;
    VehicleDamageFact proof;proof.victim=n->netId;proof.model=n->model;proof.handle=n->handle;proof.generation=Net().EntityGeneration(ENTITY_VEHICLE,n->netId);
    proof.owner=Net().MyId();proof.at=NowMs();proof.stamp=(uint32_t)proof.at;proof.run=StoryRunId();proof.phase=CooperativeStoryPhase();
    if(source){
        if(source==victim||!IsVehicleValid(source)||!Cmd(0x051D).I(n->handle).I(GetVehicleRef(source)).Run())return;
        proof.player=VehiclePlayer(source);proof.source=CaptureDamageSource(source,proof.player,false);if(!proof.source.vehicle)return;
    }
    if(auto i=lastReportedVehicleDamage.find(proof.victim);i!=lastReportedVehicleDamage.end()){
        auto&old=i->second;
        if(old.run==proof.run&&old.phase==proof.phase&&old.generation==proof.generation&&old.owner==proof.owner&&old.player==proof.player&&
           old.source.vehicle==proof.source.vehicle&&old.source.generation==proof.source.generation&&old.source.roleSource==proof.source.roleSource&&
           old.source.roleSourceGeneration==proof.source.roleSourceGeneration&&old.source.assignment==proof.source.assignment&&NowMs()-old.at<100)return;
    }
    if(pendingVehicleDamage.size()<128||pendingVehicleDamage.count(proof.victim)){pendingVehicleDamage[proof.victim]=proof;lastReportedVehicleDamage[proof.victim]=proof;}
}
void RecordStoryVehicleObjectContact(CVehicle*vehicle,CObject*object){
    if(!Net().Online()||Net().IsHost()||!StoryRunId()||!HostMissionRecent(0)||!core::IsPlayerReady()||!vehicle||!object||!IsVehicleValid(vehicle)||!IsObjectValid(object))return;
    PrepareActionRun();
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
void ClearStoryEntityDamage(uint8_t kind,int handle,bool clearSource){
    uint16_t id=0;if(kind==ENTITY_VEHICLE){if(auto*n=Vehicles::Instance().Find(GetVehicle(handle)))id=n->netId;}
    else if(kind==ENTITY_OBJECT)id=SharedMissionObjectId(GetObject(handle));
    if(!id)return;
    if(kind==ENTITY_VEHICLE){
        auto i=entityDamage.find((uint32_t(kind)<<16)|id);
        if(i!=entityDamage.end()){if(clearSource)i->second.actorValid=false;else i->second.weaponValid=false;if(!i->second.actorValid&&!i->second.weaponValid)entityDamage.erase(i);}
        if(clearSource){vehicleDamage.erase(id);pendingVehicleDamage.erase(id);lastReportedVehicleDamage.erase(id);}
    }else entityDamage.erase((uint32_t(kind)<<16)|id);
}
void ClearStoryDamage(int handle){if(auto*p=GetPed(handle))if(auto*n=Peds::Instance().Find(p))guestDamage.erase(n->netId);}
int CooperativeStoryActor(void*script,int opcode,int cjHandle){
    auto*me=FindPlayerPed(0);if(!me||cjHandle!=GetPedRef(me)||!Net().IsHost()||!HostReplicating()||(!StoryContinuationQuery(opcode).inputs&&opcode!=0x0448&&opcode!=0x044B&&opcode!=0x047A))return -1;
    auto i=participants.find(script);if(i==participants.end())return -1;auto&b=i->second;
    if(!StageParticipantLive(script,b)){participants.erase(i);return -1;}
    if(b.reason==6&&!BoundContact(script))return -1;
    auto*p=Players::Instance().Get(b.player);auto*n=Vehicles::Instance().Find(b.vehicle);
    if(!p||!p->ped||!p->hasState||!FreshStoryContinuation(NowMs(),b.at,p->stateRecvMs)||p->ped->Health()<=0||(!StoryContinuationQuery(opcode).vehicle&&p->state.area!=me->AreaCode())||(b.vehicle&&(!n||!Vehicles::Alive(*n))))return -1;
    if(StoryContinuationQuery(opcode).vehicle&&!b.vehicle)return -1;


    return p->pedHandle;
}
game::CVehicle* CooperativeStoryVehicle(void*script){
    auto i=participants.find(script);if(i==participants.end()||!StageParticipantLive(script,i->second)||(i->second.reason==6&&!BoundContact(script)))return nullptr;auto*n=Vehicles::Instance().Find(i->second.vehicle);
    return n&&Vehicles::Alive(*n)&&Net().EntityGeneration(ENTITY_VEHICLE,n->netId)==i->second.generation?n->veh:nullptr;
}
int CooperativeStorySourceVehicle(void*script,int originalCarHandle){
    if(!Net().IsHost()||!HostReplicating())return -1;
    auto i=participants.find(script);if(i==participants.end()||!StageParticipantLive(script,i->second))return -1;auto&b=i->second;
    if((b.reason!=7&&b.reason!=8)||!b.contextVehicle||!b.contextGeneration)return -1;
    auto*original=Vehicles::Instance().Find(b.contextVehicle);auto*source=Vehicles::Instance().Find(b.vehicle);
    if(!original||!Vehicles::Alive(*original)||original->handle!=originalCarHandle||Net().EntityGeneration(ENTITY_VEHICLE,b.contextVehicle)!=b.contextGeneration||!source||!Vehicles::Alive(*source))return -1;
    return source->handle;
}
bool CooperativeStoryButton(void*script,int player,int button,bool&pressed){
    if(!Net().IsHost()||!HostReplicating()||player!=0||!script||button<0||button>19)return false;
    auto i=participants.find(script);if(i==participants.end())return false;auto&b=i->second;
    if(b.reason!=4||!StageParticipantLive(script,b)||NowMs()-b.at>1000)return false;
    auto*rp=Players::Instance().Get(b.player);if(!rp||!rp->ped||!rp->hasState||rp->ped->Health()<=0||NowMs()-rp->stateRecvMs>500)return false;
    pressed=GuestButton(*rp,button);if(pressed)b.inputAt=NowMs();return true;
}
bool CooperativeStoryAnimation(void*script,int cjHandle,const std::string&name,const std::string&block,float blend,int loop,int lockX,int lockY,int freeze,int duration){
    auto*me=FindPlayerPed(0);if(!Net().IsHost()||!HostReplicating()||!me||cjHandle!=GetPedRef(me))return false;
    auto i=participants.find(script);if(i==participants.end()||!StageParticipantLive(script,i->second))return false;auto&b=i->second;
    if(b.player>=3||b.player==Net().MyId()||b.reason!=4||b.targetKind!=ENTITY_OBJECT||!b.inputAt||NowMs()-b.inputAt>1000)return false;
    auto*player=Players::Instance().Get(b.player);auto*peer=Net().Peer(b.player);auto*object=SharedMissionObjectById(b.targetId);
    if(!player||!player->ped||!player->hasState||NowMs()-player->stateRecvMs>500||player->ped->Health()<=0||!peer||!peer->incarnation||!object)return false;
    GuestAnimation a;a.name=name;a.block=block;a.blend=blend;a.loop=loop;a.lockX=lockX;a.lockY=lockY;a.freeze=freeze;a.duration=duration;if(!SnatchAnimation(a))return false;
    Writer w(proto::G_STORY_ACTION);w.U64(StoryRunId());w.U32(CooperativeStoryPhase());if(++sequence==0)++sequence;w.U32(sequence);w.U8(7);
    w.U8(b.player);w.U32(peer->incarnation);w.U32((uint32_t)NowMs());w.I8(player->state.area);w.Entity(ENTITY_OBJECT,b.targetId,ENTITY_USE|ENTITY_OPTIONAL,255,b.targetGeneration);w.U16(object->ModelIndex());
    w.Str(name,23);w.Str(block,16);w.F32(blend);w.U8((uint8_t)loop);w.U8((uint8_t)lockX);w.U8((uint8_t)lockY);w.U8((uint8_t)freeze);w.I16((int16_t)duration);Net().Send(proto::CH_RELIABLE,w);
    b.inputAt=0;LOGI("Story guest animation sent player=%u object=%04X name=%s",b.player,b.targetId,name.c_str());return true;
}
bool CooperativeStoryCondition(int opcode,const int32_t*p,void*script){
    if(!Net().IsHost()||!HostReplicating())return false;auto*me=FindPlayerPed(0);if(!me)return false;PrepareActionRun();
    if(opcode==0x051D){
        auto*target=GetVehicle(p[0]);auto*original=GetVehicle(p[1]);auto*n=target?Vehicles::Instance().Find(target):nullptr;
        if(!n||!original)return false;
        // Preserve literal NPC-vs-NPC checks. Only the native player's actual
        // source car or assigned RC permits a cooperative source substitution.
        auto i=vehicleDamage.find(n->netId);
        if(Cmd(0x051D).I(p[0]).I(p[1]).Run()){
            bool bound=false;
            if(i!=vehicleDamage.end()&&LiveVehicleDamage(i->second)){auto*source=Vehicles::Instance().Find(i->second.source.vehicle);if(source&&Vehicles::Alive(*source)&&source->handle==p[1]){BindVehicleDamage(script,i->second,original);bound=true;}}
            if(!bound&&script){auto b=participants.find(script);auto*source=b!=participants.end()?Vehicles::Instance().Find(b->second.vehicle):nullptr;
                if(b==participants.end()||!StageParticipantLive(script,b->second)||!source||!Vehicles::Alive(*source)||source->handle!=p[1])participants.erase(script);}
            return false;
        }
        if(i==vehicleDamage.end()||!LiveVehicleDamage(i->second))return false;auto&proof=i->second;
        auto*source=Vehicles::Instance().Find(proof.source.vehicle);if(!source||!Vehicles::Alive(*source))return false;
        bool exact=source->handle==p[1];bool cooperative=proof.player<3&&proof.player!=Net().MyId()&&HostVehicleContext(original);
        if(cooperative&&proof.source.roleSource){auto*context=Vehicles::Instance().Find(proof.source.roleSource);cooperative=context&&Vehicles::Alive(*context)&&context->handle==p[1]&&Net().EntityGeneration(ENTITY_VEHICLE,context->netId)==proof.source.roleSourceGeneration;}
        if(!exact&&!cooperative)return false;
        BindVehicleDamage(script,proof,original);return true;
    }
    if(opcode==0x051C&&p[1]==GetPedRef(me)){
        if(Cmd(0x051C).I(p[0]).I(p[1]).Run()){if(script)participants.erase(script);return false;}
        auto*n=Vehicles::Instance().Find(GetVehicle(p[0]));auto i=vehicleDamage.find(n?n->netId:0);
        if(i!=vehicleDamage.end()&&LiveVehicleDamage(i->second)){
            auto&proof=i->second;if(proof.player>=3||proof.player==Net().MyId())return false;
            BindVehicleDamage(script,proof,nullptr);return true;
        }
    }
    if(opcode==0x0897){
        auto*car=GetVehicle(p[0]);auto*object=GetObject(p[1]);auto*n=car?Vehicles::Instance().Find(car):nullptr;auto id=object?SharedMissionObjectId(object):0;
        if(!n||!Vehicles::Alive(*n)||!object||!id)return false;
        auto generation=Net().EntityGeneration(ENTITY_OBJECT,id);
        if(auto*targets=ScriptContactTargets(script,true);targets&&(targets->objects.size()<64||targets->objects.count(id)))targets->objects[id]=generation;
        auto i=vehicleContacts.find(ContactKey(n->netId,id));
        return i!=vehicleContacts.end()&&i->second.vehicleHandle==p[0]&&i->second.objectHandle==p[1]&&LiveContact(i->second);
    }
    if(opcode==0x00DF&&p[0]==GetPedRef(me)&&BindContactParticipant(script,me))return true;
    int regionCount=(opcode==0x0474||opcode==0x00A3||opcode==0x02D6||(opcode>=0x00EC&&opcode<=0x00EE))?6:
        (opcode==0x00A4||(opcode>=0x00FE&&opcode<=0x0100))?8:0;
    if(regionCount&&script){auto i=participants.find(script);if(i!=participants.end()&&i->second.predicate==opcode&&i->second.predicateTarget&&
        i->second.predicateTarget!=StoryPredicateTarget(opcode,p,regionCount))participants.erase(i);}
    int nativeCount=(opcode==0x0448||opcode==0x00DB||opcode==0x00DD||opcode==0x0179||opcode==0x023B||opcode==0x02D8||opcode==0x0737)?2:opcode==0x00DF?1:
        (opcode==0x0474||opcode==0x00A3||opcode==0x02D6||(opcode>=0x00EC&&opcode<=0x00EE))?6:(opcode==0x00A4||(opcode>=0x00FE&&opcode<=0x0100))?8:0;
    if(nativeCount&&p[0]==GetPedRef(me)){
        Cmd local((uint16_t)opcode);for(int i=0;i<nativeCount;++i)local.I(p[i]);
        if(local.Run()){if(script)participants.erase(script);return false;}
    }
    if(opcode==0x00E1){
        bool pressed=false;return CooperativeStoryButton(script,p[0],p[1],pressed)&&pressed;
    }else if(opcode==0x051C||opcode==0x031E||opcode==0x0366||opcode==0x08FF){
        uint8_t kind=(opcode==0x051C||opcode==0x031E)?ENTITY_VEHICLE:ENTITY_OBJECT;CEntity*entity=kind==ENTITY_VEHICLE?(CEntity*)GetVehicle(p[0]):(CEntity*)GetObject(p[0]);if(!entity)return false;
        uint16_t id=kind==ENTITY_OBJECT?SharedMissionObjectId((CObject*)entity):Vehicles::Instance().Find((CVehicle*)entity)?Vehicles::Instance().Find((CVehicle*)entity)->netId:0;
        auto i=entityDamage.find((uint32_t(kind)<<16)|id);if(i==entityDamage.end())return false;auto&proof=i->second;
        if(proof.run!=StoryRunId()||proof.phase!=CooperativeStoryPhase()||proof.handle!=p[0]||proof.model!=entity->ModelIndex()||proof.generation!=Net().EntityGeneration(kind,id))return false;
        if((opcode==0x051C&&!proof.actorValid)||((opcode==0x031E||opcode==0x08FF)&&!proof.weaponValid))return false;
        if(opcode==0x051C&&p[1]!=GetPedRef(me))return false;if((opcode==0x031E||opcode==0x08FF)&&!SharedWeaponCondition(proof.weapon,p[1]))return false;
        if(script&&proof.player<3&&proof.player!=Net().MyId()){
            bool source=proof.source.vehicle&&LiveDamageSource(proof.source,proof.player);
            SetStageParticipant(script,proof.player,source?proof.source.vehicle:0,source?8:5,kind,id,p[0]);
            if(source){auto&b=participants[script];b.proofToken=proof.token;b.acceptedSource=proof.source;b.contextVehicle=proof.source.roleSource;b.contextGeneration=proof.source.roleSourceGeneration;}
        }return true;
    }else if(opcode==0x0448||opcode==0x00DB||opcode==0x00DD||opcode==0x00DF){
        if(p[0]!=GetPedRef(me))return false;
        auto test=[&](int h){auto cmd=Cmd((uint16_t)opcode);cmd.I(h);if(opcode!=0x00DF)cmd.I(p[1]);return cmd.Run();};
        if(test(p[0])){if(script)participants.erase(script);return false;}
        for(auto&[id,rp]:Players::Instance().All())if(rp.ped&&rp.hasState&&NowMs()-rp.stateRecvMs<1500&&rp.ped->Health()>0&&rp.state.area==me->AreaCode())
            if(test(rp.pedHandle)){
                auto*car=rp.ped->Vehicle();auto*n=car&&IsVehicleValid(car)?Vehicles::Instance().Find(car):nullptr;
                if(script&&n)SetStageParticipant(script,id,n->netId,0,ENTITY_VEHICLE,n->netId,n->handle);
                return true;
            }
    }else if(opcode==0x0474||opcode==0x0179||opcode==0x023B||opcode==0x02D6||opcode==0x02D8||opcode==0x0737||opcode==0x00A3||opcode==0x00A4||
             (opcode>=0x00EC&&opcode<=0x00EE)||(opcode>=0x00FE&&opcode<=0x0100)){
        if(p[0]!=GetPedRef(me))return false;
        int count=(opcode==0x0474||opcode==0x00A3||opcode==0x02D6)?6:opcode==0x00A4?8:(opcode>=0x00EC&&opcode<=0x00EE)?6:(opcode>=0x00FE&&opcode<=0x0100)?8:2;
        auto test=[&](int actor){auto cmd=Cmd((uint16_t)opcode);cmd.I(actor);for(int i=1;i<count;++i)cmd.I(p[i]);return cmd.Run();};
        if(test(p[0])){if(script)participants.erase(script);return false;}
        bool snatch=opcode==0x0474&&script&&!_strnicmp(Field<char[8]>(script,8),"DRIV2",8);
        for(int pass=0;pass<(snatch?2:1);++pass)for(auto&[id,rp]:Players::Instance().All())if(rp.ped&&rp.hasState&&NowMs()-rp.stateRecvMs<1500&&rp.ped->Health()>0&&rp.state.area==me->AreaCode()){
            if(snatch){
                if(!Cmd(0x047A).I(rp.pedHandle).Run()&&!Cmd(0x044B).I(rp.pedHandle).Run())continue;
                bool pressed=NowMs()-rp.stateRecvMs<500&&GuestButton(rp,4);if((pass==0)!=pressed)continue;
            }
            bool accepted=opcode==0x0737?SharedStoryObjectHolder(GetObject(p[1]))==id:test(rp.pedHandle);
            if(accepted&&opcode!=0x02D6&&opcode!=0x0474&&count>2&&p[count-1]){auto f=[&](int i){float value;memcpy(&value,p+i,4);return value;};
                if(opcode==0x00A3)accepted=StoryGroupReady((f(1)+f(3))*.5f,(f(2)+f(4))*.5f,me->GetPosition().z,me->AreaCode(),std::max(std::fabs(f(3)-f(1)),std::fabs(f(4)-f(2)))*.5f+1,100000);
                else if(opcode==0x00A4)accepted=StoryGroupReady((f(1)+f(4))*.5f,(f(2)+f(5))*.5f,(f(3)+f(6))*.5f,me->AreaCode(),std::max(std::fabs(f(4)-f(1)),std::fabs(f(5)-f(2)))*.5f+1,std::fabs(f(6)-f(3))*.5f+1);
                else accepted=StoryGroupReady(f(1),f(2),count==8?f(3):me->GetPosition().z,me->AreaCode(),std::max(std::fabs(f(count==8?4:3)),std::fabs(f(count==8?5:4)))+1,count==8?std::fabs(f(6))+1:100000);}
            if(!accepted)continue;auto*n=rp.ped->InVehicle()?Vehicles::Instance().Find(rp.ped->Vehicle()):nullptr;
            if(script){
                uint16_t target=(opcode==0x0474||opcode==0x02D8||opcode==0x0737||opcode==0x0179||opcode==0x023B)?SharedMissionObjectId(GetObject(p[1])):0;
                SetStageParticipant(script,id,(uint16_t)(n?n->netId:0),(uint8_t)(opcode==0x02D8?1:opcode==0x0737?2:(opcode==0x0474||opcode==0x0179||opcode==0x023B)?4:3),target?ENTITY_OBJECT:0,target,target?p[1]:-1);
                auto&binding=participants[script];binding.predicate=(uint16_t)opcode;binding.predicateTarget=StoryPredicateTarget(opcode,p,count);
            }
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
        if(accepted&&script)SetStageParticipant(script,i->second.player,0,5,ENTITY_PED,n->netId,p[0]);return accepted;
    }else if(opcode==0x051A){
        if(p[1]!=GetPedRef(me)||!GetPed(p[0]))return false;
        auto*n=Peds::Instance().Find(GetPed(p[0]));auto i=guestDamage.find(n?n->netId:0);if(n&&i!=guestDamage.end()&&i->second.run==StoryRunId()&&i->second.phase==CooperativeStoryPhase()&&i->second.handle==p[0]&&i->second.model==n->model&&i->second.generation==Net().EntityGeneration(ENTITY_PED,n->netId)&&NowMs()-i->second.at<600000){if(script)SetStageParticipant(script,i->second.player,0,5,ENTITY_PED,n->netId,p[0]);return true;}
        for(auto&[id,rp]:Players::Instance().All())if(rp.ped&&Cmd(0x051A).I(p[0]).I(rp.pedHandle).Run())return true;
    }else if(opcode==0x04C5||opcode==0x0833){
        uint8_t kind=opcode==0x04C5?1:2;CEntity*e=kind==1?(CEntity*)GetPed(p[0]):(CEntity*)GetObject(p[0]);if(!e)return false;
        uint16_t id=0;if(kind==1){if(auto*n=Peds::Instance().Find((CPed*)e))id=n->netId;}else id=SharedMissionObjectId((CObject*)e);
        auto i=photos.find(((uint32_t)kind<<16)|id);bool accepted=id&&i!=photos.end()&&i->second.run==StoryRunId()&&i->second.phase==CooperativeStoryPhase()&&i->second.handle==p[0]&&i->second.model==e->ModelIndex()&&i->second.generation==Net().EntityGeneration(kind==1?ENTITY_PED:ENTITY_OBJECT,id);
        if(accepted){if(script)SetStageParticipant(script,i->second.player,0,5,kind==1?ENTITY_PED:ENTITY_OBJECT,id,p[0]);photos.erase(i);}return accepted;
    }else if(opcode==0x04DC&&photoTaken){photoTaken=0;return true;}
    return false;
}
void ClearCooperativeStoryScript(void*script){participants.erase(script);contactTargets.erase(script);}
bool CooperativePhotoFov(float&fov){
    if(!Net().IsHost()||!HostReplicating()||photoViewPlayer>=3||photoViewPlayer==Net().MyId()||!photoViewAt||NowMs()-photoViewAt>1000)return false;
    fov=photoViewFov;return true;
}

}
