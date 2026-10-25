#include <array>
#include "shared/api/story-world/StoryWorld.h"
#include <map>
#include <set>
#include "shared/api/story-stage/StoryStage.h"
#include "shared/lib/sync/StoryPosition.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/mission-objects/SharedObject.h"
#include "shared/api/net-session/Session.h"
#include "entities/player/Players.h"
#include "entities/ped/Peds.h"
#include "entities/vehicle/Vehicles.h"
#include "entities/session/Flow.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "common/lib/util/Util.h"
#include "common/lib/log/Log.h"
namespace coop {namespace {
using namespace game;using game::script::Cmd;
struct Dependency{uint8_t kind;uint16_t id;uint64_t generation=0;};
struct Gate{uint64_t run=0,since=0,sent=0;uint32_t token=0;uint32_t worldRevision=0;int opcode=0;void*script=nullptr;void*ip=nullptr;int8_t area=0;CVector point;std::vector<Dependency>entities;std::vector<EntityRef>refs;std::set<uint8_t>ready;};
uint64_t phaseRun=0;uint32_t phase=0;bool transitionPending=false,recoveryPending=false;
uint64_t reportAt=0,hostReportAt=0;uint8_t blockedReason=0;uint16_t blockedEntity=0;uint8_t blockedKind=0;
uint64_t clockRun=0;uint32_t pausedTime=0,gateTime=0;std::map<void*,std::pair<void*,uint64_t>>armed;
Gate host,client;uint32_t sequence=0;uint64_t ackSent=0,completedRun=0;uint32_t completedToken=0;
void SendGate(){
    bool removed=false;for(auto i=host.entities.begin();i!=host.entities.end();){bool alive=i->kind==ENTITY_VEHICLE?Vehicles::Instance().Find(i->id)&&Vehicles::Alive(*Vehicles::Instance().Find(i->id)):i->kind==ENTITY_PED?GetPed(Peds::Instance().HandleOf(i->id))!=nullptr:SharedMissionObjectById(i->id)!=nullptr;
        if(!alive||Net().EntityGeneration(i->kind,i->id)!=i->generation){i=host.entities.erase(i);removed=true;}else ++i;}
    if(removed){host.ready.clear();host.token=++sequence;if(!host.token)host.token=++sequence;}
    Writer w(proto::G_STORY_STAGE);w.U8(1);w.U64(host.run);w.U32(host.token);w.U16((uint16_t)host.opcode);w.U32(StoryWorldRevision());w.I8(host.area);w.Vec({host.point.x,host.point.y,host.point.z});w.U8((uint8_t)host.entities.size());for(auto&e:host.entities){w.U8(e.kind);w.Entity(e.kind,e.id);}Net().Send(proto::CH_RELIABLE,w);host.sent=NowMs();}
bool DependenciesReady(bool includeRecovery=true){
    blockedReason=blockedKind=0;blockedEntity=0;
    auto blocked=[](uint8_t reason,uint8_t kind=0,uint16_t id=0){blockedReason=reason;blockedKind=kind;blockedEntity=id;return false;};
    if(!client.run||client.run!=StoryRunId())return blocked(1);
    if(!core::IsPlayerReady()||!flow::InHostWorld())return blocked(2);
    if(!Net().EntityContextValid(client.refs))return blocked(3);
    if(transitionPending)return blocked(4);
    if(includeRecovery&&recoveryPending)return blocked(5);
    auto*me=FindPlayerPed(0);if(me->AreaCode()!=client.area)return blocked(6);
    if(!LocalCooperativeRoleReady())return blocked(7);
    if(!StoryWorldReady(client.worldRevision,client.point))return blocked(11);
    for(auto&e:client.entities){
        if(e.kind==ENTITY_VEHICLE){auto*n=Vehicles::Instance().Find(e.id);if(!n||!Vehicles::Alive(*n))return blocked(8,e.kind,e.id);}
        else if(e.kind==ENTITY_PED){auto*p=GetPed(Peds::Instance().HandleOf(e.id));if(!p||!IsPedValid(p))return blocked(8,e.kind,e.id);}
        else if(e.kind==ENTITY_OBJECT&&!SharedMissionObjectReady(e.id))return blocked(9,e.kind,e.id);
    }
    if(Cmd(0x04F0).I(GetPedRef(me)).Run())return blocked(10);
    return true;
}
class Stages:public Module{const char*Name()const override{return "CooperativeStages";}
 void OnInstallHooks()override{Net().SetHandler(proto::G_STORY_STAGE,[](uint8_t from,Reader&r){auto op=r.U8();auto run=r.U64();auto token=r.U32();
    if(op==2){if(Net().IsHost()&&r.Ok()&&from<3&&Net().Peer(from)&&host.run==run&&host.token==token){host.ready.insert(from);LOGD("Story stage ready token=%u player=%u",token,from);}return;}
    if(op==3){if(!Net().IsHost()&&from==Net().HostId()&&r.Ok()){if(run>completedRun||(run==completedRun&&(int32_t)(token-completedToken)>0)){completedRun=run;completedToken=token;}if(client.run==run&&(int32_t)(token-client.token)>=0)client={};}return;}
    if(op==4){auto next=r.U32();if(!Net().IsHost()&&from==Net().HostId()&&r.Ok()&&run==StoryRunId())AdoptCooperativeStoryPhase(run,next);return;}
    if(op==5){auto revision=r.U32();auto reason=r.U8();auto kind=r.U8();auto entity=r.U16();
        if(Net().IsHost()&&r.Ok()&&from<3&&Net().Peer(from)&&host.run==run&&host.token==token){
            if((reason==8||reason==9)&&std::any_of(host.entities.begin(),host.entities.end(),[&](auto&e){return e.kind==kind&&e.id==entity;})){
                if(kind==ENTITY_VEHICLE)Vehicles::Instance().ReannounceOne(entity);
                if(kind==ENTITY_PED)Peds::Instance().ReannounceOne(entity);
            }
            LOGI("Story blocked player=%u run=%llu phase=%u gate=%u opcode=%04X reason=%u entity=%u:%04X",from,(unsigned long long)run,revision,token,host.opcode,reason,kind,entity);return;}
        return;}
    if(op!=1||Net().IsHost()||from!=Net().HostId())return;Gate g;g.run=run;g.token=token;g.opcode=r.U16();g.worldRevision=r.U32();g.area=r.I8();auto p=r.Vec();g.point={p.x,p.y,p.z};auto count=r.U8();if(count>72)return;
    for(int i=0;i<count;++i){auto kind=r.U8();auto id=r.U16();if(kind<ENTITY_VEHICLE||kind>ENTITY_OBJECT)return;g.entities.push_back({kind,id});}g.refs=Net().EntityContext();
    if(!r.Ok()||!run||!token||(run==completedRun&&(int32_t)(token-completedToken)<=0)||run!=StoryRunId()||(client.run==run&&(int32_t)(token-client.token)<0))return;
    if(client.run!=run||client.token!=token){ackSent=0;Cmd(0x04E4).F(g.point.x).F(g.point.y).Run();LOGI("Story stage waiting token=%u opcode=%04X dependencies=%u",token,g.opcode,count);}client=std::move(g);
 });}
 void OnPostProcess()override{if(!Net().Online())return;
    if(!StoryRunId()&&phaseRun){ResetCooperativeStages();return;}
    if(host.run&&host.run!=StoryRunId())host={};if(client.run&&client.run!=StoryRunId())client={};
    if(Net().IsHost()){if(clockRun!=StoryRunId()){clockRun=StoryRunId();pausedTime=0;armed.clear();}if(host.run&&NowMs()-host.sent>750)SendGate();}
    else if(client.run&&!DependenciesReady()){
        if(NowMs()-reportAt>3000){Writer w(proto::G_STORY_STAGE);w.U8(5);w.U64(client.run);w.U32(client.token);w.U32(CooperativeStoryPhase());w.U8(blockedReason);w.U8(blockedKind);w.U16(blockedEntity);Net().Send(proto::CH_RELIABLE,w);reportAt=NowMs();
            LOGI("Story dependency wait gate=%u phase=%u reason=%u entity=%u:%04X",client.token,CooperativeStoryPhase(),blockedReason,blockedKind,blockedEntity);}
    }else if(client.run&&NowMs()-ackSent>500){Writer w(proto::G_STORY_STAGE);w.U8(2);w.U64(client.run);w.U32(client.token);Net().Send(proto::CH_RELIABLE,w);ackSent=NowMs();}
 }
 void OnPeerLeave(uint8_t id)override{host.ready.erase(id);}
 void OnPeerResync(uint8_t id)override{host.ready.erase(id);if(Net().IsHost()&&host.run){host.ready.clear();host.token=++sequence;if(!host.token)host.token=++sequence;SendGate();}}
 void OnGameShutdown()override{ResetCooperativeStages();}
 void OnSessionEnd()override{OnGameShutdown();sequence=0;}
}feature;struct Register{Register(){core::RegisterModule(&feature);}}registration;
}
void ArmCooperativeStage(void*script){
    if(!Net().IsHost()||!script||!StoryRunId())return;
    if(clockRun!=StoryRunId()){clockRun=StoryRunId();pausedTime=0;armed.clear();}
    if(armed.size()<16||armed.count(script))armed[script]={Field<void*>(script,0x10),Field<uint64_t>(script,8)};
}
bool CooperativeStageArmed(void*script){auto i=armed.find(script);return i!=armed.end()&&clockRun==StoryRunId()&&i->second.first==Field<void*>(script,0x10)&&i->second.second==Field<uint64_t>(script,8);}
uint32_t CooperativeStoryClock(){
    if(clockRun!=StoryRunId())return Ref<uint32_t>(addr::TimeInMs);
    auto pending=host.run?Ref<uint32_t>(addr::TimeInMs)-gateTime:0;
    return Ref<uint32_t>(addr::TimeInMs)-pausedTime-pending;
}
void CancelCooperativeStage(void*script){
    armed.erase(script);if(!Net().IsHost()||!host.run||host.script!=script)return;
    Writer done(proto::G_STORY_STAGE);done.U8(3);done.U64(host.run);done.U32(host.token);Net().Send(proto::CH_RELIABLE,done);
    pausedTime+=Ref<uint32_t>(addr::TimeInMs)-gateTime;LOGI("Story stage cancelled token=%u",host.token);host={};
}
bool CooperativeStageWaiting(void*script){return Net().IsHost()&&host.run==StoryRunId()&&host.run&&(!script||host.script==script);}
bool AwaitCooperativeStage(void*script,int opcode,uint16_t source){
    if(!Net().IsHost()||!StoryRunId()||Net().Peers().empty()||!core::IsPlayerReady())return true;
    if(!host.run){if(clockRun!=StoryRunId()){clockRun=StoryRunId();pausedTime=0;armed.clear();}PrepareCooperativeStoryStage();gateTime=Ref<uint32_t>(addr::TimeInMs);host={};host.run=StoryRunId();host.since=NowMs();host.token=++sequence;if(!host.token)host.token=++sequence;host.script=script;host.ip=Field<void*>(script,0x14);host.opcode=opcode;host.point=StoryPosition(FindPlayerPed(0));host.area=FindPlayerPed(0)->AreaCode();
        std::set<std::pair<uint8_t,uint16_t>>included;
        auto add=[&](uint8_t kind,uint16_t id){if(id&&host.entities.size()<72&&included.insert({kind,id}).second)host.entities.push_back({kind,id,Net().EntityGeneration(kind,id)});};
        add(ENTITY_VEHICLE,source);
        // Mirrors are marked MISSION by their protection code too. They do
        // not become dependencies of the host's screenplay merely for that.
        for(auto&[id,n]:Vehicles::Instance().All())if(Vehicles::Alive(n)&&((n.owner==Net().MyId()&&(n.script||Vehicles::ScriptControlled(n.veh)))||IsCooperativeRoleVehicle(n.veh))&&n.veh->AreaCode()==host.area&&(n.veh->GetPosition()-host.point).Length()<200){add(ENTITY_VEHICLE,id);if(auto*p=Peds::Instance().Find(n.veh->Driver()))add(ENTITY_PED,p->netId);}
        for(auto&[id,n]:Peds::Instance().All())if(n.owner==Net().MyId()&&n.ped&&IsPedValid(n.ped)&&n.ped->CreatedBy()==PED_MISSION&&(n.ped->AreaCode()==host.area||n.ped->AreaCode()==13)&&(n.ped->GetPosition()-host.point).Length()<200)add(ENTITY_PED,id);
        std::vector<SyncAuditEntry>audit;core::CollectSyncAudit(audit);for(auto&e:audit)if(e.kind==AUDIT_OBJECT&&e.alive&&(e.area==host.area||e.area==13)&&(e.pos-host.point).Length()<200)add(ENTITY_OBJECT,e.id);
        SendGate();LOGI("Story stage opened token=%u opcode=%04X dependencies=%u",host.token,opcode,(unsigned)host.entities.size());return false;
    }
    if(host.script==script&&host.ip!=Field<void*>(script,0x14)){CancelCooperativeStage(script);return AwaitCooperativeStage(script,opcode,source);}
    if(host.script!=script)return false;
    for(auto&[id,p]:Net().Peers())if(id!=Net().MyId()&&!host.ready.count(id)){
        if(NowMs()-hostReportAt>3000){LOGI("Story waiting for player=%u gate=%u phase=%u opcode=%04X at=%.1f,%.1f,%.1f area=%d elapsed=%llu",id,host.token,CooperativeStoryPhase(),host.opcode,host.point.x,host.point.y,host.point.z,host.area,(unsigned long long)(NowMs()-host.since));hostReportAt=NowMs();}return false;}

    LOGI("Story stage released token=%u waited=%llu",host.token,(unsigned long long)(NowMs()-host.since));Writer done(proto::G_STORY_STAGE);done.U8(3);done.U64(host.run);done.U32(host.token);Net().Send(proto::CH_RELIABLE,done);pausedTime+=Ref<uint32_t>(addr::TimeInMs)-gateTime;armed.erase(script);host={};return true;
}
uint32_t CooperativeStoryPhase(){auto run=StoryRunId();return run?(phaseRun==run?phase:1):0;}
void AdoptCooperativeStoryPhase(uint64_t run,uint32_t next){
    if(!run||!next||run!=StoryRunId())return;
    if(phaseRun!=run||(int32_t)(next-phase)>0){phaseRun=run;phase=next;LOGI("Story phase received run=%llu phase=%u",(unsigned long long)run,phase);}
}
void AdvanceCooperativeStoryPhase(void*script,const char*reason){
    if(!Net().IsHost()||!StoryRunId())return;
    if(phaseRun!=StoryRunId()){phaseRun=StoryRunId();phase=1;}
    if(++phase==0)++phase;
    if(host.run)CancelCooperativeStage(host.script);
    Writer w(proto::G_STORY_STAGE);w.U8(4);w.U64(phaseRun);w.U32(0);w.U32(phase);Net().Send(proto::CH_RELIABLE,w);
    LOGI("Story phase advanced run=%llu phase=%u reason=%s",(unsigned long long)phaseRun,phase,reason);
    if(script)ArmCooperativeStage(script);
}
void ResetCooperativeStages(){
    if(Net().IsHost()&&host.run)CancelCooperativeStage(host.script);
    host={};client={};clockRun=phaseRun=0;phase=0;pausedTime=gateTime=0;armed.clear();ackSent=completedRun=reportAt=hostReportAt=0;completedToken=0;transitionPending=recoveryPending=false;
}
bool LocalStoryTransitionPending(){return transitionPending;}
void SetLocalStoryTransitionPending(bool pending){transitionPending=pending;}
void SetLocalStoryRecoveryPending(bool pending){recoveryPending=pending;}
bool LocalStoryRecoveryReady(){return !transitionPending&&(!client.run||DependenciesReady(false));}

}
