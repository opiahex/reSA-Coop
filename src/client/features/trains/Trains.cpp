// Trains both games share. A train is its own game's (a random one, a mission's): that game tells the others where it
// is (kind, track, distance along the rails, speed), and they show it as a mission train of the same kind kept where the
// real one is, set every frame. The copies are locked, the driver is in the train's game. Random trains come from the
// host's game while the players are together: the partner's game makes its own only while its player is far from the
// host (CTrain::DisableRandomTrains). Whoever rides a train sits in the cab of its copy (Players). The kind of a train is
// not kept on it: it is noted when the train is made (CTrain::CreateMissionTrain makes the random ones too).
#include <cmath>
#include <map>

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
#include "shared/lib/sync/TrainSupport.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { TRAIN_STATE = 1, TRAIN_GONE = 2 };
constexpr uintptr_t kDisableRandomTrains = 0xC38010;  // CTrain::bDisableRandomTrains
constexpr uint16_t kFrontCarriage = 0x8, kClockwise = 0x40;  // CTrain::m_nTrainFlags (+0x5B8)
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
std::map<CVehicle*, uint16_t> g_ids;   // the ones announced
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
};
std::map<uint32_t, Copy> g_copies;  // by owner << 16 | its id
bool g_randomOff = false;           // we switched the random trains off
struct PendingTrain {uint8_t kind=0;int8_t track=0;bool clockwise=false;float rail=0,speed=0;CVector pos;uint64_t received=0;};
std::map<uint32_t,PendingTrain> g_pending;
std::map<uint32_t,uint32_t> g_latest;
std::array<uint8_t,3> g_trainSources{255,255,255};

void SendGone(uint16_t id);
std::map<CVehicle*,int>g_engineRefs;

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

void SendGone(uint16_t id) {
    Writer w(proto::G_TRAIN);
    w.U8(TRAIN_GONE);
    w.Entity(ENTITY_TRAIN,id,ENTITY_REMOVE);
    w.U32((uint32_t)NowMs());
    Net().Send(proto::CH_RELIABLE, w);
}

void SendOurs() {
    for (auto it = g_kinds.begin(); it != g_kinds.end();) {
        CVehicle* t = it->first;
        auto id = g_ids.find(t);
        if (!IsEngine(t)) {
            if (id != g_ids.end()) SendGone(id->second), g_ids.erase(id);
            it = g_kinds.erase(it);
            continue;
        }
        if(id==g_ids.end()){
            uint16_t next;bool used;do{next=(uint16_t)(g_nextId++&255u);used=!next;for(auto&[engine,n]:g_ids)used|=n==next;}while(used);
            Net().BeginEntity(ENTITY_TRAIN,next);id=g_ids.emplace(t,next).first;
        }
        CVector p = t->GetPosition();
        if (RemoteDistance(p, 0) > kSendWithin) {
            ++it;
            continue;
        }
        Writer w(proto::G_TRAIN);
        w.U8(TRAIN_STATE);
        w.Entity(ENTITY_TRAIN,id->second,ENTITY_BIRTH|ENTITY_SNAPSHOT);
        w.U32((uint32_t)NowMs());
        w.U8(it->second);
        w.I8(Track(t));
        w.U8((Flags(t) & kClockwise) ? 1 : 0);
        w.F32(RailDistance(t));
        w.F32(Speed(t));
        w.Vec({p.x, p.y, p.z});
        Net().Send(proto::CH_STATE, w);
        ++it;
    }
}

// the copy goes with whoever is in it put out first (a vehicle deleted with peds inside leaves them dangling)
void RemoveCopy(const Copy& c) {
    if (!IsEngine(c.engine)||GetVehicleRef(c.engine)!=c.handle) return;
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
    Call<0x6F5DC0>(c.engine);  // CTrain::RemoveOneMissionTrain
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
void ApplyTrain(uint32_t key,const PendingTrain&p){
    auto it=g_copies.find(key);
    if(it!=g_copies.end()&&(it->second.kind!=p.kind||it->second.track!=p.track||it->second.clockwise!=p.clockwise||
       !IsEngine(it->second.engine)||GetVehicleRef(it->second.engine)!=it->second.handle)){
        RemoveCopy(it->second);g_copies.erase(it);it=g_copies.end();
    }
    if(it==g_copies.end()){
        bool ready=true;for(int model:kTrainModels)if(!IsModelLoaded(model)){RequestModel(model,2);ready=false;}
        if(!ready){g_pending[key]=p;return;}
        if(!HasRoom(VehiclePool(),kKeepVehicles+10,"train")){g_pending[key]=p;return;}
        int node=NearestTrackNode(p.track,p.pos);if(node<0){g_pending[key]=p;return;}
        CVehicle *first=nullptr,*last=nullptr;g_copying=true;
        g_origCreateTrain(p.pos,p.clockwise,p.kind,&first,&last,node,p.track,true);g_copying=false;
        if(!first){g_pending[key]=p;return;}
        for(int model:kTrainModels)MarkModelNotNeeded(model);
        for(auto*t=first;t;t=NextCarriage(t)){t->DoorLock()=2;Field<int8_t>(t,0x5C0)=p.track;}
        Call<0x6F7140>(first,p.pos); // initialise native track/node cache at the authoritative location
        it=g_copies.emplace(key,Copy{first,p.kind,p.rail,p.speed,p.received,GetVehicleRef(first),p.track,p.clockwise}).first;
        LOGI("Train %u of player %u copied asynchronously",(uint16_t)key,key>>16);
    }
    it->second.rail=p.rail;it->second.speed=p.speed;it->second.ms=p.received;g_pending.erase(key);
}
void HandleTrain(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    uint16_t id = r.U16();
    uint32_t stamp=r.U32();
    if (!r.Ok() || !id || id>255 || !core::IsPlayerReady()) return;
    uint32_t key = (uint32_t)from << 16 | id;
    auto it = g_copies.find(key);
    auto previous=g_latest.find(key);if(previous!=g_latest.end()&&(int32_t)(stamp-previous->second)<=0)return;
    if (op == TRAIN_GONE) {
        g_latest[key]=stamp;g_pending.erase(key);
        if (it != g_copies.end()) RemoveCopy(it->second), g_copies.erase(it);
        return;
    }
    if(op!=TRAIN_STATE)return;
    uint8_t kind = r.U8();
    int8_t track = r.I8();
    bool clockwise = r.U8() != 0;
    float rail = r.F32(), speed = r.F32();
    Vec3 p = r.Vec();
    if (!r.Ok() || kind > 15 || track < 0 || track > 3) return;  // CTrain's kinds and tracks
    // CTrain::ProcessControl brings the distance back onto the track a length at a time: NaN or a huge one froze it
    if (!std::isfinite(rail)||!std::isfinite(speed)||std::fabs(rail)>1e6f||std::fabs(speed)>50.f||
        !std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))return;
    if(g_latest.size()>=512&&!g_latest.count(key))return;
    g_latest[key]=stamp;ApplyTrain(key,{kind,track,clockwise,rail,speed,{p.x,p.y,p.z},NowMs()});
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
    void OnEntityReplaced(uint8_t kind,uint8_t scope,uint16_t id)override{if(kind!=ENTITY_TRAIN)return;auto key=(uint32_t(scope)<<16)|id;g_pending.erase(key);g_latest.erase(key);if(auto i=g_copies.find(key);i!=g_copies.end()){RemoveCopy(i->second);g_copies.erase(i);}}

public:
    const char* Name() const override { return "Trains"; }

    void OnInstallHooks() override {
        mem::HookFn(0x6F7550, &HookCreateMissionTrain, &g_origCreateTrain, "CTrain::CreateMissionTrain");
        Net().SetHandler(proto::G_TRAIN, &HandleTrain);
    }
    void OnSessionEnd() override {
        TrainSupports().clear();g_engineRefs.clear();RemoveCopies();
        g_ids.clear();
        if (g_randomOff) Ref<uint8_t>(kDisableRandomTrains) = 0, g_randomOff = false;
    }
    void OnPeerLeave(uint8_t id) override { RemoveCopies(id); }
    void OnPeerResync(uint8_t id) override { RemoveCopies(id); }  // its game started over
    void OnGameStarted() override {
        g_copies.clear();  // the old world's trains are gone
        g_kinds.clear();
        g_ids.clear();
        g_randomOff = false;
        g_pending.clear();g_latest.clear();g_trainSources={255,255,255};TrainSupports().clear();g_engineRefs.clear();
    }

    void OnPreProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        SwitchRandomTrains();
        if(!g_pending.empty()){auto pending=*g_pending.begin();if(NowMs()-pending.second.received>3000)g_pending.erase(pending.first);else ApplyTrain(pending.first,pending.second);}
        // where the real train is now: its place and speed as sent, moved on by the time since (a step is 20 ms)
        for (auto it = g_copies.begin(); it != g_copies.end();) {
            Copy& c = it->second;
            if (!IsEngine(c.engine)||GetVehicleRef(c.engine)!=c.handle||NowMs()-c.ms>10000) {
                RemoveCopy(c);
                it = g_copies.erase(it);
                continue;
            }
            auto age=NowMs()-c.ms;
            Speed(c.engine)=age>500?0:c.speed;
            RailDistance(c.engine)=c.rail+c.speed*(float)std::min<uint64_t>(age,250)/20.f;
            ++it;
        }
    }

    void OnPostProcess() override {
        if (!Net().Online() || Net().Peers().empty() || !core::IsPlayerReady() || NowMs() - g_sendMs < 100) return;
        g_sendMs = NowMs();
        SendOurs();
        TrainSupports().clear();
        auto publish=[](uint8_t owner,uint16_t id,CVehicle*engine){
            int index=0;for(auto*t=engine;index<16&&t&&IsVehicleValid(t);t=NextCarriage(t),++index)
                TrainSupports()[TrainSupportKey(owner,id,(uint8_t)index)]={t,GetVehicleRef(t)};
        };
        for(auto&[engine,id]:g_ids)if(IsEngine(engine))publish(Net().MyId(),id,engine);
        for(auto&[key,c]:g_copies)if(IsEngine(c.engine)&&GetVehicleRef(c.engine)==c.handle)publish((uint8_t)(key>>16),(uint16_t)key,c.engine);
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

}  // namespace coop
