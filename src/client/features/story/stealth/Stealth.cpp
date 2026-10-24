#include <map>
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
    Writer w(proto::G_STEALTH);w.U64(StoryRunId());w.U32(++sequence);w.Entity(ENTITY_PED,victim->netId);w.U8(victim->owner);WriteHitEvidence(w,target);Net().Send(proto::CH_RELIABLE,w);
    LOGI("Native stealth action sent player=%u ped=%04X",Net().MyId(),victim->netId);return done;
}
class Stealth:public Module{const char*Name()const override{return "StoryStealth";}
 void OnInstallHooks()override{
    mem::HookFn(0x62E540,&StealthProcess,&original,"CTaskSimpleStealthKill::ProcessPed");
    Net().SetHandler(proto::G_STEALTH,[](uint8_t from,Reader&r){auto run=r.U64();auto token=r.U32();auto id=r.U16();auto owner=r.U8();auto*ped=Peds::Instance().FindOwned(id);auto*rp=Players::Instance().Get(from);
        if(!ReadHitEvidence(r,ped)||!r.Ok()||owner!=Net().MyId()||run!=StoryRunId()||from>=3||!token||!rp||!rp->ped||!rp->hasState||NowMs()-rp->stateRecvMs>1500||!ped||ped->Health()<=0||ped->InVehicle()||!seen[from].Accept(token))return;
        auto*attacker=rp->ped;auto delta=attacker->GetPosition()-ped->GetPosition();auto*matrix=ped->Matrix();bool fire=false;
        auto proof=ped->PhysicalFlags()&CPhysical::PF_INVULNERABLE;ped->PhysicalFlags()&=~CPhysical::PF_INVULNERABLE;bool damage=CallMethod<0x5448B0,bool>(ped,4,&fire);ped->PhysicalFlags()|=proof;
        if(!damage||!matrix||rp->state.weapon!=4||attacker->Health()<=0||rp->state.area!=AreaOf(ped)||delta.Length()>3.f||delta.Dot(matrix->forward)>.5f||!ped->Intelligence()||anim::FindTask(ped,kStealthTask))return;
        // The native task streams/owns its associations and produces the real
        // death event, including bKilledByStealth used by corpse discovery.
        int index=Call<0x4D3990,int>("knife");if(index>=0&&!anim::BlockLoaded(index))RequestModel(anim::kIfpBase+index,2);
        void*task=Call<0x61A5A0,void*>((size_t)0x20);if(!task)return;
        CallMethod<0x6225F0>(task,false,attacker,kStealthGroup);CallMethod<0x681AF0>((uint8_t*)ped->Intelligence()+4,task,0,false);
        LOGI("Native stealth action accepted player=%u ped=%04X",from,id);
    });
 }
 void OnPostProcess()override{for(auto i=sent.begin();i!=sent.end();)i=NowMs()-i->second.at>15000?sent.erase(i):std::next(i);}
 void OnPeerLeave(uint8_t id)override{if(id<3)seen[id]={};}
 void OnPeerResync(uint8_t id)override{OnPeerLeave(id);}
 void OnGameShutdown()override{sent.clear();for(auto&w:seen)w={};}
 void OnSessionEnd()override{OnGameShutdown();sequence=0;}
}feature;struct Register{Register(){core::RegisterModule(&feature);}}registration;
}}
