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
    uint64_t generation=0;
};
std::map<uint32_t, Copy> g_copies;  // by owner << 16 | its id
bool g_randomOff = false;           // we switched the random trains off
struct CarriageLook {
    uint16_t model=0;std::array<int8_t,2>components{-1,-1};std::array<uint8_t,4>colours{};uint8_t flags=3;
    void Write(Writer&w)const{w.U16(model);for(auto c:components)w.I8(c);for(auto c:colours)w.U8(c);w.U8(flags);}
    bool Read(Reader&r){model=r.U16();for(auto&c:components)c=r.I8();for(auto&c:colours)c=r.U8();flags=r.U8();
        return r.Ok()&&std::find(std::begin(kTrainModels),std::end(kTrainModels),model)!=std::end(kTrainModels)&&flags<=3&&
            std::all_of(components.begin(),components.end(),[](auto c){return c>=-1&&c<=5;})&&std::all_of(colours.begin(),colours.end(),[](auto c){return c<128;});}
};
struct PendingTrain {uint8_t kind=0;int8_t track=0;bool clockwise=false;float rail=0,speed=0;CVector pos;uint64_t received=0,generation=0;std::vector<CarriageLook>consist;};
std::map<uint32_t,PendingTrain> g_pending;
std::map<uint32_t,uint32_t> g_latest;
std::map<uint32_t,std::pair<const char*,uint64_t>>g_waitLog;
void TrainWait(uint32_t key,const char*reason){
    auto&state=g_waitLog[key];if(state.first!=reason||NowMs()-state.second>=1000){state={reason,NowMs()};
        LOGD("Train %u of player %u waiting: %s models=%d,%d vehiclePoolSize=%d pedPoolSize=%d",(uint16_t)key,key>>16,reason,IsModelLoaded(537),IsModelLoaded(569),VehiclePool()?VehiclePool()->size:0,PedPool()?PedPool()->size:0);}
}
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

void PublishSupports(uint8_t owner,uint16_t id,uint64_t generation,CVehicle*engine){
    for(uint8_t index=0;index<16&&engine&&IsVehicleValid(engine);engine=NextCarriage(engine),++index)
        TrainSupports()[TrainSupportKey(owner,id,index)]={engine,GetVehicleRef(engine),generation};
}
uint16_t EnsureOwnedTrain(CVehicle*engine){
    if(!IsEngine(engine)||!g_kinds.count(engine)||g_engineRefs[engine]!=GetVehicleRef(engine))return 0;
    auto existing=g_ids.find(engine);if(existing!=g_ids.end())return existing->second;
    for(unsigned attempt=0;attempt<255;++attempt){
        uint16_t next=(uint16_t)(g_nextId++&255u);if(!next)continue;
        bool used=false;for(auto&[other,id]:g_ids)if(id==next){used=true;break;}if(used)continue;
        auto generation=Net().BeginEntity(ENTITY_TRAIN,next);g_ids[engine]=next;PublishSupports(Net().MyId(),next,generation,engine);return next;
    }
    return 0; // all identities are in use: retry rather than overwriting one
}
std::vector<CarriageLook> CaptureConsist(CVehicle*engine){
    std::vector<CarriageLook>out;
    for(auto*t=engine;t&&IsVehicleValid(t);t=NextCarriage(t)){
        if(out.size()>=15)return {}; // the native configuration has 16 slots, including its terminator
        CarriageLook c;c.model=(uint16_t)t->ModelIndex();for(int i=0;i<2;++i)c.components[i]=Field<int8_t>(t,0x438+i);
        memcpy(c.colours.data(),(uint8_t*)t+0x434,4);c.flags=((t->Flags()&128u)?1:0)|((t->Flags()&1u)?2:0);out.push_back(c);
    }
    return out;
}
void SendTrainState(CVehicle*t,uint8_t kind,uint16_t id){
    auto consist=CaptureConsist(t);if(consist.empty())return;auto p=t->GetPosition();
    Writer w(proto::G_TRAIN);w.U8(TRAIN_STATE);w.Entity(ENTITY_TRAIN,id,ENTITY_BIRTH|ENTITY_SNAPSHOT);
    w.U32((uint32_t)NowMs());w.U8(kind);w.I8(Track(t));w.U8((Flags(t)&kClockwise)?1:0);
    w.F32(RailDistance(t));w.F32(Speed(t));w.Vec({p.x,p.y,p.z});w.U8((uint8_t)consist.size());for(auto&c:consist)c.Write(w);
    Net().Send(proto::CH_STATE,w);
}
void SendOurs() {
    for(auto it=g_kinds.begin();it!=g_kinds.end();){
        auto*t=it->first;auto id=g_ids.find(t);
        if(!IsEngine(t)||g_engineRefs[t]!=GetVehicleRef(t)){
            if(id!=g_ids.end()){SendGone(id->second);g_ids.erase(id);}g_engineRefs.erase(t);it=g_kinds.erase(it);continue;
        }
        auto number=EnsureOwnedTrain(t);if(number){
            PublishSupports(Net().MyId(),number,Net().EntityGeneration(ENTITY_TRAIN,number,Net().MyId()),t);
            float distance=RemoteDistance(t->GetPosition(),0);
            for(auto*carriage=NextCarriage(t);carriage&&IsVehicleValid(carriage);carriage=NextCarriage(carriage))distance=std::min(distance,RemoteDistance(carriage->GetPosition(),0));
            if(distance<=kSendWithin)SendTrainState(t,it->second,number);
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
bool ConsistModelsMatch(CVehicle*engine,const std::vector<CarriageLook>&consist){
    auto*t=engine;for(auto&c:consist){if(!t||!IsVehicleValid(t)||t->ModelIndex()!=c.model)return false;t=NextCarriage(t);}return !t;
}
void ApplyCarriageLooks(CVehicle*engine,const std::vector<CarriageLook>&consist){
    auto*t=engine;for(auto&c:consist){
        if(!t||!IsVehicleValid(t))return;
        if(Field<int8_t>(t,0x438)!=c.components[0]||Field<int8_t>(t,0x439)!=c.components[1]){
            const auto saved=Ref<uint16_t>(0x8A6458);WorldRemove(t);
            for(int i=0;i<2;++i)Ref<int8_t>(0x8A6458+i)=c.components[i];
            CallMethod<0x6F5530>(t,(uint32_t)c.model);Ref<uint16_t>(0x8A6458)=saved;WorldAdd(t);
        }
        memcpy((uint8_t*)t+0x434,c.colours.data(),4);t->Flags()=(t->Flags()&~129u)|((c.flags&1)?128u:0)|((c.flags&2)?1u:0);t=NextCarriage(t);
    }
}
void ApplyTrain(uint32_t key,const PendingTrain&p){
    if(!Net().EntityLive(ENTITY_TRAIN,(uint16_t)key,(uint8_t)(key>>16))||Net().EntityGeneration(ENTITY_TRAIN,(uint16_t)key,(uint8_t)(key>>16))!=p.generation)return;
    auto it=g_copies.find(key);
    if(it!=g_copies.end()&&(it->second.kind!=p.kind||it->second.track!=p.track||it->second.clockwise!=p.clockwise||it->second.generation!=p.generation||
       !IsEngine(it->second.engine)||GetVehicleRef(it->second.engine)!=it->second.handle||!ConsistModelsMatch(it->second.engine,p.consist))){
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
        CVehicle *first=nullptr,*last=nullptr;g_copying=true;
        g_origCreateTrain(p.pos,p.clockwise,p.kind,&first,&last,node,p.track,true);g_copying=false;memcpy(configuration,saved.data(),sizeof saved);
        if(!first){g_pending[key]=p;TrainWait(key,"native_creation");return;}
        for(auto*t=first;t&&IsVehicleValid(t);t=NextCarriage(t)){t->DoorLock()=2;Field<int8_t>(t,0x5C0)=p.track;}
        Call<0x6F7140>(first,p.pos);
        it=g_copies.emplace(key,Copy{first,p.kind,p.rail,p.speed,p.received,GetVehicleRef(first),p.track,p.clockwise,p.generation}).first;
        LOGI("Train %u of player %u copied asynchronously (%u carriages) native=%d model=%u subclass=%d flags=%04X",(uint16_t)key,key>>16,(unsigned)p.consist.size(),GetVehicleRef(first),first->ModelIndex(),first->VehicleSubClass(),Flags(first));
    }
    ApplyCarriageLooks(it->second.engine,p.consist);
    it->second.rail=p.rail;it->second.speed=p.speed;it->second.ms=p.received;
    PublishSupports((uint8_t)(key>>16),(uint16_t)key,p.generation,it->second.engine);g_pending.erase(key);g_waitLog.erase(key);
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
    Vec3 p = r.Vec();auto count=r.U8();if(count<1||count>15)return;std::vector<CarriageLook>consist(count);for(auto&c:consist)if(!c.Read(r)){TrainWait(key,"invalid_carriage_appearance");return;}
    if (!r.Ok() || from>=3 || kind > 15 || track < 0 || track > 3) return;  // CTrain's kinds and tracks
    // CTrain::ProcessControl brings the distance back onto the track a length at a time: NaN or a huge one froze it
    if (!std::isfinite(rail)||!std::isfinite(speed)||std::fabs(rail)>1e6f||std::fabs(speed)>50.f||
        !std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))return;
    if(g_latest.size()>=512&&!g_latest.count(key))return;
    g_latest[key]=stamp;ApplyTrain(key,{kind,track,clockwise,rail,speed,{p.x,p.y,p.z},NowMs(),Net().EntityGeneration(ENTITY_TRAIN,id,from),std::move(consist)});
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
                LOGD("Train %u of player %u copy retired valid=%d native=%d flags=%04X subclass=%d age=%llu",(uint16_t)it->first,it->first>>16,IsVehicleValid(c.engine),IsVehicleValid(c.engine)?GetVehicleRef(c.engine):-1,IsVehicleValid(c.engine)?Flags(c.engine):0,IsVehicleValid(c.engine)?c.engine->VehicleSubClass():-1,(unsigned long long)(NowMs()-c.ms));
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
        for(auto&[engine,id]:g_ids)if(IsEngine(engine)&&g_engineRefs[engine]==GetVehicleRef(engine))PublishSupports(Net().MyId(),id,Net().EntityGeneration(ENTITY_TRAIN,id,Net().MyId()),engine);
        for(auto&[key,c]:g_copies)if(IsEngine(c.engine)&&GetVehicleRef(c.engine)==c.handle)PublishSupports((uint8_t)(key>>16),(uint16_t)key,c.generation,c.engine);
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
        for(uint8_t carriage=0;carriage<16&&engine&&game::IsVehicleValid(engine);engine=NextCarriage(engine),++carriage)
            if(engine==vehicle){out={id,owner,carriage,generation};return out.Valid();}return false;
    };
    for(auto&[key,c]:g_copies)if(IsEngine(c.engine)&&GetVehicleRef(c.engine)==c.handle&&find(c.engine,(uint8_t)(key>>16),(uint16_t)key,c.generation))return true;
    for(auto&[engine,kind]:g_kinds){
        if(!IsEngine(engine)||g_engineRefs[engine]!=GetVehicleRef(engine))continue;
        bool wasKnown=g_ids.count(engine)!=0;auto id=EnsureOwnedTrain(engine);if(!id)continue;
        auto generation=Net().EntityGeneration(ENTITY_TRAIN,id,Net().MyId());
        if(find(engine,Net().MyId(),id,generation)){PublishSupports(Net().MyId(),id,generation,engine);if(!wasKnown)SendTrainState(engine,kind,id);return true;}
    }
    return false;
}
game::CVehicle*ResolveTrainAttachment(const TrainAttachment&a){
    if(!a.Valid()||!Net().EntityLive(ENTITY_TRAIN,a.id,a.owner)||Net().EntityGeneration(ENTITY_TRAIN,a.id,a.owner)!=a.generation)return nullptr;
    auto i=TrainSupports().find(TrainSupportKey(a.owner,a.id,a.carriage));
    return i!=TrainSupports().end()&&i->second.generation==a.generation?ResolveTrainSupport(i->first):nullptr;
}
}  // namespace coop
