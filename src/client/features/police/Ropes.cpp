#include <map>
#include "entities/ped/Peds.h"
#include "shared/lib/sync/PopulationShare.h"
#include "entities/player/Players.h"
#include "shared/game/Anim.h"
#include "shared/api/net-session/Session.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "common/lib/util/Util.h"
namespace coop {namespace {
using namespace game;
struct Landing {float z=0;uint64_t seen=0;bool valid=false,held=false,gravity=false;uint32_t aiFrame=0;};
std::map<int,Landing>landings;CPed*descending=nullptr;
using GroundFn=void(__cdecl*)(CVector*);GroundFn originalGround=nullptr;
void __cdecl RopeGround(CVector*point){
    auto before=*point;originalGround(point);if(!descending)return;
    auto&landing=landings[GetPedRef(descending)];landing.seen=NowMs();bool found=false;
    Call<0x5696C0,float>(before.x,before.y,before.z+1,&found,(CEntity**)nullptr);
    if(found){landing.z=point->z;landing.valid=true;}
    else if(landing.valid)point->z=landing.z;
}
using ControlFn=void*(__fastcall*)(void*,void*,CPed*);ControlFn originalControl=nullptr;
void*__fastcall RopeControl(void*task,void*edx,CPed*ped){auto*previous=descending;descending=Net().Online()?ped:nullptr;auto*result=originalControl(task,edx,ped);descending=previous;return result;}
using IntelligenceFn=void(__fastcall*)(void*,void*);IntelligenceFn originalIntelligence=nullptr;
void __fastcall Intelligence(void*self,void*edx){auto*p=Field<CPed*>(self,0);if(p&&IsPedValid(p)){auto i=landings.find(GetPedRef(p));if(i!=landings.end())i->second.aiFrame=Ref<uint32_t>(addr::FrameCounter);}originalIntelligence(self,edx);}
void MaintainLandings(){
    for(auto i=landings.begin();i!=landings.end();){auto&[handle,landing]=*i;auto*p=GetPed(handle);auto*n=p?Peds::Instance().Find(p):nullptr;
        bool keep=n&&n->owner==Net().MyId()&&p->Health()>0&&PopulationNeededAt(p->GetPosition(),AreaOf(p),140);
        if(!keep){if(p&&landing.held&&landing.gravity)p->PhysicalFlags()|=2;i=landings.erase(i);continue;}
        bool outside=AreaOf(p)!=AreaOf(FindPlayerPed(0));
        if(outside&&landing.valid&&!p->InVehicle()&&!anim::FindTask(p,108)){
            if(!landing.held){landing.held=true;landing.gravity=(p->PhysicalFlags()&2)!=0;}
            p->PhysicalFlags()&=~2u;if(std::fabs(p->GetPosition().z-landing.z)>.01f){auto at=p->GetPosition();at.z=std::max(landing.z,at.z-.08f*TimeStep());SetEntityPosition(p,at);}p->MoveSpeed().z=0;
        }else if(landing.held){if(landing.gravity)p->PhysicalFlags()|=2;landing.held=false;}
        ++i;
    }
}
void ContinueOffAreaAI(){
    auto frame=Ref<uint32_t>(addr::FrameCounter);for(auto&[handle,l]:landings)if(l.held&&l.aiFrame!=frame)if(auto*p=GetPed(handle);p&&p->Intelligence()){
        l.aiFrame=frame;originalIntelligence(p->Intelligence(),nullptr);
        if(p->RwObject())Call<0x4D34F0>(p->RwObject(),TimeStep()/50.f,true);
    }
}
using RenderFn=void(__cdecl*)();RenderFn original=nullptr;
void __cdecl Render(){
    original();if(!Net().Online()||!core::IsPlayerReady())return;
    for(auto&[id,n]:Peds::Instance().All())if(n.owner!=Net().MyId()&&n.hasState&&n.state.interaction.rope&&NowMs()-n.recvMs<1500&&n.ped&&IsPedValid(n.ped)&&n.ped->IsVisible()&&AreaOf(n.ped)==AreaOf(FindPlayerPed(0))){
        // A visual CRope uses the owner's sampled curve, without occupying
        // one of GTA's eight physical ropes or running a second abseil task.
        alignas(4)uint8_t rope[0x328]{};auto*segments=(CVector*)rope;auto&p=n.state.interaction.ropePoints;
        for(int i=0;i<32;++i){float t=i/31.f*3;int a=std::min(2,(int)t);segments[i]=p[a]+(p[a+1]-p[a])*(t-a);}
        rope[0x325]=8;CallMethod<0x556800>(rope);
    }
}
class Ropes:public Module{const char*Name()const override{return "PoliceRopeVisuals";}
    void OnPreProcess()override{if(Net().Online()&&core::IsPlayerReady())MaintainLandings();}
    void OnPostProcess()override{if(Net().Online()&&core::IsPlayerReady())ContinueOffAreaAI();}
    void OnGameShutdown()override{landings.clear();descending=nullptr;}
    void OnSessionEnd()override{for(auto&[handle,l]:landings)if(auto*p=GetPed(handle);p&&l.held&&l.gravity)p->PhysicalFlags()|=2;landings.clear();}
    void OnInstallHooks()override{mem::HookFn(0x608260,&Intelligence,&originalIntelligence,"CPedIntelligence::Process abseil continuity");mem::HookFn(0x65A460,&RopeControl,&originalControl,"CTaskComplexUseSwatRope retained landing floor");mem::RedirectCallFn(0x65A540,&RopeGround,&originalGround);mem::HookFn(0x556AE0,&Render,&original,"CRopes::Render authoritative abseil curve");}
}feature;struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}}
