#include "entities/ped/Peds.h"
#include "shared/api/net-session/Session.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "common/lib/util/Util.h"
namespace coop {namespace {
using namespace game;using RenderFn=void(__cdecl*)();RenderFn original=nullptr;
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
    void OnInstallHooks()override{mem::HookFn(0x556AE0,&Render,&original,"CRopes::Render authoritative abseil curve");}
}feature;struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}}
