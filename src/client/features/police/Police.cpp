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
        }else PlayerInFocus()=0;
    }
    ~Context(){slot.reset();PlayerInFocus()=savedFocus;activeTarget=savedTarget;}
};
bool AmbientCop(CPed*p){
    if(!p||p->PedType()!=PED_TYPE_COP||p->CreatedBy()!=1)return false;
    auto*n=Peds::Instance().Find(p);return !n||n->owner==Net().MyId();
}
// Damage events are cloned/informed to witnesses after the damage callback
// returns. Restoring CWanted around that callback cannot suppress a later
// copy's ReportCriminalEvent, which always charges FindPlayerWanted().
using CrimeEventFn=void(__fastcall*)(void*,void*,CPed*);
CrimeEventFn originalDamageCrime=nullptr;
void __fastcall DamageCrime(void*event,void*edx,CPed*witness){
    if(Net().Online()&&core::IsPlayerReady()){
        auto*source=CallMethod<0x4ADA70,CEntity*>(event);
        if(source&&source->IsPed()&&Players::Instance().FindByPed((CPed*)source)){
            LOGD("Police replica damage crime suppressed source=%u witness=%d",Players::Instance().FindByPed((CPed*)source)->id,witness?GetPedRef(witness):-1);
            return; // the shooter's game reports this crime once
        }
        // A witness may report this event from inside another suspect's AI
        // callback. Native ReportCriminalEvent uses the focused slot, not the
        // event's player. Keep a local offender's crime in their own slot.
        if(source==FindPlayerPed(0)){
            Context c(FindPlayerPed(0));originalDamageCrime(event,edx,witness);return;
        }
    }
    originalDamageCrime(event,edx,witness);
}
using StolenCarFn=void(__fastcall*)(void*,void*,void*,void*,void*);
StolenCarFn originalStolenCar=nullptr;
void __fastcall StolenCopCar(void*handler,void*edx,void*event,void*active,void*simple){
    auto*hijacker=Field<CPed*>(event,0x0C);
    if(Net().Online()&&hijacker&&(hijacker==FindPlayerPed(0)||Players::Instance().FindByPed(hijacker))){
        Context c((CPlayerPed*)hijacker);originalStolenCar(handler,edx,event,active,simple);return;
    }
    originalStolenCar(handler,edx,event,active,simple);
}
void WantedChange(void*w,uint32_t before,const char*source,void*caller){
    if(!w)return;
    auto*me=FindPlayerPed(0);if(!me||!IsPedValid(me)||!me->PlayerData()||me->Wanted()!=w)return;
    uint32_t after=Field<uint32_t>(w,0x2C);if(after==before)return;
    auto pos=me->GetPosition();auto*v=me->VehicleIfIn();
    LOGI("Police local wanted %u -> %u source=%s caller=%p chaos=%u focus=%d position=%.1f,%.1f,%.1f vehicle=%d",
        before,after,source,caller,Field<uint32_t>(w,0),PlayerInFocus(),pos.x,pos.y,pos.z,v?GetVehicleRef(v):-1);
}
using WantedFn=void(__fastcall*)(void*,void*,uint32_t);WantedFn originalWanted=nullptr,originalNoDrop=nullptr;
void __fastcall WantedLevel(void*w,void*edx,uint32_t level){
    auto before=Field<uint32_t>(w,0x2C);originalWanted(w,edx,level);WantedChange(w,before,"set",__builtin_return_address(0));
}
void __fastcall WantedNoDrop(void*w,void*edx,uint32_t level){
    auto before=Field<uint32_t>(w,0x2C);originalNoDrop(w,edx,level);WantedChange(w,before,"no-drop",__builtin_return_address(0));
}
using UpdateWantedFn=void(__fastcall*)(void*,void*);UpdateWantedFn originalUpdateWanted=nullptr;
void __fastcall UpdateWanted(void*w,void*edx){
    auto before=Field<uint32_t>(w,0x2C);originalUpdateWanted(w,edx);WantedChange(w,before,"chaos",__builtin_return_address(0));
}
using CopFn=void(__fastcall*)(CPed*,void*);CopFn originalCop=nullptr;
void __fastcall CopControl(CPed*p,void*edx){
    if(!AmbientCop(p))return originalCop(p,edx);
    Context c(SelectTarget(p));originalCop(p,edx);
}
using IntelFn=void(__fastcall*)(void*,void*);IntelFn originalIntel=nullptr;
void __fastcall Intelligence(void*intel,void*edx){
    auto*p=Field<CPed*>(intel,0);
    ObservePoliceRopeIntelligence(p);
    if(!AmbientCop(p))return originalIntel(intel,edx);
    Context c(SelectTarget(p));originalIntel(intel,edx);
}
using CarFn=void(__cdecl*)(CVehicle*);CarFn originalCar=nullptr;
void __cdecl CarAI(CVehicle*v){
    ProcessPoliceVehicle(v,originalCar);
}
using PursuitFn=bool(__fastcall*)(void*,void*,CPed*);PursuitFn originalPursuit=nullptr;
bool __fastcall SetPursuit(void*t,void*edx,CPed*cop){
    if(!Net().Online()||!AmbientCop(cop))return originalPursuit(t,edx,cop);
    auto*target=activeTarget?activeTarget:SelectTarget(cop);
    // Native SetPursuit picks the closest of two slots, even if that player
    // is innocent. Use the suspect whose wanted context runs this cop.
    auto*&old=Field<CEntity*>(t,0x14);
    if(old)CallMethod<0x571A00>(old,&old);old=nullptr;
    if(!Available(target,cop->GetPosition(),AreaOf(cop)))return false;
    if(!CallMethod<0x563060,bool>(target->Wanted(),cop))return false;
    old=target;CallMethod<0x571B70>(old,&old);
    LOGD("Police pursuit suspect=%u cop=%d",Players::Instance().FindByPed(target)?Players::Instance().FindByPed(target)->id:Net().MyId(),GetPedRef(cop));
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
using PersistFn=bool(__fastcall*)(void*,void*,CPed*);PersistFn originalPersist=nullptr;
bool __fastcall PersistPursuit(void*t,void*edx,CPed*cop){
    if(Net().Online()&&AmbientCop(cop)){
        auto*p=Field<CPed*>(t,0x14);
        if(p&&(p==FindPlayerPed(0)||Players::Instance().FindByPed(p))&&
           !Available((CPlayerPed*)p,cop->GetPosition(),AreaOf(cop))){
            ClearPursuit(cop);return false;
        }
    }
    return originalPersist(t,edx,cop);
}
void __fastcall CopDestructor(CPed*cop,void*edx){
    ClearPursuit(cop);originalDestructor(cop,edx);
}
class Police:public Module {
    const char*Name()const override{return "Police";}
    void OnGameStarted()override{
        auto*me=FindPlayerPed(0);if(me&&me->PlayerData()&&me->Wanted())
            LOGI("Police world loaded: local wanted=%u chaos=%u",Field<uint32_t>(me->Wanted(),0x2C),Field<uint32_t>(me->Wanted(),0));
    }
    void OnInstallHooks()override{
        mem::HookFn(0x5DE160,&CopControl,&originalCop,"CCopPed::ProcessControl");
        mem::HookFn(0x4B3440,&DamageCrime,&originalDamageCrime,"CEventDamage::ReportCriminalEvent personal offender");
        mem::HookFn(0x4BB740,&StolenCopCar,&originalStolenCar,"CEventHandler::ComputeCopCarBeingStolenResponse personal offender");
        mem::HookFn(0x562470,&WantedLevel,&originalWanted,"CWanted::SetWantedLevel");
        mem::HookFn(0x562570,&WantedNoDrop,&originalNoDrop,"CWanted::SetWantedLevelNoDrop");
        mem::HookFn(0x561C90,&UpdateWanted,&originalUpdateWanted,"CWanted::UpdateWantedLevel");
        mem::HookFn(0x608260,&Intelligence,&originalIntel,"CPedIntelligence::Process");
        mem::HookFn(0x41DA30,&CarAI,&originalCar,"CCarAI::UpdateCarAI");
        mem::HookFn(0x68BBD0,&SetPursuit,&originalPursuit,"CTaskComplexPolicePursuit::SetPursuit");
        mem::HookFn(0x68BDC0,&PersistPursuit,&originalPersist,"CTaskComplexPolicePursuit::PersistPursuit");
        mem::HookFn(0x68BD90,&ClearPursuit,&originalClear,"CTaskComplexPolicePursuit::ClearPursuit");
        mem::HookFn(0x5DE0D0,&CopDestructor,&originalDestructor,"CCopPed::~CCopPed");
    }
}feature;struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
}
void ProcessPolicePedIntelligence(game::CPed*ped){
    if(ped&&game::IsPedValid(ped)&&ped->Intelligence()&&originalIntel)
        Intelligence(ped->Intelligence(),nullptr);
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
