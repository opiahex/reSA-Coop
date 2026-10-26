// One native gang war runs in the host world. Kill confirmation comes from
// the NPC's simulator, while the native zone/wave checks use its participant.
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "shared/api/gangwar/GangWarCoop.h"
#include "shared/api/net-session/Session.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-stage/StoryStage.h"
#include "shared/game/Game.h"
#include "shared/game/PlayerContext.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/sync/EventWindow.h"

namespace coop { namespace {
using namespace game;
constexpr uintptr_t kProvocation=0x96AB5C,kState=0x96AB64,kState2=0x96AB4C;
constexpr uintptr_t kZone=0x96AB88,kZoneInfo=0x96AB8C,kZoneInfos=0xBA1DF0;
constexpr uintptr_t kActive=0x96AB90,kArea=0xB72914;
using VoidFn=void(__cdecl*)();
using KillFn=void(__cdecl*)(int);
VoidFn originalUpdate=nullptr;
KillFn originalAddKill=nullptr;
using TellFn=void(__cdecl*)(bool);
TellFn originalTellGangs=nullptr;
VoidFn originalClearStreets=nullptr;

struct Provocation {int zone=-1;float value=0;};
std::array<Provocation,3> provocations;
EventWindow receivedKills[3];
std::map<std::pair<uint16_t,uint64_t>,uint64_t> confirmedDeaths;
uint32_t killSequence=0,stateSequence=0,latestState=0;
uint32_t contextWorld=0,contextPhase=0;
uint64_t contextRun=0,lastStateSent=0,stateReceived=0;
uint8_t warLeader=255;
bool addingConfirmedKill=false;

struct Battle {
    uint8_t leader=255,state=0,defence=0;
    int16_t zone=-1,minX=0,minY=0,maxX=0,maxY=0;
    bool Fighting()const{return state!=0||defence==2;}
    bool Contains(const CVector&p,int8_t area)const{
        return Fighting()&&area==0&&std::isfinite(p.x)&&std::isfinite(p.y)&&
            p.x>=minX-50.f&&p.x<=maxX+50.f&&p.y>=minY-50.f&&p.y<=maxY+50.f;
    }
} battle;

// CTheZones::GetZoneInfo takes a reference rather than a vector value.
int NativeZoneIndex(const CVector&position){
    void*zone=nullptr;
    auto fn=reinterpret_cast<void*(__cdecl*)(const CVector&,void**)>(0x572400);
    void*info=fn(position,&zone);
    auto offset=(uintptr_t)info-kZoneInfos;
    int count=std::clamp<int>(Ref<int16_t>(0xBA1DE8),0,380);
    return info&&offset%0x11==0&&offset/0x11<(uintptr_t)count?(int)(offset/0x11):-1;
}

CPlayerPed* Participant(uint8_t id){
    if(id==Net().MyId()){
        auto*p=FindPlayerPed(0);
        return p&&IsPedValid(p)&&p->Health()>0&&AreaOf(p)==0?p:nullptr;
    }
    auto*rp=Players::Instance().Get(id);
    return rp&&rp->ped&&IsPedValid(rp->ped)&&rp->hasState&&rp->state.health>0&&
        rp->state.area==0&&NowMs()-rp->stateRecvMs<2000?rp->ped:nullptr;
}

// Borrow a native player slot only for the gang-war engine call. This never
// changes either player's physical location, camera, pad or real wanted data.
struct ParticipantContext {
    int8_t focus=PlayerInFocus();int area=Ref<int32_t>(kArea);
    std::optional<PlayerSlotContext> slot;
    explicit ParticipantContext(uint8_t id){
        if(id!=Net().MyId())if(auto*rp=Players::Instance().Get(id)){
            slot.emplace(*PlayerInfo(1),rp->info);PlayerInFocus()=1;
        }
        if(!slot)PlayerInFocus()=0;
        auto*player=id==Net().MyId()?FindPlayerPed(0):Participant(id);
        if(player)Ref<int32_t>(kArea)=AreaOf(player);
    }
    ~ParticipantContext(){slot.reset();PlayerInFocus()=focus;Ref<int32_t>(kArea)=area;}
};

void Clear(){
    provocations={};for(auto&w:receivedKills)w={};confirmedDeaths.clear();
    battle={};warLeader=255;lastStateSent=stateReceived=0;latestState=0;
}
void Fence(){
    uint32_t world=Net().WorldEpoch(),phase=CooperativeStoryPhase();uint64_t run=StoryRunId();
    if(world!=contextWorld||run!=contextRun||phase!=contextPhase){
        // A mission's specific-zone rules are evaluated afresh. Its old kills
        // must not provoke the next free-roam or story stage.
        provocations={};if(world!=contextWorld)confirmedDeaths.clear();
        contextWorld=world;contextRun=run;contextPhase=phase;
    }
}

Battle CaptureBattle(){
    Battle next;next.leader=warLeader;next.state=(uint8_t)std::clamp(Ref<int32_t>(kState),0,6);
    next.defence=(uint8_t)std::clamp(Ref<int32_t>(kState2),0,2);
    auto*zone=Ref<void*>(kZone);auto*info=Ref<void*>(kZoneInfo);
    auto offset=(uintptr_t)info-kZoneInfos;
    int count=std::clamp<int>(Ref<int16_t>(0xBA1DE8),0,380);
    if(zone&&info&&offset%0x11==0&&offset/0x11<(uintptr_t)count){
        next.zone=(int16_t)(offset/0x11);
        next.minX=Field<int16_t>(zone,0x10);next.minY=Field<int16_t>(zone,0x12);
        next.maxX=Field<int16_t>(zone,0x16);next.maxY=Field<int16_t>(zone,0x18);
    }else if(next.Fighting())return {};
    return next;
}

bool InWarArea(CPed*ped){
    const auto current=CaptureBattle();const auto position=ped->GetPosition();
    return current.zone>=0&&AreaOf(ped)==0&&position.x>=current.minX-50.f&&position.x<=current.maxX+50.f&&
        position.y>=current.minY-50.f&&position.y<=current.maxY+50.f;
}

// Native gang-war commands scan the entire ped pool. With several independently
// streamed player areas that includes unrelated streets across the map. Keep
// their eligibility local to this battle during the synchronous native scan.
template<class Eligible,class Invoke>
void InWarAreaOnly(Eligible eligible,int replacement,Invoke invoke){
    std::vector<std::pair<CPed*,int>> outside;
    auto*pool=PedPool();
    for(int i=0;pool&&i<pool->size;++i){
        if(pool->byteMap[i]<0)continue;
        auto*ped=(CPed*)(pool->objects+(size_t)i*kPedPoolElemSize);
        if(!ped->IsPlayer()&&eligible(ped->PedType())&&!InWarArea(ped)){
            outside.push_back({ped,ped->PedType()});ped->PedType()=replacement;
        }
    }
    invoke();for(auto[ped,type]:outside)if(IsPedValid(ped))ped->PedType()=type;
}
void __cdecl TellGangs(bool ending){
    if(!Net().Online()||!Net().IsHost())return originalTellGangs(ending);
    InWarAreaOnly([](int type){return type>=PED_TYPE_GANG1&&type<=PED_TYPE_GANG10;},PED_TYPE_CIVMALE,[&]{originalTellGangs(ending);});
}
void __cdecl ClearStreets(){
    if(!Net().Online()||!Net().IsHost())return originalClearStreets();
    InWarAreaOnly([](int type){return type==PED_TYPE_CIVMALE||type==PED_TYPE_CIVFEMALE;},PED_TYPE_GANG2,[]{originalClearStreets();});
}
void PublishBattle(bool force=false){
    if(!force&&NowMs()-lastStateSent<500)return;
    lastStateSent=NowMs();battle=CaptureBattle();
    Writer w(proto::G_GANG_WAR);w.U8(1);w.U32(++stateSequence);
    w.U8(battle.leader);w.U8(battle.state);w.U8(battle.defence);w.I16(battle.zone);
    w.I16(battle.minX);w.I16(battle.minY);w.I16(battle.maxX);w.I16(battle.maxY);
    Net().Send(proto::CH_RELIABLE,w);
}

void AcceptKill(uint8_t owner,uint8_t killer,uint16_t victim,uint64_t generation,int type,const CVector&position){
    if(!Net().IsHost()||owner>=3||killer>=3||!generation||
       (type!=PED_TYPE_GANG1&&type!=PED_TYPE_GANG3)||!Ref<bool>(kActive))return;
    const auto&known=Peds::Instance().All();auto found=known.find(victim);
    const NetPed*np=found!=known.end()?&found->second:nullptr;
    if(!np||np->owner!=owner||np->pedType!=type||Net().EntityGeneration(ENTITY_PED,victim)!=generation)return;
    auto*player=Participant(killer);
    if(!player||player->InVehicle()||!std::isfinite(position.x)||!std::isfinite(position.y)||!std::isfinite(position.z)||
       (player->GetPosition()-position).Length()>1500)return;
    auto identity=std::make_pair(victim,generation);
    if(confirmedDeaths.count(identity))return;
    confirmedDeaths[identity]=NowMs();
    if(confirmedDeaths.size()>512){auto oldest=std::min_element(confirmedDeaths.begin(),confirmedDeaths.end(),[](const auto&a,const auto&b){return a.second<b.second;});confirmedDeaths.erase(oldest);}
    if(Ref<int32_t>(kState)||Ref<int32_t>(kState2))return;
    int zone=NativeZoneIndex(player->GetPosition());if(zone<0)return;
    auto&record=provocations[killer];if(record.zone!=zone)record={zone,0};
    ParticipantContext context(killer);
    float saved=Ref<float>(kProvocation);Ref<float>(kProvocation)=record.value;
    addingConfirmedKill=true;originalAddKill(type);addingConfirmedKill=false;
    record.value=Ref<float>(kProvocation);Ref<float>(kProvocation)=saved;
    LOGI("Gang war confirmed kill owner=%u player=%u victim=%04X generation=%llu zone=%d provocation=%.3f",
        owner,killer,victim,(unsigned long long)generation,zone,record.value);
}

void __cdecl AddKill(int type){
    if(!Net().Online()||addingConfirmedKill)originalAddKill(type);
    // All co-op deaths pass through RecordCooperativeGangKill. The native
    // RegisterKill on a shooter's replica only credits personal statistics.
}

uint8_t PickLeader(){
    if(Ref<int32_t>(kState)||Ref<int32_t>(kState2)==2){
        if(Participant(warLeader))return warLeader;
        auto current=CaptureBattle();
        for(uint8_t id=0;id<3;++id)if(auto*p=Participant(id);p&&current.Contains(p->GetPosition(),0))return id;
        return 255;
    }
    if(Ref<int32_t>(kState2)==1){
        auto point=Ref<CVector>(0x96ABC8);float distance=70.f;uint8_t best=Net().MyId();
        for(uint8_t id=0;id<3;++id)if(auto*p=Participant(id)){
            float d=(p->GetPosition()-point).Length2D();if(d<distance){distance=d;best=id;}
        }
        return best;
    }
    uint8_t best=Net().MyId();float value=0;
    for(uint8_t id=0;id<3;++id){
        auto&record=provocations[id];
        auto*p=Participant(id);
        if(!p||NativeZoneIndex(p->GetPosition())!=record.zone){record={};continue;}
        if(record.value>value){value=record.value;best=id;}
    }
    return best;
}

// Update borrows a player slot only while running the gang-war state machine.
// Native streaming runs later, after that slot is restored: its local-player
// and SCM-cleanup scans do not include the native defensive-wave peds. Append
// their positions to those same native load/unload passes, retaining the host's
// region and any script requests. Offensive campaign follow-up stays deferred.
CPlayerPed* BattleStreamParticipant(){
    if(!Net().Online()||!Net().IsHost()||!core::IsPlayerReady()||core::IsRestarting()||
       !Ref<int32_t>(kState2))return nullptr;
    auto*participant=Participant(PickLeader());if(!participant)return nullptr;
    if(Ref<int32_t>(kState2)==1&&
       (participant->GetPosition()-Ref<CVector>(0x96ABC8)).Length2D()>=150.f)return nullptr;
    return participant;
}
template<class Mark>void ForBattleStreamPosition(Mark mark){
    auto*participant=BattleStreamParticipant();if(!participant)return;
    mark(participant->GetPosition());
    mark(Ref<CVector>(0x96ABC8));
    auto*pool=PedPool();
    for(int i=0;pool&&i<pool->size;++i){
        if(pool->byteMap[i]<0)continue;
        auto*ped=(CPed*)(pool->objects+(size_t)i*kPedPoolElemSize);
        if(!(ped->PedFlags3()&0x1000u)||ped->Health()<=0||!InWarArea(ped))continue;
        auto*np=Peds::Instance().Find(ped);
        if(!np||np->owner==Net().MyId())mark(ped->GetPosition());
    }
}
using PathMarkFn=void(__fastcall*)(void*,void*,CVector,float);
PathMarkFn originalMarkPaths=nullptr;
void __fastcall MarkPaths(void*paths,void*edx,CVector position,float radius){
    originalMarkPaths(paths,edx,position,radius);
    ForBattleStreamPosition([&](CVector p){originalMarkPaths(paths,edx,p,350.f);});
}
using RegionLoadFn=void(__cdecl*)(CVector,bool);
RegionLoadFn originalLoadCollision[2]{},originalLoadIpls[2]{};
using RequireRegionFn=void(__cdecl*)(const CVector&,int);
template<int Site>void __cdecl LoadCollision(CVector position,bool ignoreVehicle){
    ForBattleStreamPosition([](CVector p){reinterpret_cast<RequireRegionFn>(0x4104E0)(p,0);});
    originalLoadCollision[Site](position,ignoreVehicle);
}
template<int Site>void __cdecl LoadIpls(CVector position,bool ignoreVehicle){
    ForBattleStreamPosition([](CVector p){reinterpret_cast<RequireRegionFn>(0x404700)(p,0);});
    originalLoadIpls[Site](position,ignoreVehicle);
}
using NeededGangsFn=void(__cdecl*)(uint32_t&);
NeededGangsFn originalNeededGangs=nullptr;
void __cdecl NeededGangs(uint32_t&flags){
    originalNeededGangs(flags);
    // CPopCycle still describes the host's own neighbourhood. Keep the native
    // war's gang models requested while its participant occupies another one.
    if(BattleStreamParticipant()){
        int gang=Ref<int32_t>(0x96AB84);if(gang>=0&&gang<10)flags|=1u<<gang;
    }
}

void __cdecl Update(){
    if(!Net().Online()||!core::IsPlayerReady())return originalUpdate();
    if(!Net().IsHost())return; // guests render entities/territories, never pick a second wave or winner
    Fence();
    uint8_t leader=PickLeader();
    if(leader==255){
        Call<0x4464C0>(true);warLeader=255;PublishBattle(true);return;
    }
    bool fighting=Ref<int32_t>(kState)||Ref<int32_t>(kState2);
    float saved=Ref<float>(kProvocation);
    float nativeDecay=std::clamp(Ref<float>(0xB7CB5C),0.f,10.f)/2000.f;
    if(!fighting){
        for(uint8_t id=0;id<3;++id)if(id!=leader)provocations[id].value=std::max(0.f,provocations[id].value-nativeDecay);
        // The selected player's counter is advanced by the original function,
        // including its native early returns and invalid-zone reset.
        Ref<float>(kProvocation)=provocations[leader].value;
    }
    {
        ParticipantContext context(leader);
        originalUpdate();
    }
    float remaining=Ref<float>(kProvocation);
    if(!fighting&&Ref<int32_t>(kState)){warLeader=leader;provocations={};
        LOGI("Gang war started player=%u zone=%d hostPositionUnchanged=1",leader,CaptureBattle().zone);}
    else if(Ref<int32_t>(kState2)==2)warLeader=leader;
    else if(!Ref<int32_t>(kState)&&!Ref<int32_t>(kState2)){
        warLeader=255;if(!fighting)provocations[leader].value=remaining;
    }
    // Provocation belongs to the player and zone which produced it. Never
    // expose a guest's remaining count to a subsequent host native update.
    Ref<float>(kProvocation)=saved;
    PublishBattle();
}

void Receive(uint8_t from,Reader&r){
    uint8_t operation=r.U8();
    if(operation==0){
        auto world=r.U32();auto run=r.U64();auto phase=r.U32();auto sequence=r.U32();
        uint8_t killer=r.U8();uint16_t victim=r.U16();auto generation=r.U64();int type=r.U8();auto p=r.Vec();
        // World epochs count each process's own reloads; the envelope has
        // already established which sender epoch is current for this peer.
        if(!r.Ok()||!Net().IsHost()||from>=3||world!=Net().PeerWorldEpoch(from)||run!=StoryRunId()||
            phase!=CooperativeStoryPhase()||!receivedKills[from].Accept(sequence))return;
        Fence();AcceptKill(from,killer,victim,generation,type,{p.x,p.y,p.z});
    }else if(operation==1){
        auto sequence=r.U32();Battle next;next.leader=r.U8();next.state=r.U8();next.defence=r.U8();next.zone=r.I16();
        next.minX=r.I16();next.minY=r.I16();next.maxX=r.I16();next.maxY=r.I16();
        if(!r.Ok()||Net().IsHost()||from!=Net().HostId()||(latestState&&(int32_t)(sequence-latestState)<=0)||
            (next.leader>=3&&next.leader!=255)||next.state>6||next.defence>2||next.zone< -1||next.zone>=380||
            next.minX>next.maxX||next.minY>next.maxY)return;
        latestState=sequence;battle=next;stateReceived=NowMs();
    }
}

class GangWarCoop:public Module {
    const char*Name()const override{return "GangWarCoop";}
    void OnInstallHooks()override{
        mem::HookFn(0x443950,&AddKill,&originalAddKill,"CGangWars::AddKillToProvocation confirmed co-op death");
        mem::HookFn(0x446610,&Update,&originalUpdate,"CGangWars::Update co-op participant context");
        mem::HookFn(0x444530,&TellGangs,&originalTellGangs,"CGangWars::TellGangMembersTo battle area");
        mem::HookFn(0x4444B0,&ClearStreets,&originalClearStreets,"CGangWars::ClearTheStreets battle area");
        mem::RedirectCallFn(0x450AE7,&MarkPaths,&originalMarkPaths);
        // This EXE reads function-entry bytes as data in native allocator
        // thunks. Redirect the ordinary streaming callers, retaining each
        // caller's own previous chain, without changing those entry keys.
        mem::RedirectCallFn(0x40E855,&LoadCollision<0>,&originalLoadCollision[0]);
        mem::RedirectCallFn(0x40E8FD,&LoadCollision<1>,&originalLoadCollision[1]);
        mem::RedirectCallFn(0x40E8B1,&LoadIpls<0>,&originalLoadIpls[0]);
        mem::RedirectCallFn(0x40E92A,&LoadIpls<1>,&originalLoadIpls[1]);
        mem::RedirectCallFn(0x40AABF,&NeededGangs,&originalNeededGangs);
        Net().SetHandler(proto::G_GANG_WAR,&Receive);
    }
    void OnPeerJoin(uint8_t)override{lastStateSent=0;}
    void OnPeerLeave(uint8_t id)override{if(id<3){provocations[id]={};receivedKills[id]={};}if(id==Net().HostId())Clear();}
    void OnPeerResync(uint8_t id)override{OnPeerLeave(id);lastStateSent=0;}
    void OnGameStarted()override{Clear();}
    void OnGameShutdown()override{Clear();}
    void OnSessionEnd()override{Clear();killSequence=stateSequence=0;contextWorld=contextPhase=0;contextRun=0;}
}feature;
struct Reg{Reg(){core::RegisterModule(&feature);}}reg;
} // namespace

void RecordCooperativeGangKill(CPed*victim,CEntity*killer){
    if(!Net().Online()||!core::IsPlayerReady()||core::IsRestarting()||!victim||!killer||!originalAddKill)return;
    auto*source=killer->IsPed()?(CPed*)killer:killer->IsVehicle()?((CVehicle*)killer)->Driver():nullptr;
    uint8_t player=255;
    if(source==FindPlayerPed(0))player=Net().MyId();
    else if(auto*rp=source?Players::Instance().FindByPed(source):nullptr)player=rp->id;
    if(player>=3||source->InVehicle())return;
    auto*np=Peds::Instance().Find(victim);
    if(!np&&victim->PedType()!=PED_TYPE_GANG1&&victim->PedType()!=PED_TYPE_GANG3)return;
    if(!np)np=Peds::Instance().EnsureNetworked(victim);
    if(!np||np->owner!=Net().MyId()||(np->pedType!=PED_TYPE_GANG1&&np->pedType!=PED_TYPE_GANG3))return;
    uint64_t generation=Net().EntityGeneration(ENTITY_PED,np->netId);if(!generation)return;
    Fence();
    if(Net().IsHost())AcceptKill(Net().MyId(),player,np->netId,generation,np->pedType,victim->GetPosition());
    else{
        Writer w(proto::G_GANG_WAR);w.U8(0);w.U32(Net().WorldEpoch());w.U64(StoryRunId());w.U32(CooperativeStoryPhase());
        if(++killSequence==0)++killSequence;w.U32(killSequence);w.U8(player);w.Entity(ENTITY_PED,np->netId);
        w.U64(generation);w.U8(np->pedType);auto p=victim->GetPosition();w.Vec({p.x,p.y,p.z});Net().Send(proto::CH_RELIABLE,w);
    }
}

bool CooperativeGangWarFightingAt(const CVector&position,int8_t area){
    if(!Net().Online()||!core::IsPlayerReady()||!flow::InHostWorld())return false;
    if(Net().IsHost())return CaptureBattle().Contains(position,area);
    return stateReceived&&NowMs()-stateReceived<3500&&battle.Contains(position,area);
}
} // namespace coop
