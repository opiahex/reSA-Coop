// Trains both games share. A train is its own game's (a random one, a mission's): that game tells the others where it
// is (kind, track, distance along the rails, speed), and they show it as a mission train of the same kind kept where the
// real one is, set every frame. The copies are locked, the driver is in the train's game. Random trains come from the
// host's game while the players are together: the partner's game makes its own only while its player is far from the
// host (CTrain::DisableRandomTrains). Whoever rides a train sits in the cab of its copy (Players). The kind of a train is
// not kept on it: it is noted when the train is made (CTrain::CreateMissionTrain makes the random ones too).
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/sync/PopulationShare.h"
#include "shared/lib/sync/ModelRequests.h"
#include "shared/lib/sync/TrainSupport.h"
#include "shared/lib/sync/Quat.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { TRAIN_STATE = 1, TRAIN_GONE = 2, TRAIN_AUTHORITY = 3, TRAIN_RELEASE = 4, TRAIN_CONTROL = 5 };
constexpr uintptr_t kDisableRandomTrains = 0xC38010;  // CTrain::bDisableRandomTrains
constexpr uint32_t kTrainPhysicsMask=0x20FE24F6u; // simulation/proofs/scorch, never attachment/reference bits
constexpr uint16_t kFrontCarriage = 0x8, kMissionTrain = 0x20, kClockwise = 0x40, kOffRail = 0x100;  // CTrain::m_nTrainFlags (+0x5B8)
constexpr int kTrainModels[] = {449, 537, 538, 569, 570, 590};  // tram, freight, streak and their carriages
// The partner's game makes random trains this far from the host. The host's game removes its random trains 220 m from
// its camera (CTrain::DoTrainGenerationAndRemoval, 0x863994): the partner 220 to 600 m away saw none.
constexpr float kOwnRandomFrom = 220.0f;
constexpr float kSendWithin = 300.0f;  // a train is sent this near another player (a copy goes 10 s after the last state)

float& Speed(CVehicle* t) { return Field<float>(t, 0x5A4); }         // CTrain::m_fTrainSpeed
float& RailDistance(CVehicle* t) { return Field<float>(t, 0x5A8); }  // CTrain::m_fCurrentRailDistance
uint16_t Flags(CVehicle* t) { return Field<uint16_t>(t, 0x5B8); }
int8_t Track(CVehicle* t) { return Field<int8_t>(t, 0x5C0); }
CVehicle* NextCarriage(CVehicle* t) { return Field<CVehicle*>(t, 0x5D4); }

// ours
std::map<CVehicle*, uint8_t> g_kinds;  // engines CTrain::CreateMissionTrain made here, and their kind
std::map<CVehicle*, uint32_t> g_ids;   // the ones announced
uint16_t g_nextId = 1;
uint64_t g_sendMs = 0;
bool g_copying = false;                // a copy is being made: not ours

// the other games'
struct Copy {
    CVehicle* engine;
    uint8_t kind;
    float rail, speed;
    uint64_t ms;
    int handle=-1;
    int8_t track=0;
    bool clockwise=false;
    uint64_t generation=0;
};
std::map<uint32_t, Copy> g_copies;  // by owner << 16 | its id
bool g_randomOff = false;           // we switched the random trains off
struct CarriageLook {
    uint16_t model=0;std::array<int8_t,2>components{-1,-1};std::array<uint8_t,4>colours{};uint8_t flags=3;
    uint32_t physical=0;uint16_t nativeFlags=0;uint8_t status=2,cruise=0,vehicleFlags=0,lock=2;int8_t area=0;int16_t node=0;
    float health=1000,rail=0,speed=0,gas=0,brake=0;uint32_t stoppedFor=0;
    CVector position,velocity,turn;Quat rotation;
    void Write(Writer&w)const{
        w.U16(model);for(auto c:components)w.I8(c);for(auto c:colours)w.U8(c);w.U8(flags);
        w.U32(physical);w.U16(nativeFlags);w.U8(status);w.U8(cruise);w.U8(vehicleFlags);w.U8(lock);w.I8(area);w.I16(node);w.F32(health);w.F32(rail);w.F32(speed);w.F32(gas);w.F32(brake);w.U32(stoppedFor);
        w.Vec({position.x,position.y,position.z});rotation.Write(w);w.Vec({velocity.x,velocity.y,velocity.z});w.Vec({turn.x,turn.y,turn.z});
    }
    bool Read(Reader&r){
        model=r.U16();for(auto&c:components)c=r.I8();for(auto&c:colours)c=r.U8();flags=r.U8();
        physical=r.U32();nativeFlags=r.U16();status=r.U8();cruise=r.U8();vehicleFlags=r.U8();lock=r.U8();area=r.I8();node=r.I16();health=r.F32();rail=r.F32();speed=r.F32();gas=r.F32();brake=r.F32();stoppedFor=r.U32();
        auto p=r.Vec();position={p.x,p.y,p.z};rotation.Read(r);auto v=r.Vec(),t=r.Vec();velocity={v.x,v.y,v.z};turn={t.x,t.y,t.z};
        return r.Ok()&&std::find(std::begin(kTrainModels),std::end(kTrainModels),model)!=std::end(kTrainModels)&&flags<=3&&
            std::all_of(components.begin(),components.end(),[](auto c){return c>=-1&&c<=5;})&&std::all_of(colours.begin(),colours.end(),[](auto c){return c<128;})&&
            (physical&~kTrainPhysicsMask)==0&&vehicleFlags<8&&lock<=7&&status<=9&&area>=0&&area<=18&&node>=0&&std::isfinite(health)&&health>=0&&health<=65535&&
            std::isfinite(rail)&&std::fabs(rail)<1e6f&&std::isfinite(speed)&&std::fabs(speed)<=50&&
            std::isfinite(gas)&&std::fabs(gas)<=1024&&std::isfinite(brake)&&std::fabs(brake)<=1024&&stoppedFor<=120000&&
            position.Length()<50000&&velocity.Length()<250&&turn.Length()<100&&(nativeFlags&~0x7FFu)==0;
    }
};
struct PendingTrain {uint8_t kind=0;int8_t track=0;bool clockwise=false;float rail=0,speed=0;CVector pos;uint64_t received=0,generation=0;std::vector<CarriageLook>consist;};
struct Authority {uint64_t generation=0;uint32_t term=1,sequence=0;uint8_t peer=255;};
// Identity scope never changes at handover: cargo keeps the same carriage birth.
std::map<uint32_t,Authority>g_authorities;
std::map<uint32_t,PendingTrain>g_snapshots;
std::map<uint32_t,uint64_t>g_retired;
std::map<CVehicle*,int>g_worldCandidates;
uint64_t g_registrySent=0;
uint32_t TrainKey(uint8_t scope,uint16_t id){return (uint32_t(scope)<<16)|id;}
uint8_t Scope(uint32_t key){return (uint8_t)(key>>16);}
uint16_t Number(uint32_t key){return (uint16_t)key;}
std::map<uint32_t,PendingTrain> g_pending;
std::map<uint32_t,uint32_t> g_latest;
std::map<uint32_t,std::pair<const char*,uint64_t>>g_waitLog;
void TrainWait(uint32_t key,const char*reason){
    auto&state=g_waitLog[key];if(state.first!=reason||NowMs()-state.second>=1000){state={reason,NowMs()};
        LOGD("Train %u of player %u waiting: %s models=%d,%d vehiclePoolSize=%d pedPoolSize=%d",(uint16_t)key,key>>16,reason,IsModelLoaded(537),IsModelLoaded(569),VehiclePool()?VehiclePool()->size:0,PedPool()?PedPool()->size:0);}
}
std::array<uint8_t,3> g_trainSources{255,255,255};

void SendGone(uint32_t key);
void Promote(uint32_t key);
void Transfer(uint32_t key,uint8_t peer,bool announce);
void __cdecl DeleteTrain(CVehicle*engine);
std::map<CVehicle*,int>g_engineRefs;
void Header(Writer&w,uint8_t op,uint32_t key,uint8_t flags);
bool g_nativeMutation=false;
using SpeedFn=void(__cdecl*)(CVehicle*,float);SpeedFn g_originalSpeed=nullptr,g_originalCruise=nullptr;
using TrainFn=void(__cdecl*)(CVehicle*);TrainFn g_originalRemove=nullptr,g_originalRelease=nullptr;
using PositionFn=void(__cdecl*)(CVehicle*,CVector);PositionFn g_originalPosition=nullptr;
uint32_t KnownKey(CVehicle*engine){
    if(auto i=g_ids.find(engine);i!=g_ids.end())return i->second;
    for(auto&[key,c]:g_copies)if(c.engine==engine&&GetVehicleRef(engine)==c.handle)return key;
    return 0;
}
bool ForwardControl(CVehicle*engine,uint8_t operation,float value=0,CVector position={}){
    if(g_nativeMutation||!Net().Online())return false;auto key=KnownKey(engine);auto i=g_authorities.find(key);
    if(!key||i==g_authorities.end()||i->second.peer==Net().MyId()||(Net().MyId()!=Scope(key)&&!Net().IsHost()))return false;
    Writer w(proto::G_TRAIN);Header(w,TRAIN_CONTROL,key,ENTITY_USE);w.U8(operation);w.F32(value);w.Vec({position.x,position.y,position.z});Net().Send(proto::CH_RELIABLE,w);return true;
}

using CreateTrainFn = void(__cdecl*)(CVector, bool, uint32_t, CVehicle**, CVehicle**, int32_t, int32_t, bool);
CreateTrainFn g_origCreateTrain = nullptr;
void __cdecl HookCreateMissionTrain(CVector pos, bool clockwise, uint32_t kind, CVehicle** first, CVehicle** last, int32_t node,
                                    int32_t track, bool mission) {
    g_origCreateTrain(pos, clockwise, kind, first, last, node, track, mission);
    if(first&&*first&&!g_copying){
        auto*engine=*first;auto ref=GetVehicleRef(engine);
        if(auto old=g_ids.find(engine);old!=g_ids.end()&&g_engineRefs[engine]!=ref){SendGone(old->second);g_ids.erase(old);}
        g_kinds[engine]=(uint8_t)kind;g_engineRefs[engine]=ref;
    }
}

bool IsEngine(CVehicle* v) { return IsVehicleValid(v) && v->IsTrain() && (Flags(v) & kFrontCarriage); }

void Header(Writer&w,uint8_t op,uint32_t key,uint8_t flags=ENTITY_USE){
    auto i=g_authorities.find(key);if(i==g_authorities.end())return;auto&a=i->second;
    w.U8(op);w.Entity(ENTITY_TRAIN,Number(key),flags,Scope(key),a.generation);w.U8(Scope(key));w.U32(a.term);w.U8(a.peer);
}
void RetireIdentity(uint32_t key,uint64_t generation){
    g_retired[key]=generation;g_snapshots.erase(key);g_pending.erase(key);g_latest.erase(key);g_waitLog.erase(key);
    Net().RetireEntity(ENTITY_TRAIN,Number(key),Scope(key),generation);
    g_authorities.erase(key);
}
void SendGone(uint32_t key) {
    auto i=g_authorities.find(key);if(i==g_authorities.end()||i->second.peer!=Net().MyId())return;
    auto generation=i->second.generation;
    Writer w(proto::G_TRAIN);Header(w,TRAIN_GONE,key);Net().Send(proto::CH_RELIABLE,w);
    RetireIdentity(key,generation);
}
void SendRegistry(){
    for(auto&[key,a]:g_authorities)if((Net().IsHost()||a.peer==Net().MyId())&&a.generation&&a.peer<3&&Net().EntityLive(ENTITY_TRAIN,Number(key),Scope(key))){
        Writer w(proto::G_TRAIN);Header(w,TRAIN_AUTHORITY,key,ENTITY_BIRTH|ENTITY_SNAPSHOT);Net().Send(proto::CH_RELIABLE,w);
    }
    g_registrySent=NowMs();
}
uint8_t Successor(uint8_t excluding){
    auto alive=[](uint8_t peer){return peer==Net().MyId()||Net().Peer(peer)!=nullptr;};
    auto host=Net().HostId();if(host<3&&host!=excluding&&alive(host))return host;
    // Peer() excludes the local player. Include self in the same ordered
    // survivor set on every participant, or host and guests elect each other.
    for(uint8_t id=0;id<3;++id)if(id!=excluding&&alive(id))return id;
    return 255;
}
void PublishSupports(uint8_t owner,uint16_t id,uint64_t generation,CVehicle*engine){
    for(uint8_t index=0;index<16&&engine&&IsVehicleValid(engine);engine=NextCarriage(engine),++index)
        TrainSupports()[TrainSupportKey(owner,id,index)]={engine,GetVehicleRef(engine),generation};
}
uint32_t EnsureOwnedTrain(CVehicle*engine){
    if(!IsEngine(engine)||!g_kinds.count(engine)||g_engineRefs[engine]!=GetVehicleRef(engine))return 0;
    auto existing=g_ids.find(engine);if(existing!=g_ids.end())return existing->second;
    for(unsigned attempt=0;attempt<255;++attempt){
        uint16_t next=(uint16_t)(g_nextId++&255u);if(!next)continue;
        auto key=TrainKey(Net().MyId(),next);if(g_authorities.count(key))continue;
        auto generation=Net().BeginEntity(ENTITY_TRAIN,next);g_ids[engine]=key;
        g_authorities[key]={generation,1,0,Net().MyId()};g_retired.erase(key);PublishSupports(Net().MyId(),next,generation,engine);return key;
    }
    return 0;
}
std::vector<CarriageLook> CaptureConsist(CVehicle*engine){
    std::vector<CarriageLook>out;
    for(auto*t=engine;t&&IsVehicleValid(t);t=NextCarriage(t)){
        if(out.size()>=15)return {}; // the native configuration has 16 slots, including its terminator
        CarriageLook c;c.model=(uint16_t)t->ModelIndex();for(int i=0;i<2;++i)c.components[i]=Field<int8_t>(t,0x438+i);
        memcpy(c.colours.data(),(uint8_t*)t+0x434,4);c.flags=((t->Flags()&128u)?1:0)|((t->Flags()&1u)?2:0);
        c.physical=t->PhysicalFlags()&kTrainPhysicsMask;c.nativeFlags=Flags(t)&0x7FFu;c.status=t->Status();c.vehicleFlags=(t->EngineOn()?1:0)|(t->LightsOn()?2:0)|(t->HandbrakeOn()?4:0);c.lock=(uint8_t)std::clamp(t->DoorLock(),0,7);c.cruise=Field<uint8_t>(t,0x3D0);c.area=t->AreaCode();c.node=Field<int16_t>(t,0x5A0);
        c.health=std::clamp(t->Health(),0.f,65535.f);c.rail=RailDistance(t);c.speed=Speed(t);c.gas=Field<float>(t,0x5B0);c.brake=Field<float>(t,0x5B4);
        c.stoppedFor=std::min<uint32_t>(120000,TimeMs()-Field<uint32_t>(t,0x5BC));c.position=t->GetPosition();c.velocity=t->MoveSpeed()*50.f;c.turn=t->TurnSpeed()*50.f;
        if(t->Matrix())c.rotation=Quat::FromMatrix(*t->Matrix());out.push_back(c);
    }
    return out;
}
PendingTrain CaptureTrain(CVehicle*t,uint8_t kind,uint64_t generation){
    return {kind,Track(t),(Flags(t)&kClockwise)!=0,RailDistance(t),Speed(t),t->GetPosition(),NowMs(),generation,CaptureConsist(t)};
}
void WriteSnapshot(Writer&w,const PendingTrain&p){
    w.U8(p.kind);w.I8(p.track);w.Bool(p.clockwise);w.F32(p.rail);w.F32(p.speed);w.Vec({p.pos.x,p.pos.y,p.pos.z});
    w.U8((uint8_t)p.consist.size());for(auto&c:p.consist)c.Write(w);
}
void SendTrainState(CVehicle*t,uint8_t kind,uint32_t key,bool reliable=false){
    auto i=g_authorities.find(key);if(i==g_authorities.end()||i->second.peer!=Net().MyId())return;auto&a=i->second;
    auto state=CaptureTrain(t,kind,a.generation);if(state.consist.empty())return;g_snapshots[key]=state;
    Writer w(proto::G_TRAIN);Header(w,TRAIN_STATE,key,ENTITY_BIRTH|ENTITY_SNAPSHOT);if(++a.sequence==0)++a.sequence;w.U32(a.sequence);WriteSnapshot(w,state);
    Net().Send(reliable?proto::CH_RELIABLE:proto::CH_STATE,w);
}
bool HasAttachments(CVehicle*engine){
    auto attached=[&](CEntity*e){if(!e||(!e->IsObject()&&!e->IsPed()&&!e->IsVehicle()))return false;auto*parent=((CPhysical*)e)->AttachedTo();
        for(auto*t=engine;t&&IsVehicleValid(t);t=NextCarriage(t))if(parent==t)return true;return false;};
    auto scan=[&](CPoolHeader*pool,size_t stride){for(int i=0;pool&&i<pool->size;++i)if(pool->byteMap[i]>=0&&attached((CEntity*)(pool->objects+size_t(i)*stride)))return true;return false;};
    return scan(ObjectPool(),kObjectPoolElemSize)||scan(PedPool(),kPedPoolElemSize)||scan(VehiclePool(),kVehiclePoolElemSize);
}
void SendOurs() {
    for(auto it=g_kinds.begin();it!=g_kinds.end();){
        auto*t=it->first;auto id=g_ids.find(t);
        if(!IsEngine(t)||g_engineRefs[t]!=GetVehicleRef(t)){
            if(id!=g_ids.end()){SendGone(id->second);g_ids.erase(id);}g_engineRefs.erase(t);it=g_kinds.erase(it);continue;
        }
        auto key=EnsureOwnedTrain(t);if(key){
            auto a=g_authorities.find(key);
            if(a!=g_authorities.end()&&a->second.peer==Net().MyId()){
                PublishSupports(Scope(key),Number(key),a->second.generation,t);
                float distance=RemoteDistance(t->GetPosition(),t->AreaCode());
                for(auto*c=NextCarriage(t);c&&IsVehicleValid(c);c=NextCarriage(c))distance=std::min(distance,RemoteDistance(c->GetPosition(),c->AreaCode()));
                if((Flags(t)&kMissionTrain)||HasAttachments(t)||distance<=kSendWithin)SendTrainState(t,it->second,key);
            }
        }
        ++it;
    }
}

// the copy goes with whoever is in it put out first (a vehicle deleted with peds inside leaves them dangling)
void RemoveCopy(const Copy& c) {
    if (!IsEngine(c.engine)||GetVehicleRef(c.engine)!=c.handle) return;
    std::vector<CVehicle*>carriages;for(auto*t=c.engine;t&&IsVehicleValid(t)&&carriages.size()<16;t=NextCarriage(t))carriages.push_back(t);
    for(auto i=TrainSupports().begin();i!=TrainSupports().end();)i=i->second.generation==c.generation&&std::find(carriages.begin(),carriages.end(),i->second.vehicle)!=carriages.end()?TrainSupports().erase(i):std::next(i);
    for (CVehicle* t = c.engine; t; t = NextCarriage(t)) {
        CVector p = t->GetPosition();
        if (CPed* d = t->Driver()) Cmd(0x0362).I(GetPedRef(d)).F(p.x).F(p.y).F(p.z + 3.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        for (int s = 0; s < 8; ++s)
            if (CPed* q = t->Passengers()[s]) Cmd(0x0362).I(GetPedRef(q)).F(p.x).F(p.y).F(p.z + 3.0f).Run();
        // CTrain::RemoveOneMissionTrain deletes the carriages without CWorld::RemoveReferencesToDeletedObject: a player
        // standing on the copy's roof (a mission's train, deleted and made again by its script) kept standing on freed
        // memory
        Call<0x565510>(t);
    }
    g_originalRemove(c.engine);  // native, bypass only our source-command forwarding
}

void RemoveCopies(int owner = -1) {
    for(auto it=g_pending.begin();it!=g_pending.end();)it=(owner<0||(int)(it->first>>16)==owner)?g_pending.erase(it):std::next(it);
    for(auto it=g_latest.begin();it!=g_latest.end();)it=(owner<0||(int)(it->first>>16)==owner)?g_latest.erase(it):std::next(it);
    for (auto it = g_copies.begin(); it != g_copies.end();)
        if (owner < 0 || (int)(it->first >> 16) == owner) RemoveCopy(it->second), it = g_copies.erase(it);
        else ++it;
}

int NearestTrackNode(int track,const CVector&at){
    if(track<0||track>3)return -1;
    int count=Ref<int32_t>(0xC38014+track*4);auto*nodes=Ref<const int16_t*>(0xC38024+track*4);
    if(!nodes||count<=0||count>65536)return -1;
    int best=-1;float distance=1e30f;
    for(int i=0;i<count;++i){const auto*n=nodes+i*5;CVector delta{n[0]/8.f-at.x,n[1]/8.f-at.y,n[2]/8.f-at.z};float d=delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;if(d<distance){best=i;distance=d;}}
    return best;
}
bool ConsistModelsMatch(CVehicle*engine,const std::vector<CarriageLook>&consist){
    auto*t=engine;for(auto&c:consist){if(!t||!IsVehicleValid(t)||t->ModelIndex()!=c.model)return false;t=NextCarriage(t);}return !t;
}
void NativeRuntime(CVehicle*t,const CarriageLook&c,bool authority){
    Field<uint16_t>(t,0x5B8)=(Flags(t)&~0x7FFu)|(authority?c.nativeFlags:(c.nativeFlags|kMissionTrain));
    Field<int16_t>(t,0x5A0)=c.node;t->AreaCode()=c.area;t->Health()=c.health;
    t->SetEngineOn((c.vehicleFlags&1)!=0);t->VehicleFlagsLo()=(t->VehicleFlagsLo()&~64u)|((c.vehicleFlags&2)?64u:0u);t->SetHandbrake((c.vehicleFlags&4)!=0);
    t->DoorLock()=authority?c.lock:2;
    if(authority)t->PhysicalFlags()=(t->PhysicalFlags()&~kTrainPhysicsMask)|c.physical;
    else t->PhysicalFlags()|=0xFC0000u;
    Field<uint8_t>(t,0x3D0)=c.cruise;Field<float>(t,0x5B0)=c.gas;Field<float>(t,0x5B4)=c.brake;
    Field<uint32_t>(t,0x5BC)=TimeMs()-c.stoppedFor;
    // A remote driver's pad must never become this observer's local pad.
    bool localDriver=t->Driver()==FindPlayerPed(0);
    t->SetStatus(c.status==0&&!localDriver?2:c.status);
    if(authority){Speed(t)=c.speed;RailDistance(t)=c.rail;t->MoveSpeed()=c.velocity*(1.f/50.f);t->TurnSpeed()=c.turn*(1.f/50.f);}
}
void ApplyCarriageLooks(CVehicle*engine,const std::vector<CarriageLook>&consist,bool authority=false){
    auto*t=engine;for(auto&c:consist){
        if(!t||!IsVehicleValid(t))return;
        if(t->ModelIndex()!=c.model||Field<int8_t>(t,0x438)!=c.components[0]||Field<int8_t>(t,0x439)!=c.components[1]){
            const auto saved=Ref<uint16_t>(0x8A6458);WorldRemove(t);
            for(int i=0;i<2;++i)Ref<int8_t>(0x8A6458+i)=c.components[i];
            CallMethod<0x6F5530>(t,(uint32_t)c.model);Ref<uint16_t>(0x8A6458)=saved;WorldAdd(t);
        }
        memcpy((uint8_t*)t+0x434,c.colours.data(),4);t->Flags()=(t->Flags()&~129u)|((c.flags&1)?128u:0)|((c.flags&2)?1u:0);
        NativeRuntime(t,c,authority);t=NextCarriage(t);
    }
}
void PlacePhysicalCarriage(CVehicle*t,const CarriageLook&c,uint64_t age){
    float dt=(float)std::min<uint64_t>(age,250)*.001f;
    WorldRemove(t);if(auto*m=t->Matrix()){Quat::Turned(c.rotation,c.turn,dt).ToMatrix(*m);m->pos=c.position+c.velocity*dt;}else t->SetPositionRaw(c.position+c.velocity*dt);
    CallMethod<0x446F90>(t);CallMethod<0x532B00>(t);WorldAdd(t);
    t->MoveSpeed()=c.velocity*(1.f/50.f);t->TurnSpeed()=c.turn*(1.f/50.f);
}
bool HasExactCount(CVehicle*engine,size_t count){for(size_t i=0;i<count;++i){if(!engine||!IsVehicleValid(engine))return false;engine=NextCarriage(engine);}return !engine;}
CVehicle* WorldTwin(const PendingTrain&p){
    for(auto i=g_worldCandidates.begin();i!=g_worldCandidates.end();){
        auto*t=i->first;if(!IsEngine(t)||GetVehicleRef(t)!=i->second){i=g_worldCandidates.erase(i);continue;}
        ++i;
        if(Track(t)!=p.track||(Flags(t)&kClockwise)!=(p.clockwise?kClockwise:0)||!ConsistModelsMatch(t,p.consist)||
           (t->GetPosition()-p.pos).Length()>3.f||std::fabs(RailDistance(t)-p.rail)>5.f)continue;
        g_worldCandidates.erase(t);LOGI("Train native world instance adopted after reload native=%d",GetVehicleRef(t));return t;
    }
    return nullptr;
}
void ApplyTrain(uint32_t key,const PendingTrain&p){
    if(!Net().EntityLive(ENTITY_TRAIN,(uint16_t)key,(uint8_t)(key>>16))||Net().EntityGeneration(ENTITY_TRAIN,(uint16_t)key,(uint8_t)(key>>16))!=p.generation)return;
    auto it=g_copies.find(key);
    if(it!=g_copies.end()&&(it->second.generation!=p.generation||!IsEngine(it->second.engine)||GetVehicleRef(it->second.engine)!=it->second.handle||!HasExactCount(it->second.engine,p.consist.size()))){
        RemoveCopy(it->second);g_copies.erase(it);it=g_copies.end();
    }
    bool ready=true;
    if(it==g_copies.end())for(auto&c:p.consist){
        // All carriage models must coexist before the native constructor runs.
        // Retain the loaded half too, or streaming alternates engine and wagon.
        RequestSharedModel(c.model,true,true);ready=IsModelLoaded(c.model)&&ready;
    }
    if(!ready){g_pending[key]=p;TrainWait(key,"model_not_loaded");return;}
    if(it==g_copies.end()){
        if(!HasRoom(VehiclePool(),kKeepVehicles+(int)p.consist.size(),"train")||!HasRoom(PedPool(),kKeepPeds+1,"train driver")){g_pending[key]=p;TrainWait(key,"pool_full");return;}
        int node=NearestTrackNode(p.track,p.pos);if(node<0){g_pending[key]=p;TrainWait(key,"track_node_missing");return;}
        // The native creator reads this 16-model configuration synchronously.
        // Scope its override to this one copy, preserving local train generation.
        auto*configuration=(int32_t*)(0x8D44F8+(uintptr_t)p.kind*64);std::array<int32_t,16>saved;memcpy(saved.data(),configuration,sizeof saved);
        memset(configuration,0,sizeof saved);for(size_t i=0;i<p.consist.size();++i)configuration[i]=p.consist[i].model;
        CVehicle *first=WorldTwin(p),*last=nullptr;g_copying=true;
        if(!first)g_origCreateTrain(p.pos,p.clockwise,p.kind,&first,&last,node,p.track,true);g_copying=false;memcpy(configuration,saved.data(),sizeof saved);
        if(!first){g_pending[key]=p;TrainWait(key,"native_creation");return;}
        for(auto*t=first;t&&IsVehicleValid(t);t=NextCarriage(t)){t->DoorLock()=2;Field<int8_t>(t,0x5C0)=p.track;}
        g_originalPosition(first,p.pos);
        it=g_copies.emplace(key,Copy{first,p.kind,p.rail,p.speed,p.received,GetVehicleRef(first),p.track,p.clockwise,p.generation}).first;
        LOGI("Train %u of player %u copied asynchronously (%u carriages) native=%d model=%u subclass=%d flags=%04X",(uint16_t)key,key>>16,(unsigned)p.consist.size(),GetVehicleRef(first),first->ModelIndex(),first->VehicleSubClass(),Flags(first));
    }
    ApplyCarriageLooks(it->second.engine,p.consist);
    it->second.kind=p.kind;it->second.track=p.track;it->second.clockwise=p.clockwise;it->second.rail=p.rail;it->second.speed=p.speed;it->second.ms=p.received;
    for(auto*t=it->second.engine;t&&IsVehicleValid(t);t=NextCarriage(t))Field<int8_t>(t,0x5C0)=p.track;
    PublishSupports(Scope(key),Number(key),p.generation,it->second.engine);g_pending.erase(key);g_waitLog.erase(key);
    auto authority=g_authorities.find(key);if(authority!=g_authorities.end()&&authority->second.peer==Net().MyId())Promote(key);
}
bool ReadSnapshot(Reader&r,uint64_t generation,PendingTrain&out){
    out.kind=r.U8();out.track=r.I8();out.clockwise=r.Bool();out.rail=r.F32();out.speed=r.F32();auto p=r.Vec();out.pos={p.x,p.y,p.z};
    auto count=r.U8();if(count<1||count>15)return false;out.consist.resize(count);for(auto&c:out.consist)if(!c.Read(r))return false;
    if(!r.Ok()||out.kind>15||out.track<0||out.track>3||!std::isfinite(out.rail)||std::fabs(out.rail)>1e6f||
       !std::isfinite(out.speed)||std::fabs(out.speed)>50.f||out.pos.Length()>50000)return false;
    auto nodes=Ref<int32_t>(0xC38014+(uintptr_t)out.track*4);if(nodes<=0)return false;
    for(auto&c:out.consist)if(c.node>=nodes)return false;
    out.received=NowMs();out.generation=generation;return true;
}
void Promote(uint32_t key){
    if(!core::IsPlayerReady()||core::IsRestarting())return;
    auto a=g_authorities.find(key);auto c=g_copies.find(key);auto state=g_snapshots.find(key);
    if(a==g_authorities.end()||a->second.peer!=Net().MyId()||c==g_copies.end()||state==g_snapshots.end())return;
    auto*engine=c->second.engine;if(!IsEngine(engine)||GetVehicleRef(engine)!=c->second.handle)return;
    auto snapshot=state->second;auto age=std::min<uint64_t>(NowMs()-snapshot.received,250);
    ApplyCarriageLooks(engine,snapshot.consist,true);
    auto*carriage=engine;for(auto&look:snapshot.consist){
        if(!carriage||!IsVehicleValid(carriage))break;
        // An adopted wreck must not jump back onto the constructor's track.
        // Live rail copies retain the same bounded extrapolation at takeover.
        if((look.nativeFlags&kOffRail)||look.status==5||look.health<=0)PlacePhysicalCarriage(carriage,look,age);
        else RailDistance(carriage)=look.rail+look.speed*(float)age/20.f;
        Field<uint32_t>(carriage,0x5BC)=TimeMs()-std::min<uint32_t>(120000,look.stoppedFor+(uint32_t)age);
        carriage=NextCarriage(carriage);
    }
    g_kinds[engine]=c->second.kind;g_engineRefs[engine]=c->second.handle;g_ids[engine]=key;
    // Do not delete or recreate a native carriage. All child references remain valid.
    g_copies.erase(c);PublishSupports(Scope(key),Number(key),a->second.generation,engine);
    LOGI("Train %u scope %u authority adopted peer=%u term=%u native=%d generation=%llu",Number(key),Scope(key),Net().MyId(),a->second.term,GetVehicleRef(engine),(unsigned long long)a->second.generation);
    SendTrainState(engine,g_kinds[engine],key,true);
}
void ChangeAuthority(uint32_t key,uint8_t peer,uint32_t term){
    auto a=g_authorities.find(key);if(a==g_authorities.end()||peer>=3||!term||term<a->second.term)return;
    if(a->second.peer==peer&&a->second.term==term)return;
    for(auto i=g_ids.begin();i!=g_ids.end();++i)if(i->second==key){
        auto*engine=i->first;auto s=g_snapshots.find(key);if(IsEngine(engine)&&g_engineRefs[engine]==GetVehicleRef(engine)&&s!=g_snapshots.end()){
            auto&p=s->second;g_copies[key]={engine,g_kinds[engine],p.rail,p.speed,NowMs(),GetVehicleRef(engine),p.track,p.clockwise,p.generation};
        }
        g_kinds.erase(engine);g_engineRefs.erase(engine);g_ids.erase(i);break;
    }
    a->second.peer=peer;a->second.term=term;a->second.sequence=0;g_latest.erase(key);
    if(auto c=g_copies.find(key);c!=g_copies.end())c->second.ms=NowMs();
    LOGI("Train %u scope %u authority peer=%u term=%u generation=%llu",Number(key),Scope(key),peer,term,(unsigned long long)a->second.generation);
    if(peer==Net().MyId()&&!g_copies.count(key))if(auto s=g_snapshots.find(key);s!=g_snapshots.end()){
        auto snapshot=s->second;snapshot.received=NowMs();g_pending[key]=snapshot;
        if(core::IsPlayerReady()&&!core::IsRestarting())ApplyTrain(key,snapshot);
    }
    Promote(key);
}
void Transfer(uint32_t key,uint8_t peer,bool announce){
    auto a=g_authorities.find(key);if(a==g_authorities.end()||peer>=3||peer==a->second.peer)return;
    if(announce&&a->second.peer==Net().MyId()){
        for(auto&[engine,id]:g_ids)if(id==key&&IsEngine(engine)&&g_engineRefs[engine]==GetVehicleRef(engine))g_snapshots[key]=CaptureTrain(engine,g_kinds[engine],a->second.generation);
        auto snapshot=g_snapshots.find(key);if(snapshot!=g_snapshots.end()){
            Writer w(proto::G_TRAIN);Header(w,TRAIN_RELEASE,key,ENTITY_BIRTH|ENTITY_SNAPSHOT);w.U8(peer);WriteSnapshot(w,snapshot->second);Net().Send(proto::CH_RELIABLE,w);
        }
    }
    auto term=a->second.term+1;if(!term)++term;ChangeAuthority(key,peer,term);
}
void HandleTrain(uint8_t from, Reader& r) {
    auto op=r.U8();auto id=r.U16();auto scope=r.U8();auto term=r.U32();auto authority=r.U8();
    if(!r.Ok()||from>=3||scope>=3||!id||id>255||!term||authority>=3||!core::IsPlayerReady())return;
    auto key=TrainKey(scope,id);uint64_t generation=0;
    for(auto&ref:Net().EntityContext())if(ref.kind==ENTITY_TRAIN&&ref.scope==scope&&ref.id==id)generation=ref.generation;
    if(!generation||!Net().EntityLive(ENTITY_TRAIN,id,scope)||Net().EntityGeneration(ENTITY_TRAIN,id,scope)!=generation||g_retired[key]==generation)return;
    auto known=g_authorities.find(key);
    if(known!=g_authorities.end()&&known->second.generation!=generation)return;
    if(op==TRAIN_AUTHORITY){
        if(authority!=Net().MyId()&&!Net().Peer(authority))return;
        bool host=from==Net().HostId();
        bool incumbent=from==authority&&(known==g_authorities.end()||
            (known->second.peer==from&&known->second.term==term));
        if(!host&&!incumbent)return;
        if(known==g_authorities.end())g_authorities[key]={generation,term,0,authority};
        else if(term>=known->second.term)ChangeAuthority(key,authority,term);
        return;
    }
    if(op==TRAIN_CONTROL){
        auto operation=r.U8();auto value=r.F32();auto p=r.Vec();CVector position{p.x,p.y,p.z};
        if(!r.Ok()||known==g_authorities.end()||known->second.peer!=Net().MyId()||known->second.term!=term||authority!=Net().MyId()||
           (from!=scope&&from!=Net().HostId())||operation<1||operation>5||!std::isfinite(value)||std::fabs(value)>255||position.Length()>50000)return;
        CVehicle*engine=nullptr;for(auto&[native,id]:g_ids)if(id==key&&IsEngine(native)&&g_engineRefs[native]==GetVehicleRef(native))engine=native;
        if(!engine)return;g_nativeMutation=true;
        if(operation==1)g_originalSpeed(engine,value);else if(operation==2)g_originalCruise(engine,value);else if(operation==3)g_originalRelease(engine);
        else if(operation==4){g_nativeMutation=false;DeleteTrain(engine);return;}
        else g_originalPosition(engine,position);
        g_nativeMutation=false;SendTrainState(engine,g_kinds[engine],key,true);return;
    }
    if(from!=authority)return;
    if(known==g_authorities.end()){
        // A new observer learns migrated authorities from the host registry.
        if(from!=scope&&from!=Net().HostId())return;
        known=g_authorities.emplace(key,Authority{generation,term,0,authority}).first;
    }
    if(known->second.peer!=from||known->second.term!=term)return;
    if(op==TRAIN_GONE){
        if(auto c=g_copies.find(key);c!=g_copies.end()){RemoveCopy(c->second);g_copies.erase(c);}
        RetireIdentity(key,generation);return;
    }
    if(op==TRAIN_RELEASE){
        auto next=r.U8();PendingTrain state;if(next>=3||next==from||(next!=Net().MyId()&&!Net().Peer(next))||!ReadSnapshot(r,generation,state))return;
        g_snapshots[key]=state;ApplyTrain(key,state);Transfer(key,next,false);return;
    }
    if(op!=TRAIN_STATE)return;
    auto stamp=r.U32();auto previous=g_latest.find(key);if(!stamp||(previous!=g_latest.end()&&(int32_t)(stamp-previous->second)<=0))return;
    PendingTrain state;if(!ReadSnapshot(r,generation,state))return;
    g_latest[key]=stamp;g_snapshots[key]=state;ApplyTrain(key,state);
}

void __cdecl SetTrainSpeed(CVehicle*engine,float value){ForwardControl(engine,1,value);g_originalSpeed(engine,value);}
void __cdecl SetTrainCruise(CVehicle*engine,float value){ForwardControl(engine,2,value);g_originalCruise(engine,value);}
void __cdecl ReleaseTrain(CVehicle*engine){ForwardControl(engine,3);g_originalRelease(engine);}
void __cdecl PositionTrain(CVehicle*engine,CVector position){ForwardControl(engine,5,0,position);g_originalPosition(engine,position);}
void __cdecl DeleteTrain(CVehicle*engine){
    if(g_nativeMutation){g_originalRemove(engine);return;}
    auto key=KnownKey(engine);auto copy=g_copies.find(key);
    if(copy!=g_copies.end()){
        ForwardControl(engine,4);RemoveCopy(copy->second);g_copies.erase(copy);return;
    }
    if(key){SendGone(key);g_ids.erase(engine);g_kinds.erase(engine);g_engineRefs.erase(engine);}
    g_originalRemove(engine);
}
using TrainControlFn=void(__fastcall*)(CVehicle*,void*);TrainControlFn g_originalControl=nullptr;
void __fastcall TrainControl(CVehicle*train,void*edx){
    if(Net().Online()&&core::IsPlayerReady())for(auto&[key,copy]:g_copies){
        auto snapshot=g_snapshots.find(key);if(snapshot==g_snapshots.end()||!IsEngine(copy.engine)||GetVehicleRef(copy.engine)!=copy.handle)continue;
        auto*t=copy.engine;for(auto&car:snapshot->second.consist){
            if(!t||!IsVehicleValid(t))break;
            if(t==train&&((car.nativeFlags&kOffRail)||car.status==5||car.health<=0))return;
            t=NextCarriage(t);
        }
    }
    g_originalControl(train,edx);
}
void YieldUnloadedSources(){
    auto*me=FindPlayerPed(0);if(!me)return;std::vector<std::pair<uint32_t,uint8_t>>transfers;
    for(auto&[engine,key]:g_ids){
        if(!IsEngine(engine)||g_engineRefs[engine]!=GetVehicleRef(engine)||engine->AreaCode()==13||engine->AreaCode()==me->AreaCode())continue;
        uint8_t selected=255;float distance=300.f;
        for(auto&[id,p]:Players::Instance().All())if(p.ped&&p.hasState&&NowMs()-p.stateRecvMs<1500&&p.state.health&&p.state.area==engine->AreaCode()){
            float candidate=(p.state.pos-engine->GetPosition()).Length();if(candidate<distance){distance=candidate;selected=id;}
        }
        if(selected<3)transfers.push_back({key,selected});
    }
    for(auto&[key,peer]:transfers)Transfer(key,peer,true);
}

void LostAuthority(uint8_t peer){
    std::vector<uint32_t>keys;for(auto&[key,a]:g_authorities)if(a.peer==peer)keys.push_back(key);
    auto next=Successor(peer);
    for(auto key:keys){
        if(next<3)Transfer(key,next,false);
        else{if(auto c=g_copies.find(key);c!=g_copies.end()){RemoveCopy(c->second);g_copies.erase(c);}auto a=g_authorities.find(key);if(a!=g_authorities.end())RetireIdentity(key,a->second.generation);}
    }
    if(Net().IsHost())SendRegistry();
}

// the partner near the host makes no random trains: the host's game makes them for both
void SwitchRandomTrains() {
    bool off = false;
    g_trainSources=ElectPopulationSources(PopulationViewers(),Net().HostId(),g_trainSources,kOwnRandomFrom,kOwnRandomFrom+40);
    if(Net().MyId()<3){auto source=g_trainSources[Net().MyId()];off=source<3&&source!=Net().MyId();}
    if (off == g_randomOff) return;
    Ref<uint8_t>(kDisableRandomTrains) = off ? 1 : 0;
    g_randomOff = off;
}

class Trains : public Module {
    void OnEntityReplaced(uint8_t kind,uint8_t scope,uint16_t id)override{
        if(kind!=ENTITY_TRAIN)return;auto key=TrainKey(scope,id);g_pending.erase(key);g_latest.erase(key);g_snapshots.erase(key);g_authorities.erase(key);g_retired.erase(key);
        if(auto i=g_copies.find(key);i!=g_copies.end()){RemoveCopy(i->second);g_copies.erase(i);}
        for(auto i=g_ids.begin();i!=g_ids.end();++i)if(i->second==key){auto*engine=i->first;
            Copy old{engine,g_kinds[engine],0,0,0,g_engineRefs[engine]};RemoveCopy(old);g_kinds.erase(engine);g_engineRefs.erase(engine);g_ids.erase(i);break;}
    }

public:
    const char* Name() const override { return "Trains"; }
    void OnInstallHooks() override {
        mem::HookFn(0x6F7550,&HookCreateMissionTrain,&g_origCreateTrain,"CTrain::CreateMissionTrain");
        mem::HookFn(0x6F86A0,&TrainControl,&g_originalControl,"CTrain::ProcessControl replicated derail state");
        mem::HookFn(0x6F5E20,&SetTrainSpeed,&g_originalSpeed,"CTrain::SetTrainSpeed current authority");
        mem::HookFn(0x6F5E50,&SetTrainCruise,&g_originalCruise,"CTrain::SetTrainCruiseSpeed current authority");
        mem::HookFn(0x6F5DC0,&DeleteTrain,&g_originalRemove,"CTrain::RemoveOneMissionTrain current authority");
        mem::HookFn(0x6F5DF0,&ReleaseTrain,&g_originalRelease,"CTrain::ReleaseOneMissionTrain current authority");
        mem::HookFn(0x6F7140,&PositionTrain,&g_originalPosition,"CTrain::SetNewTrainPosition current authority");
        Net().SetHandler(proto::G_TRAIN,&HandleTrain);
    }
    void OnSessionEnd() override {
        std::vector<Copy>adopted;
        for(auto&[engine,key]:g_ids)if(Scope(key)!=Net().MyId()&&IsEngine(engine)&&g_engineRefs[engine]==GetVehicleRef(engine))
            adopted.push_back({engine,g_kinds[engine],0,0,0,GetVehicleRef(engine)});
        for(auto&copy:adopted)RemoveCopy(copy);
        RemoveCopies();TrainSupports().clear();g_engineRefs.clear();g_ids.clear();g_kinds.clear();g_authorities.clear();g_snapshots.clear();g_retired.clear();g_worldCandidates.clear();
        if(g_randomOff)Ref<uint8_t>(kDisableRandomTrains)=0,g_randomOff=false;
    }
    void OnPeerLeave(uint8_t id)override{LostAuthority(id);}
    void OnPeerResync(uint8_t)override{
        // Resnapshot alone does not surrender a live driver's authority.
        // Real native reloads release before shutdown; disconnects elect once.
        // A reload is also a request for a coherent complete train snapshot.
        for(auto&[engine,key]:g_ids)if(IsEngine(engine)&&g_engineRefs[engine]==GetVehicleRef(engine))SendTrainState(engine,g_kinds[engine],key,true);
        if(Net().IsHost())SendRegistry();
    }
    void OnPeerJoin(uint8_t)override{g_registrySent=0;}
    void OnHostChanged(uint8_t)override{
        std::vector<uint8_t>missing;for(auto&[key,a]:g_authorities)if(!Net().Peer(a.peer)&&std::find(missing.begin(),missing.end(),a.peer)==missing.end())missing.push_back(a.peer);
        for(auto peer:missing)LostAuthority(peer);if(Net().IsHost())SendRegistry();
    }
    void OnGameShutdown()override{
        if(Net().Online()){
            auto next=Successor(Net().MyId());std::vector<uint32_t>keys;for(auto&[engine,key]:g_ids)keys.push_back(key);
            if(next<3)for(auto key:keys)Transfer(key,next,true);
        }
        // Native pools are about to be destroyed; never retain their pointers.
        g_copies.clear();g_kinds.clear();g_ids.clear();g_engineRefs.clear();g_worldCandidates.clear();TrainSupports().clear();g_pending.clear();
    }
    void OnGameStarted() override {
        g_copies.clear();g_kinds.clear();g_ids.clear();g_pending.clear();g_latest.clear();g_engineRefs.clear();TrainSupports().clear();g_worldCandidates.clear();
        auto*pool=VehiclePool();for(int i=0;pool&&i<pool->size;++i)if(pool->byteMap[i]>=0){auto*t=(CVehicle*)(pool->objects+(size_t)i*kVehiclePoolElemSize);if(IsEngine(t))g_worldCandidates[t]=GetVehicleRef(t);}
        g_randomOff=false;g_trainSources={255,255,255};g_registrySent=0;
        for(auto&[key,a]:g_authorities)if(a.peer==Net().MyId())if(auto s=g_snapshots.find(key);s!=g_snapshots.end()){
            auto pending=s->second;pending.received=NowMs();g_pending[key]=pending;
        }
    }
    void OnPreProcess() override {
        if(!Net().Online()||!core::IsPlayerReady()||core::IsRestarting())return;
        SwitchRandomTrains();
        if(!g_pending.empty()){auto pending=*g_pending.begin();if(NowMs()-pending.second.received>3000)g_pending.erase(pending.first);else ApplyTrain(pending.first,pending.second);}
        for(auto it=g_copies.begin();it!=g_copies.end();){
            auto key=it->first;auto&c=it->second;auto a=g_authorities.find(key);auto snapshot=g_snapshots.find(key);
            bool persistent=snapshot!=g_snapshots.end()&&!snapshot->second.consist.empty()&&((snapshot->second.consist[0].nativeFlags&kMissionTrain)||HasAttachments(c.engine));
            bool freshAuthority=a!=g_authorities.end()&&Net().Peer(a->second.peer);
            if(!IsEngine(c.engine)||GetVehicleRef(c.engine)!=c.handle||(!persistent&&NowMs()-c.ms>10000)||(!freshAuthority&&NowMs()-c.ms>10000)){
                RemoveCopy(c);it=g_copies.erase(it);continue;
            }
            if(a!=g_authorities.end()&&a->second.peer==Net().MyId()){++it;Promote(key);continue;}
            auto age=NowMs()-c.ms;
            Speed(c.engine)=age>500?0:c.speed;RailDistance(c.engine)=c.rail+c.speed*(float)std::min<uint64_t>(age,250)/20.f;
            if(snapshot!=g_snapshots.end()){
                auto*t=c.engine;for(auto&look:snapshot->second.consist){
                    if(!t||!IsVehicleValid(t))break;
                    if((look.nativeFlags&kOffRail)||look.status==5||look.health<=0||
                       (look.area!=13&&look.area!=FindPlayerPed(0)->AreaCode()))PlacePhysicalCarriage(t,look,age);
                    t=NextCarriage(t);
                }
            }
            ++it;
        }
    }
    void OnPostProcess() override {
        if(!Net().Online()||!core::IsPlayerReady()||core::IsRestarting()||NowMs()-g_sendMs<100)return;
        g_sendMs=NowMs();YieldUnloadedSources();SendOurs();
        if(NowMs()-g_registrySent>1500)SendRegistry();
        TrainSupports().clear();
        for(auto&[engine,key]:g_ids)if(IsEngine(engine)&&g_engineRefs[engine]==GetVehicleRef(engine)){
            auto a=g_authorities.find(key);if(a!=g_authorities.end())PublishSupports(Scope(key),Number(key),a->second.generation,engine);
        }
        for(auto&[key,c]:g_copies)if(IsEngine(c.engine)&&GetVehicleRef(c.engine)==c.handle)PublishSupports(Scope(key),Number(key),c.generation,c.engine);
    }

    static Trains& Instance() {
        static Trains m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Trains::Instance()); }
} g_reg;
}  // namespace

bool DescribeTrainAttachment(game::CVehicle*vehicle,TrainAttachment&out){
    out={};if(!Net().Online()||!core::IsPlayerReady()||!vehicle||!game::IsVehicleValid(vehicle)||!vehicle->IsTrain())return false;
    auto find=[&](game::CVehicle*engine,uint8_t owner,uint16_t id,uint64_t generation){
        for(uint8_t carriage=0;carriage<16&&engine&&game::IsVehicleValid(engine);engine=NextCarriage(engine),++carriage){
            if(engine==vehicle){out={id,owner,carriage,generation};return out.Valid();}
        }
        return false;
    };
    for(auto&[key,c]:g_copies)if(IsEngine(c.engine)&&GetVehicleRef(c.engine)==c.handle&&find(c.engine,(uint8_t)(key>>16),(uint16_t)key,c.generation))return true;
    for(auto&[engine,kind]:g_kinds){
        if(!IsEngine(engine)||g_engineRefs[engine]!=GetVehicleRef(engine))continue;
        bool wasKnown=g_ids.count(engine)!=0;auto id=EnsureOwnedTrain(engine);if(!id)continue;
        auto generation=Net().EntityGeneration(ENTITY_TRAIN,Number(id),Scope(id));
        if(find(engine,Scope(id),Number(id),generation)){PublishSupports(Scope(id),Number(id),generation,engine);if(!wasKnown)SendTrainState(engine,kind,id);return true;}
    }
    return false;
}
bool HandoverTrainAuthority(game::CVehicle*engine,uint8_t peer){
    auto key=KnownKey(engine);auto i=g_authorities.find(key);if(!key||peer>=3||!Net().Peer(peer)||i==g_authorities.end()||i->second.peer!=Net().MyId()||peer==Net().MyId())return false;
    Transfer(key,peer,true);return true;
}
uint8_t TrainAuthorityOf(const TrainAttachment&a){
    auto i=g_authorities.find(TrainKey(a.owner,a.id));return a.Valid()&&i!=g_authorities.end()&&i->second.generation==a.generation?i->second.peer:255;
}
game::CVehicle*ResolveTrainAttachment(const TrainAttachment&a){
    if(!a.Valid()||TrainAuthorityOf(a)>=3||!Net().EntityLive(ENTITY_TRAIN,a.id,a.owner)||Net().EntityGeneration(ENTITY_TRAIN,a.id,a.owner)!=a.generation)return nullptr;
    auto i=TrainSupports().find(TrainSupportKey(a.owner,a.id,a.carriage));
    return i!=TrainSupports().end()&&i->second.generation==a.generation?ResolveTrainSupport(i->first):nullptr;
}
}  // namespace coop
