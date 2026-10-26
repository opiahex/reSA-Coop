#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include "shared/api/story-rc-flight/StoryRCFlight.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-scripts/StoryScripts.h"
#include "shared/api/story-stage/StoryStage.h"
#include "shared/api/net-session/Session.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/sync/EventWindow.h"
#include "shared/game/Script.h"
#include "entities/vehicle/Vehicles.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"

namespace coop { namespace {
using namespace game;using game::script::Cmd;
constexpr uint8_t kRCFlight=6,kUsage=1,kBudget=2;
constexpr float kNativeCapacity=390000.f,kNativeRate=20.f;
constexpr uint32_t kFuelCounter=29692;
struct Definition {void*script=nullptr;void*base=nullptr;uint64_t run=0;bool fuelPolled=false;};
Definition definition;
struct Budget {float capacity=kNativeCapacity,left=kNativeCapacity;bool failed=false;};
using BudgetKey=std::tuple<uint16_t,uint64_t,std::string>;
std::map<BudgetKey,Budget> budgets;
struct Binding {
    bool valid=false;StoryRemoteRole role;BudgetKey key;
    float start=kNativeCapacity,lastUsed=0;uint64_t boundAt=0,sent=0;uint32_t ownerTick=0;EventWindow usage;
};
std::array<Binding,3> bindings;
struct LocalBudget {
    bool bound=false,confirmed=false,enabled=false,failed=false,stopped=false;
    uint64_t run=0,carGeneration=0,sourceGeneration=0;uint16_t car=0,source=0;
    uint32_t assignment=0,phase=0,sequence=0,revision=0;uint64_t sent=0;
    float capacity=kNativeCapacity,start=kNativeCapacity,used=0;
};
LocalBudget local;
struct PendingBudget {
    bool valid=false,enabled=false,failed=false;uint64_t run=0,carGeneration=0,sourceGeneration=0;
    uint16_t car=0,source=0;uint32_t assignment=0,phase=0,revision=0,usageSequence=0;
    float capacity=kNativeCapacity,start=kNativeCapacity,ackUsed=0,left=kNativeCapacity;
};
PendingBudget pending;
uint64_t activeRun=0;uint32_t stateSequence=0;

int32_t* LocalPointer(void*script,uint16_t index){
    if(!script||index>=1024||!Field<uint8_t>(script,0xDC))return nullptr;
    alignas(16)uint8_t copy[0xE0];memcpy(copy,script,sizeof copy);
    uint8_t descriptor[3]={3,(uint8_t)index,(uint8_t)(index>>8)};Field<uint8_t*>(copy,0x14)=descriptor;
    auto*p=CallMethod<0x464790,int32_t*>(copy,0);
    return p&&((uintptr_t)p&3)==0&&!((uint8_t*)p>=copy&&(uint8_t*)p<copy+sizeof copy)?p:nullptr;
}
int ReadInt(uint16_t index){auto*p=LocalPointer(definition.script,index);return p?*p:-1;}
float ReadFloat(uint16_t index){auto*p=LocalPointer(definition.script,index);float v=NAN;if(p)memcpy(&v,p,4);return v;}
bool ScriptMatches(void*script){
    return script&&definition.run&&definition.run==StoryRunId()&&script==definition.script&&
        Field<void*>(script,0x10)==definition.base&&Field<uint8_t>(script,0xDC)&&
        !_strnicmp(Field<char[8]>(script,8),"ZERO2",8)&&IsSharedStoryScript(script);
}
bool DefinitionReady(StoryRemoteRole&source){
    if(!Net().IsHost()||!CooperativeRemoteMission("ZERO2")||!ScriptMatches(definition.script)||
       !CooperativeRemoteRole(Net().MyId(),source)||!source.car||source.car->ModelIndex()!=464||
       source.car->AreaCode()!=0||GetVehicle(ReadInt(34))!=source.car)return false;
    float capacity=ReadFloat(60),left=ReadFloat(59);
    if(!std::isfinite(capacity)||std::fabs(capacity-kNativeCapacity)>.1f||!std::isfinite(left)||left< -10000||left>capacity+.1f)return false;
    for(uint16_t index:{96,100,101,110,111,112,113,114,115,116}){int value=ReadInt(index);if(value!=0&&value!=1)return false;}
    return true;
}
bool FlyingStage(){return definition.fuelPolled&&ReadInt(100)==0&&ReadInt(101)==0&&(ReadInt(96)==1||ReadInt(116)==1);}
// Keep identity comparisons explicit: phase changes do not replenish a tank.
bool SameBinding(const Binding&b,const StoryRemoteRole&r){
    return b.valid&&b.role.run==r.run&&b.role.assignment==r.assignment&&b.role.vehicle==r.vehicle&&
        b.role.vehicleGeneration==r.vehicleGeneration&&b.role.source==r.source&&b.role.sourceGeneration==r.sourceGeneration;
}
bool SameLocal(const StoryRemoteRole&r){
    return local.bound&&local.run==r.run&&local.assignment==r.assignment&&local.car==r.vehicle&&
        local.carGeneration==r.vehicleGeneration&&local.source==r.source&&local.sourceGeneration==r.sourceGeneration;
}
bool PendingMatches(const StoryRemoteRole&r){
    return pending.valid&&pending.run==r.run&&pending.assignment==r.assignment&&pending.car==r.vehicle&&
        pending.carGeneration==r.vehicleGeneration&&pending.source==r.source&&pending.sourceGeneration==r.sourceGeneration;
}
BudgetKey Key(const StoryRemoteRole&r,uint8_t player){return {r.source,r.sourceGeneration,Net().PeerName(player)};}
Budget* BudgetFor(const BudgetKey&key,bool create,float initial){
    auto i=budgets.find(key);if(i!=budgets.end())return &i->second;
    if(!create||budgets.size()>=64||std::get<2>(key).empty())return nullptr;
    Budget b;b.left=std::clamp(initial,0.f,kNativeCapacity);return &budgets.emplace(key,b).first->second;
}
Binding* Bind(uint8_t player,const StoryRemoteRole&r){
    if(player>=3)return nullptr;auto&b=bindings[player];
    if(!SameBinding(b,r)){
        auto key=Key(r,player);auto*budget=BudgetFor(key,true,ReadFloat(59));if(!budget)return nullptr;
        b={};b.valid=true;b.role=r;b.key=key;b.start=budget->left;b.boundAt=NowMs();
        LOGI("Story RC fuel bound player=%u car=%04X run=%llu assignment=%u fuel=%.3f failed=%d",player,r.vehicle,(unsigned long long)r.run,r.assignment,b.start,budget->failed);
    }else b.role=r;
    return &b;
}
void WriteRole(Writer&w,const StoryRemoteRole&r){
    w.U32(r.phase);w.U32(r.assignment);w.Entity(ENTITY_VEHICLE,r.vehicle);w.U64(r.vehicleGeneration);
    w.Entity(ENTITY_VEHICLE,r.source);w.U64(r.sourceGeneration);
}
void SendBudget(uint8_t player,Binding&b,Budget&budget,bool reliable=false){
    Writer w(proto::G_STORY_COOP);w.U8(kRCFlight);w.U64(b.role.run);w.U8(kBudget);w.U8(player);WriteRole(w,b.role);
    if(++stateSequence==0)++stateSequence;w.U32(stateSequence);w.F32(budget.capacity);w.F32(b.start);w.F32(b.lastUsed);w.U32(b.usage.newest);w.F32(budget.left);
    w.Bool(FlyingStage());w.Bool(budget.failed);Net().Send(reliable?proto::CH_RELIABLE:proto::CH_STATE,w);b.sent=NowMs();
}
void Clear(){definition={};budgets.clear();bindings={};local={};pending={};activeRun=0;stateSequence=0;}
void ApplyPending(const StoryRemoteRole&r){
    if(!PendingMatches(r)||pending.phase!=CooperativeStoryPhase())return;
    if(local.revision&&(int32_t)(pending.revision-local.revision)<=0)return;
    local.capacity=pending.capacity;local.start=pending.start;local.enabled=pending.enabled;local.failed=pending.failed;
    local.used=std::max(local.used,pending.ackUsed);if((int32_t)(pending.usageSequence-local.sequence)>0)local.sequence=pending.usageSequence;local.revision=pending.revision;local.confirmed=true;pending.valid=false;
}
void StopEmptyAircraft(const StoryRemoteRole&r){
    if(local.stopped||!r.car||!IsVehicleValid(r.car))return;
    // These are ZERO2's native empty-tank commands. The host then retires this
    // participant's finite attempt; it does not fail everybody's mission.
    Cmd(0x0918).I(GetVehicleRef(r.car)).I(0).Run();Cmd(0x01EC).I(GetVehicleRef(r.car)).I(1).Run();local.stopped=true;
}
void SendUsage(const StoryRemoteRole&r,bool urgent){
    Writer w(proto::G_STORY_COOP);w.U8(kRCFlight);w.U64(r.run);w.U8(kUsage);WriteRole(w,r);
    if(++local.sequence==0)++local.sequence;w.U32(local.sequence);w.U32((uint32_t)NowMs());w.F32(local.used);
    Net().Send(urgent?proto::CH_RELIABLE:proto::CH_STATE,w);local.sent=NowMs();
}
void OwnerTick(){
    StoryRemoteRole r;if(!CooperativeRemoteMission("ZERO2")||!CooperativeRemoteRole(Net().MyId(),r)||!r.car||r.car->ModelIndex()!=464)return;
    if(!SameLocal(r)){
        local={};local.bound=true;local.run=r.run;local.car=r.vehicle;local.source=r.source;local.assignment=r.assignment;
        local.carGeneration=r.vehicleGeneration;local.sourceGeneration=r.sourceGeneration;local.phase=r.phase;
    }
    local.phase=r.phase;ApplyPending(r);
    if(local.confirmed&&local.enabled&&!local.failed&&!MenuActive()&&!Ref<uint8_t>(0xB5F851)&&!CooperativeStageWaiting()&&GetPad(0)->DisablePlayerControls==0&&Cmd(0x00E1).I(0).I(16).Run()){
        float step=TimeStep();if(std::isfinite(step)&&step>=0&&step<20){
            float left=std::max(0.f,local.start-local.used);left-=kNativeRate*step;local.used=local.start-std::max(0.f,left);
        }
    }
    if(local.confirmed&&local.enabled&&!local.failed){
        auto point=r.car->GetPosition();int city=-1;Cmd(0x07EF).F(point.x).F(point.y).F(point.z).Out(&city).Run();
        if(city==1||city==3){local.failed=true;Cmd(0x020B).I(GetVehicleRef(r.car)).Run();LOGI("Story RC left native flight region car=%04X city=%d",r.vehicle,city);}
    }
    bool empty=local.confirmed&&(local.failed||local.used>=local.start);
    if(empty&&r.car->Health()>0)StopEmptyAircraft(r);
    if(NowMs()-local.sent>200||(empty&&NowMs()-local.sent>50))SendUsage(r,empty);
}
void HostTick(){
    StoryRemoteRole source;if(!DefinitionReady(source))return;
    for(uint8_t player=0;player<3;++player){
        if(player==Net().MyId())continue;StoryRemoteRole role;
        if(!CooperativeRemoteRole(player,role)||role.source!=source.vehicle||role.sourceGeneration!=source.vehicleGeneration||!role.car||role.car->ModelIndex()!=464)continue;
        auto*b=Bind(player,role);if(!b){RetireCooperativeRemoteRole(player,role.run,role.phase,role.assignment);continue;}
        auto*budget=BudgetFor(b->key,false,0);if(!budget)continue;
        if(FlyingStage()){
            auto*n=Vehicles::Instance().Find(role.vehicle);
            if(n&&n->hasState&&NowMs()-n->recvMs<1500){auto point=role.car->GetPosition();int city=-1;Cmd(0x07EF).F(point.x).F(point.y).F(point.z).Out(&city).Run();if(city==1||city==3)budget->failed=true;}
        }
        if(budget->failed||budget->left<=0){
            budget->failed=true;SendBudget(player,*b,*budget,true);
            RetireCooperativeRemoteRole(player,role.run,role.phase,role.assignment);continue;
        }
        if(NowMs()-b->sent>300)SendBudget(player,*b,*budget,b->sent==0);
    }
}
class Flight:public Module {
public:
    const char*Name()const override{return "StoryRCFlight";}
    void OnPostProcess()override{
        if(!Net().Online()||!core::IsPlayerReady()||core::IsRestarting())return;
        auto run=StoryRunId();if(activeRun!=run){Clear();activeRun=run;}if(!run)return;
        if(Net().IsHost())HostTick();else OwnerTick();
    }
    void OnPeerLeave(uint8_t player)override{if(player<3)bindings[player]={};}
    void OnPeerResync(uint8_t player)override{if(player<3){bindings[player].ownerTick=0;bindings[player].boundAt=NowMs();bindings[player].sent=0;}}
    void OnHostChanged(uint8_t)override{Clear();}
    void OnGameStarted()override{if(!Net().IsHost())Clear();}
    void OnGameShutdown()override{Clear();}
    void OnSessionEnd()override{Clear();}
};
Flight feature;struct Register{Register(){core::RegisterModule(&feature);}}registration;
} // namespace

void ObserveCooperativeRCFlightScript(void*script){
    if(!Net().IsHost()||!StoryRunId()||!script||!Field<uint8_t>(script,0xDC)||
       !IsSharedStoryScript(script)||_strnicmp(Field<char[8]>(script,8),"ZERO2",8))return;
    auto*base=Field<void*>(script,0x10);auto run=StoryRunId();
    if(definition.script!=script||definition.base!=base||definition.run!=run)definition={script,base,run,false};
}
void ObserveCooperativeRCFlightCommand(void*script,int opcode,const int32_t*p){
    if(!p||opcode!=0x00E1||p[0]!=0||p[1]!=16||!ScriptMatches(script))return;
    StoryRemoteRole source;if(DefinitionReady(source)&&!definition.fuelPolled){definition.fuelPolled=true;for(auto&b:bindings)b.sent=0;}
}
bool CooperativeRCFlightPreventsRecovery(uint16_t source,uint64_t generation){
    StoryRemoteRole native;return DefinitionReady(native)&&native.vehicle==source&&native.vehicleGeneration==generation;
}
void CooperativeRCFlightRoleFailed(uint8_t player,uint64_t run,uint32_t phase,uint32_t assignment){
    if(!Net().IsHost()||player>=3||run!=StoryRunId()||phase!=CooperativeStoryPhase())return;
    auto&b=bindings[player];Budget*budget=nullptr;
    if(b.valid){
        if(b.role.run!=run||b.role.assignment!=assignment)return;
        budget=BudgetFor(b.key,false,0);
    }else{
        // The trusted role lifecycle may detect a destroyed copy before its
        // first fuel tick. Preserve that finite attempt across reconnection.
        StoryRemoteRole source;if(!assignment||!DefinitionReady(source))return;
        budget=BudgetFor(Key(source,player),true,ReadFloat(59));
    }
    if(!budget)return;budget->failed=true;
    LOGI("Story RC attempt failed player=%u run=%llu assignment=%u fuel=%.3f",player,(unsigned long long)run,assignment,budget->left);
}
bool CooperativeRCFlightFuel(uint8_t player,float&remaining,float&capacity,bool&failed){
    if(player>=3||!CooperativeRemoteMission("ZERO2")||!StoryRunId())return false;
    if(Net().IsHost()){
        if(player==Net().MyId()){StoryRemoteRole source;if(!DefinitionReady(source))return false;remaining=std::max(0.f,ReadFloat(59));capacity=kNativeCapacity;failed=ReadInt(101)==1;return true;}
        auto&b=bindings[player];if(!b.valid||b.role.run!=StoryRunId())return false;auto*budget=BudgetFor(b.key,false,0);if(!budget)return false;
        remaining=budget->left;capacity=budget->capacity;failed=budget->failed;return true;
    }
    if(player!=Net().MyId()||!local.confirmed||!local.bound||local.run!=StoryRunId())return false;
    remaining=std::max(0.f,local.start-local.used);capacity=local.capacity;failed=local.failed||remaining<=0;return true;
}
bool CooperativeRCFlightCounter(uint32_t offset,int32_t&value){
    if(offset!=kFuelCounter||Net().IsHost())return false;StoryRemoteRole r;
    if(!CooperativeRemoteRole(Net().MyId(),r)||!SameLocal(r))return false;
    float left,capacity;bool failed;if(!CooperativeRCFlightFuel(Net().MyId(),left,capacity,failed)||capacity<=0)return false;
    value=(int32_t)std::clamp(100.f*left/capacity,0.f,100.f);return true;
}
bool CooperativeRCFlightCondition(void*script,int opcode,const int32_t*p,bool&value){
    if(opcode!=0x01AF||!p||!ScriptMatches(script))return false;StoryRemoteRole source;
    if(!DefinitionReady(source)||p[0]!=GetVehicleRef(source.car)||ReadInt(110)!=1||ReadInt(116)!=1||ReadInt(100)!=0||ReadInt(101)!=0)return false;
    for(uint16_t i=111;i<=115;++i)if(ReadInt(i)!=1)return false;
    float f[6];memcpy(f,p+1,sizeof f);const float expected[6]={-2237.310f,140.080f,57.230f,4,4,4};
    for(int i=0;i<6;++i)if(!std::isfinite(f[i])||std::fabs(f[i]-expected[i])>.02f)return false;
    if(p[7]!=1)return false;
    for(uint8_t player=0;player<3;++player){
        if(player==Net().MyId())continue;StoryRemoteRole role;if(!CooperativeRemoteRole(player,role)||role.source!=source.vehicle||role.sourceGeneration!=source.vehicleGeneration)continue;
        auto*b=Bind(player,role);auto*budget=b?BudgetFor(b->key,false,0):nullptr;auto*n=Vehicles::Instance().Find(role.vehicle);
        if(!budget||budget->failed||budget->left<=0||!n||!Vehicles::Alive(*n)||!n->hasState||n->owner!=player||NowMs()-n->recvMs>1500)continue;
        if(Cmd(0x01AF).I(n->handle).F(f[0]).F(f[1]).F(f[2]).F(f[3]).F(f[4]).F(f[5]).I(0).Run()){
            value=true;LOGI("Story RC return accepted player=%u car=%04X run=%llu fuel=%.3f",player,role.vehicle,(unsigned long long)role.run,budget->left);return true;
        }
    }
    return false;
}
void ReceiveCooperativeRCFlight(uint8_t from,uint64_t token,Reader&r){
    auto op=r.U8();uint8_t target=op==kBudget?r.U8():from;
    auto phase=r.U32(),assignment=r.U32();auto car=r.U16();auto carGeneration=r.U64();auto source=r.U16();auto sourceGeneration=r.U64();
    auto sequence=r.U32();
    if(op==kUsage){
        auto tick=r.U32();float used=r.F32();StoryRemoteRole native,role;
        if(!r.Ok()||!Net().IsHost()||from>=3||from==Net().MyId()||!token||token!=StoryRunId()||phase!=CooperativeStoryPhase()||
           !std::isfinite(used)||used<0||used>kNativeCapacity||!DefinitionReady(native)||!CooperativeRemoteRole(from,role)||
           role.run!=token||role.assignment!=assignment||role.vehicle!=car||role.vehicleGeneration!=carGeneration||
           role.source!=source||role.sourceGeneration!=sourceGeneration||source!=native.vehicle||sourceGeneration!=native.vehicleGeneration)return;
        auto*n=Vehicles::Instance().Find(car);if(!n||!Vehicles::Alive(*n)||n->owner!=from||!n->hasState||NowMs()-n->recvMs>1500||
           (int32_t)(n->state.time-tick)>1500||(int32_t)(tick-n->state.time)>1500)return;
        auto*b=Bind(from,role);auto*budget=b?BudgetFor(b->key,false,0):nullptr;if(!budget||used+0.1f<b->lastUsed||used>b->start+.1f)return;
        int64_t elapsed=b->ownerTick?(int32_t)(tick-b->ownerTick):(int64_t)(NowMs()-b->boundAt);
        if(elapsed<0||used-b->lastUsed>std::max<int64_t>(0,elapsed)*4.f+5000.f||!b->usage.Accept(sequence))return;
        b->ownerTick=tick;b->lastUsed=std::max(b->lastUsed,used);budget->left=std::max(0.f,b->start-b->lastUsed);
        if(budget->left<=0)budget->failed=true;
        if(budget->failed){SendBudget(from,*b,*budget,true);RetireCooperativeRemoteRole(from,role.run,role.phase,role.assignment);}
    }else if(op==kBudget){
        PendingBudget state;state.valid=true;state.run=token;state.phase=phase;state.assignment=assignment;state.car=car;state.carGeneration=carGeneration;
        state.source=source;state.sourceGeneration=sourceGeneration;state.revision=sequence;
        state.capacity=r.F32();state.start=r.F32();state.ackUsed=r.F32();state.usageSequence=r.U32();state.left=r.F32();state.enabled=r.Bool();state.failed=r.Bool();
        if(!r.Ok()||Net().IsHost()||from!=Net().HostId()||target!=Net().MyId()||token!=StoryRunId()||phase!=CooperativeStoryPhase()||
           !sequence||!assignment||!carGeneration||!sourceGeneration||Net().ExpectedEntityGeneration(ENTITY_VEHICLE,car)!=carGeneration||
           Net().ExpectedEntityGeneration(ENTITY_VEHICLE,source)!=sourceGeneration||!std::isfinite(state.capacity)||std::fabs(state.capacity-kNativeCapacity)>.1f||
           !std::isfinite(state.start)||state.start<0||state.start>state.capacity||!std::isfinite(state.ackUsed)||state.ackUsed<0||state.ackUsed>state.start+.1f||
           !std::isfinite(state.left)||state.left<0||state.left>state.start+.1f)return;
        if(pending.valid&&(int32_t)(sequence-pending.revision)<=0)return;
        pending=state;
    }
}
} // namespace coop
