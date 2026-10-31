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
#include "common/lib/diagnostics/PassengerTrace.h"
#include "common/lib/profile/Profile.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/activity/Activities.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
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

constexpr uint8_t STATUS_PLAYER = 0, STATUS_PHYSICS = 3, STATUS_WRECKED = 5, STATUS_IS_TOWED = 10;
constexpr uint8_t VEHICLE_CREATED_RANDOM = 1, VEHICLE_CREATED_MISSION = 2, VEHICLE_CREATED_PARKED = 3;
constexpr float kTrafficIn = 170.0f;   // owned vehicles closer than this to the partner are announced (> car spawn range)
constexpr float kTrafficOut = 220.0f;  // and dropped from the network beyond this

enum VehicleDiagnosticReason:uint16_t {
    VD_OK=0,VD_MALFORMED,VD_SENDER,VD_NOT_READY,VD_CONFLICT,VD_MODEL,VD_PENDING,
    VD_MODEL_WAIT,VD_POOL_WAIT,VD_NATIVE_FAILED,VD_OWNER,VD_GENERATION,VD_STORY,
    VD_ATTACHMENT,VD_OLD_TIME,VD_NATIVE_MISSING,VD_RETAINED,VD_DELETE_FAILED,
    VD_WRECK,VD_OWNER_HELD,VD_REPLACED,VD_BUDGET,VD_CANCELLED
};
flight::Context IncomingVehicleDiagnostic(uint16_t id,uint8_t a,uint8_t b=0){
    if(!flight::Enabled())return {};
    auto c=Net().DiagnosticContext();if(c.msg!=a&&(!b||c.msg!=b))return {};
    c.entity=id;c.generation=Net().ExpectedEntityGeneration(ENTITY_VEHICLE,id);return c;
}
flight::Context LocalVehicleDiagnostic(uint16_t id,uint8_t msg){
    if(!flight::Enabled())return {};
    flight::Context c{};c.msg=msg;c.entity=id;c.generation=Net().EntityGeneration(ENTITY_VEHICLE,id);
    c.source=Net().MyId();c.incarnation=Net().MyIncarnation();c.world=Net().WorldEpoch();return c;
}
void RecordVehicleDiagnostic(flight::Context c,uint16_t stage,uint16_t reason,const NetVehicle*nv=nullptr,float before=0,uint8_t pendingOwner=255,const float* appliedHealth=nullptr){
    if(!c.msg||!flight::Enabled())return;
    flight::Event e{};e.context=c;e.stage=stage;e.reason=reason;e.localPeer=Net().MyId();
    if(nv){
        e.owner=nv->owner;e.detailKnown|=flight::OwnerKnown;
        e.nativeHandle=(uint32_t)nv->handle;
        if(stage!=624){
            if(c.generation&&Net().EntityGeneration(ENTITY_VEHICLE,nv->netId)!=c.generation){e.stage=624;e.reason=VD_GENERATION;}
            else if(Vehicles::Alive(*nv)){
                e.beforeHealth=before;e.afterHealth=nv->veh->Health();e.health=appliedHealth?*appliedHealth:nv->hasState?nv->state.health:e.afterHealth;
            }else if(stage!=622){e.stage=624;e.reason=VD_NATIVE_MISSING;}
        }
    }else if(pendingOwner!=255){e.owner=pendingOwner;e.detailKnown|=flight::OwnerKnown;}
    (void)flight::Record(e);
}
void RetireVehicleStateDiagnostic(NetVehicle&nv,uint16_t reason){
    if(nv.diagnosticStatePending)RecordVehicleDiagnostic(nv.diagnosticState,624,reason,&nv);
    nv.diagnosticStatePending=false;
}
void RecordVehicleRemoval(flight::Context c,uint8_t owner,int handle,CVehicle*expected,bool wasAlive,float before,bool requested){
    if(!c.msg||!flight::Enabled())return;
    auto*after=GetVehicle(handle);
    flight::Event e{};e.context=c;e.stage=622;e.localPeer=Net().MyId();e.owner=owner;e.detailKnown|=flight::OwnerKnown;
    e.nativeHandle=(uint32_t)handle;e.beforeHealth=before;
    e.reason=!requested?VD_RETAINED:!wasAlive?VD_NATIVE_MISSING:after==expected?VD_DELETE_FAILED:VD_OK;
    if(after&&after==expected)e.afterHealth=after->Health();
    (void)flight::Record(e);
}


// These contexts follow the actual accepted sender-clock history. They are not
// a second interpolation buffer and never participate in gameplay decisions.
enum TypedVehicleReason:uint16_t { VT_TRANSITION=32,VT_CONTEXT,VT_MISSING,VT_ADMISSION_BUSY,VT_ADMISSION_BUDGET,VT_ADMISSION_DISABLED,VT_REFERENCE_OMITTED,VT_APPLY_WRECK,VT_BODY_REPLACED,VT_APPLY_FAILED,VT_SUPERSEDED,VT_CANCELLED };
bool SameVehicleDiagnosticPoint(const VehicleDiagnosticPoint&a,const VehicleDiagnosticPoint&b){
    const auto&x=a.context;const auto&y=b.context;
    return a.clock==b.clock&&a.localWorld==b.localWorld&&x.fingerprint==y.fingerprint&&x.generation==y.generation&&x.wireTime==y.wireTime&&x.incarnation==y.incarnation&&x.world==y.world&&x.entity==y.entity&&x.msg==y.msg&&x.channel==y.channel&&x.source==y.source&&x.recipient==y.recipient;
}
void RetireVehicleDiagnosticAttempt(NetVehicle&nv,uint16_t reason){
    auto&a=nv.diagnosticTypedAttempt;if(!a.pending)return;
    if(flight::Enabled()){
        auto base=[&](){flight::Event e{};e.context=a.upper.context;if(a.mode){e.context.fingerprint=0;e.context.wireTime=0;}e.stage=629;e.reason=reason;e.localPeer=Net().MyId();e.span=a.span;e.detailKnown=flight::SpanKnown;return e;};
        auto e=base();flight::AddUnsigned(e,flight::Metric::BracketBeforeClock,a.lowerClock);flight::AddUnsigned(e,flight::Metric::BracketAfterClock,a.upperClock);flight::AddUnsigned(e,flight::Metric::ReferenceKind,a.mode);flight::AddUnsigned(e,flight::Metric::RequestedVehicleFlags,a.flags);(void)flight::Record(e);
        auto ref=[&](const VehicleDiagnosticPoint&p,unsigned role){if(!p.context.msg)return;auto r=base();r.context=p.context;flight::AddUnsigned(r,flight::Metric::StateClock,p.clock);flight::AddUnsigned(r,flight::Metric::ReferenceKind,role);(void)flight::Record(r);};
        if(a.mode==1)ref(a.lower,1);ref(a.upper,a.mode==1?2:0);
    }
    a={};
}
void ClearVehicleDiagnosticHistory(NetVehicle&nv){RetireVehicleDiagnosticAttempt(nv,VT_CANCELLED);nv.diagnosticHistoryCount=nv.diagnosticHistoryNext=0;nv.diagnosticLatest={};nv.diagnosticTypedKnown=false;nv.diagnosticTypedError=0;nv.diagnosticTypedThrottled=0;nv.diagnosticTypedAttempt={};}
void RememberVehicleDiagnosticPoint(NetVehicle&nv,const VehicleState&st,const flight::Context&context,bool pushed,bool empty){
    if(!flight::Enabled()){nv.diagnosticHistoryCount=nv.diagnosticHistoryNext=0;nv.diagnosticLatest={};nv.diagnosticTypedKnown=nv.diagnosticTypedFlagsKnown=false;nv.diagnosticTypedAttempt={};return;}
    if(empty)ClearVehicleDiagnosticHistory(nv);
    VehicleDiagnosticPoint point{st.time,Net().WorldEpoch(),context};nv.diagnosticLatest=point;
    if(pushed){nv.diagnosticHistory[nv.diagnosticHistoryNext]=point;nv.diagnosticHistoryNext=(nv.diagnosticHistoryNext+1)%nv.diagnosticHistory.size();nv.diagnosticHistoryCount=std::min<size_t>(nv.diagnosticHistory.size(),nv.diagnosticHistoryCount+1);}
}
VehicleDiagnosticPoint VehicleDiagnosticAt(const NetVehicle&nv,uint32_t clock,bool history){
    if(!history)return nv.diagnosticLatest.clock==clock?nv.diagnosticLatest:VehicleDiagnosticPoint{};
    for(size_t n=0;n<nv.diagnosticHistoryCount;++n){size_t at=(nv.diagnosticHistoryNext+nv.diagnosticHistory.size()-1-n)%nv.diagnosticHistory.size();if(nv.diagnosticHistory[at].clock==clock)return nv.diagnosticHistory[at];}
    return {};
}
struct VehicleDiagnosticSelection {uint32_t lower=0,upper=0;float alpha=0,age=0;bool blended=false,predicted=false,stale=false,history=false;};
struct VehicleDiagnosticGoal {VehicleDiagnosticPoint lower{},upper{};VehicleDiagnosticSelection selection{};int handle=-1;CVehicle*body=nullptr;uint16_t id=0;uint64_t generation=0;VehicleDiagnosticPoint latest{};uint8_t latestFlags=0;};

uint8_t VehicleDiagnosticMode(const VehicleDiagnosticSelection&s){return s.blended?1:s.predicted?(s.stale?3:2):0;}
bool VehicleDiagnosticCurrent(const VehicleDiagnosticPoint&p,uint16_t id,uint8_t owner){
    const auto&c=p.context;if(!c.msg||c.entity!=id||!c.generation||p.localWorld!=Net().WorldEpoch()||c.source!=owner||Net().EntityGeneration(ENTITY_VEHICLE,id)!=c.generation)return false;
    if(c.source==Net().MyId())return c.incarnation==Net().MyIncarnation()&&c.world==Net().WorldEpoch();
    auto*peer=Net().Peer(c.source);return peer&&peer->incarnation==c.incarnation&&Net().PeerWorldEpoch(c.source)==c.world;
}
float VehicleRequestedHeading(const Quat&q){return std::atan2(-2.f*(q.x*q.y-q.w*q.z),1.f-2.f*(q.x*q.x+q.z*q.z));}
flight::Event VehicleTypedEvent(const VehicleDiagnosticGoal&g,uint64_t span,uint16_t stage,uint16_t reason,uint8_t owner=255){
    flight::Event e{};e.context=g.upper.context;
    if(!e.context.msg){e.context.msg=proto::G_VEH_SYNC;e.context.entity=g.id;}
    // Blending/prediction has two input clocks or an extrapolation interval.
    // Its primary observation is never assigned one packet's fingerprint/time.
    if(g.selection.blended||g.selection.predicted){e.context.fingerprint=0;e.context.wireTime=0;}
    e.stage=stage;e.reason=reason;e.span=span;e.detailKnown=flight::SpanKnown;e.localPeer=Net().MyId();
    if(owner<3){e.owner=owner;e.detailKnown|=flight::OwnerKnown;}return e;
}
void ObserveVehicleNative(const VehicleDiagnosticGoal&goal,const VehicleState&st,uint16_t applyReason){
    if(!flight::Enabled())return;
    auto*nv=Vehicles::Instance().DiagnosticRecord(goal.id);if(!nv)return;
    const uint8_t mode=VehicleDiagnosticMode(goal.selection);const auto now=NowMs();
    const bool transition=nv->diagnosticTypedFlagsKnown&&((nv->diagnosticTypedFlags^st.flags)&(VehicleState::F_ENGINE|VehicleState::F_WRECKED));
    const bool same=nv->diagnosticTypedKnown&&nv->diagnosticTypedLower==goal.selection.lower&&nv->diagnosticTypedUpper==goal.selection.upper&&nv->diagnosticTypedMode==mode;
    auto&a=nv->diagnosticTypedAttempt;
    const bool sameAttempt=a.known&&a.lowerClock==goal.selection.lower&&a.upperClock==goal.selection.upper&&a.mode==mode&&a.flags==st.flags&&a.applyReason==applyReason&&SameVehicleDiagnosticPoint(a.lower,goal.lower)&&SameVehicleDiagnosticPoint(a.upper,goal.upper);
    const bool critical=transition||(sameAttempt&&a.pending&&a.critical)||(applyReason!=VD_OK&&(!same||nv->diagnosticTypedApplyReason!=applyReason));
    if(a.pending&&!sameAttempt)RetireVehicleDiagnosticAttempt(*nv,VT_SUPERSEDED);
    const bool recent=nv->diagnosticTypedAttemptKnown&&now>=nv->diagnosticTypedAttemptMs&&now-nv->diagnosticTypedAttemptMs<100;
    // Deferred critical observations retry, but never spin native admission once
    // per frame. A new critical input may bypass an unrelated ordinary throttle.
    if((sameAttempt&&recent)||(!critical&&(same||(sameAttempt&&a.skipped)||recent))){++nv->diagnosticTypedThrottled;return;}
    if(!sameAttempt){a={goal.lower,goal.upper,flight::NextDiagnosticSpan(),goal.selection.lower,goal.selection.upper,applyReason,mode,st.flags,true,false,false,false};nv->diagnosticTypedError=0;}
    nv->diagnosticTypedAttemptMs=now;nv->diagnosticTypedAttemptKnown=true;
    const auto span=a.span;
    auto unavailable=[&](uint16_t reason){a.pending=true;a.critical=critical;a.skipped=false;if(nv->diagnosticTypedError==reason&&sameAttempt)return;nv->diagnosticTypedError=reason;auto e=VehicleTypedEvent(goal,span,629,reason);flight::AddUnsigned(e,flight::Metric::StateClock,st.time);(void)flight::Record(e);};
    if(nv->handle!=goal.handle||nv->veh!=goal.body){unavailable(VT_BODY_REPLACED);return;}
    if(!VehicleDiagnosticCurrent(goal.upper,goal.id,nv->owner)||(goal.selection.blended&&!VehicleDiagnosticCurrent(goal.lower,goal.id,nv->owner))||Net().EntityGeneration(ENTITY_VEHICLE,goal.id)!=goal.generation){unavailable(VT_CONTEXT);return;}
    const auto admitted=flight::AdmitNativeObservation(goal.upper.context,625,Net().MyId(),critical);
    if(admitted==flight::NativeAdmission::Filtered||admitted==flight::NativeAdmission::Sampled){a.pending=false;a.skipped=true;return;}
    if(admitted!=flight::NativeAdmission::Admitted){unavailable(admitted==flight::NativeAdmission::Busy?VT_ADMISSION_BUSY:admitted==flight::NativeAdmission::Budget?VT_ADMISSION_BUDGET:VT_ADMISSION_DISABLED);return;}
    // Admission precedes every additional native query. Reacquire by the saved
    // handle/body/generation; never read a recycled node or an old pool body.
    if(!Vehicles::Alive(*nv)||!IsVehicleValid(nv->veh)||nv->veh->ModelIndex()!=nv->model){unavailable(VT_MISSING);return;}
    // Commit deduplication and transition flags only after a real gated native
    // binding. A Busy/Budget attempt must not look like successful observation.
    a.pending=a.critical=a.skipped=false;
    nv->diagnosticTypedKnown=true;nv->diagnosticTypedLower=goal.selection.lower;nv->diagnosticTypedUpper=goal.selection.upper;nv->diagnosticTypedMode=mode;nv->diagnosticTypedApplyReason=applyReason;
    nv->diagnosticTypedFlags=st.flags;nv->diagnosticTypedFlagsKnown=true;
    nv->diagnosticTypedError=0;auto*v=nv->veh;const auto pos=v->GetPosition(),velocity=v->MoveSpeed()*50.f;
    const float heading=v->GetHeading(),steer=v->SteerAngle(),gas=v->GasPedal(),brake=v->BrakePedal();
    const auto physical=v->PhysicalFlags(),entityFlags=v->Flags();const auto lo=v->VehicleFlagsLo(),hi=v->VehicleFlagsHi();const auto status=v->Status();const bool engine=(lo>>4)&1,siren=(hi>>15)&1;
    const uint8_t water=((physical&0x100u)?1:0)|((lo&0x40000000u)?2:0);
    uint8_t flags=(engine?VehicleState::F_ENGINE:0)|(((lo>>6)&1)?VehicleState::F_LIGHTS:0)|(siren?VehicleState::F_SIREN:0)|(status==STATUS_WRECKED?VehicleState::F_WRECKED:0)|(((lo>>5)&1)?VehicleState::F_HANDBRAKE:0)|(!v->IsVisible()?VehicleState::F_HIDDEN:0);
    if(Field<uint32_t>(v,0x514))flags|=siren?VehicleState::F_FAST_SIREN:VehicleState::F_HORN;
    const int type=v->VehicleSubClass();const bool rotorKnown=type==VEHICLE_TYPE_HELI||type==VEHICLE_TYPE_PLANE;
    const float rotor=type==VEHICLE_TYPE_HELI?Field<float>(v,0x84C):type==VEHICLE_TYPE_PLANE?Field<float>(v,0x9C4):0;
    const auto reason=applyReason==VD_NATIVE_FAILED||(applyReason==VD_WRECK&&(st.flags&VehicleState::F_WRECKED)&&status!=STATUS_WRECKED)?VT_APPLY_FAILED:applyReason!=VD_OK?VT_APPLY_WRECK:transition?VT_TRANSITION:uint16_t(0);
    auto record=[&](flight::Event&e){e.nativeHandle=(uint32_t)goal.handle;(void)flight::Record(e);};
    auto reference=[&](const VehicleDiagnosticPoint&p,uint8_t role){auto e=VehicleTypedEvent(goal,span,629,reason,nv->owner);e.context=p.context;flight::AddUnsigned(e,flight::Metric::StateClock,p.clock);flight::AddUnsigned(e,flight::Metric::ReferenceKind,role);if(!flight::Record(e)){auto missing=VehicleTypedEvent(goal,span,629,VT_REFERENCE_OMITTED,nv->owner);flight::AddUnsigned(missing,flight::Metric::StateClock,p.clock);flight::AddUnsigned(missing,flight::Metric::ReferenceKind,role);(void)flight::Record(missing);}};
    if(goal.selection.blended){reference(goal.lower,1);reference(goal.upper,2);}else reference(goal.upper,0);
    // Visibility/area are applied from nv.state before history selection. Keep
    // that distinct input when it differs from the selected motion snapshot.
    const bool mixedFlags=goal.latest.context.msg&&(goal.latest.clock!=goal.upper.clock||goal.latest.context.fingerprint!=goal.upper.context.fingerprint);
    if(mixedFlags){auto latest=VehicleTypedEvent(goal,span,629,reason,nv->owner);latest.context=goal.latest.context;flight::AddUnsigned(latest,flight::Metric::ReferenceKind,4);flight::AddUnsigned(latest,flight::Metric::StateClock,goal.latest.clock);flight::AddUnsigned(latest,flight::Metric::RequestedVehicleFlags,goal.latestFlags);if(!flight::Record(latest)){auto missing=VehicleTypedEvent(goal,span,629,VT_REFERENCE_OMITTED,nv->owner);flight::AddUnsigned(missing,flight::Metric::ReferenceKind,4);(void)flight::Record(missing);}}
    auto meta=VehicleTypedEvent(goal,span,629,reason,nv->owner);flight::AddUnsigned(meta,flight::Metric::BracketBeforeClock,goal.selection.lower);flight::AddUnsigned(meta,flight::Metric::BracketAfterClock,goal.selection.upper);flight::AddUnsigned(meta,flight::Metric::ReferenceKind,mode);flight::AddUnsigned(meta,flight::Metric::ThrottledObservations,nv->diagnosticTypedThrottled);if(goal.selection.blended)flight::AddFloat(meta,flight::Metric::InterpolationAlpha,goal.selection.alpha);if(goal.selection.predicted)flight::AddFloat(meta,flight::Metric::PredictionSeconds,goal.selection.age);record(meta);
    auto p=VehicleTypedEvent(goal,span,625,reason,nv->owner);flight::AddUnsigned(p,flight::Metric::StateClock,st.time);flight::AddUnsigned(p,flight::Metric::ReferenceKind,mode);flight::AddFloat(p,flight::Metric::RequestedX,st.pos.x);flight::AddFloat(p,flight::Metric::RequestedY,st.pos.y);flight::AddFloat(p,flight::Metric::RequestedZ,st.pos.z);flight::AddFloat(p,flight::Metric::ObservedX,pos.x);flight::AddFloat(p,flight::Metric::ObservedY,pos.y);flight::AddFloat(p,flight::Metric::ObservedZ,pos.z);record(p);
    auto motion=VehicleTypedEvent(goal,span,626,reason,nv->owner);flight::AddFloat(motion,flight::Metric::RequestedVx,st.vel.x);flight::AddFloat(motion,flight::Metric::RequestedVy,st.vel.y);flight::AddFloat(motion,flight::Metric::RequestedVz,st.vel.z);flight::AddFloat(motion,flight::Metric::ObservedVx,velocity.x);flight::AddFloat(motion,flight::Metric::ObservedVy,velocity.y);flight::AddFloat(motion,flight::Metric::ObservedVz,velocity.z);flight::AddFloat(motion,flight::Metric::RequestedHeading,VehicleRequestedHeading(st.rot));flight::AddFloat(motion,flight::Metric::ObservedHeading,heading);record(motion);
    auto effects=VehicleTypedEvent(goal,span,627,reason,nv->owner);if(mixedFlags){effects.context.fingerprint=0;effects.context.wireTime=0;}flight::AddUnsigned(effects,flight::Metric::RequestedEngine,(st.flags&VehicleState::F_ENGINE)?1:0);flight::AddUnsigned(effects,flight::Metric::ObservedEngine,engine?1:0);flight::AddUnsigned(effects,flight::Metric::RequestedVehicleFlags,st.flags);flight::AddUnsigned(effects,flight::Metric::ObservedVehicleFlags,flags);if(st.extras&VehicleState::X_ROTOR)flight::AddFloat(effects,flight::Metric::RequestedRotor,st.rotorSpeed);if(rotorKnown)flight::AddFloat(effects,flight::Metric::ObservedRotor,rotor);record(effects);
    auto controls=VehicleTypedEvent(goal,span,628,reason,nv->owner);flight::AddUnsigned(controls,flight::Metric::StateClock,st.time);flight::AddFloat(controls,flight::Metric::RequestedSteer,st.steer);flight::AddFloat(controls,flight::Metric::ObservedSteer,steer);flight::AddFloat(controls,flight::Metric::RequestedGas,st.gas);flight::AddFloat(controls,flight::Metric::ObservedGas,gas);flight::AddFloat(controls,flight::Metric::RequestedBrake,st.brake);flight::AddFloat(controls,flight::Metric::ObservedBrake,brake);record(controls);
    auto physics=VehicleTypedEvent(goal,span,630,reason,nv->owner);flight::AddUnsigned(physics,flight::Metric::NativeFlags,physical);flight::AddUnsigned(physics,flight::Metric::ObservedMotionFlags,entityFlags);flight::AddUnsigned(physics,flight::Metric::RequestedWater,st.water);flight::AddUnsigned(physics,flight::Metric::ObservedWater,water);flight::AddUnsigned(physics,flight::Metric::ObservedVehicleStatus,status);flight::AddUnsigned(physics,flight::Metric::AttachmentFlags,(nv->parentAttached?1u:0u)|(nv->trainAttached?2u:0u)|(nv->physicsHeld?4u:0u)|(nv->groundless?8u:0u));record(physics);
}

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
    if (Net().Online() && v) {
        if (NetVehicle* nv = Vehicles::Instance().Find(v)) {
            if (Vehicles::Instance().IsMirror(*nv)||nv->authorityHeld||nv->crewStaged) return;
            // A returned player car can retain its old traffic route while empty.
            // Its current owner keeps native physics, without an absent driver's AI.
            if (nv->owner==Net().MyId() && nv->playerUsed && !nv->script && !nv->heldHere &&
                v->VehicleSubClass()==VEHICLE_TYPE_AUTOMOBILE && !v->Driver() &&
                !Field<CVehicle*>(v,0x4C4) && !Field<CVehicle*>(v,0x4C8) && !ScriptHolds(v)) return;
        }
    }
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
    m_scriptRemoved[id]=NowMs()+15000;RetirePendingDiagnostic(id,VD_CANCELLED);m_pending.erase(id);m_pendingDiagnostics.erase(id);m_pendingStates.erase(id);m_pendingLooks.erase(id);m_adoptPending.erase(id);m_scriptLocks.erase(id);
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
    if(n&&(n->authorityHeld||n->crewStaged))return false;
    return n&&vehicles.IsMirror(*n)&&n->hasState&&WheelState::Count(v)?false:originalCarAI(v,edx,flags);
}
bool __fastcall MirrorBikeAI(CVehicle*v,void*edx,uint32_t*flags){
    auto&vehicles=Vehicles::Instance();auto*n=vehicles.Find(v);
    if(n&&(n->authorityHeld||n->crewStaged))return false;
    return n&&vehicles.IsMirror(*n)&&n->hasState?false:originalBikeAI(v,edx,flags);
}
}
void Vehicles::OnInstallHooks() {
    Net().SetAuthorityHandler(ENTITY_VEHICLE,[](authority::Notice&n){return Instance().HandleAuthority(n);});
    mem::HookFn(0x6B4800,&MirrorCarAI,&originalCarAI,"CAutomobile::ProcessAI (owner inputs)");
    mem::HookFn(0x6BC930,&MirrorBikeAI,&originalBikeAI,"CBike::ProcessAI (owner inputs)");
    Net().SetHandler(proto::G_SCRIPT_VEH_REMOVE,[](uint8_t from,Reader&r){
        auto id=r.U16();if(!r.Ok()||from!=Net().HostId()||Net().IsHost()||!id)return;
        auto&self=Instance();self.m_scriptRemoved[id]=NowMs()+15000;
        self.RetirePendingDiagnostic(id,VD_CANCELLED);self.m_pending.erase(id);self.m_pendingDiagnostics.erase(id);self.m_pendingStates.erase(id);self.m_pendingLooks.erase(id);self.m_adoptPending.erase(id);self.m_scriptLocks.erase(id);
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

VehicleCrew Vehicles::CaptureCrew(CVehicle*car){
    VehicleCrew crew;if(!IsVehicleValid(car))return crew;
    for(int seat=-1;seat<std::min(8,(int)car->MaxPassengers());++seat){
        auto*p=seat<0?car->Driver():car->Passengers()[seat];if(!p)continue;
        VehicleCrewMember member;if(Peds::Instance().CaptureCrewMember(p,car,(int8_t)seat,member))crew.members[crew.count++]=member;
    }
    return crew;
}

bool Vehicles::CrewReady(const NetVehicle&nv,const VehicleCrew&crew)const{
    if(!Alive(nv))return false;
    for(unsigned i=0;i<crew.count;++i){const auto&m=crew.members[i];
        if(!Net().AuthorityLease(ENTITY_VEHICLE,nv.netId)||Net().AuthorityOwner(ENTITY_VEHICLE,nv.netId)!=nv.owner||!Net().EntityLink(ENTITY_VEHICLE,nv.netId,ENTITY_PED,m.ped)||Net().EntityGeneration(ENTITY_PED,m.ped)!=m.generation||
           !Peds::Instance().CrewMemberReady(m,nv.veh))return false;
    }
    return true;
}

void Vehicles::RestoreHeldBoat(NetVehicle&nv){
    if(nv.heldBoatStatus<0)return;
    if(Alive(nv)&&nv.handle==nv.heldBoatHandle&&nv.veh->Status()==STATUS_PHYSICS)nv.veh->SetStatus((uint8_t)nv.heldBoatStatus);
    nv.heldBoatStatus=nv.heldBoatHandle=-1;
}

void Vehicles::ApplyStaging(NetVehicle&nv){
    if(!Alive(nv))return;
    if(nv.crewStaged||nv.authorityHeld){
        if(nv.crewStaged)nv.veh->Flags()&=~128u;
        if(nv.authorityHeld&&nv.heldBoatStatus<0&&nv.veh->VehicleSubClass()==VEHICLE_TYPE_BOAT&&nv.veh->Status()==STATUS_PLAYER){
            nv.heldBoatStatus=STATUS_PLAYER;nv.heldBoatHandle=nv.handle;nv.veh->SetStatus(STATUS_PHYSICS);
        }
        nv.veh->SetEngineOn(false);
        HoldUnloadedPhysics(nv,nv.terrainHeld);
    }
}

void Vehicles::UpdateCrewGate(NetVehicle&nv){
    if(!Alive(nv))return;
    const auto&crew=nv.hasState?nv.state.crew:nv.birthCrew;
    const bool wait=IsMirror(nv)&&crew.count&&!CrewReady(nv,crew);
    if(wait!=nv.crewStaged){nv.crewStaged=wait;HoldUnloadedPhysics(nv,nv.terrainHeld);
        if(!wait&&nv.hasState)nv.veh->Flags()=(nv.veh->Flags()&~128u)|((nv.state.flags&VehicleState::F_HIDDEN)?0u:128u);}
    Peds::Instance().SetCrewVisibility(nv.stagedCrew,false);nv.stagedCrew={};
    if(wait){nv.stagedCrew=crew;Peds::Instance().SetCrewVisibility(nv.stagedCrew,true);}
    ApplyStaging(nv);
}

VehicleState Vehicles::Capture(CVehicle* vehicle) {
    auto state=VehicleState::Capture(vehicle);
    state.crew=CaptureCrew(vehicle);
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
    n->scriptAttachment=state;{n->history.Clear();ClearVehicleDiagnosticHistory(*n);}
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
    prior=state;{n->history.Clear();ClearVehicleDiagnosticHistory(*n);}
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

bool Vehicles::RequestCrewAuthority(NetVehicle&nv,uint8_t target,bool voluntary){
    if(!Alive(nv)||nv.awaitingLookup||Peds::Instance().HasUnresolvedCrew(nv.veh)||!Net().EntityLive(ENTITY_VEHICLE,nv.netId)||!Net().EntityGeneration(ENTITY_VEHICLE,nv.netId))return false;
    std::vector<authority::Member> members;
    authority::Member car;car.ref={ENTITY_VEHICLE,255,nv.netId,Net().EntityGeneration(ENTITY_VEHICLE,nv.netId),0};car.seat=authority::VehicleSeat;members.push_back(car);
    // A claimant already occupies the driver slot locally. The old simulator's
    // explicit accepted crew is needed for Stop, not that contested local view.
    auto crew=voluntary?CaptureCrew(nv.veh):(nv.hasState?nv.state.crew:nv.birthCrew);
    for(unsigned i=0;i<crew.count;++i){auto&m=crew.members[i];authority::Member ped;
        ped.ref={ENTITY_PED,255,m.ped,m.generation,0};ped.seat=m.seat;
        const auto owner=Net().AuthorityOwner(ENTITY_PED,m.ped,m.generation);
        auto*np=Peds::Instance().DiagnosticRecord(m.ped);auto*body=np?GetPed(np->handle):nullptr;
        const bool nativeScript=np&&(np->owner==Net().MyId()?body&&body->CreatedBy()!=PED_GAME:(np->state.entityFlags&16)!=0);
        ped.transfer=owner==nv.owner&&!nativeScript;
        if(ped.transfer&&voluntary){auto*p=GetPed(Peds::Instance().HandleOf(m.ped));
            if(!p||p->CreatedBy()!=PED_GAME){LOGW("Vehicle %04X authority waits: native script crew %04X",nv.netId,m.ped);return false;}}
        // A foreign story passenger remains its owner's mirror. A foreign
        // driver cannot safely become this simulator's autonomous driver.
        if(m.seat==-1&&!ped.transfer){LOGW("Vehicle %04X authority waits: cross-owner NPC driver %04X",nv.netId,m.ped);return false;}
        members.push_back(ped);
    }
    return Net().RequestAuthority(std::move(members),target,voluntary);
}

bool Vehicles::RetainedSource(CVehicle*car,uint16_t&id,uint64_t&generation)const{
    if(!IsVehicleValid(car))return false;
    auto p=m_byPtr.find(car);if(p!=m_byPtr.end())if(auto i=m_byId.find(p->second);i!=m_byId.end()&&Alive(i->second)&&i->second.owner==Net().MyId()&&i->second.playerUsed){id=i->first;generation=Net().EntityGeneration(ENTITY_VEHICLE,id);return generation!=0;}
    for(const auto&[savedId,saved]:m_retainedCopies)if(saved.localOrigin&&saved.handle>=0&&GetVehicle(saved.handle)==car&&car->ModelIndex()==saved.model){id=savedId;generation=saved.generation;return true;}
    return false;
}

bool Vehicles::HandleAuthority(authority::Notice&n){
    ObserveStoryAssignment(n);
    using Op=authority::ServerOp;bool ready=true;
    if(n.op==Op::Lookup){
        auto car=std::find_if(n.members.begin(),n.members.end(),[](const authority::Member&m){return m.ref.kind==ENTITY_VEHICLE;});
        if(car==n.members.end())return true;const auto&m=*car;
        auto saved=m_retainedCopies.find(m.ref.id);auto row=m_byId.find(m.ref.id);
        if(saved!=m_retainedCopies.end()&&saved->second.localOrigin&&saved->second.generation!=m.ref.generation)return false;
        if(n.reason==authority::Reason::Missing||n.reason==authority::Reason::Retired){
            if(saved==m_retainedCopies.end()||!saved->second.localOrigin)return false;
            const auto generation=Net().EntityGeneration(ENTITY_VEHICLE,m.ref.id);
            if(row!=m_byId.end()&&(!generation||generation==m.ref.generation)){
                auto&nv=row->second;
                if(Alive(nv)&&saved->second.handle==nv.handle){const auto nativeState=nv.authoritySavedKnown?nv.authoritySaved:saved->second.state;
                    nv.authorityHeld=nv.crewStaged=false;RestoreHeldBoat(nv);nv.veh->Flags()=(nv.veh->Flags()&~128u)|((nativeState.flags&VehicleState::F_HIDDEN)?0u:128u);
                    nv.veh->SetEngineOn((nativeState.flags&VehicleState::F_ENGINE)!=0);HoldUnloadedPhysics(nv,false);m_byPtr.erase(nv.veh);}
                m_byId.erase(row);m_pending.erase(m.ref.id);m_pendingStates.erase(m.ref.id);
            }
            m_retainedCopies.erase(saved);return true;
        }
        if(n.reason!=authority::Reason::Ok||Net().EntityGeneration(ENTITY_VEHICLE,m.ref.id)!=m.ref.generation||row==m_byId.end())return false;
        auto&nv=row->second;
        if(saved!=m_retainedCopies.end()&&saved->second.handle>=0){auto*original=GetVehicle(saved->second.handle);
            if(IsVehicleValid(original)&&original->ModelIndex()==saved->second.model&&(!Alive(nv)||nv.veh!=original||saved->second.handle!=nv.handle))return false;
        }
        if(nv.owner==m.owner&&m.owner!=authority::NoOwner&&!nv.awaitingLookup&&!nv.lookupAwaitingState&&!nv.authorityHeld){if(saved!=m_retainedCopies.end())m_retainedCopies.erase(saved);return true;}
        nv.owner=m.owner;nv.awaitingLookup=false;nv.birthCrew={};
        for(const auto&p:n.members)if(p.ref.kind==ENTITY_PED&&p.seat>=-1&&nv.birthCrew.count<nv.birthCrew.members.size())nv.birthCrew.members[nv.birthCrew.count++]={p.ref.id,p.ref.generation,p.seat};
        nv.lookupAwaitingState=m.owner!=authority::NoOwner&&m.owner!=Net().MyId();nv.lookupOwnedReady=m.owner==Net().MyId();nv.authorityHeld=true;
        if(nv.lookupAwaitingState)nv.hasState=false;
        else{nv.state=nv.authoritySavedKnown?nv.authoritySaved:saved!=m_retainedCopies.end()?saved->second.state:VehicleState{};nv.state.crew=nv.birthCrew;nv.hasState=true;}
        if(!Alive(nv)){
            if(saved==m_retainedCopies.end())return false;
            auto spawn=saved->second.spawn;spawn.owner=m.owner;spawn.crew=nv.birthCrew;m_pending[m.ref.id]=spawn;
            if(m.owner==authority::NoOwner||m.owner==Net().MyId())CreateMirror(spawn);
        }
        auto*body=Find(m.ref.id);
        if(body&&body->lookupOwnedReady)ActivateRetainedOwned(*body);
        if(body&&m.owner==authority::NoOwner&&!Peds::Instance().HasUnresolvedCrew(body->veh)&&!Net().AuthorityStopped(ENTITY_VEHICLE,m.ref.id)){
            body->orphanAttemptLease=m.lease;
            if(RequestCrewAuthority(*body,Net().MyId(),false))body->claimEpisode=true;
        }
        saved=m_retainedCopies.find(m.ref.id);if(body&&saved!=m_retainedCopies.end()&&!saved->second.worldLost)m_retainedCopies.erase(saved);
        return true;
    }
    for(const auto&m:n.members){if(m.ref.kind!=ENTITY_VEHICLE)continue;
        if(Net().EntityGeneration(ENTITY_VEHICLE,m.ref.id)!=m.ref.generation)return false;
        auto*nv=Find(m.ref.id);
        if(!nv){
            auto logical=m_byId.find(m.ref.id);
            if(logical!=m_byId.end()&&!logical->second.veh&&Net().EntityGeneration(ENTITY_VEHICLE,m.ref.id)==m.ref.generation&&
               (n.op==Op::Commit||n.op==Op::Abort||n.op==Op::Reject)){
                auto&row=logical->second;row.owner=m.owner;row.awaitingLookup=row.authorityHeld=true;row.lookupAwaitingState=false;
                m_pending.erase(m.ref.id);m_pendingStates.erase(m.ref.id);
                Net().LookupAuthority({ENTITY_VEHICLE,255,m.ref.id,m.ref.generation,0});continue;
            }
            auto p=m_pending.find(m.ref.id);
            if(p!=m_pending.end()&&(n.op==Op::Commit||n.op==Op::Abort||n.op==Op::Reject))p->second.owner=m.owner;
            if(n.op==Op::Commit&&n.transaction==0)continue;return false;
        }
        if(n.op==Op::Stop){
            if(m.transfer&&(n.owner==Net().MyId()||n.source==Net().MyId())){
                if(!nv->authorityHeld&&!nv->authoritySavedKnown){nv->authoritySaved=Capture(nv->veh);nv->authoritySavedKnown=true;}
                if(n.owner==Net().MyId()&&n.source==n.target)SendState(*nv,true);
                nv->authorityHeld=true;ApplyStaging(*nv);}

        }else if(n.op==Op::Prepare){
            if(m.transfer&&(n.owner==Net().MyId()||n.target==Net().MyId())){
                if(!nv->authorityHeld&&!nv->authoritySavedKnown){nv->authoritySaved=nv->hasState?nv->state:Capture(nv->veh);nv->authoritySavedKnown=true;}
                nv->authorityHeld=true;ApplyStaging(*nv);}

            if(n.target==Net().MyId()){
                for(const auto&p:n.members)if(p.ref.kind==ENTITY_PED){VehicleCrewMember c{p.ref.id,p.ref.generation,p.seat};
                    if(!Net().EntityLink(ENTITY_VEHICLE,nv->netId,ENTITY_PED,p.ref.id)||!Peds::Instance().CrewMemberReady(c,nv->veh))ready=false;}
                if(n.source==n.target&&n.owner!=authority::NoOwner){auto*me=FindPlayerPed(0);
                    if(!me||me->VehicleIfIn()!=nv->veh||nv->veh->Driver()!=me)ready=false;}
            }
        }else{
            const bool held=nv->authorityHeld;const auto old=nv->owner;
            if(n.op==Op::Reject&&n.source==Net().MyId())nv->claimRejected=n.reason==authority::Reason::Group;
            nv->owner=m.owner;nv->authorityHeld=false;
            RestoreHeldBoat(*nv);
            if(held&&nv->authoritySavedKnown){const auto&saved=nv->authoritySaved;
                nv->veh->Flags()=(nv->veh->Flags()&~128u)|((saved.flags&VehicleState::F_HIDDEN)?0u:128u);
                nv->veh->SetEngineOn((saved.flags&VehicleState::F_ENGINE)!=0);
            }
            if(n.transaction||old!=nv->owner){if(nv->hasState)nv->birthCrew=nv->state.crew;nv->hasState=false;nv->history.Clear();ClearVehicleDiagnosticHistory(*nv);RetireVehicleStateDiagnostic(*nv,VD_OWNER);}
            if(nv->owner==Net().MyId()){
                const bool terrain=nv->terrainHeld;Release(*nv);HoldUnloadedPhysics(*nv,terrain);
                if(held&&nv->authoritySavedKnown){auto&s=nv->authoritySaved;
                    nv->veh->SetEngineOn((s.flags&VehicleState::F_ENGINE)!=0);
                    nv->veh->GasPedal()=s.gas;nv->veh->BrakePedal()=s.brake;nv->veh->SetHandbrake((s.flags&VehicleState::F_HANDBRAKE)!=0);}
                nv->authoritySavedKnown=false;
                nv->handoverUntil=NowMs()+3000;nv->firstAnnouncedMs=NowMs();
                if(held&&n.op!=Op::Reject){
                    if(!nv->script)if(auto*d=nv->veh->Driver();d&&!d->IsPlayer()&&d->VehicleIfIn()==nv->veh&&d->Health()>0){
                        Cmd(0x00A8).I(nv->handle).Run();Cmd(0x00AD).I(nv->handle).F(12.f).Run();Cmd(0x00AE).I(nv->handle).I(0).Run();}
                    Announce(*nv);SendLook(*nv);
                }
            }else{Protect(*nv);HoldUnloadedPhysics(*nv,false);
                if(nv->owner==authority::NoOwner){
                    if(!nv->authoritySavedKnown){nv->authoritySaved=Capture(nv->veh);nv->authoritySavedKnown=true;}
                    nv->authorityHeld=true;ApplyStaging(*nv);
                }
            }
            UpdateCrewGate(*nv);
        }
    }
    return ready;
}

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
    if(Net().Online()&&!flow::InSharedWorld())return nullptr;
    if (!v || !IsVehicleValid(v) || !IsSyncableVehicle(v)) return nullptr;
    if(Peds::Instance().HasUnresolvedCrew(v))return nullptr;
    if (NetVehicle* nv = Find(v)) return nv->awaitingLookup?nullptr:nv;
    // A retained foreign copy must not be announced under a new local ID
    // while the original birth is being received during a quick reconnect.
    for (auto it=m_retainedCopies.begin();it!=m_retainedCopies.end();++it) {
        const auto id=it->first;const auto saved=it->second;
        if (saved.handle!=GetVehicleRef(v) || saved.model!=v->ModelIndex()) continue;
        if(saved.localOrigin)return nullptr; // Only a current explicit Lookup may resolve this saved birth.
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
    uint16_t id=m_ids.Take((uint16_t)((Net().MyId()+1)<<12),NowMs(),[&](uint16_t v){return m_byId.count(v)||m_pending.count(v)||m_scriptRemoved.count(v)||m_retainedCopies.count(v);});
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

void Vehicles::Announce(NetVehicle& nv) {
    if(nv.authorityHeld)return;
    CVehicle* v = nv.veh;
    VehicleSpawn sp;
    sp.crew=CaptureCrew(v);
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
    // Birth and its first dynamic state share the reliable channel. An idle
    // aircraft must not wait for the next lossy/500ms snapshot to stop its spool.
    if(nv.owner==Net().MyId())SendState(nv,true);
}

void Vehicles::ReannounceOne(uint16_t id) { if(auto*n=Find(id);n&&!IsMirror(*n)){Announce(*n);SendLook(*n);} }

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

void Vehicles::TakeOwnership(NetVehicle&nv){
    if(nv.awaitingLookup||nv.lookupAwaitingState||nv.owner==Net().MyId()||nv.claimEpisode||(NowMs()<nv.jackUntil&&nv.jackDriver!=Net().MyId()))return;
    if(RequestCrewAuthority(nv,Net().MyId(),false))nv.claimEpisode=true;
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
        RecordPendingDiagnostic(sp.netId,VD_MODEL_WAIT);
        return nullptr;
    }
    if (!HasRoom(VehiclePool(), kKeepVehicles, "vehicle")){m_spawnWait[sp.netId].Block("vehicle",sp.netId,"vehicle_pool_full",m_spawnWait[sp.netId].priority);RecordPendingDiagnostic(sp.netId,VD_POOL_WAIT);return nullptr;}
    int handle = -1;
    const auto savedComponents=Ref<uint16_t>(0x8A6458);
    for(int c=0;c<2;++c)Ref<int8_t>(0x8A6458+c)=sp.components[c];
    Cmd(0x00A5).I(sp.model).F(sp.pos.x).F(sp.pos.y).F(sp.pos.z).Out(&handle).Run();
    Ref<uint16_t>(0x8A6458)=savedComponents;  // CREATE_CAR
    CVehicle* v = GetVehicle(handle);
    if (!v){m_spawnWait[sp.netId].Block("vehicle",sp.netId,"native_creation_failed");RecordPendingDiagnostic(sp.netId,VD_NATIVE_FAILED);return nullptr;}
    SetVehicleMatrix(v, sp.pos, sp.rot);
    memcpy(v->Colors(), sp.colors, 4);
    v->Health() = sp.health;
    v->AreaCode() = sp.area;
    // a random car locked there does not lock the other player out; a mission's locked car stays shut for him too
    v->DoorLock() = sp.lockState;
    return v;
}

void Vehicles::RecordPendingDiagnostic(uint16_t id,uint16_t reason,const flight::Context* diagnostic){
    auto it=m_pendingDiagnostics.find(id);if(it==m_pendingDiagnostics.end()||it->second.waitReason==reason)return;
    it->second.waitReason=reason;auto pending=m_pending.find(id);
    RecordVehicleDiagnostic(diagnostic?*diagnostic:it->second.spawn,624,reason,nullptr,0,pending==m_pending.end()?255:pending->second.owner);
}
void Vehicles::RetirePendingDiagnostic(uint16_t id,uint16_t reason){
    auto d=m_pendingDiagnostics.find(id);if(d==m_pendingDiagnostics.end())return;
    RecordVehicleDiagnostic(d->second.spawn,624,reason);RecordVehicleDiagnostic(d->second.state,624,reason);
}

void Vehicles::HandleCreate(uint8_t from, Reader& r) {
    VehicleSpawn sp;
    const bool decoded=sp.Read(r);
    auto diagnostic=IncomingVehicleDiagnostic(sp.netId,proto::G_VEH_CREATE);
    if (!decoded || from == Net().MyId() || sp.owner != from || m_scriptRemoved.count(sp.netId)) {RecordVehicleDiagnostic(diagnostic,624,!decoded?VD_MALFORMED:VD_SENDER);return;}
    if (!core::IsPlayerReady()) {RecordVehicleDiagnostic(diagnostic,624,VD_NOT_READY);return;}
    if (NetVehicle* existing = Find(sp.netId)) {
        if (existing->owner != from || sp.owner != from || existing->model!=sp.model) {RecordVehicleDiagnostic(diagnostic,624,VD_CONFLICT,existing);return;}
        existing->playerUsed = existing->playerUsed || sp.playerUsed;
        existing->script = sp.script;
        RecordVehicleDiagnostic(diagnostic,621,VD_RETAINED,existing);
        return;
    }
    if (!IsVehicleModel(sp.model)) {
        LOGW("Vehicle %04X has an unknown model %d", sp.netId, sp.model);
        RecordVehicleDiagnostic(diagnostic,624,VD_MODEL);
        return;
    }
    m_pending[sp.netId] = sp;
    if(diagnostic.msg){auto&d=m_pendingDiagnostics[sp.netId];if(d.spawn.msg)RecordVehicleDiagnostic(d.spawn,624,VD_REPLACED);d.spawn=diagnostic;d.waitReason=0;}
    else if(auto i=m_pendingDiagnostics.find(sp.netId);i!=m_pendingDiagnostics.end())i->second.spawn={};
    RecordPendingDiagnostic(sp.netId,VD_PENDING);
}

bool Vehicles::CreateMirror(const VehicleSpawn& sp) {
    if (Find(sp.netId)) return true;  // announced again meanwhile and made already
    if(auto row=m_byId.find(sp.netId);row!=m_byId.end()){
        if(row->second.awaitingLookup)return false;
        if(row->second.lookupAwaitingState){
            auto state=m_pendingStates.find(sp.netId);if(state==m_pendingStates.end()||state->second.first!=sp.owner)return false;
        }
    }
    bool adopted = false;
    bool retained = false;
    CVehicle* v = nullptr;
    CVehicle* excluded = nullptr;
    bool reconstruct=false;
    if (auto saved=m_retainedCopies.find(sp.netId); saved!=m_retainedCopies.end()) {
        auto* candidate=saved->second.handle>=0?GetVehicle(saved->second.handle):nullptr;
        reconstruct=saved->second.localOrigin&&saved->second.generation==Net().EntityGeneration(ENTITY_VEHICLE,sp.netId)&&
            (saved->second.worldLost||!IsVehicleValid(candidate)||candidate->ModelIndex()!=saved->second.model);
        if (saved->second.generation && saved->second.generation==Net().EntityGeneration(ENTITY_VEHICLE,sp.netId) &&
            saved->second.model==sp.model && IsVehicleValid(candidate) && candidate->ModelIndex()==sp.model && !Find(candidate)) {
            v=candidate;retained=true;
        } else excluded=candidate;
        // Keep an unmatched live copy quarantined until it is safe to delete;
        // otherwise the traffic scanner can reannounce it under a new ID.
        if (!reconstruct&&(retained || !IsVehicleValid(candidate) || candidate->ModelIndex()!=saved->second.model))
            m_retainedCopies.erase(saved);
    }
    if (!v&&!reconstruct) v = FindAdoptable(sp.model, sp.pos, excluded);
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
        auto pose=sp;
        if(auto latest=m_pendingStates.find(sp.netId);latest!=m_pendingStates.end()&&latest->second.first==sp.owner){
            pose.pos=latest->second.second.pos;pose.rot=latest->second.second.rot;pose.health=latest->second.second.health;pose.area=latest->second.second.area;
        }
        v = SpawnMirror(pose);
    }
    if (!v) return false;
    if(reconstruct)m_retainedCopies.erase(sp.netId);
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
    nv.birthCrew=sp.crew;
    if (adopted) Protect(nv);
    m_byPtr[v] = sp.netId;
    if(auto state=m_pendingStates.find(sp.netId);state!=m_pendingStates.end()){
        flight::Context deferred{};if(auto d=m_pendingDiagnostics.find(sp.netId);d!=m_pendingDiagnostics.end())deferred=d->second.state;
        auto latest=state->second;m_pendingStates.erase(state);ApplyDriverState(latest.first,sp.netId,latest.second,&deferred);
    }
    if(auto look=m_pendingLooks.find(sp.netId);look!=m_pendingLooks.end()){
        nv.look=look->second.first;nv.repairRevision=look->second.second;nv.hasLook=true;m_pendingLooks.erase(look);ApplyLook(nv);
    }
    UpdateCrewGate(nv);
    m_adoptPending.erase(sp.netId); // Canonical Prepare/Commit owns activation.

    LOGI("Vehicle %04X (model %d) %s for player %d", sp.netId, sp.model, adopted ? "adopted" : "created", sp.owner);
    if(auto d=m_pendingDiagnostics.find(sp.netId);d!=m_pendingDiagnostics.end())RecordVehicleDiagnostic(d->second.spawn,621,adopted?VD_RETAINED:VD_OK,&nv);
    return true;
}

void Vehicles::Forget(uint16_t netId, bool deleteVehicle) {
    auto it = m_byId.find(netId);
    if (it == m_byId.end()) return;
    NetVehicle& nv = it->second;
    RetireVehicleStateDiagnostic(nv,VD_CANCELLED);
    auto diagnostic=nv.diagnosticRemove.msg?nv.diagnosticRemove:LocalVehicleDiagnostic(netId,proto::G_VEH_REMOVE);
    const bool diagnosticAlive=diagnostic.msg&&Alive(nv);
    const float diagnosticBefore=diagnosticAlive?nv.veh->Health():0;
    const auto diagnosticHandle=nv.handle;auto*diagnosticVehicle=nv.veh;const auto diagnosticOwner=nv.owner;
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
    RecordVehicleRemoval(diagnostic,diagnosticOwner,diagnosticHandle,diagnosticVehicle,diagnosticAlive,diagnosticBefore,deleteVehicle);
    if(auto ptr=m_byPtr.find(nv.veh);ptr!=m_byPtr.end()&&ptr->second==netId)m_byPtr.erase(ptr);
    m_pendingLooks.erase(netId);
    m_ids.Retire(netId,NowMs());
    RetireVehicleDiagnosticAttempt(it->second,VT_CANCELLED);
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
    nv.terrainHeld=hold;hold=hold||nv.crewStaged||nv.authorityHeld;
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
    for(const auto&[id,saved]:m_retainedCopies)if(saved.localOrigin&&saved.generation&&std::find(ids.begin(),ids.end(),id)==ids.end())ids.push_back(id);
    return ids;
}

void Vehicles::RestorePlayerCar(CVehicle* v, const VehicleSpawn& sp) {
    // Save reload cannot invent a new birth for an existing canonical tuple.
    if(m_retainedCopies.count(sp.netId))return;
    if(!Net().EntityLive(ENTITY_VEHICLE,sp.netId)||!Net().AuthorityLease(ENTITY_VEHICLE,sp.netId)||Net().AuthorityOwner(ENTITY_VEHICLE,sp.netId)!=Net().MyId())return;
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
            m_pendingStates.erase(it->first);m_pendingLooks.erase(it->first);m_adoptPending.erase(it->first);RetirePendingDiagnostic(it->first,VD_CANCELLED);m_pendingDiagnostics.erase(it->first);it=m_pending.erase(it);
        }else ++it;
    }
    for (uint16_t vid : ids) Drop(vid);
}

void Vehicles::HandleRemove(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    bool destroyed=r.Remaining()?r.Bool():false;
    auto diagnostic=IncomingVehicleDiagnostic(id,proto::G_VEH_REMOVE);
    if (!r.Ok()) {RecordVehicleDiagnostic(diagnostic,624,VD_MALFORMED);return;}
    if(auto pending=m_pending.find(id);pending!=m_pending.end()&&pending->second.owner==from){
        RetirePendingDiagnostic(id,VD_CANCELLED);m_pending.erase(pending);m_pendingDiagnostics.erase(id);m_pendingStates.erase(id);m_pendingLooks.erase(id);m_adoptPending.erase(id);
    }
    NetVehicle* nv = Find(id);
    if (!nv || nv->owner != from) {RecordVehicleDiagnostic(diagnostic,624,!nv?VD_NATIVE_MISSING:VD_OWNER,nv);return;}
    nv->diagnosticRemove=diagnostic;
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

void Vehicles::GiveAway(NetVehicle&nv,uint8_t to){
    if(to==Net().MyId()||!Net().Peer(to)||!Alive(nv)||nv.authorityHeld)return;
    // Publish the actual current seat relation before the stopped group request.
    Peds::Instance().PublishCrew(CaptureCrew(nv.veh));Announce(nv);SendLook(nv);
    RequestCrewAuthority(nv,to,true);
}

bool Vehicles::StoryRemoteReady(const NetVehicle&n,uint8_t player)const{
    const auto generation=Net().EntityGeneration(ENTITY_VEHICLE,n.netId);
    return Alive(n)&&generation&&Net().EntityLive(ENTITY_VEHICLE,n.netId)&&n.owner==player&&
        Net().AuthorityOwner(ENTITY_VEHICLE,n.netId,generation)==player&&Net().AuthorityLease(ENTITY_VEHICLE,n.netId,generation)&&
        !n.authorityHeld&&!n.crewStaged&&!n.awaitingLookup&&!n.lookupAwaitingState;
}

bool Vehicles::AssignStoryRemote(CVehicle*v,uint8_t to){
    auto*n=Find(v);StoryRemoteRole role;
    if(!Net().IsHost()||!n||!Alive(*n)||!Net().Peer(to)||!IsCooperativeRemoteVehicle(v,to)||HasPlayerInside(v)||
       !CooperativeRemoteRole(to,role)||role.car!=v||role.vehicle!=n->netId||!role.run||!role.assignment||
       role.vehicleGeneration!=Net().EntityGeneration(ENTITY_VEHICLE,n->netId))return false;
    if(StoryRemoteReady(*n,to))return true;
    if(n->owner!=Net().MyId())return false;
    auto i=m_storyAssignments.find(n->netId);
    if(i!=m_storyAssignments.end()){
        const auto&a=i->second;
        return a.body==v&&a.handle==n->handle&&a.generation==role.vehicleGeneration&&a.target==to&&
            a.run==role.run&&a.phase==role.phase&&a.assignment==role.assignment&&a.connection==Net().MyIncarnation()&&a.world==Net().WorldEpoch();
    }
    StoryAssignment a;a.body=v;a.handle=n->handle;a.generation=role.vehicleGeneration;a.source=role.source;
    a.sourceGeneration=role.sourceGeneration;a.run=role.run;a.phase=role.phase;a.assignment=role.assignment;
    a.connection=Net().MyIncarnation();a.world=Net().WorldEpoch();a.target=to;
    m_storyAssignments.emplace(n->netId,a);
    // CreateRole owns this native car. Pending authority is not a failed role.
    return true;
}

void Vehicles::ObserveStoryAssignment(const authority::Notice&n){
    using Op=authority::ServerOp;
    if(n.source!=Net().MyId()||!n.request)return; // A birth grant has no assignment request.
    for(const auto&m:n.members){
        if(m.ref.kind!=ENTITY_VEHICLE)continue;
        auto i=m_storyAssignments.find(m.ref.id);if(i==m_storyAssignments.end())continue;auto&a=i->second;
        auto*v=Find(m.ref.id);
        if(!a.requested||a.generation!=m.ref.generation||a.target!=n.target||a.connection!=Net().MyIncarnation()||a.world!=Net().WorldEpoch()||
           !v||v->handle!=a.handle||v->veh!=a.body||!Alive(*v))continue;
        if(n.op==Op::Stop&&!n.transaction&&!a.request)a.request=n.request;
        if(!a.request||a.request!=n.request)continue;
        if(n.op==Op::Commit&&n.transaction&&m.owner==a.target)a.committed=true;
        else if(n.op==Op::Abort||n.op==Op::Reject)a.failed=true;
    }
}

void Vehicles::PollStoryAssignments(){
    std::vector<StoryAssignment> failed;
    for(auto i=m_storyAssignments.begin();i!=m_storyAssignments.end();){
        const auto id=i->first;auto&a=i->second;auto*n=Find(id);StoryRemoteRole role;
        const bool current=Net().Online()&&Net().IsHost()&&a.connection==Net().MyIncarnation()&&a.world==Net().WorldEpoch()&&
            n&&Alive(*n)&&n->handle==a.handle&&n->veh==a.body&&Net().EntityGeneration(ENTITY_VEHICLE,id)==a.generation&&
            CooperativeRemoteRole(a.target,role)&&role.car==a.body&&role.vehicle==id&&role.vehicleGeneration==a.generation&&
            role.source==a.source&&role.sourceGeneration==a.sourceGeneration&&role.run==a.run&&role.phase==a.phase&&role.assignment==a.assignment;
        if(!current){i=m_storyAssignments.erase(i);continue;}
        if(a.committed&&StoryRemoteReady(*n,a.target)){i=m_storyAssignments.erase(i);continue;}
        if(!a.requested&&!a.failed){
            const auto lease=Net().AuthorityLease(ENTITY_VEHICLE,id,a.generation);
            if(lease&&!Net().AuthorityStopped(ENTITY_VEHICLE,id)){
                if(n->owner!=Net().MyId()||Net().AuthorityOwner(ENTITY_VEHICLE,id,a.generation)!=Net().MyId())a.failed=true;
                else{
                    Peds::Instance().PublishCrew(CaptureCrew(n->veh));Announce(*n);SendLook(*n);
                    a.requested=true; // The synchronous local Stop callback binds the exact request nonce.
                    if(!RequestCrewAuthority(*n,a.target,true))a.failed=true;
                }
            }
        }
        if(a.failed){failed.push_back(a);i=m_storyAssignments.erase(i);}else ++i;
    }
    // Existing role retirement owns script deletion and decline suppression; it
    // can erase m_byId, so never invoke it while iterating native records.
    for(const auto&a:failed)RetireCooperativeRemoteRole(a.target,a.run,a.phase,a.assignment);
}

bool Vehicles::QueueGiveAway(CVehicle* v, uint8_t to) {
    NetVehicle* nv = Find(v);
    if (!nv || IsMirror(*nv) || !CanGiveAway(v)) return false;
    m_giveAway[v] = to;
    return true;
}

void Vehicles::HandleHandover(uint8_t,Reader&r){
    (void)r.U16();(void)r.U8(); // Protocol46: owner changes only through S_ENTITY_AUTHORITY.
}

void Vehicles::HandleSync(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    VehicleState st;
    if (!st.Read(r)) {RecordVehicleDiagnostic(IncomingVehicleDiagnostic(id,proto::G_VEH_SYNC),624,VD_MALFORMED);return;}
    ApplyDriverState(from, id, st);
}

void Vehicles::ApplyDriverState(uint8_t from, uint16_t netId, const VehicleState& st,const flight::Context* deferredDiagnostic) {
    auto diagnostic=deferredDiagnostic?*deferredDiagnostic:IncomingVehicleDiagnostic(netId,proto::G_VEH_SYNC,proto::G_PLAYER_DRIVER);
    NetVehicle* nv = Find(netId);
    if (!nv) {
        if(auto p=m_pending.find(netId);p!=m_pending.end()&&p->second.owner==from){
            auto prior=m_pendingStates.find(netId);
            if(prior==m_pendingStates.end()||prior->second.first!=from||(int32_t)(st.time-prior->second.second.time)>=0){
                m_pendingStates[netId]={from,st};
                if(auto d=m_pendingDiagnostics.find(netId);d!=m_pendingDiagnostics.end()&&d->second.state.msg)RecordVehicleDiagnostic(d->second.state,624,VD_REPLACED);
                if(diagnostic.msg)m_pendingDiagnostics[netId].state=diagnostic;
                else if(auto d=m_pendingDiagnostics.find(netId);d!=m_pendingDiagnostics.end())d->second.state={};
                RecordPendingDiagnostic(netId,VD_PENDING,&diagnostic);
            }else RecordVehicleDiagnostic(diagnostic,624,VD_OLD_TIME);
        }
        else RecordVehicleDiagnostic(diagnostic,624,VD_NATIVE_MISSING);
        return;
    }
    // Driver snapshots never elect a simulator. Delayed old-driver states used to
    // take ownership back every frame on an observer, making a guest-to-guest jack jitter.
    if(nv->owner!=from){RecordVehicleDiagnostic(diagnostic,624,VD_OWNER,nv);return;}
    if(st.parentVehicle&&(st.parentVehicle==netId||
       st.parentGeneration!=Net().ExpectedEntityGeneration(ENTITY_VEHICLE,st.parentVehicle))){RecordVehicleDiagnostic(diagnostic,624,VD_GENERATION,nv);return;}
    if(!st.AttachmentInStory(StoryRunId(),CooperativeStoryPhase())){RecordVehicleDiagnostic(diagnostic,624,VD_STORY,nv);return;}
    const auto&claim=nv->scriptAttachment;
    if(claim.attachmentRevision&&claim.attachmentRun==StoryRunId()&&claim.attachmentPhase==CooperativeStoryPhase()&&
       !st.FollowsAttachmentClaim(claim)){RecordVehicleDiagnostic(diagnostic,624,VD_ATTACHMENT,nv);return;}
    if (nv->hasState && (int32_t)(st.time - nv->lastTime) < 0) {RecordVehicleDiagnostic(diagnostic,624,VD_OLD_TIME,nv);return;}
    if(!nv->hasState||(st.pos-nv->state.pos).Length()>30){nv->history.Clear();ClearVehicleDiagnosticHistory(*nv);}
    const bool diagnosticHistoryEmpty=nv->history.Size()==0;
    const bool diagnosticPushed=nv->history.Push(st,NowMs());
    RememberVehicleDiagnosticPoint(*nv,st,diagnostic,diagnosticPushed,diagnosticHistoryEmpty);
    nv->lastTime = st.time;
    auto crewFingerprint=[](const VehicleCrew&crew){uint64_t f=crew.count;for(unsigned i=0;i<crew.count;++i){const auto&m=crew.members[i];f=(f*1099511628211ULL)^m.ped^m.generation^uint64_t(uint8_t(m.seat));}return f;};
    const auto priorCrew=nv->hasState?crewFingerprint(nv->state.crew):crewFingerprint(nv->birthCrew);
    if(nv->claimRejected&&priorCrew!=crewFingerprint(st.crew)){nv->claimEpisode=false;nv->claimRejected=false;}
    nv->state = st;
    nv->hasState = true;
    if(nv->lookupAwaitingState){nv->lookupAwaitingState=false;nv->authorityHeld=false;nv->authoritySavedKnown=false;HoldUnloadedPhysics(*nv,false);}
    nv->recvMs = NowMs();
    if(nv->diagnosticStatePending)RecordVehicleDiagnostic(nv->diagnosticState,624,VD_REPLACED,nv);
    nv->diagnosticState=diagnostic;nv->diagnosticStatePending=diagnostic.msg!=0;
}

void Vehicles::HandleOwner(uint8_t,Reader&r){
    (void)r.U16();(void)r.U8();(void)r.U32(); // Legacy peer election cannot mutate canonical authority.
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
    const float diagnosticBefore=nv.diagnosticStatePending&&flight::Enabled()&&Alive(nv)?v->Health():0;
    UpdateCrewGate(nv);
    v->Flags()=(v->Flags()&~128u)|((nv.crewStaged||(nv.state.flags&VehicleState::F_HIDDEN))?0u:128u);
    v->AreaCode()=nv.state.area;
    if(TrainingVehicleRestricted(v))v->DoorLock()=2;
    VehicleDiagnosticSelection selection;selection.history=nv.history.Size()!=0;
    VehicleState st = selection.history?nv.history.At(NowMs(),[&](const VehicleState&a,const VehicleState&b,float t){
        selection.blended=true;selection.lower=a.time;selection.upper=b.time;selection.alpha=t;
        auto s=b;s.pos=a.pos+(b.pos-a.pos)*t;s.vel=a.vel+(b.vel-a.vel)*t;
        s.turn=a.turn+(b.turn-a.turn)*t;s.rot=Quat::Slerp(a.rot,b.rot,t);s.wheels=WheelState::Blend(a.wheels,b.wheels,t,(uint32_t)(b.time-a.time)/1000.f);return s;
    },[&](VehicleState&s,float age,bool stale){selection.predicted=true;selection.lower=selection.upper=s.time;selection.age=age;selection.stale=stale;s.pos=s.pos+s.vel*age;if(!stale)s.rot=Quat::Turned(s.rot,s.turn,age);s.wheels.Predict(age,stale);if(stale)s.vel=s.turn={};}):nv.state;
    if(!selection.blended&&!selection.predicted)selection.lower=selection.upper=st.time;
    VehicleDiagnosticGoal diagnosticGoal{};
    if(flight::Enabled())diagnosticGoal={VehicleDiagnosticAt(nv,selection.lower,selection.history),VehicleDiagnosticAt(nv,selection.upper,selection.history),selection,nv.handle,nv.veh,nv.netId,Net().EntityGeneration(ENTITY_VEHICLE,nv.netId),nv.diagnosticLatest,nv.state.flags};
    auto diagnosticApplied=[&](uint16_t reason){
        if(nv.diagnosticStatePending&&st.time==nv.state.time){RecordVehicleDiagnostic(nv.diagnosticState,620,reason,&nv,diagnosticBefore,255,&st.health);nv.diagnosticStatePending=false;}
        ObserveVehicleNative(diagnosticGoal,st,reason);
    };
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
        if(nv.diagnosticStatePending)diagnosticApplied(Alive(nv)&&nv.veh->Status()==STATUS_WRECKED?VD_WRECK:VD_NATIVE_FAILED);
        else ObserveVehicleNative(diagnosticGoal,st,VD_WRECK);
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
        v->MoveSpeed() = nv.groundless||nv.crewStaged||nv.authorityHeld ? CVector{} : st.vel * (1.0f / 50.0f);
        v->TurnSpeed() = nv.groundless||nv.crewStaged||nv.authorityHeld ? CVector{} : st.turn * (1.0f / 50.0f);
    }
    if(wreck){diagnosticApplied(VD_WRECK);return;}
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
    v->SetEngineOn(!nv.crewStaged&&!nv.authorityHeld&&(st.flags & VehicleState::F_ENGINE) != 0);
    v->PhysicalFlags()=(v->PhysicalFlags()&~0x100u)|((st.water&1)?0x100u:0);
    v->VehicleFlagsLo()=(v->VehicleFlagsLo()&~0x40000000u)|((st.water&2)?0x40000000u:0);
    v->SetSiren((st.flags & VehicleState::F_SIREN) != 0);
    int16_t alarm=Field<int16_t>(v,0x45C);
    if(alarm==0||alarm==-1)Field<uint32_t>(v,0x514)=(st.flags&VehicleState::F_HORN)?1:0;
    ApplyParts(v, st);
    ApplyHydraulics(v, st);
    if(carry)Players::Instance().CarryLocalOnVehicle(v,before);
    diagnosticApplied(VD_OK);
}

void Vehicles::OnPreProcess() {
    if (!flow::InSharedWorld() || !core::IsPlayerReady()) return;
    for(auto&[id,nv]:m_byId){auto*me=FindPlayerPed(0);if(nv.owner!=authority::NoOwner&&(!me||me->VehicleIfIn()!=nv.veh||nv.veh->Driver()!=me))nv.claimEpisode=false;ObserveScriptOwnership(nv);}
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
        if(!IsModelLoaded(sp.model))RecordPendingDiagnostic(id,VD_MODEL_WAIT);
        if(!IsModelLoaded(sp.model))RequestSharedModel(sp.model,priority>0);
        float distance=(sp.pos-focus).Length();float age=std::min(10000.f,(NowMs()-wait.since)*.05f);
        spawnOrder.push_back({distance-priority*10000.f-age,id});
    }
    std::sort(spawnOrder.begin(),spawnOrder.end());int created=0;
    for(auto[score,id]:spawnOrder){
        auto i=m_pending.find(id);if(i==m_pending.end())continue;
        if(created>=2){RecordPendingDiagnostic(id,VD_BUDGET);break;}
        if(CreateMirror(i->second)){
            m_spawnWait[id].Done("vehicle",id);m_pending.erase(i);m_pendingDiagnostics.erase(id);++created;
            if(auto*n=Find(id);n&&n->owner==authority::NoOwner&&!n->awaitingLookup&&!n->lookupAwaitingState&&!Net().AuthorityStopped(ENTITY_VEHICLE,id)){
                const auto lease=Net().AuthorityLease(ENTITY_VEHICLE,id);
                if(lease&&n->orphanAttemptLease!=lease){n->orphanAttemptLease=lease;if(RequestCrewAuthority(*n,Net().MyId(),false))n->claimEpisode=true;}
            }
        }

    }
    for(auto i=m_spawnWait.begin();i!=m_spawnWait.end();)i=!m_pending.count(i->first)?m_spawnWait.erase(i):std::next(i);
    float dt = TimeStep() / 50.0f;
    std::vector<uint16_t> dead;
    for (auto& [id, nv] : m_byId) {
        if(!nv.veh&&(nv.awaitingLookup||nv.lookupAwaitingState||(nv.authorityHeld&&nv.owner==authority::NoOwner)))continue;
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
        RetireVehicleStateDiagnostic(it->second,VD_CANCELLED);
        const auto old=it->second;
        if (!IsMirror(old)) {Writer w(proto::G_VEH_REMOVE);w.Entity(ENTITY_VEHICLE,id,ENTITY_REMOVE);w.Bool(true);Net().Send(proto::CH_RELIABLE,w);}
        if(auto ptr=m_byPtr.find(old.veh);ptr!=m_byPtr.end()&&ptr->second==id)m_byPtr.erase(ptr);
        RetireVehicleDiagnosticAttempt(it->second,VT_CANCELLED);
    m_byId.erase(it);
    }
    // Both sampled poses must be installed before native hitch geometry runs.
    for(auto&[id,nv]:m_byId)if(IsMirror(nv)&&nv.hasState&&IsVehicleValid(nv.veh))ApplyTowLinks(nv);
    // Parents may have larger ids than their children. Re-evaluate local-space
    // bindings after every sampled parent pose is installed, before physics.
    UpdateVehicleBindings();
}

void Vehicles::SendState(NetVehicle& nv,bool reliable) {
    if(nv.authorityHeld)return;
    VehicleState st = Capture(nv.veh);
    st.time = (uint32_t)NowMs();
    Writer w(proto::G_VEH_SYNC);
    w.Entity(ENTITY_VEHICLE,nv.netId,ENTITY_SNAPSHOT);
    st.Write(w);nv.sentHealth=st.health;
    passenger_trace::Publication diagnosticPublication(st.health,0,0,0);
    if(flight::Enabled()){auto&e=passenger_trace::publication;flight::AddFloat(e,flight::Metric::RequestedX,st.pos.x);flight::AddFloat(e,flight::Metric::RequestedY,st.pos.y);flight::AddFloat(e,flight::Metric::RequestedZ,st.pos.z);flight::AddFloat(e,flight::Metric::RequestedVx,st.vel.x);flight::AddFloat(e,flight::Metric::RequestedVy,st.vel.y);flight::AddFloat(e,flight::Metric::RequestedVz,st.vel.z);flight::AddUnsigned(e,flight::Metric::RequestedEngine,(st.flags&VehicleState::F_ENGINE)?1:0);if(st.extras&VehicleState::X_ROTOR)flight::AddFloat(e,flight::Metric::RequestedRotor,st.rotorSpeed);}
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
        if (!IsMirror(nv) && !nv.authorityHeld && NowMs()-nv.firstAnnouncedMs>3000 && NowMs()>=nv.handoverUntil && !nv.playerUsed && Alive(nv) && !ScriptHolds(nv.veh) && !OccupiedTowChain(nv.veh) &&
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
    if (!flow::InSharedWorld() || !core::IsPlayerReady()) return;
    PollStoryAssignments();
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
    for(auto&[id,nv]:m_byId){
        ObserveScriptOwnership(nv);
        if(Alive(nv)&&nv.owner==authority::NoOwner&&!nv.awaitingLookup&&!nv.lookupAwaitingState&&
           PopulationOwnerAt(nv.veh->GetPosition(),nv.veh->AreaCode())==Net().MyId()&&
           Net().AuthorityLease(ENTITY_VEHICLE,id)!=nv.orphanAttemptLease){
            nv.orphanAttemptLease=Net().AuthorityLease(ENTITY_VEHICLE,id);nv.awaitingLookup=true;
            Net().LookupAuthority({ENTITY_VEHICLE,255,id,Net().EntityGeneration(ENTITY_VEHICLE,id),0});
        }
    }
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
        UpdateCrewGate(nv);
        if(nv.lookupOwnedReady)ActivateRetainedOwned(nv);
        if(nv.authorityHeld){ApplyStaging(nv);continue;}
        if (IsMirror(nv)) {
            if(TrainingVehicleRestricted(nv.veh))nv.veh->DoorLock()=2;
            // Recorded mission vehicles must render the sampled pose after
            // native NPC/recording physics too. Otherwise that extra local
            // movement is snapped back at the start of the next frame.
            if(nv.script&&nv.hasState&&nv.veh->Driver()!=me)ApplyMirror(nv,TimeStep()/50.f);
            // keep the visual controls after the physics step (wheels, brake lights)
            if (nv.hasState) {
                nv.veh->SetEngineOn(!nv.crewStaged&&!nv.authorityHeld&&(nv.state.flags&VehicleState::F_ENGINE)!=0);
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
            RequestCrewAuthority(nv,Net().HostId(),true);
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
    Peds::Instance().RetainUsedCrew();
    m_storyAssignments.clear();
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
        if(!IsMirror(nv)&&nv.playerUsed&&Alive(nv))RetainUsedCar(nv);
        if(nv.authorityHeld){nv.authorityHeld=false;nv.crewStaged=false;RestoreHeldBoat(nv);if(nv.authoritySavedKnown){nv.veh->Flags()=(nv.veh->Flags()&~128u)|((nv.authoritySaved.flags&VehicleState::F_HIDDEN)?0u:128u);nv.veh->SetEngineOn((nv.authoritySaved.flags&VehicleState::F_ENGINE)!=0);}}
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
    for(auto&[id,nv]:m_byId){RetireVehicleStateDiagnostic(nv,VD_CANCELLED);RetireVehicleDiagnosticAttempt(nv,VT_CANCELLED);}
    m_byId.clear();
    m_byPtr.clear();
    for(const auto&[id,d]:m_pendingDiagnostics)RetirePendingDiagnostic(id,VD_CANCELLED);
    m_pending.clear();m_pendingDiagnostics.clear();
    m_pendingStates.clear();m_pendingLooks.clear();
    m_adoptPending.clear();
    m_scriptRemoved.clear();
}

void Vehicles::OnPeerLeave(uint8_t id){
    for(auto i=m_storyAssignments.begin();i!=m_storyAssignments.end();)i=i->second.target==id?m_storyAssignments.erase(i):std::next(i);
    m_recent.erase(id);
    for(auto it=m_giveAway.begin();it!=m_giveAway.end();)it=it->second==id?m_giveAway.erase(it):std::next(it);
    for(auto it=m_pendingStates.begin();it!=m_pendingStates.end();)it=it->second.first==id?m_pendingStates.erase(it):std::next(it);
    // The server stops/chooses/prepares the successor, including cold crew.
    // A client cannot elect itself from peer-list order after a disconnect.
}

void Vehicles::RetainUsedCar(const NetVehicle&nv){
    if(!Alive(nv))return;
    RetainedCopy saved;saved.handle=nv.handle;saved.model=nv.model;saved.generation=Net().EntityGeneration(ENTITY_VEHICLE,nv.netId);saved.localOrigin=true;
    saved.state=nv.authoritySavedKnown?nv.authoritySaved:VehicleState::Capture(nv.veh);
    saved.state.crew=Peds::Instance().RetainedVehicleCrew(nv.netId,saved.generation);
    saved.spawn.netId=nv.netId;saved.spawn.model=nv.model;saved.spawn.owner=nv.owner;saved.spawn.pos=nv.veh->GetPosition();saved.spawn.rot=saved.state.rot;
    saved.spawn.health=nv.veh->Health();saved.spawn.area=nv.veh->AreaCode();saved.spawn.script=nv.script;saved.spawn.playerUsed=true;saved.spawn.crew=saved.state.crew;
    saved.spawn.lockState=(uint8_t)nv.veh->DoorLock();for(int c=0;c<2;++c)saved.spawn.components[c]=Field<int8_t>(nv.veh,0x438+c);memcpy(saved.spawn.colors,nv.veh->Colors(),4);
    if(saved.generation)m_retainedCopies[nv.netId]=std::move(saved);
}

void Vehicles::ActivateRetainedOwned(NetVehicle&nv){
    const auto generation=Net().EntityGeneration(ENTITY_VEHICLE,nv.netId);
    if(!nv.lookupOwnedReady||!Alive(nv)||!generation||nv.owner!=Net().MyId()||Net().AuthorityOwner(ENTITY_VEHICLE,nv.netId,generation)!=Net().MyId()||
       !Net().AuthorityLease(ENTITY_VEHICLE,nv.netId,generation)||nv.awaitingLookup||nv.lookupAwaitingState||Net().AuthorityStopped(ENTITY_VEHICLE,nv.netId)||!CrewReady(nv,nv.birthCrew))return;
    nv.lookupOwnedReady=false;nv.authorityHeld=false;UpdateCrewGate(nv);
    const bool terrain=nv.terrainHeld;Release(nv);HoldUnloadedPhysics(nv,terrain);
    if(nv.authoritySavedKnown){const auto&s=nv.authoritySaved;nv.veh->Flags()=(nv.veh->Flags()&~128u)|((s.flags&VehicleState::F_HIDDEN)?0u:128u);
        nv.veh->SetEngineOn((s.flags&VehicleState::F_ENGINE)!=0);}
    nv.authoritySavedKnown=false;nv.hasState=false;Announce(nv);SendLook(nv);
}

void Vehicles::RebindRetainedCars(){
    if(!Net().Online())return;
    for(auto&[id,saved]:m_retainedCopies){
        if(!saved.localOrigin){saved.expires=NowMs()+15000;continue;}
        auto*body=saved.handle>=0?GetVehicle(saved.handle):nullptr;if(body&&body->ModelIndex()!=saved.model)body=nullptr;
        NetVehicle nv;nv.netId=id;nv.handle=body?saved.handle:-1;nv.veh=body;nv.model=saved.model;nv.owner=authority::NoOwner;nv.playerUsed=true;nv.script=saved.spawn.script;
        nv.birthCrew=saved.spawn.crew;nv.awaitingLookup=nv.authorityHeld=true;nv.authoritySaved=body?VehicleState::Capture(body):saved.state;nv.authoritySavedKnown=true;
        m_byId[id]=nv;if(body){m_byPtr[body]=id;ApplyStaging(m_byId[id]);}
        Net().LookupAuthority({ENTITY_VEHICLE,255,id,saved.generation,0});
    }
}

void Vehicles::OnSessionStart(){
    m_storyAssignments.clear();m_ids.Reset((uint32_t)NowMs());RebindRetainedCars();
}

void Vehicles::OnGameStarted(){
    m_scriptLocks.clear();m_restoreRecent.clear();
    RebindRetainedCars();UpdateRecentProtection();
}

void Vehicles::OnGameShutdown() {
    m_restoreRecent.clear();m_storyAssignments.clear();Peds::Instance().RetainUsedCrew();
    if(Net().Online())for(const auto&[id,nv]:m_byId)
        if(!IsMirror(nv)&&nv.playerUsed&&!nv.script&&Alive(nv)&&nv.veh->Health()>0&&!ScriptHolds(nv.veh))RetainUsedCar(nv);
    for(auto i=m_retainedCopies.begin();i!=m_retainedCopies.end();){
        if(!i->second.localOrigin){i=m_retainedCopies.erase(i);continue;}
        i->second.handle=-1;i->second.worldLost=true;++i;
    }
    m_lastDriven = m_kept = -1;  // a new world: the old handles mean other cars
    m_lastArea = 0;
    for(auto&[id,nv]:m_byId){RetireVehicleStateDiagnostic(nv,VD_CANCELLED);RetireVehicleDiagnosticAttempt(nv,VT_CANCELLED);}
    m_byId.clear();
    m_byPtr.clear();
    for(const auto&[id,d]:m_pendingDiagnostics)RetirePendingDiagnostic(id,VD_CANCELLED);
    m_pending.clear();m_pendingDiagnostics.clear();
    m_pendingStates.clear();m_pendingLooks.clear();
    m_adoptPending.clear();
    m_scriptRemoved.clear();
    m_giveAway.clear();
}


}  // namespace coop

namespace coop { void Vehicles::OnEntityReplaced(uint8_t kind,uint8_t,uint16_t id){if(kind!=ENTITY_VEHICLE)return;Forget(id,true);RetirePendingDiagnostic(id,VD_CANCELLED);m_pending.erase(id);m_pendingDiagnostics.erase(id);m_pendingStates.erase(id);m_pendingLooks.erase(id);m_adoptPending.erase(id);m_scriptRemoved.erase(id);m_scriptLocks.erase(id);} }
