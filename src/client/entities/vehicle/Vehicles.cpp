#include "shared/lib/sync/ModelRequests.h"
#include "shared/api/vehicle-effects/VehicleEffects.h"
#include "shared/api/police/Police.h"
#include "entities/vehicle/Vehicles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/profile/Profile.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/activity/Activities.h"
#include "entities/player/Players.h"
#include "shared/lib/sync/PopulationShare.h"
#include "shared/api/net-session/Session.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-stage/StoryStage.h"
#include "shared/game/Script.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/sync/WorldResync.h"

namespace coop {

using namespace game;
using game::script::Cmd;

Vehicles& Vehicles::Instance() {
    static Vehicles v;
    return v;
}

namespace {
struct Reg {
    Reg() { core::RegisterModule(&Vehicles::Instance()); }
} g_reg;

constexpr uint8_t STATUS_PHYSICS = 3, STATUS_WRECKED = 5, STATUS_IS_TOWED = 10;
constexpr uint8_t VEHICLE_CREATED_RANDOM = 1, VEHICLE_CREATED_MISSION = 2, VEHICLE_CREATED_PARKED = 3;
constexpr float kTrafficIn = 170.0f;   // owned vehicles closer than this to the partner are announced (> car spawn range)
constexpr float kTrafficOut = 220.0f;  // and dropped from the network beyond this

void SetVehicleCreatedBy(CVehicle* v, uint8_t by) { CallMethod<0x6D5D70>(v, (int)by); }  // keeps CCarCtrl counters right

// A script of this game holds the car: its mission's list (CTheScripts::MissionCleanUp, 75 x {type, handle}, 1 a car)
// or the car STORE_CAR_CHAR_IS_IN stored (CTheScripts::StoreVehicleIndex). Such a car stays a mission car here.
bool ScriptHolds(CVehicle* v) {
    int h = GetVehicleRef(v);
    if (h == Ref<int32_t>(0xA447EC)) return true;
    for (int i = 0; i < 75; ++i)
        if (Ref<uint8_t>(0xA90850 + (uintptr_t)i * 8) == 1 && Ref<int32_t>(0xA90850 + (uintptr_t)i * 8 + 4) == h) return true;
    return false;
}

bool HasPlayerInside(CVehicle* v) {
    if (v->Driver() && v->Driver()->IsPlayer()) return true;
    for (int i = 0; i < 8; ++i)
        if (v->Passengers()[i] && v->Passengers()[i]->IsPlayer()) return true;
    return false;
}

bool OccupiedTowChain(CVehicle* v) {
    for(int depth=0;IsVehicleValid(v)&&depth<8;++depth){
        if(HasPlayerInside(v))return true;
        auto*parent=Field<CVehicle*>(v,0x4C4);if(parent==v)break;v=parent;
    }
    return false;
}

bool RetainedCarInUse(CVehicle* v) {
    if (!IsVehicleValid(v) || ScriptHolds(v) || OccupiedTowChain(v) || v->Driver()) return true;
    for (int i=0;i<8;++i) if (v->Passengers()[i]) return true;
    auto supports=[v](CPed* ped) {
        return ped && IsPedValid(ped) &&
            ((ped->InVehicle() && ped->Vehicle()==v) || Field<CEntity*>(ped,0x568)==v ||
             Field<CEntity*>(ped,0x584)==v || ped->AttachedTo()==v);
    };
    if (supports(FindPlayerPed(0))) return true;
    for (const auto& [id,rp] : Players::Instance().All()) if (supports(rp.ped)) return true;
    return false;
}

void SetVehicleMatrix(CVehicle* v, const CVector& pos, const Quat& q) {
    CMatrix* m = v->Matrix();
    if (!m) return;
    WorldRemove(v);
    q.ToMatrix(*m);
    m->pos = pos;
    CallMethod<0x446F90>(v);  // CEntity::UpdateRW
    CallMethod<0x532B00>(v);  // CEntity::UpdateRwFrame
    WorldAdd(v);
}
constexpr int kDamageManager = 0x5A0, kBikeTyres = 0x65C;  // CAutomobile::m_damageManager, CBike::m_nWheelStatus

bool HasDamageManager(CVehicle* v) {  // CAutomobile and its subclasses
    switch (v->VehicleSubClass()) {
    case VEHICLE_TYPE_AUTOMOBILE: case VEHICLE_TYPE_MTRUCK: case VEHICLE_TYPE_QUAD: case VEHICLE_TYPE_HELI:
    case VEHICLE_TYPE_PLANE: case VEHICLE_TYPE_TRAILER: return true;
    }
    return false;
}

// shows the damage of the owner's vehicle: parts are reset, then rebuilt from the damage state the way
// a loaded save does it (no debris flies off, the parts are just gone or bent)
void ApplyDamage(CVehicle* v, const uint8_t (&damage)[20]) {
    uint8_t* dm = (uint8_t*)v + kDamageManager + 4;
    if (!memcmp(dm, damage, sizeof(damage))) return;
    if (!v->RwObject() || !IsModelLoaded(v->ModelIndex())) {
        RequestModel(v->ModelIndex(), 2);
        return;  // retry after streaming; Fix must not recreate an unloaded clump
    }
    CallMethod<0x6A3440>(v);  // CAutomobile::Fix
    memcpy(dm, damage, sizeof(damage));
    switch (v->VehicleSubClass()) {  // SetupDamageAfterLoad of the exact class
    case VEHICLE_TYPE_HELI: CallMethod<0x6C4550>(v); break;
    case VEHICLE_TYPE_PLANE: CallMethod<0x6CAC10>(v); break;
    case VEHICLE_TYPE_QUAD: CallMethod<0x6CE340>(v); break;
    default: CallMethod<0x6B3E90>(v); break;
    }
}

VehicleLook CaptureLook(CVehicle* v) {
    VehicleLook l;
    if (HasDamageManager(v)) memcpy(l.damage, (uint8_t*)v + kDamageManager + 4, sizeof(l.damage));
    if (v->VehicleSubClass() == VEHICLE_TYPE_BIKE) memcpy(l.bikeTyres, (uint8_t*)v + kBikeTyres, 2);
    memcpy(l.colors, v->Colors(), 4);
    memcpy(l.upgrades, v->Upgrades(), sizeof(l.upgrades));
    l.lock = (uint8_t)v->DoorLock();
    int pj = -1;
    Cmd(0x0988).I(GetVehicleRef(v)).Out(&pj).Run();  // GET_CURRENT_VEHICLE_PAINTJOB
    l.paintjob = (int8_t)pj;
    return l;
}
}  // namespace

bool Vehicles::ScriptControlled(CVehicle*v){return v&&IsVehicleValid(v)&&ScriptHolds(v);}

bool VehicleLook::operator==(const VehicleLook& o) const {
    return !memcmp(colors, o.colors, 4) && !memcmp(upgrades, o.upgrades, sizeof(upgrades)) && paintjob == o.paintjob &&
           !memcmp(damage, o.damage, sizeof(damage)) && !memcmp(bikeTyres, o.bikeTyres, 2) && lock == o.lock;
}

bool IsSyncableVehicle(CVehicle* v) { return !v->IsTrain(); }

// A mirror has the AI-driven status (STATUS_PHYSICS), so every physics step ran its AI steering
// (CCarCtrl::SteerAICarWithPhysics): with no mission it brakes and pulls the handbrake, and the next frame's
// network speed pushed it on, so a mirror lurched like a car that keeps braking. Mirrors are driven by the network.
using SteerFn = void(__cdecl*)(CVehicle*);
SteerFn g_origSteerAI = nullptr;
void __cdecl HookSteerAICarWithPhysics(CVehicle* v) {
    if (Net().Online() && v)
        if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && Vehicles::Instance().IsMirror(*nv)) return;
    ProcessPoliceVehicle(v,g_origSteerAI);
}
using AddSearchLightFn=void(__cdecl*)(const CVector&,const CVector&,float,float,uint32_t,uint8_t,uint8_t);
AddSearchLightFn g_origAddSearchLight=nullptr;
void __cdecl HookAddSearchLight(const CVector& from,const CVector& to,float radius,float power,uint32_t corona,uint8_t clip,uint8_t shadow) {
    if(Ref<uint32_t>(0xC1C96C)<4)g_origAddSearchLight(from,to,radius,power,corona,clip,shadow);
}
using HornStateFn=void(__fastcall*)(void*,void*,bool*,void*);
HornStateFn g_origHornState=nullptr;
void __fastcall HookHornState(void* audio,void* edx,bool* on,void* params) {
    g_origHornState(audio,edx,on,params);
    if(!on||!params||!Net().Online())return;
    auto*vehicle=Field<CVehicle*>(params,0x10);
    if(!vehicle||!IsVehicleValid(vehicle))return;
    if(auto*n=Vehicles::Instance().Find(vehicle);n&&Vehicles::Instance().IsMirror(*n))*on=RemoteVehicleHorn(vehicle,*on);
}
using PlayHornFn=void(__fastcall*)(void*,void*,bool,bool,bool,void*);
PlayHornFn originalPlayHorn=nullptr;
void __fastcall PlayHorn(void*audio,void*edx,bool horn,bool siren,bool fast,void*params){
    auto*vehicle=params?Field<CVehicle*>(params,0x10):nullptr;
    auto*n=vehicle&&IsVehicleValid(vehicle)?Vehicles::Instance().Find(vehicle):nullptr;
    bool mirror=n&&Vehicles::Instance().IsMirror(*n);
    if(Net().Online()&&n){
        if(mirror){
            siren=n->hasState&&(n->state.flags&VehicleState::F_SIREN);
            fast=siren&&(n->state.flags&VehicleState::F_FAST_SIREN);
            horn=!siren&&RemoteVehicleHorn(vehicle,horn);
        }else CaptureVehicleHorn(vehicle,horn&&!siren);
    }
    // GTA latches the horn flag even when the sound allocation failed. Retry
    // an active horn once its sound bank/channel becomes available.
    if(horn&&!Field<void*>(audio,0x178))Field<uint8_t>(audio,0xBE)=0;
    originalPlayHorn(audio,edx,horn,siren,fast,params);
    if(mirror){static std::map<uint16_t,std::pair<bool,uint64_t>>reported;auto&last=reported[n->netId];
        if(last.first!=horn||(horn&&NowMs()-last.second>1000)){last={horn,NowMs()};auto*sound=Field<void*>(audio,0x178);
            LOGI("Horn native playback vehicle=%04X on=%d sound=%d volume=%.1f hardware=%d playing=%d",n->netId,horn,sound!=nullptr,sound?Field<float>(sound,0x60):-100.f,sound?Field<int16_t>(sound,0x5A):0,sound?Field<int16_t>(sound,0x5E):0);}}
}
using RenderLightsFn = void(__cdecl*)();
RenderLightsFn g_originalLights = nullptr;
using HeliPreRenderFn = void(__fastcall*)(CVehicle*,void*);
using ScanTowFn=void(__fastcall*)(CVehicle*,void*);
ScanTowFn originalScanTow=nullptr;
void __fastcall ScanTow(CVehicle*v,void*edx){
    auto*nv=Vehicles::Instance().Find(v);
    if(Net().Online()&&nv&&Vehicles::Instance().IsMirror(*nv))return;
    originalScanTow(v,edx);
}
HeliPreRenderFn g_originalHeliPreRender=nullptr;
void __fastcall HookHeliPreRender(CVehicle* heli,void* edx) {
    if(auto*nv=Vehicles::Instance().Find(heli);nv&&Vehicles::Instance().IsMirror(*nv)&&nv->hasState&&(nv->state.extras&VehicleState::X_ROTOR)){
        Field<float>(heli,0x84C)=nv->state.rotorSpeed;heli->SetEngineOn((nv->state.flags&VehicleState::F_ENGINE)!=0);
    }
    g_originalHeliPreRender(heli,edx);
    Vehicles::Instance().RenderSearchlights();
}
void __cdecl HookSearchlights() {
    Vehicles::Instance().RenderSearchlights();
    g_originalLights();
}

void Vehicles::ShareScriptLock(CVehicle*v){
    if(!Net().IsHost()||!v)return;
    auto*nv=EnsureNetworked(v);if(!nv)return;
    auto lock=(uint8_t)v->DoorLock();m_scriptLocks[nv->netId]=lock;LOGD("Story lock %04X set to %u (owner %u)",nv->netId,lock,nv->owner);
    Writer w(proto::G_SCRIPT_LOCK);w.Entity(ENTITY_VEHICLE,nv->netId);w.U8(lock);Net().Send(proto::CH_RELIABLE,w);
}
void Vehicles::ReleaseScriptLocks(){
    if(Net().IsHost()&&!m_scriptLocks.empty()){Writer w(proto::G_SCRIPT_LOCK);w.U16(0);w.U8(0);Net().Send(proto::CH_RELIABLE,w);}
    m_scriptLocks.clear();
}
void Vehicles::ShareScriptDeletion(CVehicle*v){
    if(!Net().Online()||!Net().IsHost()||!v)return;
    auto*nv=Find(v);if(!nv)return;const auto id=nv->netId;
    Writer w(proto::G_SCRIPT_VEH_REMOVE);w.Entity(ENTITY_VEHICLE,id,ENTITY_REMOVE);Net().Send(proto::CH_RELIABLE,w);
    m_scriptRemoved[id]=NowMs()+15000;m_pending.erase(id);m_pendingStates.erase(id);m_pendingLooks.erase(id);m_adoptPending.erase(id);m_scriptLocks.erase(id);
    // The native opcode still deletes the host's physical vehicle. Retire its
    // network record first so a stale driver cannot reclaim/reannounce it.
    Forget(id,false);LOGI("Story removed vehicle %04X for all players",id);
}
namespace {
using VehicleAIFn=bool(__fastcall*)(CVehicle*,void*,uint32_t*);
VehicleAIFn originalCarAI=nullptr,originalBikeAI=nullptr;
// A mirror's inputs belong to its owner. Native car AI still marked it
// handbraked during entry/carjacking even though AI steering was skipped.
bool __fastcall MirrorCarAI(CVehicle*v,void*edx,uint32_t*flags){
    auto&vehicles=Vehicles::Instance();auto*n=vehicles.Find(v);
    return n&&vehicles.IsMirror(*n)&&n->hasState&&WheelState::Count(v)?false:originalCarAI(v,edx,flags);
}
bool __fastcall MirrorBikeAI(CVehicle*v,void*edx,uint32_t*flags){
    auto&vehicles=Vehicles::Instance();auto*n=vehicles.Find(v);
    return n&&vehicles.IsMirror(*n)&&n->hasState?false:originalBikeAI(v,edx,flags);
}
}
void Vehicles::OnInstallHooks() {
    mem::HookFn(0x6B4800,&MirrorCarAI,&originalCarAI,"CAutomobile::ProcessAI (owner inputs)");
    mem::HookFn(0x6BC930,&MirrorBikeAI,&originalBikeAI,"CBike::ProcessAI (owner inputs)");
    Net().SetHandler(proto::G_SCRIPT_VEH_REMOVE,[](uint8_t from,Reader&r){
        auto id=r.U16();if(!r.Ok()||from!=Net().HostId()||Net().IsHost()||!id)return;
        auto&self=Instance();self.m_scriptRemoved[id]=NowMs()+15000;
        self.m_pending.erase(id);self.m_pendingStates.erase(id);self.m_pendingLooks.erase(id);self.m_adoptPending.erase(id);self.m_scriptLocks.erase(id);
        // Explicit story deletion also removes a locally adopted original.
        // Drop() would return that original to local traffic instead.
        self.m_restoreRecent.erase(std::remove_if(self.m_restoreRecent.begin(),self.m_restoreRecent.end(),[&](const auto&sp){return sp.netId==id;}),self.m_restoreRecent.end());
        self.Forget(id,core::IsPlayerReady());LOGI("Story vehicle %04X removed by host",id);
    });

    mem::HookFn(0x6C45B0,&HookAddSearchLight,&g_origAddSearchLight,"CHeli::AddHeliSearchLight");
    mem::HookFn(0x4F61E0,&HookHornState,&g_origHornState,"CAEVehicleAudioEntity::GetHornState");
    mem::HookFn(0x4F99D0,&PlayHorn,&originalPlayHorn,"CAEVehicleAudioEntity::PlayHornOrSiren network playback");
    Net().SetHandler(proto::G_SCRIPT_LOCK,[](uint8_t from,Reader&r){
        uint16_t id=r.U16();uint8_t lock=r.U8();if(!r.Ok()||from!=Net().HostId()||Net().IsHost())return;
        auto&self=Instance();if(!id){self.m_scriptLocks.clear();return;}self.m_scriptLocks[id]=lock;LOGD("Story lock %04X received: %u",id,lock);
        if(auto*nv=self.Find(id);nv&&IsVehicleValid(nv->veh))nv->veh->DoorLock()=lock;
    });
    mem::HookFn(0x6CF030,&ScanTow,&originalScanTow,"CTrailer::ScanForTowLink");
    mem::HookFn(0x6C5420,&HookHeliPreRender,&g_originalHeliPreRender,"CHeli::PreRender");
    mem::HookFn(0x6C7C50, &HookSearchlights, &g_originalLights, "CHeli::RenderAllHeliSearchLights");
    mem::HookFn(0x437C20, &HookSteerAICarWithPhysics, &g_origSteerAI, "CCarCtrl::SteerAICarWithPhysics");
    Net().SetHandler(proto::G_VEH_CREATE, [](uint8_t from, Reader& r) { Instance().HandleCreate(from, r); });
    Net().SetHandler(proto::G_VEH_REMOVE, [](uint8_t from, Reader& r) { Instance().HandleRemove(from, r); });
    Net().SetHandler(proto::G_VEH_SYNC, [](uint8_t from, Reader& r) { Instance().HandleSync(from, r); });
    Net().SetHandler(proto::G_VEH_OWNER, [](uint8_t from, Reader& r) { Instance().HandleOwner(from, r); });
    Net().SetHandler(proto::G_VEH_ATTACHMENT,[](uint8_t from,Reader&r){Instance().HandleScriptAttachment(from,r);});
    Net().SetHandler(proto::G_VEH_STATUS, [](uint8_t from, Reader& r) { Instance().HandleStatus(from, r); });
    Net().SetHandler(proto::G_VEH_HANDOVER, [](uint8_t from, Reader& r) { Instance().HandleHandover(from, r); });
    Net().SetHandler(proto::G_VEH_SCRIPT_FREE, [](uint8_t from, Reader& r) { Instance().HandleScriptFree(from, r); });
    Net().SetHandler(proto::G_VEH_CLEAR, [](uint8_t from, Reader& r) { Instance().HandleClear(from, r); });
    Net().SetHandler(proto::G_VEH_RECENT, [](uint8_t from, Reader& r) { Instance().HandleRecent(from, r); });
}

void Vehicles::RenderSearchlights() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    for (const auto& [id, nv] : m_byId) {
        if (!IsMirror(nv) || !nv.hasState || !IsVehicleValid(nv.veh) ||
            nv.veh->VehicleSubClass()!=VEHICLE_TYPE_HELI || NowMs()-nv.recvMs>750 ||
            nv.veh->AreaCode()!=FindPlayerPed(0)->AreaCode() ||
            !(nv.state.extras & VehicleState::X_SEARCHLIGHT) || nv.state.searchIntensity<=0.01f) continue;
        uint32_t count=Ref<uint32_t>(0xC1C96C);
        if(count>=4)break;
        CVector origin=nv.veh->GetPosition();origin.z-=1;
        bool present=false;
        for(uint32_t i=0;i<count;++i)
            if((Ref<CVector>(0xC1C990+i*0x4C)-origin).Length()<5)present=true;
        if(present)continue;
        Call<0x6C45B0>(&origin,&nv.state.searchTarget,16.0f,nv.state.searchIntensity,
                       (uint32_t)(uintptr_t)nv.veh,(uint8_t)0,(uint8_t)1);
    }
}

VehicleState Vehicles::Capture(CVehicle* vehicle) {
    auto state=VehicleState::Capture(vehicle);
    if(auto*n=Find(vehicle);n&&n->parentCollisionHeld)
        state.flags=(state.flags&~VehicleState::F_HIDDEN)|(n->parentWasVisible?0:VehicleState::F_HIDDEN);
    auto*child=Field<CVehicle*>(vehicle,0x4C8);
    auto*parent=Field<CVehicle*>(vehicle,0x4C4);
    if(IsVehicleValid(child)&&child!=vehicle)if(auto*n=EnsureNetworked(child))state.towChild=n->netId;
    if(IsVehicleValid(parent)&&parent!=vehicle)if(auto*n=EnsureNetworked(parent))state.towParent=n->netId;
    if(auto*physical=vehicle->AttachedTo();physical&&physical->IsVehicle()&&physical!=vehicle){
        auto*attached=(CVehicle*)physical;
        if(!attached->IsTrain()&&IsVehicleValid(attached))if(auto*n=EnsureNetworked(attached)){
            state.parentVehicle=n->netId;state.parentGeneration=Net().EntityGeneration(ENTITY_VEHICLE,n->netId);
            state.parentOffset=Field<CVector>(vehicle,0x100);state.parentRotation=Field<Quat>(vehicle,0x118);
            const auto&q=state.parentRotation;state.parentUsesEuler=q.x==0&&q.y==0&&q.z==0&&q.w==0;
            if(state.parentUsesEuler)state.parentEuler=Field<CVector>(vehicle,0x10C);
        }
    }
    if(auto*n=Find(vehicle);n&&n->scriptAttachment.attachmentRevision&&
       n->scriptAttachment.attachmentRun==StoryRunId()&&n->scriptAttachment.attachmentPhase==CooperativeStoryPhase()){
        const auto&claim=n->scriptAttachment;state.attachmentRun=claim.attachmentRun;
        state.attachmentPhase=claim.attachmentPhase;state.attachmentRevision=claim.attachmentRevision;
        state.parentVehicle=claim.parentVehicle;state.parentGeneration=claim.parentGeneration;
        state.parentOffset=claim.parentOffset;state.parentRotation=claim.parentRotation;
        state.parentUsesEuler=claim.parentUsesEuler;state.parentEuler=claim.parentEuler;
    }
    return state;
}

void Vehicles::SendScriptAttachment(NetVehicle&nv){
    if(!Net().IsHost()||!Alive(nv)||!nv.scriptAttachment.attachmentRevision)return;
    Writer w(proto::G_VEH_ATTACHMENT);w.Entity(ENTITY_VEHICLE,nv.netId,ENTITY_SNAPSHOT);
    auto current=Capture(nv.veh);current.time=(uint32_t)NowMs();
    current.Write(w);Net().Send(proto::CH_RELIABLE,w);nv.attachmentSentMs=NowMs();
}

void Vehicles::ShareScriptAttachment(CVehicle*v,bool attached){
    if(!Net().IsHost()||!StoryRunId()||!IsVehicleValid(v)||!ScriptHolds(v))return;
    auto*n=EnsureNetworked(v);if(!n)return;
    // Capture the native result before replacing the previous claim. A detach
    // must retain the engine's inherited velocity and optional release force.
    auto prior=n->scriptAttachment;n->scriptAttachment={};auto state=Capture(v);n->scriptAttachment=prior;
    if(attached!=bool(state.parentVehicle)||state.train.Valid())return;
    state.time=(uint32_t)NowMs();state.attachmentRun=StoryRunId();state.attachmentPhase=CooperativeStoryPhase();
    state.attachmentRevision=prior.attachmentRun==state.attachmentRun?prior.attachmentRevision+1:1;
    if(!state.attachmentRevision)++state.attachmentRevision;
    n->scriptAttachment=state;n->history.Clear();
    // Mark native local attachments too: an expired run may release only the
    // exact parent installed for this claim, never an unrelated new binding.
    if(state.parentVehicle)if(auto*p=Find(state.parentVehicle)){n->parentAttached=true;n->parentHandle=p->handle;n->parentGeneration=state.parentGeneration;}
    SendScriptAttachment(*n);
    LOGI("Native vehicle attachment vehicle=%04X parent=%04X run=%llu phase=%u revision=%u",n->netId,state.parentVehicle,(unsigned long long)state.attachmentRun,state.attachmentPhase,state.attachmentRevision);
}

void Vehicles::HandleScriptAttachment(uint8_t from,Reader&r){
    const auto id=r.U16();VehicleState state;if(!state.Read(r)||from!=Net().HostId()||Net().IsHost()||
       !core::IsPlayerReady()||
       !state.attachmentRevision||state.attachmentRun!=StoryRunId()||state.attachmentPhase!=CooperativeStoryPhase())return;
    auto*n=Find(id);if(!n||!Alive(*n)||state.parentVehicle==id)return;
    if(state.parentVehicle&&Net().ExpectedEntityGeneration(ENTITY_VEHICLE,state.parentVehicle)!=state.parentGeneration)return;
    auto&prior=n->scriptAttachment;
    if(prior.attachmentRun==state.attachmentRun&&prior.attachmentRevision&&
       (int32_t)(state.attachmentRevision-prior.attachmentRevision)<=0)return;
    prior=state;n->history.Clear();
    if(!state.parentVehicle){
        // This is the actual native 0684 result, not a second detach force.
        ApplyVehicleBinding(*n,state);SetVehicleMatrix(n->veh,state.pos,state.rot);
        n->veh->MoveSpeed()=state.vel*(1.f/50.f);n->veh->TurnSpeed()=state.turn*(1.f/50.f);
    }else ApplyVehicleBinding(*n,state);
    if(!n->hasState||n->state.attachmentRun!=state.attachmentRun||n->state.attachmentRevision!=state.attachmentRevision){
        n->state=state;n->hasState=true;n->lastTime=0;n->recvMs=NowMs();
    }
    LOGD("Native vehicle attachment received vehicle=%04X parent=%04X revision=%u",id,state.parentVehicle,state.attachmentRevision);
}

void Vehicles::ApplyTowLinks(NetVehicle& nv) {
    auto*v=nv.veh;
    auto*current=Field<CVehicle*>(v,0x4C8);
    auto*child=nv.state.towChild&&Net().EntityLink(ENTITY_VEHICLE,nv.netId,ENTITY_VEHICLE,nv.state.towChild)?Find(nv.state.towChild):nullptr;
    if(IsVehicleValid(current)&&(!child||current!=child->veh))CallVirtual<62,bool>(current);
    if(!child||!IsVehicleValid(child->veh)||child->veh==v||child->owner!=nv.owner||
       !child->hasState||child->state.towParent!=nv.netId||child->veh->AreaCode()!=v->AreaCode())return;
    auto*walk=child;
    for(int depth=0;walk&&depth<8;++depth){
        if(walk==&nv)return;
        walk=walk->state.towChild?Find(walk->state.towChild):nullptr;
        if(depth==7&&walk)return;
    }
    if(Field<CVehicle*>(child->veh,0x4C4)!=v&&(child->veh->GetPosition()-v->GetPosition()).Length()<35){
        if(CallVirtual<61,bool>(child->veh,v,false))LOGD("Tow link %04X -> %04X restored",nv.netId,child->netId);
    }
}

NetVehicle* Vehicles::Find(uint16_t netId) {
    auto it = m_byId.find(netId);
    return it==m_byId.end()||!Alive(it->second)||it->second.veh->ModelIndex()!=it->second.model?nullptr:&it->second;
}

NetVehicle* Vehicles::Find(CVehicle* v) {
    auto it = m_byPtr.find(v);
    auto*record=it==m_byPtr.end()?nullptr:Find(it->second);
    return record&&record->veh==v?record:nullptr;
}

bool Vehicles::IsMirror(const NetVehicle& nv) const { return nv.owner != Net().MyId(); }

bool Vehicles::IsRecent(uint16_t id) const {
    for (const auto& [player, history] : m_recent) if (history.Contains(id)) return true;
    return false;
}
void Vehicles::SendRecent() {
    Writer w(proto::G_VEH_RECENT);
    const auto& ids = m_recent[Net().MyId()].ids;
    w.U8((uint8_t)ids.size());
    for (uint16_t id : ids) w.Entity(ENTITY_VEHICLE,id);
    Net().Send(proto::CH_RELIABLE, w);
}
void Vehicles::RememberUsed(CVehicle* v) {
    if (!v || !IsSyncableVehicle(v)) return;
    if (auto* nv = EnsureNetworked(v); nv && m_recent[Net().MyId()].Touch(nv->netId)) SendRecent();
}
void Vehicles::HandleRecent(uint8_t from, Reader& r) {
    uint8_t n = r.U8();
    if (n > RecentVehicles::kLimit || !Net().Peer(from)) return;
    RecentVehicles history;
    for (uint8_t i = 0; i < n; ++i) { uint16_t id = r.U16(); if (id && !history.Contains(id)) history.ids.push_back(id); }
    if (r.Ok()) m_recent[from] = history;
}
void Vehicles::UpdateRecentProtection() {
    for (auto& [id, nv] : m_byId) {
        if (!Alive(nv)) continue;
        bool keep = IsRecent(id) || OccupiedTowChain(nv.veh) || (nv.playerUsed && !m_recent.count(nv.owner));
        bool changed = keep != nv.playerUsed;
        nv.playerUsed = keep;
        if (keep || IsMirror(nv)) Protect(nv);
        else if (nv.origBy || (changed && nv.createdByUs)) Release(nv);
        if (changed && !IsMirror(nv)) Announce(nv);
    }
}

NetVehicle* Vehicles::EnsureNetworked(CVehicle* v) {
    if (!v || !IsVehicleValid(v) || !IsSyncableVehicle(v)) return nullptr;
    if (NetVehicle* nv = Find(v)) return nv;
    // A retained foreign copy must not be announced under a new local ID
    // while the original birth is being received during a quick reconnect.
    for (auto it=m_retainedCopies.begin();it!=m_retainedCopies.end();++it) {
        const auto id=it->first;const auto saved=it->second;
        if (saved.handle!=GetVehicleRef(v) || saved.model!=v->ModelIndex()) continue;
        if (NowMs()<saved.expires || (Net().EntityLive(ENTITY_VEHICLE,id) &&
            Net().EntityGeneration(ENTITY_VEHICLE,id)==saved.generation) || RetainedCarInUse(v)) return nullptr;
        // The original birth did not return and nothing uses this foreign copy.
        // Delete it rather than re-announcing an orphan under a fresh local ID.
        m_retainedCopies.erase(it);
        StopEntering(v);
        Cmd(0x00A6).I(saved.handle).Run();
        return nullptr;
    }
    if(auto old=m_byPtr.find(v);old!=m_byPtr.end()){
        const auto id=old->second;auto stale=m_byId.find(id);
        if(stale!=m_byId.end()&&stale->second.owner==Net().MyId()){Writer gone(proto::G_VEH_REMOVE);gone.Entity(ENTITY_VEHICLE,id,ENTITY_REMOVE);gone.Bool(true);Net().Send(proto::CH_RELIABLE,gone);}
        Forget(id,false); // this native pool slot now belongs to another generation
    }
    if (!Net().Online()) return nullptr;
    uint16_t id=m_ids.Take((uint16_t)((Net().MyId()+1)<<12),NowMs(),[&](uint16_t v){return m_byId.count(v)||m_pending.count(v)||m_scriptRemoved.count(v);});
    if(!id)return nullptr;
    NetVehicle& nv = m_byId[id];
    nv.netId = id;Net().BeginEntity(ENTITY_VEHICLE,id);
    nv.veh = v;
    nv.handle = GetVehicleRef(v);
    nv.owner = Net().MyId();
    nv.model = v->ModelIndex();
    nv.script = nv.heldHere = ScriptHolds(v);
    nv.playerUsed = v->Driver() == FindPlayerPed(0);
    nv.firstAnnouncedMs=NowMs();
    nv.lastChangeMs = NowMs();
    m_byPtr[v] = id;
    Announce(nv);SendLook(nv);
    LOGI("Vehicle %04X (model %d) is now networked native=%d generation=%llu at=%.1f,%.1f,%.1f",id,nv.model,nv.handle,(unsigned long long)Net().EntityGeneration(ENTITY_VEHICLE,id),v->GetPosition().x,v->GetPosition().y,v->GetPosition().z);
    return &nv;
}

void Vehicles::Announce(const NetVehicle& nv) {
    CVehicle* v = nv.veh;
    VehicleSpawn sp;
    sp.netId = nv.netId;
    sp.model = nv.model;
    sp.owner = nv.owner;
    sp.pos = v->GetPosition();
    if (CMatrix* m = v->Matrix()) sp.rot = Quat::FromMatrix(*m);
    memcpy(sp.colors, v->Colors(), 4);
    for(int c=0;c<2;++c)sp.components[c]=Field<int8_t>(v,0x438+c);
    sp.health = v->Health();
    sp.area = v->AreaCode();
    sp.lockState = (uint8_t)v->DoorLock();
    // Temporary streaming protection is not script ownership.
    sp.script = nv.createdByUs ? nv.script : ScriptHolds(v);
    sp.playerUsed = nv.playerUsed;
    Writer w(proto::G_VEH_CREATE);
    sp.Write(w);
    Net().Send(proto::CH_RELIABLE, w);
}

void Vehicles::ReannounceOne(uint16_t id) { if(auto*n=Find(id);n&&!IsMirror(*n)){Announce(*n);SendState(*n,true);SendLook(*n);} }

void Vehicles::Reannounce() {
    SendRecent();
    if(Net().IsHost())for(auto&[id,nv]:m_byId)if(nv.scriptAttachment.attachmentRun==StoryRunId()&&nv.scriptAttachment.attachmentPhase==CooperativeStoryPhase())SendScriptAttachment(nv);
    if(Net().IsHost())for(auto&[id,lock]:m_scriptLocks){Writer w(proto::G_SCRIPT_LOCK);w.Entity(ENTITY_VEHICLE,id);w.U8(lock);Net().Send(proto::CH_RELIABLE,w);}
    for (auto& [id, nv] : m_byId)
        if (!IsMirror(nv) && IsVehicleValid(nv.veh)) {
            Announce(nv);
            SendLook(nv);
        }
}

void Vehicles::SendLook(NetVehicle& nv) {
    auto current=CaptureLook(nv.veh);
    if(nv.hasLook){
        bool tyreRepaired=false;
        if(nv.veh->VehicleSubClass()==VEHICLE_TYPE_BIKE){for(int i=0;i<2;++i)tyreRepaired|=nv.look.bikeTyres[i]&&!current.bikeTyres[i];}
        else if(HasDamageManager(nv.veh)){for(int i=1;i<=4;++i)tyreRepaired|=nv.look.damage[i]&&!current.damage[i];}
        if(tyreRepaired)++nv.repairRevision;
    }
    nv.look=current;
    nv.hasLook = true;
    Writer w(proto::G_VEH_STATUS);
    w.Entity(ENTITY_VEHICLE,nv.netId);
    w.Raw(nv.look.colors, 4);
    for (int16_t u : nv.look.upgrades) w.I16(u);
    w.I8(nv.look.paintjob);
    w.Raw(nv.look.damage, sizeof(nv.look.damage));
    w.Raw(nv.look.bikeTyres, 2);
    w.U8(nv.look.lock);
    w.U32(nv.repairRevision);
    Net().Send(proto::CH_RELIABLE, w);nv.lookSentMs=nv.lookCheckMs=NowMs();
}

void Vehicles::CloneAppearance(CVehicle*source,CVehicle*target){
    auto*n=Find(target);if(!n||!IsVehicleValid(source)||n->owner!=Net().MyId())return;
    n->look=CaptureLook(source);n->look.lock=0;memset(n->look.damage,0,sizeof n->look.damage);memset(n->look.bikeTyres,0,sizeof n->look.bikeTyres);n->hasLook=true;ApplyLook(*n);SendLook(*n);
}

void Vehicles::HandleStatus(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    VehicleLook l;
    r.Raw(l.colors, 4);
    for (int16_t& u : l.upgrades) u = r.I16();
    l.paintjob = r.I8();
    r.Raw(l.damage, sizeof(l.damage));
    r.Raw(l.bikeTyres, 2);
    l.lock = r.U8();
    uint32_t repairRevision=r.U32();
    NetVehicle* nv = Find(id);
    if(!r.Ok())return;
    if(!nv){
        if(auto p=m_pending.find(id);p!=m_pending.end()&&p->second.owner==from){
            auto old=m_pendingLooks.find(id);
            if(old==m_pendingLooks.end()||(int32_t)(repairRevision-old->second.second)>=0)m_pendingLooks[id]={l,repairRevision};
        }
        return;
    }
    if(nv->owner!=from)return;
    if((int32_t)(repairRevision-nv->repairRevision)<0)return;
    nv->repairRevision=repairRevision;nv->look = l;
    nv->hasLook = true;
    if(IsVehicleValid(nv->veh))nv->veh->DoorLock()=l.lock;
}

// mod shop work done in the owner's game: same parts (models streamed in first), paintjob, colours
void Vehicles::ApplyLook(NetVehicle& nv) {
    struct Restore{bool&flag;bool saved;~Restore(){flag=saved;}} restore{nv.applyingLook,nv.applyingLook};nv.applyingLook=true;
    CVehicle* v = nv.veh;
    memcpy(v->Colors(), nv.look.colors, 4);
    int h = nv.handle;
    for (int slot = 0; slot < 15; ++slot) {
        int16_t want = nv.look.upgrades[slot], have = v->Upgrades()[slot];
        if (want == have) continue;
        if (have >= 0) Cmd(0x06E8).I(h).I(have).Run();  // REMOVE_VEHICLE_MOD
        // a part of a mod this game lacks crashes the streamer; IsValidModForVehicle: what a mod shop here offers it
        if (want < 0 || !IsValidModel(want) || !Call<0x49B010, bool>(want, v)) continue;
        Cmd(0x06E9).I(want).Run();                          // REQUEST_VEHICLE_MOD
        if (!Cmd(0x06EA).I(want).Run()) continue;           // HAS_VEHICLE_MOD_LOADED: try again next frame
        int dummy = 0;
        Cmd(0x06E7).I(h).I(want).Out(&dummy).Run();         // ADD_VEHICLE_MOD
    }
    int pj = -1;
    Cmd(0x0988).I(h).Out(&pj).Run();  // GET_CURRENT_VEHICLE_PAINTJOB
    // CVehicle::SetRemap reads the model's four paintjobs at that index, unchecked
    if (nv.look.paintjob >= 0 && nv.look.paintjob < 4 && pj != nv.look.paintjob)
        Cmd(0x06ED).I(h).I(nv.look.paintjob).Run();  // GIVE_VEHICLE_PAINTJOB
    if (HasDamageManager(v)) ApplyDamage(v, nv.look.damage);
    else if (v->VehicleSubClass() == VEHICLE_TYPE_BIKE) memcpy((uint8_t*)v + kBikeTyres, nv.look.bikeTyres, 2);
    // the lock it had when it became known stayed: a mission car locked then held the partner who got in, after the
    // mission it was opened only in the owner's game (the Savanna of "Cesar Vialpando"); same rule as at the spawn
    v->DoorLock() = TrainingVehicleRestricted(v)?2:m_scriptLocks.count(nv.netId)?m_scriptLocks[nv.netId]:(nv.hasState?nv.state.lock:nv.look.lock);
}

void Vehicles::ExpectCarjack(uint16_t id,uint8_t driver){
    if(auto*nv=Find(id)){nv->jackDriver=driver;nv->jackUntil=NowMs()+8000;}
}

void Vehicles::TakeOwnership(NetVehicle& nv) {
    if(NowMs()<nv.jackUntil&&nv.jackDriver!=Net().MyId())return;
    if (nv.owner == Net().MyId()) return;
    nv.owner = Net().MyId();
    nv.hasState = false;
    nv.firstAnnouncedMs=NowMs();
    nv.lastChangeMs = NowMs();nv.history.Clear();
    Release(nv);
    Writer w(proto::G_VEH_OWNER);
    w.Entity(ENTITY_VEHICLE,nv.netId);
    w.U8(nv.owner);
    w.U32((uint32_t)NowMs());
    Net().Send(proto::CH_RELIABLE, w);
    Announce(nv);  // a peer that missed the original car can rebuild this same ID
    LOGI("Took ownership of vehicle %04X", nv.netId);
}

void Vehicles::ObserveScriptOwnership(NetVehicle& nv) {
    if(!Net().IsHost()||!Alive(nv))return;
    const bool held=ScriptHolds(nv.veh);
    if(held){
        const bool changed=!nv.heldHere||!nv.script;
        nv.heldHere=true;nv.script=true;
        // An observer's native slot can be transiently empty while 0683 moves
        // a guest-driven vehicle onto its parent. The real driver's accepted
        // state remains authoritative; reclaiming this visual gap elected the
        // host and made HandleOwner drag the actual guest into a passenger seat.
        auto*driver=Players::Instance().Get(nv.owner);
        const bool remoteDriving=driver&&driver->ClaimsDriver(nv.netId,
            Net().EntityGeneration(ENTITY_VEHICLE,nv.netId),nv.veh->AreaCode(),NowMs());
        // STORE_CAR_CHAR_IS_IN can adopt a guest's previously parked car into
        // the native mission. Its original id prefix still belongs to that
        // guest; creating a new host id or waiting for a host prefix loses the
        // script's authoritative edits. Keep this id and native handle.
        if(IsMirror(nv)&&!HasPlayerInside(nv.veh)&&!remoteDriving&&!IsCooperativeRemoteVehicle(nv.veh,nv.owner)){
            TakeOwnership(nv);
            if(!IsMirror(nv)){SendState(nv,true);LOGI("Native script adopted vehicle %04X with its existing identity",nv.netId);}
        }else if(changed&&!IsMirror(nv))Announce(nv);
        else if(changed&&remoteDriving)LOGD("Native script vehicle %04X retains fresh driver claim player=%u",nv.netId,nv.owner);
    }else if(nv.heldHere&&!IsMirror(nv)){
        nv.heldHere=false;nv.script=false;Announce(nv);
    }
}

CVehicle* Vehicles::FindAdoptable(uint16_t model, const CVector& pos, const CVehicle* excluded) {
    CPoolHeader* pool = VehiclePool();
    if (!pool) return nullptr;
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* best = nullptr;
    float bestD = 3.0f;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (v==excluded || v->ModelIndex() != model || m_byPtr.count(v)) continue;
        // An unresolved copy belongs to its saved birth even when a different
        // network ID for the same model appears at the same location.
        bool quarantined=false;
        for (const auto& [id,saved] : m_retainedCopies)
            if (saved.handle==GetVehicleRef(v) && saved.model==v->ModelIndex()) { quarantined=true; break; }
        if (quarantined) continue;
        if (me && me->InVehicle() && me->Vehicle() == v) continue;
        // a script's car stays its own: released as an ordinary car later, its script lost it and crashed
        if (v->CreatedBy() == VEHICLE_CREATED_MISSION) continue;
        // a parked car both games have (a car generator's): one with people in it or on the move is this game's traffic,
        // its driver kept driving it against the copy's states
        if (v->Driver() || v->NumPassengers() || v->MoveSpeed().Length() > 0.01f) continue;
        float d = (v->GetPosition() - pos).Length();
        if (d < bestD) {
            bestD = d;
            best = v;
        }
    }
    return best;
}

CVehicle* Vehicles::SpawnMirror(const VehicleSpawn& sp) {
    profile::Scope timing(profile::ENTITY_CREATE);
    if (!IsModelLoaded(sp.model)) {
        RequestSharedModel(sp.model,sp.script||sp.playerUsed);  // streamed in the background, CreateMirror is retried every frame
        return nullptr;
    }
    if (!HasRoom(VehiclePool(), kKeepVehicles, "vehicle")){m_spawnWait[sp.netId].Block("vehicle",sp.netId,"vehicle_pool_full",m_spawnWait[sp.netId].priority);return nullptr;}
    int handle = -1;
    const auto savedComponents=Ref<uint16_t>(0x8A6458);
    for(int c=0;c<2;++c)Ref<int8_t>(0x8A6458+c)=sp.components[c];
    Cmd(0x00A5).I(sp.model).F(sp.pos.x).F(sp.pos.y).F(sp.pos.z).Out(&handle).Run();
    Ref<uint16_t>(0x8A6458)=savedComponents;  // CREATE_CAR
    CVehicle* v = GetVehicle(handle);
    if (!v){m_spawnWait[sp.netId].Block("vehicle",sp.netId,"native_creation_failed");return nullptr;}
    SetVehicleMatrix(v, sp.pos, sp.rot);
    memcpy(v->Colors(), sp.colors, 4);
    v->Health() = sp.health;
    v->AreaCode() = sp.area;
    // a random car locked there does not lock the other player out; a mission's locked car stays shut for him too
    v->DoorLock() = sp.lockState;
    return v;
}

void Vehicles::HandleCreate(uint8_t from, Reader& r) {
    VehicleSpawn sp;
    if (!sp.Read(r) || from == Net().MyId() || sp.owner != from || m_scriptRemoved.count(sp.netId)) return;
    if (!core::IsPlayerReady()) return;
    if (NetVehicle* existing = Find(sp.netId)) {
        if (existing->owner != from || sp.owner != from || existing->model!=sp.model) return;
        existing->playerUsed = existing->playerUsed || sp.playerUsed;
        existing->script = sp.script;
        return;
    }
    if (!IsVehicleModel(sp.model)) {
        LOGW("Vehicle %04X has an unknown model %d", sp.netId, sp.model);
        return;
    }
    m_pending[sp.netId] = sp;
}

bool Vehicles::CreateMirror(const VehicleSpawn& sp) {
    if (Find(sp.netId)) return true;  // announced again meanwhile and made already
    bool adopted = false;
    bool retained = false;
    CVehicle* v = nullptr;
    CVehicle* excluded = nullptr;
    if (auto saved=m_retainedCopies.find(sp.netId); saved!=m_retainedCopies.end()) {
        auto* candidate=GetVehicle(saved->second.handle);
        if (saved->second.generation && saved->second.generation==Net().EntityGeneration(ENTITY_VEHICLE,sp.netId) &&
            saved->second.model==sp.model && IsVehicleValid(candidate) && candidate->ModelIndex()==sp.model && !Find(candidate)) {
            v=candidate;retained=true;
        } else excluded=candidate;
        // Keep an unmatched live copy quarantined until it is safe to delete;
        // otherwise the traffic scanner can reannounce it under a new ID.
        if (retained || !IsVehicleValid(candidate) || candidate->ModelIndex()!=saved->second.model)
            m_retainedCopies.erase(saved);
    }
    if (!v) v = FindAdoptable(sp.model, sp.pos, excluded);
    if (v) {
        adopted = true;
        if(!retained&&(Field<int8_t>(v,0x438)!=sp.components[0]||Field<int8_t>(v,0x439)!=sp.components[1])){
            const auto saved=Ref<uint16_t>(0x8A6458);
            WorldRemove(v);CallVirtual<8>(v);
            for(int c=0;c<2;++c)Ref<int8_t>(0x8A6458+c)=sp.components[c];
            CallVirtual<5>(v,(uint32_t)sp.model);Ref<uint16_t>(0x8A6458)=saved;WorldAdd(v);
            // Its original upgrades/paint/damage are reconciled from its owner.
        }
        if(!retained)SetVehicleMatrix(v, sp.pos, sp.rot);
        memcpy(v->Colors(), sp.colors, 4);  // the same parked car can have other random colours here
        v->Health() = sp.health;
    } else {
        v = SpawnMirror(sp);
    }
    if (!v) return false;
    if(auto prior=m_byId.find(sp.netId);prior!=m_byId.end())if(auto ptr=m_byPtr.find(prior->second.veh);ptr!=m_byPtr.end()&&ptr->second==sp.netId)m_byPtr.erase(ptr);
    NetVehicle& nv = m_byId[sp.netId];
    nv.netId = sp.netId;
    nv.veh = v;
    nv.handle = GetVehicleRef(v);
    nv.owner = sp.owner;
    nv.model = sp.model;
    nv.createdByUs = retained || !adopted;
    nv.script = sp.script;
    nv.heldHere = retained && ScriptHolds(v);
    nv.playerUsed = sp.playerUsed;
    if (adopted) Protect(nv);
    m_byPtr[v] = sp.netId;
    if(auto state=m_pendingStates.find(sp.netId);state!=m_pendingStates.end()){
        auto latest=state->second;m_pendingStates.erase(state);ApplyDriverState(latest.first,sp.netId,latest.second);
    }
    if(auto look=m_pendingLooks.find(sp.netId);look!=m_pendingLooks.end()){
        nv.look=look->second.first;nv.repairRevision=look->second.second;nv.hasLook=true;m_pendingLooks.erase(look);ApplyLook(nv);
    }
    if(m_adoptPending.erase(sp.netId)){
        nv.owner=Net().MyId();nv.hasState=false;nv.history.Clear();Release(nv);nv.handoverUntil=NowMs()+3000;
        nv.firstAnnouncedMs=nv.lastChangeMs=NowMs();
        if(!nv.script&&v->Driver()&&v->Driver()->Health()>0&&!HasPlayerInside(v)){Cmd(0x00A8).I(nv.handle).Run();Cmd(0x00AD).I(nv.handle).F(12.f).Run();}
        Announce(nv);SendLook(nv);SendState(nv,true);
    }
    LOGI("Vehicle %04X (model %d) %s for player %d", sp.netId, sp.model, adopted ? "adopted" : "created", sp.owner);
    return true;
}

void Vehicles::Forget(uint16_t netId, bool deleteVehicle) {
    auto it = m_byId.find(netId);
    if (it == m_byId.end()) return;
    NetVehicle& nv = it->second;
    if(!deleteVehicle&&Alive(nv)){nv.scriptAttachment={};ApplyVehicleBinding(nv,VehicleState{});}
    LOGD("Vehicle %04X forgotten (owner %u, delete %d) native=%d generation=%llu mirror_created=%d",netId,nv.owner,deleteVehicle,nv.handle,(unsigned long long)Net().EntityGeneration(ENTITY_VEHICLE,netId),nv.createdByUs);
    if (deleteVehicle && Alive(nv)) {
        auto*me=FindPlayerPed(0);
        if(me&&me->InVehicle()&&me->Vehicle()==nv.veh){auto p=me->GetPosition();Cmd(0x0362).I(GetPedRef(me)).F(p.x).F(p.y).F(p.z+1).Run();}
        // a deleted car takes its occupants along: the other player's character gets out first
        for (auto& [id, rp] : Players::Instance().All())
            if (rp.ped && IsPedValid(rp.ped) && rp.ped->InVehicle() && rp.ped->Vehicle() == nv.veh) {
                CVector p = rp.ped->GetPosition();
                Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
            }
        StopEntering(nv.veh);
        Cmd(0x00A6).I(GetVehicleRef(nv.veh)).Run();  // DELETE_CAR
    }
    if(auto ptr=m_byPtr.find(nv.veh);ptr!=m_byPtr.end()&&ptr->second==netId)m_byPtr.erase(ptr);
    m_pendingLooks.erase(netId);
    m_ids.Retire(netId,NowMs());
    m_byId.erase(it);
}

// A mirror is the other game's car: this game's population and scripts leave it alone (a car of the population taken
// over by the other player was deleted here as soon as this player got far, with the other player's character in it).
void Vehicles::Protect(NetVehicle& nv) {
    if(!Alive(nv))return;
    CVehicle* v = nv.veh;
    if (!IsVehicleValid(v) || v->CreatedBy() == VEHICLE_CREATED_MISSION) return;
    if (!nv.createdByUs && !nv.origBy) nv.origBy = v->CreatedBy();
    SetVehicleCreatedBy(v, VEHICLE_CREATED_MISSION);
}

// Ours again: what it was in this game before, and a mirror spawned here an ordinary car (a script's car of the other
// game stays a mission car: dropping it here would make the owner drop it in the middle of its mission)
void Vehicles::Release(NetVehicle& nv) {
    if(!Alive(nv))return;
    HoldUnloadedPhysics(nv,false);
    CVehicle* v = nv.veh;
    if (nv.crashProofHere && IsVehicleValid(v)) v->PhysicalFlags() &= ~CPhysical::PF_COLLISION_PROOF;
    nv.crashProofHere = false;
    uint8_t by = nv.createdByUs ? (nv.script ? VEHICLE_CREATED_MISSION : VEHICLE_CREATED_RANDOM) : nv.origBy;
    if (IsVehicleValid(v) && by && v->CreatedBy() != by && !ScriptHolds(v)) SetVehicleCreatedBy(v, by);
    nv.origBy = 0;
}

void Vehicles::HoldUnloadedPhysics(NetVehicle&nv,bool hold){
    if(!Alive(nv))return;
    auto*v=nv.veh;
    if(hold){
        if(!nv.physicsHeld){nv.physicsHeld=true;nv.heldPhysicsMask=(~v->PhysicalFlags())&0x2000u;nv.heldGravity=(v->PhysicalFlags()&2)!=0;LOGD("Vehicle %04X physics held outside local collision",nv.netId);}
        v->PhysicalFlags()=(v->PhysicalFlags()|nv.heldPhysicsMask)&~2u;
        v->MoveSpeed()={};v->TurnSpeed()={};
    }else if(nv.physicsHeld){
        v->PhysicalFlags()&=~nv.heldPhysicsMask;if(nv.heldGravity)v->PhysicalFlags()|=2;
        nv.physicsHeld=false;nv.heldPhysicsMask=0;LOGD("Vehicle %04X local physics restored",nv.netId);
    }
}

// The owner let the vehicle go: a mirror spawned here goes too (unless this player sits in it), an instance this game had
// before (adopted, or ours until the other player took it) is this game's again, a script's car its script's.
void Vehicles::Drop(uint16_t netId) {
    NetVehicle* nv = Find(netId);
    if (!nv) return;
    CPlayerPed* me = FindPlayerPed(0);
    bool inside = me && me->InVehicle() && me->Vehicle() == nv->veh;
    bool gone = nv->createdByUs && !inside;
    if (!gone) Release(*nv);
    Forget(netId, gone);
}

std::vector<uint16_t> Vehicles::ClearMirrors(const CVector& at, float radius) {
    std::vector<uint16_t> ids;
    for (auto& [id, nv] : m_byId) {
        CVehicle* v = nv.veh;
        if (!IsMirror(nv) || nv.script || nv.playerUsed || IsRecent(id) || !IsVehicleValid(v) || v->Driver()) continue;
        bool empty = true;
        for (int i = 0; i < 8; ++i) empty = empty && !v->Passengers()[i];
        CVector d = v->GetPosition() - at;
        if (empty && d.x * d.x + d.y * d.y < radius * radius) ids.push_back(id);
    }
    for (uint16_t id : ids) Forget(id, true);
    return ids;
}

// the owner's side of ClearMirrors: what the host's copy no longer has goes here too, as the area's ordinary cars went
void Vehicles::HandleClear(uint8_t from, Reader& r) {
    uint8_t n = r.U8();
    for (uint8_t i = 0; i < n && r.Ok() && from == Net().HostId(); ++i) {
        uint16_t id = r.U16();
        NetVehicle* nv = r.Ok() ? Find(id) : nullptr;
        if (!nv || IsMirror(*nv) || !IsVehicleValid(nv->veh)) continue;
        if (nv->playerUsed || IsRecent(id) || nv->veh->Driver() || HasPlayerInside(nv->veh)) {  // got in meanwhile: the host gets its copy back
            Announce(*nv);
            continue;
        }
        LOGI("Vehicle %04X (model %d) cleared away by the host's mission", id, nv->model);
        StopEntering(nv->veh);
        Cmd(0x00A6).I(nv->handle).Run();  // DELETE_CAR: its removal goes out with the other vanished ones (Update)
    }
}

std::vector<uint16_t> Vehicles::PreservedIds() const {
    std::vector<uint16_t> ids;
    for (const auto& [id, nv] : m_byId)
        if (nv.owner == Net().MyId() && nv.playerUsed && IsVehicleValid(nv.veh)) ids.push_back(id);
    return ids;
}

void Vehicles::RestorePlayerCar(CVehicle* v, const VehicleSpawn& sp) {
    if(!Net().EntityLive(ENTITY_VEHICLE,sp.netId))Net().BeginEntity(ENTITY_VEHICLE,sp.netId);
    if (!v || !IsVehicleValid(v) || !sp.netId) return;
    NetVehicle nv;
    nv.netId = sp.netId;
    nv.veh = v;
    nv.handle = GetVehicleRef(v);
    nv.owner = Net().MyId();
    nv.model = sp.model;
    nv.playerUsed = true;
    nv.firstAnnouncedMs=NowMs();
    nv.lastChangeMs = NowMs();
    m_byId[sp.netId] = nv;
    m_byPtr[v] = sp.netId;
    m_lastDriven = nv.handle;
    Announce(nv);
    SendLook(m_byId[sp.netId]);
}

void Vehicles::ForgetOwnedBy(uint8_t owner, const std::vector<uint16_t>& preserved) {
    // The sender loaded another world and restored its protected vehicle IDs.
    // Its repair counters restart with that new world/connection context.
    for(auto id:preserved)if(auto*n=Find(id);n&&n->owner==owner){n->repairRevision=0;n->hasLook=false;}
    std::vector<uint16_t> ids;
    for (auto& [vid, nv] : m_byId)
        if (nv.owner == owner && std::find(preserved.begin(), preserved.end(), vid) == preserved.end()) ids.push_back(vid);
    for(auto it=m_pending.begin();it!=m_pending.end();){
        if(it->second.owner==owner&&std::find(preserved.begin(),preserved.end(),it->first)==preserved.end()){
            m_pendingStates.erase(it->first);m_pendingLooks.erase(it->first);m_adoptPending.erase(it->first);it=m_pending.erase(it);
        }else ++it;
    }
    for (uint16_t vid : ids) Drop(vid);
}

void Vehicles::HandleRemove(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    bool destroyed=r.Remaining()?r.Bool():false;
    if (!r.Ok()) return;
    if(auto pending=m_pending.find(id);pending!=m_pending.end()&&pending->second.owner==from){
        m_pending.erase(pending);m_pendingStates.erase(id);m_pendingLooks.erase(id);m_adoptPending.erase(id);
    }
    NetVehicle* nv = Find(id);
    if (!nv || nv->owner != from) return;
    if(destroyed)Forget(id,true);else Drop(id);
}

// An ordinary car of this game (traffic, a parked one) the other player is at while this game's player is far: this
// game would drop it soon (CCarCtrl removes cars beyond its player's reach) and it vanished in front of the other player.
// The other game takes it over with whoever sits in it, as its own, and this game's one goes.
static bool CanGiveAway(CVehicle* v) {
    uint8_t by = v->CreatedBy();
    auto* network = Vehicles::Instance().Find(v);
    bool protectedRecent = network && network->playerUsed && !network->script && !HasPlayerInside(v);
    if ((by != VEHICLE_CREATED_RANDOM && by != VEHICLE_CREATED_PARKED && !protectedRecent) || v->Status() == STATUS_WRECKED ||
        Field<CVehicle*>(v, 0x4C4) || Field<CVehicle*>(v, 0x4C8) ||  // towing or towed: the pair stays together
        (!protectedRecent && !CallMethod<0x6D1180, bool>(v)) ||                             // CVehicle::CanBeDeleted: the engine's own test
        (network && NowMs()<network->handoverUntil) || ScriptHolds(v))
        return false;
    CPed* seats[9] = {v->Driver()};
    for (int i = 0; i < 8; ++i) seats[i + 1] = v->Passengers()[i];
    for (CPed* p : seats) {  // its occupants go along: only this game's own passers-by the other game knows
        NetPed* np = p ? Peds::Instance().Find(p) : nullptr;
        if (p && (p->IsPlayer() || Players::Instance().FindByPed(p) || !np || np->owner != Net().MyId() ||
                  p->CreatedBy() != 1 /* PED_GAME */ || !Peds::Instance().CanGiveAwayType(*np)))
            return false;
    }
    return true;
}

void Vehicles::GiveAway(NetVehicle& nv, uint8_t to) {
    if(to==Net().MyId()||!Net().Peer(to))return;
    CVehicle* v = nv.veh;
    CPed* seats[9] = {v->Driver()};
    for (int i = 0; i < 8; ++i) seats[i + 1] = v->Passengers()[i];
    for (CPed* p : seats)  // the peds first: the car's message drives them on
        if (NetPed* np = p ? Peds::Instance().Find(p) : nullptr) Peds::Instance().GiveAway(np->netId, to);
    SendLook(nv); // the pending receiver needs damage/repair metadata before it can adopt
    Writer w(proto::G_VEH_HANDOVER);
    w.Entity(ENTITY_VEHICLE,nv.netId);
    w.U8(to);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Vehicle %04X (model %d) handed over to player %d", nv.netId, nv.model, to);
    nv.owner=to;nv.hasState=false;nv.history.Clear();nv.handoverUntil=NowMs()+3000;
    Protect(nv);
}

bool Vehicles::AssignStoryRemote(CVehicle*v,uint8_t to){auto*n=Find(v);if(!Net().IsHost()||!n||!Alive(*n)||n->owner!=Net().MyId()||!IsCooperativeRemoteVehicle(v,to)||HasPlayerInside(v))return false;GiveAway(*n,to);return n->owner==to;}

bool Vehicles::QueueGiveAway(CVehicle* v, uint8_t to) {
    NetVehicle* nv = Find(v);
    if (!nv || IsMirror(*nv) || !CanGiveAway(v)) return false;
    m_giveAway[v] = to;
    return true;
}

void Vehicles::HandleHandover(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t to = r.U8();
    if (!r.Ok()||to>=proto::kMaxPlayers||(to!=Net().MyId()&&!Net().Peer(to))) return;
    if (auto pending = m_pending.find(id); pending != m_pending.end() && pending->second.owner == from)
        pending->second.owner = to;
    NetVehicle* nv = Find(id);
    if (!nv) {
        if (to == Net().MyId()) {
            if(m_pending.count(id))m_adoptPending.insert(id);
        }
        return;
    }
    if (nv->owner != from) {  // this game took it just before: the giver deleted its own, it gets this one back
        if (nv->owner == Net().MyId() && IsVehicleValid(nv->veh)) Announce(*nv);
        return;
    }
    // Other observers keep their existing mirror and its id across the handover.
    nv->owner = to;
    nv->hasState = false;  // the new owner's timestamp is on another clock
    nv->history.Clear();
    if (to != Net().MyId()) return;
    if (!IsVehicleValid(nv->veh)) {
        Writer gone(proto::G_VEH_REMOVE);
        gone.Entity(ENTITY_VEHICLE,id,ENTITY_REMOVE);
        Net().Send(proto::CH_RELIABLE, gone);
        return Forget(id, true);
    }
    CVehicle* v = nv->veh;
    int h = nv->handle;
    if (!ScriptHolds(v)) SetVehicleCreatedBy(v, VEHICLE_CREATED_RANDOM);  // this game's traffic now
    if (CPed* driver = v->Driver(); driver && driver->Health()>0 && driver->PedState()!=54 && driver->PedState()!=55 && !driver->IsPlayer() && !Players::Instance().FindByPed(driver)) {
        Cmd(0x00A8).I(h).Run();           // CAR_WANDER_RANDOMLY
        Cmd(0x00AD).I(h).F(12.0f).Run();  // SET_CAR_CRUISE_SPEED
        Cmd(0x00AE).I(h).I(0).Run();      // SET_CAR_DRIVING_STYLE: stops for cars and lights
    }
    Release(*nv);
    if (nv->playerUsed || IsRecent(id)) Protect(*nv);
    nv->firstAnnouncedMs=NowMs();nv->handoverUntil=NowMs()+3000;
    nv->lastSendMs = nv->lastChangeMs = 0;
    Announce(*nv);
    SendLook(*nv);
    SendState(*nv,true);
    LOGI("Vehicle %04X (model %d) taken over from player %d", id, (int)v->ModelIndex(), from);
}

void Vehicles::HandleSync(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    VehicleState st;
    if (!st.Read(r)) return;
    ApplyDriverState(from, id, st);
}

void Vehicles::ApplyDriverState(uint8_t from, uint16_t netId, const VehicleState& st) {
    NetVehicle* nv = Find(netId);
    if (!nv) {
        if(auto p=m_pending.find(netId);p!=m_pending.end()&&p->second.owner==from){
            auto prior=m_pendingStates.find(netId);
            if(prior==m_pendingStates.end()||prior->second.first!=from||(int32_t)(st.time-prior->second.second.time)>=0)m_pendingStates[netId]={from,st};
        }
        return;
    }
    // Driver snapshots never elect a simulator. Delayed old-driver states used to
    // take ownership back every frame on an observer, making a guest-to-guest jack jitter.
    if(nv->owner!=from)return;
    if(st.parentVehicle&&(st.parentVehicle==netId||
       st.parentGeneration!=Net().ExpectedEntityGeneration(ENTITY_VEHICLE,st.parentVehicle)))return;
    if(!st.AttachmentInStory(StoryRunId(),CooperativeStoryPhase()))return;
    const auto&claim=nv->scriptAttachment;
    if(claim.attachmentRevision&&claim.attachmentRun==StoryRunId()&&claim.attachmentPhase==CooperativeStoryPhase()&&
       !st.FollowsAttachmentClaim(claim))return;
    if (nv->hasState && (int32_t)(st.time - nv->lastTime) < 0) return;
    if(!nv->hasState||(st.pos-nv->state.pos).Length()>30)nv->history.Clear();
    nv->history.Push(st,NowMs());
    nv->lastTime = st.time;
    nv->state = st;
    nv->hasState = true;
    nv->recvMs = NowMs();
}

void Vehicles::HandleOwner(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t owner = r.U8();
    uint32_t claimTime=r.U32();
    if (!r.Ok()) return;
    if(from==owner)if(auto*rp=Players::Instance().Get(from);rp&&rp->hasState&&
        rp->mode!=RemotePlayer::Mode::Driver&&(int32_t)(rp->lastStateTime-claimTime)>0)return;
    NetVehicle* nv = Find(id);
    if(owner>=proto::kMaxPlayers||(!Net().Peer(owner)&&owner!=Net().MyId()))return;
    if(!nv){if(auto it=m_pending.find(id);it!=m_pending.end()&&(it->second.owner==from||owner==from)){it->second.owner=owner;m_pendingStates.erase(id);if(owner==Net().MyId())m_adoptPending.insert(id);else m_adoptPending.erase(id);}return;}
    if(from!=nv->owner&&from!=owner)return;
    bool jack=NowMs()<nv->jackUntil;
    if(jack&&owner!=nv->jackDriver)return;
    CPlayerPed* me = FindPlayerPed(0);
    bool iDrive = me && nv->veh && me->InVehicle() && me->Vehicle() == nv->veh && nv->veh->Driver() == me;
    if (iDrive && owner != Net().MyId() && !jack) {
        if (owner > Net().MyId() || from != owner) {
            // we are driving it right now: keep ownership, tell the others again
            Writer w(proto::G_VEH_OWNER);
            w.Entity(ENTITY_VEHICLE,id);
            w.U8(Net().MyId());
            w.U32((uint32_t)NowMs());
            Net().Send(proto::CH_RELIABLE, w);
            return;
        }
        // It drives it too, in its game (both took the seat at once, or it pulled us out and our game never saw it).
        // Each kept answering the other's claim while both drove, each showed the other standing frozen beside the car.
        // The lower id keeps it (the host); our player gives way and sits beside it if there is room.
        int h = GetPedRef(me);
        CVector p = me->GetPosition();
        Cmd(0x0362).I(h).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        if (SeatOrAny(nv->veh, 0) == 0 && !nv->veh->Passengers()[0])
            Cmd(0x0430).I(h).I(nv->handle).I(0).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
        LOGI("Vehicle %04X: player %d drives it as well, ours gives way", id, owner);
    }
    bool wasMine = nv->owner == Net().MyId();
    nv->owner = owner;
    nv->hasState = false;nv->history.Clear();
    if (owner == Net().MyId()) {
        nv->handoverUntil=NowMs()+2000;
        Release(*nv);
    } else if (wasMine) {
        nv->heldHere = IsVehicleValid(nv->veh) && ScriptHolds(nv->veh);
        Protect(*nv);
    }
}

// The script that held this car here let go (its mission ended): the owner's copy, kept a mission car for it, is an
// ordinary car again (else it never went away and stood twice when announced again)
void Vehicles::HandleScriptFree(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    NetVehicle* nv = r.Ok() ? Find(id) : nullptr;
    if (!nv || IsMirror(*nv) || !IsVehicleValid(nv->veh)) return;
    nv->script = false;
    if (nv->createdByUs && nv->veh->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(nv->veh))
        SetVehicleCreatedBy(nv->veh, VEHICLE_CREATED_RANDOM);
}

// The parts a driver moves: nozzles, hoists and forks (m_wMiscComponentAngle), the landing gear, a turret. Nothing ticks
// them on a vehicle without a player's status, so the owner's values stand here.
void ApplyParts(CVehicle* v, const VehicleState& st) {
    // only what this vehicle has as well (VehicleState::Capture's rules): a net id that meant another vehicle here wrote
    // a plane's gear (CPlane::SetGearUp) or a tank's turret into a bike or a boat
    int type = v->VehicleSubClass(), model = v->ModelIndex();
    if(st.extras&VehicleState::X_ROTOR){
        if(type==VEHICLE_TYPE_HELI)Field<float>(v,0x84C)=st.rotorSpeed;
        else if(type==VEHICLE_TYPE_PLANE)Field<float>(v,0x9C4)=st.rotorSpeed;
    }
    if ((st.extras & VehicleState::X_MISC) && (type <= VEHICLE_TYPE_PLANE || type == VEHICLE_TYPE_TRAILER))
        Field<uint16_t>(v, 0x86C) = st.misc;
    if ((st.extras & VehicleState::X_GEAR) && type == VEHICLE_TYPE_PLANE) {
        float& gear = Field<float>(v, 0x9CC);
        if (st.gear >= 1.0f && gear < 1.0f) CallMethod<0x6CAC20>(v);         // CPlane::SetGearUp: no wheels
        else if (st.gear == 0.0f && gear != 0.0f) CallMethod<0x6CAC70>(v);  // CPlane::SetGearDown
        gear = st.gear;
        Field<float>(v, 0x994) = std::clamp(st.gas - st.brake, -1.0f, 1.0f);  // m_fAccelerationBreakStatus: jet flames
    }
    if ((st.extras & VehicleState::X_TURRET) && (model == 432 || model == 601 || model == 407)) {
        Field<float>(v, 0x94C) = st.turretYaw;
        Field<float>(v, 0x950) = st.turretPitch;
    }
}

// Hydraulics of a car a remote player drives. Its game keeps the wheel heights only while computing them from the
// right stick, so they are computed here from his stick the same way; a mirror has the mission-car status for which
// CAutomobile::HydraulicControl takes them from CVehicle::m_aSpecialHydraulicData (4 slots for the whole game).
void ApplyHydraulics(CVehicle* v, const VehicleState& st) {
    if (v->VehicleSubClass() != VEHICLE_TYPE_AUTOMOBILE || !(Field<uint32_t>(v, 0x38C) & 0x20000)) return;  // bHydraulicInst
    RemotePlayer* rp = v->Driver() ? Players::Instance().FindByPed(v->Driver()) : nullptr;
    if (!rp) return;
    const PadState& pad = rp->state.pad;
    float wheel[4] = {1, 1, 1, 1};  // front left, rear left, front right, rear right; all up while the jump is held
    if (!(pad.buttons & PadState::B_SHOCKR)) {
        float lr = pad.rx, ud = pad.ry;
        float jump = std::sqrt(ud * ud + lr * lr) * 1.5f / 128.0f, a = std::atan2(lr, ud) - 0.7853982f;
        float rr = std::cos(a) * jump, fr = std::sin(a) * jump;
        wheel[0] = std::max(0.0f, -rr);
        wheel[1] = std::max(0.0f, -fr);
        wheel[2] = std::max(0.0f, fr);
        wheel[3] = std::max(0.0f, rr);
    }
    bool active = st.misc != 0 || wheel[0] + wheel[1] + wheel[2] + wheel[3] > 0.0f;
    int8_t slot = Field<int8_t>(v, 0x48B);  // CVehicle::m_vehicleSpecialColIndex
    if (active && slot < 0 && CallMethod<0x6DF3D0, bool>(v)) slot = Field<int8_t>(v, 0x48B);  // GetSpecialColModel
    if (slot >= 0 && slot < 4) memcpy((void*)(0xC1CB60 + (uintptr_t)slot * 0x28 + 0x18), wheel, sizeof(wheel));
}

// A train-bound mission car follows its carriage in local space, rather than
// alternating native attachment physics with delayed world-space snapshots.
bool ApplyTrainBinding(NetVehicle&nv,const VehicleState&st){
    auto*v=nv.veh;auto*parent=st.train.Valid()?ResolveTrainAttachment(st.train):nullptr;
    if(parent&&!Net().EntityLink(ENTITY_VEHICLE,nv.netId,ENTITY_TRAIN,st.train.id))parent=nullptr;
    if(nv.trainAttached&&v->AttachedTo()!=parent){if(v->AttachedTo())CallMethod<0x5442F0>(v,0.f,0.f,0.f,false);nv.trainAttached=false;}
    if(st.train.Valid()&&!parent){
        if(!nv.trainCollisionHeld){nv.trainCollisionHeld=true;nv.trainHadCollision=(v->Flags()&1)!=0;}
        v->Flags()&=~129u;SetVehicleMatrix(v,st.pos,st.rot);v->MoveSpeed()={};v->TurnSpeed()={};return true;
    }
    if(nv.trainCollisionHeld){if(nv.trainHadCollision)v->Flags()|=1u;nv.trainCollisionHeld=false;}
    if(!parent)return false;
    auto offset=st.trainOffset;auto rotation=st.trainRotation;
    if(v->AttachedTo()!=parent)CallMethod<0x54D690>(v,parent,&offset,&rotation);
    Field<CVector>(v,0x100)=offset;Field<Quat>(v,0x118)=rotation;
    CallMethod<0x546FF0>(v);nv.trainAttached=true;return true;
}

bool Vehicles::ApplyVehicleBinding(NetVehicle&nv,const VehicleState&sample){
    if(!Alive(nv))return false;
    auto*v=nv.veh;auto state=sample;
    auto&claim=nv.scriptAttachment;
    if(claim.attachmentRevision){
        if(claim.attachmentRun==StoryRunId()&&claim.attachmentPhase==CooperativeStoryPhase())state=claim;
        else claim={};
    }
    if(state.attachmentRevision&&(state.attachmentRun!=StoryRunId()||state.attachmentPhase!=CooperativeStoryPhase()))state.parentVehicle=0;
    auto*n=state.parentVehicle?Find(state.parentVehicle):nullptr;
    auto*parent=n&&Alive(*n)&&n->netId!=nv.netId&&n->veh->AreaCode()==v->AreaCode()&&
        Net().EntityLive(ENTITY_VEHICLE,n->netId)&&Net().EntityGeneration(ENTITY_VEHICLE,n->netId)==state.parentGeneration?n->veh:nullptr;
    // Reject native pointer cycles too, including a parent not yet represented
    // by the current packet. A pool address is never itself an identity.
    auto*walk=parent;for(int depth=0;walk&&depth<9;++depth){
        if(walk==v||depth==8){parent=nullptr;break;}
        auto*next=walk->AttachedTo();walk=next&&next->IsVehicle()&&IsVehicleValid((CVehicle*)next)?(CVehicle*)next:nullptr;
    }
    auto*installed=GetVehicle(nv.parentHandle);
    bool ours=nv.parentAttached&&installed&&v->AttachedTo()==installed;
    if(nv.parentAttached&&(!parent||parent!=installed)){
        // An independent native task already reparented this car: do not let a
        // stale cleanup detach that unrelated new physical parent.
        if(ours)CallMethod<0x5442F0>(v,0.f,0.f,0.f,false);
        nv.parentAttached=false;nv.parentHandle=-1;nv.parentGeneration=0;
    }
    if(state.parentVehicle&&!parent){
        if(!nv.parentCollisionHeld){nv.parentCollisionHeld=true;nv.parentHadCollision=(v->Flags()&1)!=0;nv.parentWasVisible=v->IsVisible();
            nv.parentHeldPhysicsMask=(~v->PhysicalFlags())&0x2000u;nv.parentHadGravity=(v->PhysicalFlags()&2)!=0;}
        v->Flags()&=~129u;v->PhysicalFlags()=(v->PhysicalFlags()|nv.parentHeldPhysicsMask)&~2u;
        v->MoveSpeed()={};v->TurnSpeed()={};return true;
    }
    if(nv.parentCollisionHeld){
        if(nv.parentHadCollision)v->Flags()|=1u;
        if(nv.parentWasVisible&&!(state.flags&VehicleState::F_HIDDEN))v->Flags()|=128u;
        v->PhysicalFlags()&=~nv.parentHeldPhysicsMask;if(nv.parentHadGravity)v->PhysicalFlags()|=2u;
        nv.parentCollisionHeld=false;
    }
    if(!parent)return false;
    auto offset=state.parentOffset;auto rotation=state.parentRotation;
    if(v->AttachedTo()!=parent){
        if(state.parentUsesEuler)CallMethod<0x54D570>(v,parent,offset,state.parentEuler);
        else CallMethod<0x54D690>(v,parent,&offset,&rotation);
    }
    Field<CVector>(v,0x100)=offset;
    if(state.parentUsesEuler){Field<CVector>(v,0x10C)=state.parentEuler;Field<Quat>(v,0x118)={0,0,0,0};}
    else Field<Quat>(v,0x118)=rotation;
    CallMethod<0x546FF0>(v);nv.parentAttached=true;nv.parentHandle=n->handle;nv.parentGeneration=state.parentGeneration;
    return true;
}

void Vehicles::UpdateVehicleBindings(){
    std::vector<std::pair<int,uint16_t>>order;
    for(auto&[id,nv]:m_byId)if(Alive(nv)&&(nv.parentAttached||nv.parentCollisionHeld||nv.scriptAttachment.attachmentRevision||
       (IsMirror(nv)&&nv.hasState&&nv.state.parentVehicle))){
        auto*walk=&nv;int depth=0;
        for(;walk&&depth<8;++depth){const auto&st=walk->scriptAttachment.attachmentRevision?walk->scriptAttachment:walk->state;if(!st.parentVehicle)break;walk=Find(st.parentVehicle);}
        order.push_back({depth,id});
    }
    std::sort(order.begin(),order.end());
    for(auto[depth,id]:order)if(auto*n=Find(id))ApplyVehicleBinding(*n,IsMirror(*n)&&n->hasState?n->state:VehicleState{});
}

void Vehicles::ApplyMirror(NetVehicle& nv, float dt) {
    if (!nv.hasState) return;
    CVehicle* v = nv.veh;
    v->Flags()=(v->Flags()&~128u)|((nv.state.flags&VehicleState::F_HIDDEN)?0u:128u);
    v->AreaCode()=nv.state.area;
    if(TrainingVehicleRestricted(v))v->DoorLock()=2;
    VehicleState st = nv.history.Size()?nv.history.At(NowMs(),[](const VehicleState&a,const VehicleState&b,float t){
        auto s=b;s.pos=a.pos+(b.pos-a.pos)*t;s.vel=a.vel+(b.vel-a.vel)*t;
        s.turn=a.turn+(b.turn-a.turn)*t;s.rot=Quat::Slerp(a.rot,b.rot,t);s.wheels=WheelState::Blend(a.wheels,b.wheels,t,(uint32_t)(b.time-a.time)/1000.f);return s;
    },[](VehicleState&s,float age,bool stale){s.pos=s.pos+s.vel*age;if(!stale)s.rot=Quat::Turned(s.rot,s.turn,age);s.wheels.Predict(age,stale);if(stale)s.vel=s.turn={};}):nv.state;
    nv.renderedWheels=st.wheels;
    CMatrix before{};bool carry=v->Matrix()!=nullptr;if(carry)before=*v->Matrix();
    CPlayerPed* me = FindPlayerPed(0);

    if (nv.heldHere && !ScriptHolds(v)) {  // the script here let go of it (its mission ended): the owner may drop it
        nv.heldHere = false;
        Writer w(proto::G_VEH_SCRIPT_FREE);
        w.Entity(ENTITY_VEHICLE,nv.netId);
        Net().Send(proto::CH_RELIABLE, w);
    }
    if (v->CreatedBy() != VEHICLE_CREATED_MISSION) Protect(nv);  // a script let it go meanwhile (its mission ended)
    if (!(v->PhysicalFlags() & CPhysical::PF_COLLISION_PROOF)) {
        v->PhysicalFlags() |= CPhysical::PF_COLLISION_PROOF;nv.crashProofHere=true;
    }
    bool wreck=v->Status()==STATUS_WRECKED;
    if (!wreck && (st.flags & VehicleState::F_WRECKED)) {
        Cmd(0x020B).I(nv.handle).Run();  // EXPLODE_CAR: it blew up in the owner's game
        return;
    }
    // a remote player (or nobody) drives it: no local pad input must reach this vehicle. A trailer keeps its own
    // status: an abandoned one looks for a cab to hitch (CTrailer::ScanForTowLink), whoever's game that cab is in
    if (!wreck && v->Driver() != me) {
        if(st.towParent&&IsVehicleValid(Field<CVehicle*>(v,0x4C4)))v->SetStatus(STATUS_IS_TOWED);
        else if(v->VehicleSubClass()!=VEHICLE_TYPE_TRAILER)v->SetStatus(STATUS_PHYSICS);
    }

    CVector target = st.pos;
    // no ground under it in this game (its collision is not loaded here): physics would drop it through the map,
    // so it stands exactly where its owner has it
    if (NowMs() - nv.groundCheckMs > 250) {
        nv.groundCheckMs = NowMs();
        bool found = false;
        Call<0x5696C0, float>(target.x, target.y, target.z + 2.0f, &found, (CEntity**)nullptr);  // FindGroundZFor3DCoord
        nv.groundless = !found;
    }
    // The history already gives a continuous transform. Native AI can cancel a
    // mirror's velocity (notably with a remote driver); velocity-only correction
    // then accumulated metres of error before snapping the car and its riders.
    // Use the sampled transform each tick, retaining physical velocity for local
    // contacts. Only the owner publishes the resulting vehicle physics.
    if(!ApplyVehicleBinding(nv,st)&&!ApplyTrainBinding(nv,st)){
        SetVehicleMatrix(v, target, st.rot);
        v->MoveSpeed() = nv.groundless ? CVector{} : st.vel * (1.0f / 50.0f);
        v->TurnSpeed() = nv.groundless ? CVector{} : st.turn * (1.0f / 50.0f);
    }
    if(wreck)return;
    if (nv.hasLook && NowMs() - nv.lookCheckMs >= 500) {
        nv.lookCheckMs = NowMs();
        ApplyLook(nv);
    }
    v->SteerAngle() = st.steer;
    v->GasPedal() = st.gas;
    v->BrakePedal() = st.brake;
    v->SetHandbrake((st.flags & VehicleState::F_HANDBRAKE)!=0);
    if(st.extras & VehicleState::X_WHEELS)nv.renderedWheels.Apply(v);
    v->Health() = st.health;
    // The owner decides when the burning vehicle explodes; keep native smoke
    // and flames without a replica's independent five-second detonation.
    int fireType=v->VehicleSubClass();
    if(fireType==VEHICLE_TYPE_BIKE||fireType==VEHICLE_TYPE_BMX)Field<float>(v,0x7BC)=std::min(Field<float>(v,0x7BC),4000.f);
    else if(fireType!=VEHICLE_TYPE_BOAT&&fireType!=VEHICLE_TYPE_TRAIN)Field<float>(v,0x8E4)=std::min(Field<float>(v,0x8E4),4000.f);
    v->DoorLock() = TrainingVehicleRestricted(v)?2:m_scriptLocks.count(nv.netId)?m_scriptLocks[nv.netId]:st.lock;
    v->SetEngineOn((st.flags & VehicleState::F_ENGINE) != 0);
    v->PhysicalFlags()=(v->PhysicalFlags()&~0x100u)|((st.water&1)?0x100u:0);
    v->VehicleFlagsLo()=(v->VehicleFlagsLo()&~0x40000000u)|((st.water&2)?0x40000000u:0);
    v->SetSiren((st.flags & VehicleState::F_SIREN) != 0);
    int16_t alarm=Field<int16_t>(v,0x45C);
    if(alarm==0||alarm==-1)Field<uint32_t>(v,0x514)=(st.flags&VehicleState::F_HORN)?1:0;
    ApplyParts(v, st);
    ApplyHydraulics(v, st);
    if(carry)Players::Instance().CarryLocalOnVehicle(v,before);
}

void Vehicles::OnPreProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    for(auto&[id,nv]:m_byId)ObserveScriptOwnership(nv);
    // Native entry starts the engine before Players publishes its driver packet.
    // Claim first: a mirror must never overwrite that native entry result.
    auto* localDriver=FindPlayerPed(0);
    if(auto*v=localDriver->VehicleIfIn();v&&v->Driver()==localDriver)
        if(auto*n=Find(v);n&&IsMirror(*n))TakeOwnership(*n);
    for(auto it=m_scriptRemoved.begin();it!=m_scriptRemoved.end();)it=NowMs()>=it->second?m_scriptRemoved.erase(it):std::next(it);
    for(auto&[id,lock]:m_scriptLocks)if(auto*nv=Find(id);nv&&IsVehicleValid(nv->veh))nv->veh->DoorLock()=lock;
    std::vector<std::pair<float,uint16_t>> spawnOrder;
    auto focus=localDriver->GetPosition();
    for(auto&[id,sp]:m_pending){
        bool crew=false;for(auto&[pid,p]:Peds::Instance().All())if(p.hasState&&p.state.vehNetId==id&&(p.state.flags&PedSyncState::F_IN_VEHICLE)){crew=true;break;}
        int priority=sp.script?3:sp.playerUsed?2:crew?1:0;
        auto&wait=m_spawnWait[id];wait.Block("vehicle",id,!IsModelLoaded(sp.model)?"model_not_loaded":"creation_budget",priority);
        if(!IsModelLoaded(sp.model))RequestSharedModel(sp.model,priority>0);
        float distance=(sp.pos-focus).Length();float age=std::min(10000.f,(NowMs()-wait.since)*.05f);
        spawnOrder.push_back({distance-priority*10000.f-age,id});
    }
    std::sort(spawnOrder.begin(),spawnOrder.end());int created=0;
    for(auto[score,id]:spawnOrder){
        auto i=m_pending.find(id);if(i==m_pending.end())continue;
        if(created>=2)break;
        if(CreateMirror(i->second)){m_spawnWait[id].Done("vehicle",id);m_pending.erase(i);++created;}

    }
    for(auto i=m_spawnWait.begin();i!=m_spawnWait.end();)i=!m_pending.count(i->first)?m_spawnWait.erase(i):std::next(i);
    float dt = TimeStep() / 50.0f;
    std::vector<uint16_t> dead;
    for (auto& [id, nv] : m_byId) {
        if (!Alive(nv) || nv.veh->ModelIndex() != nv.model) {
            LOGD("Vehicle %04X expired (valid %d, expected model %u)",id,IsVehicleValid(nv.veh),nv.model);
            dead.push_back(id);
            continue;
        }
        if (IsMirror(nv)){HoldUnloadedPhysics(nv,false);ApplyMirror(nv, dt);}
        else{
            auto*me=FindPlayerPed(0);auto type=nv.veh->VehicleSubClass();
            bool ground=type==VEHICLE_TYPE_AUTOMOBILE||type==VEHICLE_TYPE_BIKE||type==VEHICLE_TYPE_BMX||type==VEHICLE_TYPE_QUAD||type==VEHICLE_TYPE_MTRUCK||type==VEHICLE_TYPE_TRAILER;
            bool away=me&&(nv.veh->AreaCode()!=me->AreaCode()||(nv.veh->GetPosition()-me->GetPosition()).Length()>180);
            auto*driver=nv.veh->Driver();
            bool corpse=driver&&(driver->Health()<=0||driver->PedState()==54||driver->PedState()==55);
            HoldUnloadedPhysics(nv,ground&&away&&(!driver||corpse)&&!HasPlayerInside(nv.veh)&&(nv.playerUsed||nv.script||ScriptHolds(nv.veh)||(corpse&&PopulationNeededAt(nv.veh->GetPosition(),nv.veh->AreaCode(),140))));
        }
    }
    for (uint16_t id : dead) {
        // Find() intentionally rejects an expired native generation. Retirement
        // must use the logical record, or REMOVE and pointer-map cleanup vanish.
        auto it=m_byId.find(id);if(it==m_byId.end())continue;
        const auto old=it->second;
        if (!IsMirror(old)) {Writer w(proto::G_VEH_REMOVE);w.Entity(ENTITY_VEHICLE,id,ENTITY_REMOVE);w.Bool(true);Net().Send(proto::CH_RELIABLE,w);}
        if(auto ptr=m_byPtr.find(old.veh);ptr!=m_byPtr.end()&&ptr->second==id)m_byPtr.erase(ptr);
        m_byId.erase(it);
    }
    // Both sampled poses must be installed before native hitch geometry runs.
    for(auto&[id,nv]:m_byId)if(IsMirror(nv)&&nv.hasState&&IsVehicleValid(nv.veh))ApplyTowLinks(nv);
    // Parents may have larger ids than their children. Re-evaluate local-space
    // bindings after every sampled parent pose is installed, before physics.
    UpdateVehicleBindings();
}

void Vehicles::SendState(NetVehicle& nv,bool reliable) {
    VehicleState st = Capture(nv.veh);
    st.time = (uint32_t)NowMs();
    Writer w(proto::G_VEH_SYNC);
    w.Entity(ENTITY_VEHICLE,nv.netId,ENTITY_SNAPSHOT);
    st.Write(w);nv.sentHealth=st.health;
    Net().Send(reliable?proto::CH_RELIABLE:proto::CH_STATE, w);
}

int Vehicles::RemoveSharedAmbient(const CVector& centre,int8_t area,int budget) {
    auto*pool=VehiclePool();int removed=0;
    for(int i=0;pool&&i<pool->size&&removed<budget;++i){
        if(pool->byteMap[i]<0)continue;
        auto*v=(CVehicle*)(pool->objects+(size_t)i*kVehiclePoolElemSize);
        if((v->CreatedBy()!=VEHICLE_CREATED_RANDOM&&v->CreatedBy()!=VEHICLE_CREATED_PARKED)||v->AreaCode()!=area||HasPlayerInside(v)||ScriptHolds(v)||
           (v->GetPosition()-centre).Length2D()>300)continue;
        auto*nv=Find(v);if(nv&&(IsMirror(*nv)||nv->playerUsed||IsRecent(nv->netId)||NowMs()<nv->handoverUntil||(nv->netId>>12)!=Net().MyId()+1))continue;
        bool protectedOccupant=false;
        CPed*occupants[9]{};occupants[0]=v->Driver();for(int j=0;j<8;++j)occupants[j+1]=v->Passengers()[j];
        for(auto*p:occupants)if(p){auto*np=Peds::Instance().Find(p);if(p->IsPlayer()||Players::Instance().FindByPed(p)||p->CreatedBy()!=PED_GAME||(np&&(np->owner!=Net().MyId()||NowMs()<np->handoverUntil||(np->netId>>12)!=Net().MyId()+1)))protectedOccupant=true;}
        if(protectedOccupant)continue;
        auto source=NearbyPopulationShare().source;
        if(source==255)continue;
        if(!nv)nv=EnsureNetworked(v);
        for(auto*p:occupants)if(p)Peds::Instance().EnsureNetworked(p);
        if(nv&&CanGiveAway(v)){GiveAway(*nv,source);++removed;}
    }
    return removed;
}

void Vehicles::ScanTraffic() {
    CPoolHeader* pool = VehiclePool();
    if (!pool) return;
    std::vector<uint16_t> gone;
    for (auto& [id, nv] : m_byId)
        if (!IsMirror(nv) && NowMs()-nv.firstAnnouncedMs>3000 && NowMs()>=nv.handoverUntil && !nv.playerUsed && Alive(nv) && !ScriptHolds(nv.veh) && !OccupiedTowChain(nv.veh) &&
            // A handover's new owner can be next to the car while every remote
            // player is far away/in a school interior. Retiring solely by remote
            // distance discarded its ID and re-announced the same car repeatedly.
            (FindPlayerPed(0)->AreaCode()!=nv.veh->AreaCode()||(FindPlayerPed(0)->GetPosition()-nv.veh->GetPosition()).Length()>kTrafficOut) &&
            RemoteDistance(nv.veh->GetPosition(), nv.veh->AreaCode()) > kTrafficOut)
            gone.push_back(id);
    for (uint16_t id : gone) {
        LOGD("Vehicle %04X outside traffic range, no occupied tow ancestor",id);
        Writer w(proto::G_VEH_REMOVE);
        w.Entity(ENTITY_VEHICLE,id,ENTITY_REMOVE);
        Net().Send(proto::CH_RELIABLE, w);
        // a copy of the other game's car this player took: off the network an ordinary car (a mission one stayed forever)
        if (NetVehicle* nv = Find(id); nv && nv->createdByUs && IsVehicleValid(nv->veh) &&
                                       nv->veh->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(nv->veh) &&
                                       GetVehicleRef(nv->veh) != m_kept)
            SetVehicleCreatedBy(nv->veh, VEHICLE_CREATED_RANDOM);
        Forget(id, false);
    }
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (Find(v) || !IsSyncableVehicle(v)) continue;
        if((v->CreatedBy()==VEHICLE_CREATED_RANDOM||v->CreatedBy()==VEHICLE_CREATED_PARKED)&&!HasPlayerInside(v)&&!ScriptHolds(v)&&SharesPopulationAt(v->GetPosition(),v->AreaCode()))continue;
        if (ScriptHolds(v) || RemoteDistance(v->GetPosition(), v->AreaCode()) < kTrafficIn) EnsureNetworked(v);
    }
}

// The car the player came in waits outside while the player is in an interior. An interior lies kilometres from its
// door (CCarCtrl::PossiblyRemoveVehicle measures on the map), so the game dropped the car, or handed it to the other
// player's game as an ordinary car that went as soon as that player got far too: both found nothing outside.
void Vehicles::KeepCarWhileInside(CPlayerPed* me) {
    if (me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me) {
        m_lastDriven = GetVehicleRef(me->Vehicle());
        if (auto* nv = EnsureNetworked(me->Vehicle()); nv && !nv->playerUsed) {
            nv->playerUsed = true;
            Announce(*nv);
        }
    }
    int8_t area = me->AreaCode();
    if (area != 0 && m_lastArea == 0 && m_kept < 0) {
        CVehicle* v = GetVehicle(m_lastDriven);
        uint8_t by = v ? v->CreatedBy() : 0;
        if (v && v->AreaCode() == 0 && (by == VEHICLE_CREATED_RANDOM || by == VEHICLE_CREATED_PARKED)) {
            m_kept = m_lastDriven;
            m_keptBy = by;
            SetVehicleCreatedBy(v, VEHICLE_CREATED_MISSION);
            LOGI("The player's car (model %d) is kept outside while the player is in interior %d", v->ModelIndex(), area);
        }
    } else if (area == 0 && m_lastArea != 0) {
        ReleaseKeptCar();
    }
    m_lastArea = area;
}

void Vehicles::ReleaseKeptCar() {
    CVehicle* v = m_kept >= 0 ? GetVehicle(m_kept) : nullptr;
    NetVehicle* nv = v ? Find(v) : nullptr;
    if (v && (!nv || (!IsMirror(*nv) && !nv->playerUsed && !IsRecent(nv->netId))) && v->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(v))
        SetVehicleCreatedBy(v, m_keptBy);
    m_kept = -1;
}

void Vehicles::OnPostProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    uint64_t now = NowMs();
    if(Net().IsHost())for(auto&[id,nv]:m_byId)if(Alive(nv)&&nv.scriptAttachment.parentVehicle&&
       nv.scriptAttachment.AttachmentInStory(StoryRunId(),CooperativeStoryPhase())&&!nv.parentCollisionHeld){
        const auto&claim=nv.scriptAttachment;auto*parent=Find(claim.parentVehicle);
        if(parent&&Alive(*parent)&&Net().EntityGeneration(ENTITY_VEHICLE,parent->netId)==claim.parentGeneration&&nv.veh->AttachedTo()!=parent->veh){
            // Native physics can release a binding itself (tilt/collision).
            // Publish that real result before the mirror/binding pass, rather
            // than silently pinning it back to a parent the game released.
            ShareScriptAttachment(nv.veh,nv.veh->AttachedTo()!=nullptr);
        }
    }
    for(auto&[id,nv]:m_byId)ObserveScriptOwnership(nv);
    if(Net().IsHost())for(auto&[id,nv]:m_byId)if(nv.scriptAttachment.attachmentRevision&&
       nv.scriptAttachment.AttachmentInStory(StoryRunId(),CooperativeStoryPhase())&&now-nv.attachmentSentMs>1000)SendScriptAttachment(nv);
    if(auto*v=me->VehicleIfIn();v&&v->Driver()==me)
        if(auto*n=Find(v);n&&IsMirror(*n))TakeOwnership(*n);
    if (me->InVehicle() && me->Vehicle()) RememberUsed(me->Vehicle());
    UpdateRecentProtection();
    KeepCarWhileInside(me);
    for (auto& [v, to] : m_giveAway)  // still ours and still free to go
        if (NetVehicle* nv = IsVehicleValid(v) ? Find(v) : nullptr; nv && !IsMirror(*nv) && CanGiveAway(v)) GiveAway(*nv, to);
    m_giveAway.clear();
    // A different interior can be kilometres away in native coordinates. Handover must
    // not depend on the old owner's population removal callback being processed.
    if (now - m_lastScan >= 250) for (auto& [id, nv] : m_byId) {
        if (IsMirror(nv) || !IsVehicleValid(nv.veh)) continue;
        CVehicle* v = nv.veh;
        const auto preferred=PopulationOwnerAt(v->GetPosition(),v->AreaCode());
        if(preferred==Net().HostId()&&preferred!=Net().MyId()&&(nv.netId>>12)==Net().HostId()+1)QueueGiveAway(v,preferred);
        if (v->AreaCode() == me->AreaCode() && (v->GetPosition() - me->GetPosition()).Length2D() < 130) continue;
        uint8_t best=PopulationOwnerAt(v->GetPosition(),v->AreaCode());
        if (best != proto::kServerId && best != Net().MyId()) QueueGiveAway(v, best);
    }
    // a trailer or a car this player's vehicle tows is simulated here from now on
    if (CVehicle* mine = me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me ? me->Vehicle() : nullptr)
        for(int depth=0;depth<6;++depth){
            auto*towed=Field<CVehicle*>(mine,0x4C8);
            if(!IsVehicleValid(towed)||towed==mine)break;
            if(auto*tn=EnsureNetworked(towed);tn&&IsMirror(*tn))TakeOwnership(*tn);
            mine=towed;
        }
    if (now - m_lastScan >= 250) {
        m_lastScan = now;
        ScanTraffic();
    }
    for (auto& [id, nv] : m_byId) {
        if(!Alive(nv)||nv.veh->ModelIndex()!=nv.model)continue;
        if (IsMirror(nv)) {
            if(TrainingVehicleRestricted(nv.veh))nv.veh->DoorLock()=2;
            // Recorded mission vehicles must render the sampled pose after
            // native NPC/recording physics too. Otherwise that extra local
            // movement is snapped back at the start of the next frame.
            if(nv.script&&nv.hasState&&nv.veh->Driver()!=me)ApplyMirror(nv,TimeStep()/50.f);
            // keep the visual controls after the physics step (wheels, brake lights)
            if (nv.hasState) {
                nv.veh->SetEngineOn((nv.state.flags&VehicleState::F_ENGINE)!=0);
                nv.veh->PhysicalFlags()=(nv.veh->PhysicalFlags()&~0x100u)|((nv.state.water&1)?0x100u:0);
                nv.veh->VehicleFlagsLo()=(nv.veh->VehicleFlagsLo()&~0x40000000u)|((nv.state.water&2)?0x40000000u:0);
                if(nv.state.extras&VehicleState::X_ROTOR){
                    if(nv.veh->VehicleSubClass()==VEHICLE_TYPE_HELI)Field<float>(nv.veh,0x84C)=nv.state.rotorSpeed;
                    else if(nv.veh->VehicleSubClass()==VEHICLE_TYPE_PLANE)Field<float>(nv.veh,0x9C4)=nv.state.rotorSpeed;
                }
                nv.veh->SteerAngle() = nv.state.steer;
                nv.veh->BrakePedal() = nv.state.brake;
                nv.veh->GasPedal() = nv.state.gas;
                nv.veh->SetHandbrake((nv.state.flags&VehicleState::F_HANDBRAKE)!=0);
                if(nv.state.extras&VehicleState::X_WHEELS)nv.renderedWheels.Apply(nv.veh);
            }
            continue;
        }
        auto*child=Find(Field<CVehicle*>(nv.veh,0x4C8));auto*parent=Find(Field<CVehicle*>(nv.veh,0x4C4));
        uint16_t childId=child?child->netId:0,parentId=parent?parent->netId:0;
        if(childId!=nv.lastTowChild||parentId!=nv.lastTowParent){
            nv.lastTowChild=childId;nv.lastTowParent=parentId;SendState(nv,true);
        }
        // owned: mod shop changes; repeated every 10 s for mirrors that were still streaming their model
        if (now - nv.lookCheckMs >= 1000) {
            nv.lookCheckMs = now;
            if (!nv.hasLook || !(CaptureLook(nv.veh) == nv.look) || now - nv.lookSentMs > 10000) {
                SendLook(nv);
                nv.lookSentMs = now;
            }
        }
        // owned: the local driver's state goes in G_PLAYER_DRIVER (Players); here only unoccupied vehicles
        if (me && nv.veh->Driver() == me) {
            nv.lastLocalDriverMs=now;
            nv.lastChangeMs = now;
            continue;
        }
        // A car of the host's mission (its id has the host's prefix) the partner drove was the partner's game's from then
        // on, and the host's mission moving, freezing, mending or deleting it only touched its copy there, which the
        // owner's states undid. Left standing empty for 2 s, it goes back to the host.
        // An NPC (including a corpse still occupying the driver slot) does not
        // hold a story car in a former guest driver's simulation indefinitely.
        if (nv.script && !Net().IsHost() && (nv.netId >> 12) == Net().HostId() + 1 &&
            !HasPlayerInside(nv.veh) && !IsCooperativeRemoteVehicle(nv.veh,Net().MyId()) && now - nv.lastLocalDriverMs > 500) {
            nv.owner = Net().HostId();
            nv.hasState = false;
            Protect(nv);
            Writer w(proto::G_VEH_OWNER);
            w.Entity(ENTITY_VEHICLE,nv.netId);
            w.U8(nv.owner);
            w.U32((uint32_t)NowMs());
            Net().Send(proto::CH_RELIABLE, w);
            LOGI("Vehicle %04X of the host's mission goes back to the host", nv.netId);
            continue;
        }
        const bool engine=nv.veh->EngineOn();const int type=nv.veh->VehicleSubClass();
        const bool aircraft=type==VEHICLE_TYPE_HELI||type==VEHICLE_TYPE_PLANE;
        const float rawRotor=aircraft?Field<float>(nv.veh,type==VEHICLE_TYPE_HELI?0x84C:0x9C4):0.f;
        const float rotor=std::isfinite(rawRotor)?std::clamp(rawRotor,0.f,5.f):0.f;
        if(engine!=nv.lastEngine||(aircraft&&std::fabs(rotor-nv.lastRotor)>.005f&&now-nv.lastSendMs>=100)){
            nv.lastEngine=engine;nv.lastRotor=rotor;nv.lastChangeMs=now;nv.lastSendMs=now;SendState(nv,true);
        }
        float speed = nv.veh->MoveSpeed().Length() * 50.0f;
        if (speed > 0.1f || nv.veh->TurnSpeed().Length()>.0002f || (aircraft&&rotor>.001f) ||
            std::fabs(nv.veh->Health()-nv.sentHealth)>=1.f) nv.lastChangeMs = now;
        bool active = now - nv.lastChangeMs < 3000;
        bool towed = Field<CVehicle*>(nv.veh, 0x4C4) != nullptr;  // CVehicle::m_pTowingVehicle
        uint64_t interval = towed ? 50 : active ? 100 : 500;
        if (now - nv.lastSendMs >= interval) {
            nv.lastSendMs = now;
            SendState(nv);
        }
    }
    UpdateVehicleBindings();
}

void Vehicles::OnSessionEnd() {
    m_scriptLocks.clear();
    for(auto&[id,nv]:m_byId){nv.scriptAttachment={};ApplyVehicleBinding(nv,VehicleState{});}
    ReleaseKeptCar();
    m_recent.clear();
    m_restoreRecent.clear();
    // Foreign cars created solely for the network are not local traffic. If
    // released here, the next connection re-announces them under new IDs while
    // their original owner also recreates them. Native originals and local
    // ownership remain; a vehicle supporting the local player or held by a
    // local script is retained and can rebind only to its exact birth.
    auto* me=FindPlayerPed(0);
    std::vector<uint16_t> discard;
    for (auto& [id, nv] : m_byId) {
        if (IsMirror(nv) && nv.createdByUs && Alive(nv)) {
            auto* v=nv.veh;
            const bool supportsLocal=me && ((me->InVehicle() && me->Vehicle()==v) ||
                Field<CEntity*>(me,0x568)==v || Field<CEntity*>(me,0x584)==v ||
                me->AttachedTo()==v || OccupiedTowChain(v));
            if (!supportsLocal && !ScriptHolds(v)) { discard.push_back(id); continue; }
            const auto generation=Net().EntityGeneration(ENTITY_VEHICLE,id);
            if (generation) m_retainedCopies[id]={nv.handle,nv.model,generation,0};
        }
        if (IsMirror(nv) || nv.playerUsed || nv.origBy) { nv.script=false; Release(nv); }
    }
    for (auto id : discard) Forget(id,true);
    m_byId.clear();
    m_byPtr.clear();
    m_pending.clear();
    m_pendingStates.clear();m_pendingLooks.clear();
    m_adoptPending.clear();
    m_scriptRemoved.clear();
}

void Vehicles::OnPeerLeave(uint8_t id) {
    m_recent.erase(id);
    std::vector<uint8_t> survivors{Net().MyId()};
    for(auto&[peerId,peer]:Net().Peers())if(peerId!=id)survivors.push_back(peerId);
    const auto successor=VehicleSuccessor(Net().HostId(),id,survivors);
    for(auto it=m_giveAway.begin();it!=m_giveAway.end();)it=it->second==id?m_giveAway.erase(it):std::next(it);
    for(auto it=m_pendingStates.begin();it!=m_pendingStates.end();)it=it->second.first==id?m_pendingStates.erase(it):std::next(it);
    // Streaming can still be loading the departed player's model. Keep the
    // same identity instead of losing that car and later creating a second one.
    for(auto&[vid,spawn]:m_pending)if(spawn.owner==id){
        spawn.owner=successor;spawn.script=false;
        if(successor==Net().MyId())m_adoptPending.insert(vid);else m_adoptPending.erase(vid);
    }
    for(auto&[vid,nv]:m_byId)if(nv.owner==id){
        nv.owner=successor;nv.hasState=false;nv.history.Clear();
        nv.script=Alive(nv)&&ScriptHolds(nv.veh);
        nv.firstAnnouncedMs=NowMs();nv.handoverUntil=NowMs()+3000;
        if(successor==Net().MyId()){
            Release(nv);
            if(Alive(nv)){Announce(nv);SendLook(nv);SendState(nv,true);}
        }else Protect(nv);
        LOGI("Vehicle %04X inherited from player %u by %u native=%d generation=%llu",vid,id,successor,nv.handle,(unsigned long long)Net().EntityGeneration(ENTITY_VEHICLE,vid));
    }
}

void Vehicles::OnSessionStart() {
    m_ids.Reset((uint32_t)NowMs());
    for (auto& [id, saved] : m_retainedCopies) saved.expires=NowMs()+15000;
}

void Vehicles::OnGameStarted() {
    m_scriptLocks.clear();
    auto restored = std::move(m_restoreRecent);
    m_restoreRecent.clear();
    for (const auto& spawn : restored) {
        if (Find(spawn.netId) || !LoadModelNow(spawn.model)) continue;
        CVehicle* vehicle = FindAdoptable(spawn.model, spawn.pos);
        if (!vehicle) {
            vehicle = SpawnMirror(spawn);
            if (vehicle) SetVehicleCreatedBy(vehicle, VEHICLE_CREATED_RANDOM);
        }
        if (vehicle) RestorePlayerCar(vehicle, spawn);
    }
    UpdateRecentProtection();
}

void Vehicles::OnGameShutdown() {
    m_restoreRecent.clear();
    m_retainedCopies.clear(); // native pool handles no longer identify this world
    if (Net().Online()) for (const auto& [id, nv] : m_byId) {
        if (IsMirror(nv) || !nv.playerUsed || nv.script || !IsVehicleValid(nv.veh) || nv.veh->Health() <= 0 || ScriptHolds(nv.veh)) continue;
        VehicleSpawn spawn;
        spawn.netId = id; spawn.owner = Net().MyId(); spawn.model = nv.model;
        spawn.pos = nv.veh->GetPosition(); spawn.health = nv.veh->Health(); spawn.area = nv.veh->AreaCode();
        if (nv.veh->Matrix()) spawn.rot = Quat::FromMatrix(*nv.veh->Matrix());
        for(int c=0;c<2;++c)spawn.components[c]=Field<int8_t>(nv.veh,0x438+c);
        memcpy(spawn.colors, nv.veh->Colors(), 4); spawn.lockState = (uint8_t)nv.veh->DoorLock(); spawn.playerUsed = true;
        m_restoreRecent.push_back(spawn);
        if (m_restoreRecent.size() >= RecentVehicles::kLimit * proto::kMaxPlayers) break;
    }
    m_lastDriven = m_kept = -1;  // a new world: the old handles mean other cars
    m_lastArea = 0;
    m_byId.clear();
    m_byPtr.clear();
    m_pending.clear();
    m_pendingStates.clear();m_pendingLooks.clear();
    m_adoptPending.clear();
    m_scriptRemoved.clear();
    m_giveAway.clear();
}


}  // namespace coop

namespace coop { void Vehicles::OnEntityReplaced(uint8_t kind,uint8_t,uint16_t id){if(kind!=ENTITY_VEHICLE)return;Forget(id,true);m_pending.erase(id);m_pendingStates.erase(id);m_pendingLooks.erase(id);m_adoptPending.erase(id);m_scriptRemoved.erase(id);m_scriptLocks.erase(id);} }
