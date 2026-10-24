// Ambient police simulate in the population owner's game. A guest's stars
// must reach native AI as well as the name tag, without becoming our stars.
#include <algorithm>
#include <optional>
#include "entities/player/Players.h"
#include "entities/ped/Peds.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/police/Police.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Anim.h"
#include "shared/game/PlayerContext.h"
#include "shared/lib/memory/Memory.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"

namespace coop { namespace {
using namespace game;
CPlayerPed* activeTarget=nullptr;

void SyncWanted(RemotePlayer& rp){
    if(!rp.ped||!rp.ped->PlayerData()||!rp.ped->Wanted())return;
    auto*w=rp.ped->Wanted();bool fresh=rp.hasState&&NowMs()-rp.stateRecvMs<2000&&rp.state.health>0;
    unsigned level=fresh?std::min<unsigned>(rp.state.wanted,6):0;
    static constexpr uint32_t chaos[]={0,50,180,550,1200,2400,4600};
    static constexpr uint8_t cops[]={0,1,3,4,6,8,10},cars[]={0,1,2,2,2,3,3};
    // Do not call SetWantedLevel: it changes this game's stats and scanner.
    Field<uint32_t>(w,0)=chaos[level];Field<uint32_t>(w,0x2C)=level;
    Field<uint8_t>(w,0x1E)=(Field<uint8_t>(w,0x1E)&~7u)|(fresh?rp.state.policeFlags:7);
    bool backOff=(Field<uint8_t>(w,0x1E)&7)!=0;
    Field<uint8_t>(w,0x19)=backOff?0:cops[level];Field<uint8_t>(w,0x1A)=backOff?0:cars[level];
}
bool Available(CPlayerPed*p,const CVector&point,int8_t area,bool requireWanted=true){
    if(!p||!IsPedValid(p)||p->Health()<=0||!p->PlayerData()||!p->Wanted()||AreaOf(p)!=area)return false;
    if(auto*rp=Players::Instance().FindByPed(p))if(!rp->hasState||NowMs()-rp->stateRecvMs>2000)return false;
    return (p->GetPosition()-point).Length()<160&&(!requireWanted||
        (Field<uint32_t>(p->Wanted(),0x2C)>0&&!(Field<uint8_t>(p->Wanted(),0x1E)&7)));
}
CPlayerPed* SelectTarget(CEntity* source,bool generation=false){
    if(!Net().Online()||!core::IsPlayerReady()||core::IsRestarting())return nullptr;
    auto*me=FindPlayerPed(0);if(!me)return nullptr;
    for(const auto&[id,_]:Players::Instance().All())if(auto*rp=Players::Instance().Get(id))SyncWanted(*rp);
    auto pos=source?source->GetPosition():me->GetPosition();int8_t area=source?(source->IsPed()?AreaOf((CPed*)source):source->AreaCode()):AreaOf(me);
    // Keep a native pursuit's suspect until it ends, including the frame in
    // which their wanted level clears. Otherwise another player's stars can
    // accidentally keep an innocent old target under arrest.
    if(source&&source->IsPed())for(int type:{1103,1106})if(void*t=anim::FindTask((CPed*)source,type)){
        auto*p=Field<CPed*>(t,0x14);
        if(p&&(p==me||Players::Instance().FindByPed(p))&&Available((CPlayerPed*)p,pos,area,false))return (CPlayerPed*)p;
    }
    CPlayerPed*best=nullptr;float score=1e9f;
    auto consider=[&](CPlayerPed*p){if(!Available(p,pos,area))return;
        float distance=(p->GetPosition()-pos).Length();
        float value=generation?-1000.f*Field<uint32_t>(p->Wanted(),0x2C)+distance:distance;
        if(value<score){best=p;score=value;}};
    consider(me);for(const auto&[id,rp]:Players::Instance().All())consider(rp.ped);
    return best;
}
class Context {
    int8_t savedFocus=PlayerInFocus();CPlayerPed*savedTarget=activeTarget;
    std::optional<PlayerSlotContext> slot;
public:
    explicit Context(CPlayerPed*target){
        activeTarget=target;
        if(auto*rp=target?Players::Instance().FindByPed(target):nullptr){
            slot.emplace(*PlayerInfo(1),rp->info);PlayerInFocus()=1;
        }
    }
    ~Context(){slot.reset();PlayerInFocus()=savedFocus;activeTarget=savedTarget;}
};
bool AmbientCop(CPed*p){
    if(!p||p->PedType()!=PED_TYPE_COP||p->CreatedBy()!=1)return false;
    auto*n=Peds::Instance().Find(p);return !n||n->owner==Net().MyId();
}
using CopFn=void(__fastcall*)(CPed*,void*);CopFn originalCop=nullptr;
void __fastcall CopControl(CPed*p,void*edx){
    if(!AmbientCop(p))return originalCop(p,edx);
    Context c(SelectTarget(p));originalCop(p,edx);
}
using IntelFn=void(__fastcall*)(void*,void*);IntelFn originalIntel=nullptr;
void __fastcall Intelligence(void*intel,void*edx){
    auto*p=Field<CPed*>(intel,0);
    if(!AmbientCop(p))return originalIntel(intel,edx);
    Context c(SelectTarget(p));originalIntel(intel,edx);
}
using CarFn=void(__cdecl*)(CVehicle*);CarFn originalCar=nullptr;
void __cdecl CarAI(CVehicle*v){
    ProcessPoliceVehicle(v,originalCar);
}
using PursuitFn=bool(__fastcall*)(void*,void*,CPed*);PursuitFn originalPursuit=nullptr;
bool __fastcall SetPursuit(void*t,void*edx,CPed*cop){
    if(!activeTarget)return originalPursuit(t,edx,cop);
    // Native SetPursuit picks the closest of two slots, even if that player
    // is innocent. Use the suspect whose wanted context runs this cop.
    auto*&old=Field<CEntity*>(t,0x14);
    if(old)CallMethod<0x571A00>(old,&old);old=nullptr;
    if(!CallMethod<0x563060,bool>(activeTarget->Wanted(),cop))return false;
    old=activeTarget;CallMethod<0x571B70>(old,&old);
    LOGD("Police pursuit suspect=%u cop=%d",Players::Instance().FindByPed(activeTarget)?Players::Instance().FindByPed(activeTarget)->id:Net().MyId(),GetPedRef(cop));
    return true;
}
using ClearFn=void(__stdcall*)(CPed*);ClearFn originalClear=nullptr;
void __stdcall ClearPursuit(CPed*cop){
    originalClear(cop);
    // Native pursuit lists contain raw pointers. Task/destructor cleanup may
    // run outside a borrowed player context: remove this cop from every list.
    for(const auto&[id,rp]:Players::Instance().All())if(rp.ped&&IsPedValid(rp.ped)&&rp.ped->PlayerData()&&rp.ped->Wanted())
        CallMethod<0x562C10>(rp.ped->Wanted(),cop);
}
CopFn originalDestructor=nullptr;
void __fastcall CopDestructor(CPed*cop,void*edx){
    ClearPursuit(cop);originalDestructor(cop,edx);
}
class Police:public Module {
    const char*Name()const override{return "Police";}
    void OnInstallHooks()override{
        mem::HookFn(0x5DE160,&CopControl,&originalCop,"CCopPed::ProcessControl");
        mem::HookFn(0x608260,&Intelligence,&originalIntel,"CPedIntelligence::Process");
        mem::HookFn(0x41DA30,&CarAI,&originalCar,"CCarAI::UpdateCarAI");
        mem::HookFn(0x68BBD0,&SetPursuit,&originalPursuit,"CTaskComplexPolicePursuit::SetPursuit");
        mem::HookFn(0x68BD90,&ClearPursuit,&originalClear,"CTaskComplexPolicePursuit::ClearPursuit");
        mem::HookFn(0x5DE0D0,&CopDestructor,&originalDestructor,"CCopPed::~CCopPed");
    }
}feature;struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}
void GeneratePoliceAwareTraffic(void(*generate)()){
    Context c(SelectTarget(nullptr,true));generate();
}
void ProcessPoliceVehicle(game::CVehicle*v,void(*process)(game::CVehicle*)){
    auto*n=v?Vehicles::Instance().Find(v):nullptr;
    if(!v||!(v->VehicleFlagsLo()&1)||v->CreatedBy()!=1||(n&&Vehicles::Instance().IsMirror(*n)))return process(v);
    Context c(SelectTarget(v->Driver()?static_cast<CEntity*>(v->Driver()):v));process(v);
}
}
