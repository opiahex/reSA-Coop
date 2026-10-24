#include "entities/player/Players.h"
#include "shared/game/Anim.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
namespace coop {namespace {
using Fn=void(__fastcall*)(game::CPed*,void*);Fn original=nullptr;
void __fastcall Footsteps(game::CPed*ped,void*edx){
    // 1.0 US dereferences its first association without checking. A streamed
    // model or a completed action can have no association for this frame.
    if(!ped||!ped->RwObject())return;
    if(!game::anim::First(ped->RwObject())){
        // Retain the remote-player transition recovery at the single hook.
        // NPCs with an unloaded pose safely skip this frame instead.
        if(Players::Instance().FindByPed(ped))game::Call<0x4D4610,void*>(ped->RwObject(),0,3,8.0f);
        if(!game::anim::First(ped->RwObject()))return;
    }
    original(ped,edx);
}
class PedAnimations:public Module{
 const char*Name()const override{return "PedAnimationGuards";}
 void OnInstallHooks()override{mem::HookFn(0x5E57F0,&Footsteps,&original,"CPed::PlayFootSteps (empty animation guard)");}
}feature;
struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}}
