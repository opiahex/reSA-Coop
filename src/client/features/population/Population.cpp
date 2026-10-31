// Population control for co-op:
//  * near the host only the host generates random peds, traffic and parked cars (the partner sees the
//    host's population through Peds/Vehicles sync);
//  * entities owned by this game are not removed while the other player is close to them
//    (CPopulation::ManagePed / CCarCtrl::PossiblyRemoveVehicle measure distance to the nearest player);
//  * farther from this game's player, an ordinary ped or car the other player is nearer to goes to the other game
//    instead of being dropped here (it vanished in front of the other player).
#include <algorithm>
#include <cmath>

#include "features/population/Population.h"
#include "shared/api/police/Police.h"
#include "common/lib/log/Log.h"
#include "common/lib/diagnostics/FlightRecorder.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/activity/Activities.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/sync/PopulationShare.h"

namespace coop {

namespace {
using namespace game;

// partner closer than this to the host: the host's population (spawned 40-55 m around the host) covers both
constexpr float kShareRadius = 60.0f;
constexpr float kShareRadiusOff = 90.0f;  // hysteresis

bool g_suppress = false;
CVector g_populationCentre;
int8_t g_populationArea=0;
uint64_t g_cullAt=0;

enum PopulationDiagnosticReason:uint16_t { PG_OK=0,PG_NATIVE_RETURNED,PG_SUPPRESSED,PG_POOL_UNAVAILABLE,PG_OBSERVATION_BUSY,PG_OBSERVATION_BUDGET,PG_CANCELLED,PG_CONTEXT };
flight::Context PopulationDiagnosticContext(){flight::Context c{};c.source=Net().MyId();c.incarnation=Net().MyIncarnation();c.world=Net().WorldEpoch();return c;}
void RecordPopulationDecision(uint16_t stage,uint16_t reason,unsigned operation,uint8_t viewer,uint8_t previous,uint8_t leader,int area,bool requested=false,bool forwarded=false,const flight::Context*origin=nullptr){
    if(!flight::Enabled())return;flight::Event e{};e.context=origin?*origin:PopulationDiagnosticContext();e.stage=stage;e.reason=reason;e.localPeer=Net().MyId();e.context.recipient=viewer;
    flight::AddUnsigned(e,flight::Metric::Operation,operation);
    flight::AddUnsigned(e,flight::Metric::PreviousLeader,previous);flight::AddUnsigned(e,flight::Metric::ZoneLeader,leader);
    if(stage!=782&&area>=0&&area<=18)flight::AddSigned(e,flight::Metric::ObservedArea,area);
    flight::AddUnsigned(e,flight::Metric::RequestedDecision,requested);flight::AddUnsigned(e,flight::Metric::ObservedDecision,forwarded);
    (void)flight::Record(e);
}
struct PopulationGeneratorDiagnosticState {uint64_t last=0;bool known=false,forwarded=false;};
std::array<PopulationGeneratorDiagnosticState,6>populationGeneratorDiagnostics{};
int DiagnosticPopulationCount(uint8_t kind){
    auto*p=kind==ENTITY_PED?PedPool():VehiclePool();if(!p||!p->byteMap||p->size<0||p->size>4096)return -1;
    int count=0;for(int i=0;i<p->size;++i)if(p->byteMap[i]>=0)++count;return count;
}
struct PopulationGeneratorObservation {
    flight::Context context{};unsigned operation=0;uint8_t kind=0;int before=-1;bool active=false,forwarded=false,requested=false,changed=false;
    PopulationGeneratorObservation(unsigned op,uint8_t type,bool requested,bool passed):operation(op),kind(type),forwarded(passed),requested(requested){
        if(!flight::Enabled()||op<1||op>populationGeneratorDiagnostics.size())return;
        context=PopulationDiagnosticContext();auto&d=populationGeneratorDiagnostics[op-1];changed=!d.known||d.forwarded!=passed;
        d.known=true;d.forwarded=passed;
        if(changed)RecordPopulationDecision(782,PG_OK,op,Net().MyId(),255,PopulationSources()[Net().MyId()<3?Net().MyId():0],g_populationArea,requested,passed);
        if(!passed||(!changed&&NowMs()-d.last<1000))return;d.last=NowMs();
        auto admission=flight::AdmitNativeObservation(context,783,Net().MyId(),false);
        if(admission!=flight::NativeAdmission::Admitted)return;
        before=DiagnosticPopulationCount(type);active=before>=0;
        if(!active)RecordPopulationDecision(782,PG_POOL_UNAVAILABLE,op,Net().MyId(),255,255,g_populationArea,requested,passed);
    }
    ~PopulationGeneratorObservation(){
        if(!flight::Enabled())return;
        if(changed)RecordPopulationDecision(782,forwarded?PG_NATIVE_RETURNED:PG_SUPPRESSED,operation,Net().MyId(),255,255,g_populationArea,requested,forwarded,&context);
        if(!active)return;
        if(context.source!=Net().MyId()||context.incarnation!=Net().MyIncarnation()||context.world!=Net().WorldEpoch()){
            RecordPopulationDecision(782,PG_CONTEXT,operation,Net().MyId(),255,255,g_populationArea,requested,forwarded,&context);return;
        }
        auto admission=flight::AdmitNativeObservation(context,783,Net().MyId(),false);if(admission!=flight::NativeAdmission::Admitted)return;
        int after=DiagnosticPopulationCount(kind);if(after<0){RecordPopulationDecision(782,PG_POOL_UNAVAILABLE,operation,Net().MyId(),255,255,g_populationArea);return;}
        flight::Event e{};e.context=context;e.stage=783;e.localPeer=Net().MyId();
        flight::AddUnsigned(e,flight::Metric::Operation,operation);flight::AddUnsigned(e,flight::Metric::EntityKind,kind);
        flight::AddUnsigned(e,flight::Metric::BeforeCount,(uint32_t)before);flight::AddUnsigned(e,flight::Metric::AfterCount,(uint32_t)after);
        flight::AddUnsigned(e,flight::Metric::RequestedDecision,requested);flight::AddUnsigned(e,flight::Metric::ObservedDecision,forwarded);(void)flight::Record(e);
    }
};

int LocalWantedLevel() {
    void* wanted=Call<0x56E230,void*>(-1);
    return wanted?Field<int32_t>(wanted,0x2C):0;
}

void UpdateSuppression() {
    const auto previousSources=PopulationSources();const auto previousViewers=PopulationViewers();
    bool want = false;
    uint8_t source=255;
    PopulationViewers()={};PopulationHost()=Net().HostId();
    if (Net().Online() && core::IsPlayerReady()) {
        CPlayerPed* me = FindPlayerPed(0);
        if(Net().MyId()<3)PopulationViewers()[Net().MyId()]={Net().MyId(),me->AreaCode(),true,me->GetPosition(),PopulationAvailable(Net().MyId())};
        for(const auto&[id,rp]:Players::Instance().All())if(id<3&&rp.hasState&&NowMs()-rp.stateRecvMs<5000)
            PopulationViewers()[id]={id,rp.state.area,true,rp.state.pos,PopulationAvailable(id)};
        // The host wins, then the lower player id. This also shares one population between two partners
        // travelling together far from the host, with no cycle where both stop generating traffic.
        PopulationSources()=ElectPopulationSources(PopulationViewers(),Net().HostId(),PopulationSources(),kShareRadius,kShareRadiusOff);
        source=Net().MyId()<3?PopulationSources()[Net().MyId()]:255;
        want=source<3&&source!=Net().MyId();
        if(want){g_populationCentre=PopulationViewers()[source].position;g_populationArea=PopulationViewers()[source].area;}
    }
    if (want != g_suppress || (want&&source!=NearbyPopulationShare().source)) LOGI("Population generation %s (source %u)", want ? "shared with a nearby player" : "local",want?source:Net().MyId());
    if (g_suppress && !want && Ref<float>(addr::TheCamera+0xBFC)>200.f) {
        // the engine adds one ped a frame at the edge of view: the empty streets around would fill slowly. The
        // countdowns of a game's start (and of a respawn) fill them at once
        Ref<uint8_t>(0xC0EC23) = 2;  // CPopulation::m_CountDownToPedsAtStart
        Ref<uint8_t>(0x9690C0) = 2;  // CCarCtrl::CountDownToCarsAtStart
    }
    if(flight::Enabled()){
        for(size_t i=0;i<3;++i){const auto&v=PopulationViewers()[i];const auto&before=previousViewers[i];
            if(previousSources[i]!=PopulationSources()[i]||v.active!=before.active||v.area!=before.area)
                RecordPopulationDecision(780,PG_OK,1,(uint8_t)i,previousSources[i],PopulationSources()[i],v.active?v.area:-1,v.active,v.active&&v.canGenerate);
        }
        const auto localArea=Net().MyId()<3&&PopulationViewers()[Net().MyId()].active?PopulationViewers()[Net().MyId()].area:-1;
        if(want!=g_suppress||(want&&source!=NearbyPopulationShare().source))RecordPopulationDecision(781,PG_OK,2,Net().MyId(),NearbyPopulationShare().source,source,localArea,g_suppress,want);
    }
    g_suppress = want;
    NearbyPopulationShare()={want,g_populationCentre,g_populationArea,source};
}

using UpdatePopFn = void(__cdecl*)(bool);
UpdatePopFn g_origPopUpdate = nullptr;
void __cdecl HookPopulationUpdate(bool generate) {
    PopulationGeneratorObservation diagnostic(1,ENTITY_PED,generate,generate&&!g_suppress);
    // Update(false) still runs GeneratePedsAtStartOfGame if the countdown is
    // nonzero. Do not create a local population burst behind the shared one.
    if(g_suppress)Ref<uint8_t>(0xC0EC23)=0;
    g_origPopUpdate(generate && !g_suppress);
}

using VoidFn = void(__cdecl*)();
VoidFn g_origGenerateRandomCars = nullptr;
void __cdecl HookGenerateRandomCars() {
    PopulationGeneratorObservation diagnostic(2,ENTITY_VEHICLE,true,!g_suppress);
    if (g_suppress) {PopulationBudgetExcluded()=0;return;}
    // GenerateOneRandomCar (0x4300D3..0x430132) includes NumMissionCars
    // in its global density limit. Network proxies use that classification
    // for protection; another simulator's far traffic must not exhaust this
    // generator's local street budget. Keep the real counters outside this
    // synchronous call and preserve every native increment/decrement within it.
    int excluded=0;
    auto*me=Net().Online()&&core::IsPlayerReady()?FindPlayerPed(0):nullptr;
    if(me)for(const auto&[id,nv]:Vehicles::Instance().All()){
        if(nv.owner==Net().MyId()||!Vehicles::Alive(nv)||nv.script||nv.playerUsed||Vehicles::Instance().IsRecent(id)||nv.veh->CreatedBy()!=2)continue;
        auto*v=nv.veh;bool player=v->Driver()&&v->Driver()->IsPlayer();for(int i=0;i<8;++i)player|=v->Passengers()[i]&&v->Passengers()[i]->IsPlayer();
        if(!player&&(v->AreaCode()!=me->AreaCode()||(v->GetPosition()-me->GetPosition()).Length2D()>170))++excluded;
    }
    auto&missions=Ref<int32_t>(0x96909C);excluded=std::min(excluded,std::max(0,missions));
    PopulationBudgetExcluded()=excluded;
    missions-=excluded;
    GeneratePoliceAwareTraffic(g_origGenerateRandomCars);
    missions+=excluded;
}

VoidFn g_origPlaneGeneration=nullptr;
void __cdecl HookPlaneGeneration(){PopulationGeneratorObservation diagnostic(3,ENTITY_VEHICLE,true,!g_suppress);if(!g_suppress)g_origPlaneGeneration();}
VoidFn g_origCarGenerators = nullptr;
VoidFn g_origRemoveAllPeds = nullptr;
void __cdecl HookRemoveAllRandomPeds(){
    if(!Net().Online()||!core::IsPlayerReady()||core::IsRestarting())return g_origRemoveAllPeds();
    auto*pool=PedPool();
    for(int i=0;pool&&i<pool->size;++i){
        if(pool->byteMap[i]<0)continue;
        auto*p=(CPed*)(pool->objects+(size_t)i*kPedPoolElemSize);
        if(!CallMethod<0x5DF100,bool>(p))continue;
        if(Peds::Instance().Find(p)&&p->Health()>0&&PopulationNeededAt(p->GetPosition(),AreaOf(p),140))continue;
        Call<0x610F20>(p);
    }
}
void __cdecl HookCarGeneratorsProcess() {
    PopulationGeneratorObservation diagnostic(4,ENTITY_VEHICLE,true,!g_suppress);
    if (g_suppress) return;
    g_origCarGenerators();
}

// the other player is in the game (states keep coming): what it is given goes nowhere while it loads a world
bool CanTake(uint8_t who) {
    const RemotePlayer* rp = Players::Instance().Get(who);
    return rp && rp->ped && NowMs() - rp->stateRecvMs < 1500;
}

// nearest remote player position (same interior) within maxDist, or nullptr
const CVector* NearestRemote(const CVector& p, float maxDist, int8_t area, uint8_t* who = nullptr) {
    const CVector* best = nullptr;
    float bestD = maxDist;
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.hasState || NowMs() - rp.stateRecvMs >= 1500 || rp.state.area != area) continue;
        float d = (rp.state.pos - p).Length2D();
        if (d < bestD) {
            bestD = d;
            best = &rp.state.pos;
            if (who) *who = id;
        }
    }
    return best;
}

using ManagePedFn = void(__cdecl*)(CPed*, const CVector*);
ManagePedFn g_origManagePed = nullptr;
void __cdecl HookManagePed(CPed* ped, const CVector* centre) {
    if (Net().Online() && ped && centre && !(ped->PedFlags2()&8)) {
        if(auto*np=Peds::Instance().Find(ped);np&&ped->Health()>0){
            const auto p=ped->GetPosition();const auto area=AreaOf(ped);
            if(np->owner!=Net().MyId())return;
            const auto recipient=PopulationOwnerAt(p,area);
            auto*me=FindPlayerPed(0);
            if(me&&(area!=me->AreaCode()||(p-me->GetPosition()).Length2D()>110)&&recipient!=255&&recipient!=Net().MyId()&&CanTake(recipient)&&Peds::Instance().QueueGiveAway(ped,recipient))return;
            // Native ManagePed deletes off-camera ambient peds after four
            // seconds, even with another real player looking straight at them.
            if(PopulationNeededAt(p,area,100))return;
        }
        CVector p = ped->GetPosition();
        float local = (p - *centre).Length2D();
        uint8_t who = 0;
        if (const CVector* r = NearestRemote(p, local, AreaOf(ped), &who)) {
            // far from this game's player and clearly nearer the other one: the other game keeps it (the margin keeps a
            // ped from going back and forth between two players standing together)
            if (local > 60.0f && (p - *r).Length2D() < local - 30.0f && CanTake(who) && Peds::Instance().QueueGiveAway(ped, who))
                return;
            // the other player still needs this ped, yet the engine culls by this game's camera too (off screen and
            // over 25 m: gone in 4 s). While this game has ground under it (collision loads ~120 m around its
            // player), a live one near the other player stays; farther it would fall through the map here, so it
            // goes as usual and the other player's own population takes over
            if (local < 100.0f && (p - *r).Length2D() < 50.0f && ped->Health() > 0.0f) return;
            g_origManagePed(ped, r);
            return;
        }
    }
    g_origManagePed(ped, centre);
}

using PossiblyRemoveFn = void(__cdecl*)(CVehicle*);
PossiblyRemoveFn g_origPossiblyRemove = nullptr;
void __cdecl HookPossiblyRemoveVehicle(CVehicle* v) {
    // kept for the other player while this game has ground under it (see HookManagePed), then given to it; a wreck
    // goes as usual (kept, they filled the vehicle pool around two players)
    uint8_t who = 0;
    if(Net().Online()&&v&&v->Status()!=5)if(auto*nv=Vehicles::Instance().Find(v)){
        const auto p=v->GetPosition();auto*me=FindPlayerPed(0);const auto recipient=PopulationOwnerAt(p,v->AreaCode());
        if(Vehicles::Instance().IsMirror(*nv))return;
        if(me&&(v->AreaCode()!=me->AreaCode()||(p-me->GetPosition()).Length2D()>130)&&recipient!=255&&recipient!=Net().MyId()&&CanTake(recipient)&&Vehicles::Instance().QueueGiveAway(v,recipient))return;
        if(nv->playerUsed||PopulationNeededAt(p,v->AreaCode(),140))return;
    }
    if (Net().Online() && v && v->Status() != 5 /* STATUS_WRECKED */)
        if (const CVector* r = NearestRemote(v->GetPosition(), 140.0f, v->AreaCode(), &who)) {
            float local = (v->GetPosition() - FindPlayerPed(0)->GetPosition()).Length2D();
            if (local < 100.0f) return;
            if ((v->GetPosition() - *r).Length2D() < local - 40.0f && CanTake(who) && Vehicles::Instance().QueueGiveAway(v, who))
                return;
        }
    g_origPossiblyRemove(v);
}

// Entry/house population uses other native paths than street generation. Keep
// the same host/lower-id authority there; "any remote is inside" would make
// both guests suppress each other and leave an interior empty.
bool OtherAuthorityInside(const CVector& at) {
    int area=Ref<int32_t>(0xB72914);
    if(!Net().Online()||!core::IsPlayerReady()||area==0||area==13)return false;
    for(auto&[id,rp]:Players::Instance().All()) {
        bool precedes=id==Net().HostId()||(!Net().IsHost()&&id<Net().MyId());
        if(precedes&&PopulationAvailable(id)&&rp.hasState&&NowMs()-rp.stateRecvMs<1500&&rp.state.area==area&&
           (rp.state.pos-at).Length()<80)return true;
    }
    return false;
}
using PopulateInteriorFn=void(__cdecl*)(int,CVector);
PopulateInteriorFn g_origPopulateInterior=nullptr;
void __cdecl HookPopulateInterior(int count,CVector at) {
    const bool generate=!OtherAuthorityInside(at);PopulationGeneratorObservation diagnostic(5,ENTITY_PED,true,generate);
    if(generate)g_origPopulateInterior(count,at);
}
using SetupPedsFn=void(__fastcall*)(void*,void*);
SetupPedsFn g_origSetupInteriorPeds=nullptr;
void __fastcall HookSetupInteriorPeds(void* group,void* edx) {
    auto* me=FindPlayerPed(0);
    const bool generate=!me||!OtherAuthorityInside(me->GetPosition());PopulationGeneratorObservation diagnostic(6,ENTITY_PED,true,generate);
    if(generate)g_origSetupInteriorPeds(group,edx);
}

class Population : public Module {
public:
    const char* Name() const override { return "Population"; }
    void OnInstallHooks() override {
        // Three retained population areas can exhaust vanilla's 500 task slots
        // before the ped pool fills. Native task Clone() then dereferences null
        // (e.g. 0x682DF6). Reserve task capacity, without increasing NPC counts.
        // Do not replace another plugin's constructor or larger pool setting.
        const uint8_t taskCapacity[] = {0x68,0xF4,0x01,0x00,0x00};
        if(mem::Check(0x55113E,taskCapacity,sizeof(taskCapacity))){
            mem::Put<uint32_t>(0x55113F,1500);
            LOGI("Native task pool capacity raised from 500 to 1500 for three population areas");
        }
        mem::RedirectCallFn(0x59690F,&HookSetupInteriorPeds,&g_origSetupInteriorPeds);
        mem::RedirectCallFn(0x43E983,&HookPopulateInterior,&g_origPopulateInterior);
        mem::HookFn(0x616650, &HookPopulationUpdate, &g_origPopUpdate, "CPopulation::Update");
        mem::HookFn(0x6CD2F0,&HookPlaneGeneration,&g_origPlaneGeneration,"CPlane::DoPlaneGenerationAndRemoval shared source");
        mem::HookFn(0x4341C0, &HookGenerateRandomCars, &g_origGenerateRandomCars, "CCarCtrl::GenerateRandomCars");
        mem::HookFn(0x6F3F40, &HookCarGeneratorsProcess, &g_origCarGenerators, "CTheCarGenerators::Process");
        mem::HookFn(0x611FC0, &HookManagePed, &g_origManagePed, "CPopulation::ManagePed");
        mem::HookFn(0x6122C0,&HookRemoveAllRandomPeds,&g_origRemoveAllPeds,"CPopulation::RemoveAllRandomPeds");
        mem::HookFn(0x424F80, &HookPossiblyRemoveVehicle, &g_origPossiblyRemove, "CCarCtrl::PossiblyRemoveVehicle");
    }
    void OnPreProcess() override {
        UpdateSuppression();
        if(g_suppress&&NowMs()-g_cullAt>125){
            g_cullAt=NowMs();
            Peds::Instance().RemoveSharedAmbient(g_populationCentre,g_populationArea);
            Vehicles::Instance().RemoveSharedAmbient(g_populationCentre,g_populationArea);
        }
    }
    void OnSessionEnd() override { if(flight::Enabled())RecordPopulationDecision(781,PG_CANCELLED,3,Net().MyId(),NearbyPopulationShare().source,255,-1);populationGeneratorDiagnostics={};g_suppress = false; NearbyPopulationShare()={};PopulationViewers()={};PopulationSources()={255,255,255};PopulationBudgetExcluded()=0; }
};

Population g_pop;
struct Reg {
    Reg() { core::RegisterModule(&g_pop); }
} g_reg;
}  // namespace

bool PopulationSuppressed() { return g_suppress; }

}  // namespace coop
