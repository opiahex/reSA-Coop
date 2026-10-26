#include "shared/api/story-stage/StoryStage.h"
#include <map>
#include <vector>
#include "shared/api/hit-evidence/HitEvidence.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-actions/StoryActions.h"
#include "shared/api/net-session/Session.h"
#include "entities/player/Players.h"
#include "entities/ped/Peds.h"
#include "entities/mission/Mission.h"
#include "shared/game/Anim.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/sync/EventWindow.h"
#include "common/lib/util/Util.h"
#include "common/lib/log/Log.h"
namespace coop {namespace {
using namespace game;
constexpr int kStealthTask=1027,kStealthGroup=87;
using Process=bool(__fastcall*)(void*,void*,CPed*);Process original=nullptr;
struct Sent{int handle=-1;uint64_t generation=0,at=0;};std::map<void*,Sent>sent;
EventWindow seen[3];uint32_t sequence=0;
struct PendingStealth {
    uint8_t from=255;
    uint16_t victim=0;
    uint32_t world=0,phase=0,token=0;
    uint64_t run=0,generation=0,received=0;
};
std::vector<PendingStealth>pending;
enum class StartResult { Waiting,Started,Rejected };

StartResult StartStealth(const PendingStealth&request){
    auto reject=[&](const char*reason){LOGW("Native stealth action rejected player=%u ped=%04X reason=%s wait=%llu",request.from,request.victim,reason,(unsigned long long)(NowMs()-request.received));return StartResult::Rejected;};
    if(request.world!=Net().WorldEpoch()||request.run!=StoryRunId()||request.phase!=CooperativeStoryPhase()||
       request.generation!=Net().EntityGeneration(ENTITY_PED,request.victim))return reject("context");
    auto*ped=Peds::Instance().FindOwned(request.victim);
    if(!ped||!IsPedValid(ped)||ped->Health()<=0||ped->InVehicle()||!ped->Intelligence())return reject("victim");
    auto*rp=Players::Instance().Get(request.from);
    // Action messages are reliable, while player snapshots and weapon models
    // arrive independently. Retain this exact victim until its attacker exists.
    if(!rp||!rp->ped||!IsPedValid(rp->ped)||!rp->hasState||NowMs()-rp->stateRecvMs>1500)return StartResult::Waiting;
    auto*attacker=rp->ped;
    if(attacker->Health()<=0||rp->state.area!=AreaOf(ped)||attacker->InVehicle())return reject("attacker");
    if(rp->state.weapon!=4||attacker->ActiveWeapon().type!=4)return StartResult::Waiting;
    auto delta=attacker->GetPosition()-ped->GetPosition();auto*matrix=ped->Matrix();
    if(!matrix||delta.Length()>3.f||delta.Dot(matrix->forward)>.5f){LOGD("Native stealth geometry distance=%.3f forward=%.3f state=%.3f,%.3f,%.3f rendered=%.3f,%.3f,%.3f",delta.Length(),matrix?delta.Dot(matrix->forward):0.f,rp->state.pos.x,rp->state.pos.y,rp->state.pos.z,attacker->GetPosition().x,attacker->GetPosition().y,attacker->GetPosition().z);return reject("geometry");}
    if(auto*task=anim::FindTask(ped,kStealthTask))
        return Field<CPed*>(task,0xC)==attacker&&!Field<uint8_t>(task,8)?StartResult::Started:reject("victim-task");
    int index=Call<0x4D3990,int>("knife");
    if(index<0)return reject("knife-block");
    if(!anim::BlockLoaded(index)){RequestModel(anim::kIfpBase+index,2);return StartResult::Waiting;}
    bool fire=false;auto proof=ped->PhysicalFlags()&CPhysical::PF_INVULNERABLE;
    ped->PhysicalFlags()&=~CPhysical::PF_INVULNERABLE;
    bool damage=CallMethod<0x5448B0,bool>(ped,4,&fire);ped->PhysicalFlags()|=proof;
    if(!damage)return reject("melee-proof");
    // The owner executes the real victim task and its damage/death callbacks.
    // The attacker's native knife must exist before those callbacks read it.
    void*task=Call<0x61A5A0,void*>((size_t)0x20);if(!task)return StartResult::Waiting;
    CallMethod<0x6225F0>(task,false,attacker,kStealthGroup);
    CallMethod<0x681AF0>((uint8_t*)ped->Intelligence()+4,task,0,false);
    LOGI("Native stealth action accepted player=%u ped=%04X wait=%llu",request.from,request.victim,
         (unsigned long long)(NowMs()-request.received));
    return StartResult::Started;
}
bool __fastcall StealthProcess(void*task,void*edx,CPed*ped){
    auto*target=Field<CPed*>(task,0xC);bool attacker=Field<uint8_t>(task,8)!=0;
    auto*n=ped?Peds::Instance().Find(ped):nullptr;auto*rp=target?Players::Instance().FindByPed(target):nullptr;
    // The local attack creates a victim task even on a network mirror. Its
    // damage calculator bypasses ordinary weapon forwarding: only the owner
    // may run it. The mirror receives the owner's timed victim animation.
    if(Net().Online()&&n&&n->owner!=Net().MyId()&&!attacker){
        CallMethod<0x6226F0,bool>(task,ped,0,(void*)nullptr);
        return true;
    }
    bool owned=n&&n->owner==Net().MyId()&&!attacker&&rp;
    bool onlyPlayer=owned&&(ped->PhysicalFlags()&CPhysical::PF_INVULNERABLE);
    if(onlyPlayer)ped->PhysicalFlags()&=~CPhysical::PF_INVULNERABLE;
    float before=ped?ped->Health():0;bool done=original(task,edx,ped);
    if(onlyPlayer)ped->PhysicalFlags()|=CPhysical::PF_INVULNERABLE;
    if(owned&&before>ped->Health()){if(ped->Health()<=0)Field<uint8_t>(ped,0x475)|=1u;RecordStoryDamage(ped,rp->id,4);}
    if(!Net().Online()||ped!=FindPlayerPed(0)||!attacker||!target||!IsPedValid(target)||!Field<void*>(task,0x18)||Field<uint8_t>(task,0x14)||ped->ActiveWeapon().type!=4)return done;
    auto*victim=Peds::Instance().Find(target);if(!victim||victim->owner==Net().MyId()||target->Health()<=0)return done;
    auto birth=Net().EntityGeneration(ENTITY_PED,victim->netId);auto i=sent.find(task);
    if(i!=sent.end()&&i->second.handle==victim->handle&&i->second.generation==birth&&NowMs()-i->second.at<10000)return done;
    if(sent.size()>=64)sent.erase(sent.begin());sent[task]={victim->handle,birth,NowMs()};
    Writer w(proto::G_STEALTH);w.U64(StoryRunId());w.U32(CooperativeStoryPhase());w.U32(++sequence);w.Entity(ENTITY_PED,victim->netId);w.U8(victim->owner);WriteHitEvidence(w,target);Net().Send(proto::CH_RELIABLE,w);
    LOGI("Native stealth action sent player=%u ped=%04X",Net().MyId(),victim->netId);return done;
}
class Stealth:public Module{const char*Name()const override{return "StoryStealth";}
 void OnInstallHooks()override{
    mem::HookFn(0x62E540,&StealthProcess,&original,"CTaskSimpleStealthKill::ProcessPed");
    Net().SetHandler(proto::G_STEALTH,[](uint8_t from,Reader&r){auto run=r.U64();auto phase=r.U32();auto token=r.U32();auto id=r.U16();auto owner=r.U8();auto*ped=Peds::Instance().FindOwned(id);
        if(!ReadHitEvidence(r,ped)||!r.Ok()||owner!=Net().MyId()||run!=StoryRunId()||phase!=CooperativeStoryPhase()||
           from>=3||from==Net().MyId()||!token||!ped||ped->Health()<=0||ped->InVehicle()||!seen[from].Accept(token)){
            LOGD("Native stealth envelope rejected player=%u ped=%04X owner=%u local=%u run=%llu current=%llu phase=%u currentPhase=%u token=%u reader=%u",from,id,owner,Net().MyId(),(unsigned long long)run,(unsigned long long)StoryRunId(),phase,CooperativeStoryPhase(),token,r.Ok());return;
        }
        PendingStealth request{from,id,Net().WorldEpoch(),phase,token,run,Net().EntityGeneration(ENTITY_PED,id),NowMs()};
        if(StartStealth(request)==StartResult::Waiting&&pending.size()<32){
            pending.push_back(request);LOGD("Native stealth dependency queued player=%u ped=%04X",from,id);
        }
    });
 }
 void OnPostProcess()override{
    for(auto i=sent.begin();i!=sent.end();)i=NowMs()-i->second.at>15000?sent.erase(i):std::next(i);
    for(auto i=pending.begin();i!=pending.end();){
        bool expired=NowMs()-i->received>2500;
        if(expired){LOGW("Native stealth dependency expired player=%u ped=%04X",i->from,i->victim);i=pending.erase(i);}
        else if(StartStealth(*i)!=StartResult::Waiting)i=pending.erase(i);else ++i;
    }
 }
 void OnPeerLeave(uint8_t id)override{
    if(id<3)seen[id]={};sent.clear();
    pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const PendingStealth&r){return r.from==id;}),pending.end());
 }
 void OnPeerResync(uint8_t id)override{OnPeerLeave(id);}
 void OnGameShutdown()override{sent.clear();pending.clear();for(auto&w:seen)w={};}
 void OnSessionEnd()override{OnGameShutdown();sequence=0;}
}feature;struct Register{Register(){core::RegisterModule(&feature);}}registration;
}}
