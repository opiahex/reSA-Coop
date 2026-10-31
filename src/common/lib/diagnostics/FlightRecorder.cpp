// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#include "common/lib/diagnostics/FlightRecorder.h"
#include "common/api/protocol/Protocol.h"
#include "common/lib/diagnostics/PassengerTrace.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <locale.h>
#include <memory>
#include <new>
#include <type_traits>

namespace coop::flight {

const char* MetricName(Metric key)noexcept{switch(key){
case Metric::EntityKind:return "entity_kind";
case Metric::StateClock:return "state_clock";
case Metric::ActionRevision:return "action_revision";
case Metric::RequestedModel:return "requested_model";
case Metric::ObservedModel:return "observed_model";
case Metric::ParentKind:return "parent_kind";
case Metric::ParentId:return "parent_id";
case Metric::ParentGeneration:return "parent_generation";
case Metric::RequestedArea:return "requested_area";
case Metric::ObservedArea:return "observed_area";
case Metric::RequestedTask:return "requested_task";
case Metric::ObservedTask:return "observed_task";
case Metric::ObservedPedState:return "observed_ped_state";
case Metric::NativeFlags:return "native_flags";
case Metric::RequestedAnimBlock:return "requested_anim_block";
case Metric::RequestedAnimIndex:return "requested_anim_index";
case Metric::RequestedAnimTime:return "requested_anim_time";
case Metric::ObservedAnimBlock:return "observed_anim_block";
case Metric::ObservedAnimIndex:return "observed_anim_index";
case Metric::ObservedAnimTime:return "observed_anim_time";
case Metric::RequestedBlend:return "requested_blend";
case Metric::ObservedBlend:return "observed_blend";
case Metric::RequestedX:return "requested_x";
case Metric::RequestedY:return "requested_y";
case Metric::RequestedZ:return "requested_z";
case Metric::ObservedX:return "observed_x";
case Metric::ObservedY:return "observed_y";
case Metric::ObservedZ:return "observed_z";
case Metric::RequestedVx:return "requested_vx";
case Metric::RequestedVy:return "requested_vy";
case Metric::RequestedVz:return "requested_vz";
case Metric::ObservedVx:return "observed_vx";
case Metric::ObservedVy:return "observed_vy";
case Metric::ObservedVz:return "observed_vz";
case Metric::RequestedHeading:return "requested_heading";
case Metric::ObservedHeading:return "observed_heading";
case Metric::RequestedEngine:return "requested_engine";
case Metric::ObservedEngine:return "observed_engine";
case Metric::RequestedRotor:return "requested_rotor";
case Metric::ObservedRotor:return "observed_rotor";
case Metric::RequestedDamage:return "requested_damage";
case Metric::Weapon:return "weapon";
case Metric::ActorKind:return "actor_kind";
case Metric::ActorId:return "actor_id";
case Metric::ActorGeneration:return "actor_generation";
case Metric::TargetKind:return "target_kind";
case Metric::TargetId:return "target_id";
case Metric::TargetGeneration:return "target_generation";
case Metric::ProjectileId:return "projectile_id";
case Metric::ExplosionId:return "explosion_id";
        case Metric::ExplosionType:return "explosion_type";
        case Metric::RequestedOwner:return "requested_owner";
case Metric::ModelId:return "model_id";
case Metric::IplId:return "ipl_id";
case Metric::RequestedLoaded:return "requested_loaded";
case Metric::ObservedLoaded:return "observed_loaded";
case Metric::RequestId:return "request_id";
case Metric::CancelReason:return "cancel_reason";
        case Metric::ObservedStreamingState:return "observed_streaming_state";
        case Metric::AfterCount:return "after_count";
        case Metric::BeforeCount:return "before_count";
        case Metric::Revision:return "revision";
        case Metric::Operation:return "operation";
case Metric::ZoneId:return "zone_id";
case Metric::ZoneLeader:return "zone_leader";
case Metric::PreviousLeader:return "previous_leader";
case Metric::PopulationCount:return "population_count";
case Metric::FromRun:return "from_run";
case Metric::ToRun:return "to_run";
case Metric::FromPhase:return "from_phase";
case Metric::ToPhase:return "to_phase";
case Metric::ControlFlags:return "control_flags";
        case Metric::BeforeControlFlags:return "before_control_flags";
        case Metric::ProposedPhase:return "proposed_phase";
        case Metric::ProposedRun:return "proposed_run";
case Metric::AttachmentFlags:return "attachment_flags";
case Metric::ObservedMotionFlags:return "observed_motion_flags";
case Metric::NativeResult:return "native_result";
case Metric::TargetX:return "target_x";
case Metric::TargetY:return "target_y";
case Metric::TargetZ:return "target_z";
case Metric::RequestedDecision:return "requested_decision";
case Metric::ObservedDecision:return "observed_decision";
case Metric::ReferenceKind:return "reference_kind";
        case Metric::ObservedBrake:return "observed_brake";
        case Metric::ObservedVehicleStatus:return "observed_vehicle_status";
        case Metric::ThrottledObservations:return "throttled_observations";
        case Metric::SuppressedErrors:return "suppressed_errors";
case Metric::ObservedArmour:return "observed_armour";
case Metric::RequestedArmour:return "requested_armour";
case Metric::ObservedHealth:return "observed_health";
case Metric::RequestedHealth:return "requested_health";
case Metric::ObservedAnimSpeed:return "observed_anim_speed";
case Metric::RequestedAnimSpeed:return "requested_anim_speed";
case Metric::LayerIndex:return "layer_index";
case Metric::ObservedPedFlags:return "observed_ped_flags";
case Metric::RequestedPedFlags:return "requested_ped_flags";
case Metric::ObservedMoveState:return "observed_move_state";
case Metric::RequestedMoveState:return "requested_move_state";
        case Metric::ObservedWater:return "observed_water";
        case Metric::RequestedWater:return "requested_water";
        case Metric::RequestedBrake:return "requested_brake";
        case Metric::ObservedGas:return "observed_gas";
        case Metric::RequestedGas:return "requested_gas";
        case Metric::ObservedSteer:return "observed_steer";
        case Metric::RequestedSteer:return "requested_steer";
        case Metric::ObservedVehicleFlags:return "observed_vehicle_flags";
        case Metric::RequestedVehicleFlags:return "requested_vehicle_flags";
        case Metric::PredictionSeconds:return "prediction_seconds";
        case Metric::InterpolationAlpha:return "interpolation_alpha";
        case Metric::BracketAfterClock:return "bracket_after_clock";
        case Metric::BracketBeforeClock:return "bracket_before_clock";
case Metric::TaskParentSpan:return "task_parent_span";
case Metric::TaskRecordingToken:return "task_recording_token";
case Metric::TaskLocalId:return "task_local_id";
case Metric::TaskType:return "task_type";
case Metric::TaskOperation:return "task_operation";
case Metric::TaskSlot:return "task_slot";
case Metric::TaskReason:return "task_reason";
case Metric::TaskEventType:return "task_event_type";
case Metric::TaskAbortPriority:return "task_abort_priority";
case Metric::TaskCallerImageKind:return "task_caller_image_kind";
case Metric::TaskCallerRva:return "task_caller_rva";
case Metric::TaskSubjectKind:return "task_subject_kind";
case Metric::TaskNativePedRef:return "task_native_ped_ref";
case Metric::TaskEventPresent:return "task_event_present";
case Metric::TaskTreeSlotKind:return "task_tree_slot_kind";
case Metric::TaskTreeNode:return "task_tree_node";
case Metric::TaskTreeDepth:return "task_tree_depth";
case Metric::TaskTreeLinkState:return "task_tree_link_state";
case Metric::TaskTreeTargetNode:return "task_tree_target_node";
case Metric::TaskTreeEdgeKind:return "task_tree_edge_kind";
case Metric::TaskHistoryRole:return "task_history_role";
case Metric::TaskEventPriority:return "task_event_priority";
case Metric::TaskTreeFlags:return "task_tree_flags";
case Metric::TaskTreeComplete:return "task_tree_complete";
case Metric::TaskTreeNodeCount:return "task_tree_node_count";
case Metric::TaskTreeSimple:return "task_tree_simple";
case Metric::TaskTreeRowsAttempted:return "task_tree_rows_attempted";
case Metric::TaskTreeRowsAdmitted:return "task_tree_rows_admitted";
default:return "UNKNOWN";}}
NumberType MetricType(Metric key)noexcept{switch(key){
case Metric::EntityKind:return NumberType::Unsigned;
case Metric::StateClock:return NumberType::Unsigned;
case Metric::ActionRevision:return NumberType::Unsigned;
case Metric::RequestedModel:return NumberType::Signed;
case Metric::ObservedModel:return NumberType::Signed;
case Metric::ParentKind:return NumberType::Unsigned;
case Metric::ParentId:return NumberType::Unsigned;
case Metric::ParentGeneration:return NumberType::Unsigned;
case Metric::RequestedArea:return NumberType::Signed;
case Metric::ObservedArea:return NumberType::Signed;
case Metric::RequestedTask:return NumberType::Signed;
case Metric::ObservedTask:return NumberType::Signed;
case Metric::ObservedPedState:return NumberType::Unsigned;
case Metric::NativeFlags:return NumberType::Unsigned;
case Metric::RequestedAnimBlock:return NumberType::Unsigned;
case Metric::RequestedAnimIndex:return NumberType::Unsigned;
case Metric::RequestedAnimTime:return NumberType::Float;
case Metric::ObservedAnimBlock:return NumberType::Unsigned;
case Metric::ObservedAnimIndex:return NumberType::Unsigned;
case Metric::ObservedAnimTime:return NumberType::Float;
case Metric::RequestedBlend:return NumberType::Float;
case Metric::ObservedBlend:return NumberType::Float;
case Metric::RequestedX:return NumberType::Float;
case Metric::RequestedY:return NumberType::Float;
case Metric::RequestedZ:return NumberType::Float;
case Metric::ObservedX:return NumberType::Float;
case Metric::ObservedY:return NumberType::Float;
case Metric::ObservedZ:return NumberType::Float;
case Metric::RequestedVx:return NumberType::Float;
case Metric::RequestedVy:return NumberType::Float;
case Metric::RequestedVz:return NumberType::Float;
case Metric::ObservedVx:return NumberType::Float;
case Metric::ObservedVy:return NumberType::Float;
case Metric::ObservedVz:return NumberType::Float;
case Metric::RequestedHeading:return NumberType::Float;
case Metric::ObservedHeading:return NumberType::Float;
case Metric::RequestedEngine:return NumberType::Unsigned;
case Metric::ObservedEngine:return NumberType::Unsigned;
case Metric::RequestedRotor:return NumberType::Float;
case Metric::ObservedRotor:return NumberType::Float;
case Metric::RequestedDamage:return NumberType::Float;
case Metric::Weapon:return NumberType::Unsigned;
case Metric::ActorKind:return NumberType::Unsigned;
case Metric::ActorId:return NumberType::Unsigned;
case Metric::ActorGeneration:return NumberType::Unsigned;
case Metric::TargetKind:return NumberType::Unsigned;
case Metric::TargetId:return NumberType::Unsigned;
case Metric::TargetGeneration:return NumberType::Unsigned;
case Metric::ProjectileId:return NumberType::Unsigned;
case Metric::ExplosionId:return NumberType::Unsigned;
        case Metric::ExplosionType:return NumberType::Unsigned;
        case Metric::RequestedOwner:return NumberType::Unsigned;
case Metric::ModelId:return NumberType::Signed;
case Metric::IplId:return NumberType::Signed;
case Metric::RequestedLoaded:return NumberType::Unsigned;
case Metric::ObservedLoaded:return NumberType::Unsigned;
case Metric::RequestId:return NumberType::Unsigned;
case Metric::CancelReason:return NumberType::Unsigned;
        case Metric::ObservedStreamingState:return NumberType::Unsigned;
        case Metric::AfterCount:return NumberType::Unsigned;
        case Metric::BeforeCount:return NumberType::Unsigned;
        case Metric::Revision:return NumberType::Unsigned;
        case Metric::Operation:return NumberType::Unsigned;
case Metric::ZoneId:return NumberType::Unsigned;
case Metric::ZoneLeader:return NumberType::Unsigned;
case Metric::PreviousLeader:return NumberType::Unsigned;
case Metric::PopulationCount:return NumberType::Unsigned;
case Metric::FromRun:return NumberType::Unsigned;
case Metric::ToRun:return NumberType::Unsigned;
case Metric::FromPhase:return NumberType::Unsigned;
case Metric::ToPhase:return NumberType::Unsigned;
case Metric::ControlFlags:return NumberType::Unsigned;
        case Metric::BeforeControlFlags:return NumberType::Unsigned;
        case Metric::ProposedPhase:return NumberType::Unsigned;
        case Metric::ProposedRun:return NumberType::Unsigned;
case Metric::AttachmentFlags:return NumberType::Unsigned;
case Metric::ObservedMotionFlags:return NumberType::Unsigned;
case Metric::NativeResult:return NumberType::Signed;
case Metric::TargetX:return NumberType::Float;
case Metric::TargetY:return NumberType::Float;
case Metric::TargetZ:return NumberType::Float;
case Metric::RequestedDecision:return NumberType::Unsigned;
case Metric::ObservedDecision:return NumberType::Unsigned;
case Metric::ReferenceKind:return NumberType::Unsigned;
        case Metric::ObservedBrake:return NumberType::Float;
        case Metric::ObservedVehicleStatus:return NumberType::Unsigned;
        case Metric::ThrottledObservations:return NumberType::Unsigned;
        case Metric::SuppressedErrors:return NumberType::Unsigned;
case Metric::ObservedArmour:return NumberType::Float;
case Metric::RequestedArmour:return NumberType::Float;
case Metric::ObservedHealth:return NumberType::Float;
case Metric::RequestedHealth:return NumberType::Float;
case Metric::ObservedAnimSpeed:return NumberType::Float;
case Metric::RequestedAnimSpeed:return NumberType::Float;
case Metric::LayerIndex:return NumberType::Unsigned;
case Metric::ObservedPedFlags:return NumberType::Unsigned;
case Metric::RequestedPedFlags:return NumberType::Unsigned;
case Metric::ObservedMoveState:return NumberType::Unsigned;
case Metric::RequestedMoveState:return NumberType::Unsigned;
        case Metric::ObservedWater:return NumberType::Unsigned;
        case Metric::RequestedWater:return NumberType::Unsigned;
        case Metric::RequestedBrake:return NumberType::Float;
        case Metric::ObservedGas:return NumberType::Float;
        case Metric::RequestedGas:return NumberType::Float;
        case Metric::ObservedSteer:return NumberType::Float;
        case Metric::RequestedSteer:return NumberType::Float;
        case Metric::ObservedVehicleFlags:return NumberType::Unsigned;
        case Metric::RequestedVehicleFlags:return NumberType::Unsigned;
        case Metric::PredictionSeconds:return NumberType::Float;
        case Metric::InterpolationAlpha:return NumberType::Float;
        case Metric::BracketAfterClock:return NumberType::Unsigned;
        case Metric::BracketBeforeClock:return NumberType::Unsigned;
case Metric::TaskParentSpan:return NumberType::Unsigned;
case Metric::TaskRecordingToken:return NumberType::Unsigned;
case Metric::TaskLocalId:return NumberType::Unsigned;
case Metric::TaskType:return NumberType::Signed;
case Metric::TaskOperation:return NumberType::Unsigned;
case Metric::TaskSlot:return NumberType::Signed;
case Metric::TaskReason:return NumberType::Signed;
case Metric::TaskEventType:return NumberType::Signed;
case Metric::TaskAbortPriority:return NumberType::Signed;
case Metric::TaskCallerImageKind:return NumberType::Unsigned;
case Metric::TaskCallerRva:return NumberType::Unsigned;
case Metric::TaskSubjectKind:return NumberType::Unsigned;
case Metric::TaskNativePedRef:return NumberType::Unsigned;
case Metric::TaskEventPresent:return NumberType::Unsigned;
case Metric::TaskTreeSlotKind:return NumberType::Unsigned;
case Metric::TaskTreeNode:return NumberType::Unsigned;
case Metric::TaskTreeDepth:return NumberType::Unsigned;
case Metric::TaskTreeLinkState:return NumberType::Unsigned;
case Metric::TaskTreeTargetNode:return NumberType::Unsigned;
case Metric::TaskTreeEdgeKind:return NumberType::Unsigned;
case Metric::TaskHistoryRole:return NumberType::Unsigned;
case Metric::TaskEventPriority:return NumberType::Signed;
case Metric::TaskTreeFlags:return NumberType::Unsigned;
case Metric::TaskTreeComplete:return NumberType::Unsigned;
case Metric::TaskTreeNodeCount:return NumberType::Unsigned;
case Metric::TaskTreeSimple:return NumberType::Unsigned;
case Metric::TaskTreeRowsAttempted:return NumberType::Unsigned;
case Metric::TaskTreeRowsAdmitted:return NumberType::Unsigned;
default:return NumberType::Unavailable;}}
bool ValidNumber(const Number&n)noexcept{
    const auto expected=MetricType(n.key);
    if(expected==NumberType::Unavailable)return false;
    if(n.type==NumberType::Unavailable)return n.bits==0;
    if(n.type!=expected)return false;
    return n.type!=NumberType::Float || (!(n.bits>>32)&&((uint32_t(n.bits)&0x7f800000u)!=0x7f800000u));
}
namespace {
bool AppendNumber(Event&e,Number n,uint8_t error=0)noexcept{
    if(MetricType(n.key)==NumberType::Unavailable){e.numberFlags|=NumbersInvalid|InvalidMetric;return false;}
    if(e.numberCount>kNumbers){e.numberCount=(uint8_t)kNumbers;e.numberFlags|=NumbersInvalid|NumbersTruncated;return false;}
    for(size_t i=0;i<e.numberCount;++i)if(e.numbers[i].key==n.key){e.numberFlags|=NumbersInvalid|DuplicateNumber;return false;}
    if(e.numberCount==kNumbers){e.numberFlags|=NumbersTruncated;return false;}
    if(n.type!=MetricType(n.key)){n.type=NumberType::Unavailable;n.bits=0;error|=NumbersInvalid|InvalidNumberType;}
    if(error){n.type=NumberType::Unavailable;n.bits=0;e.numberFlags|=error;}
    e.numbers[e.numberCount++]=n;return !error;
}
void NormalizeNumbers(Event&e)noexcept{
    constexpr uint8_t known=NumbersTruncated|NumbersInvalid|DuplicateNumber|InvalidMetric|InvalidNumberType|NonFiniteNumber;
    if(e.numberFlags&~known){e.numberFlags=(e.numberFlags&known)|NumbersInvalid;}
    if(e.numberCount>kNumbers){e.numberCount=(uint8_t)kNumbers;e.numberFlags|=NumbersInvalid|NumbersTruncated;}
    for(size_t i=0;i<e.numberCount;++i){auto&n=e.numbers[i];
        if(!ValidNumber(n)){
            e.numberFlags|=NumbersInvalid;
            if(MetricType(n.key)==NumberType::Unavailable){n={Metric::None,NumberType::Unavailable,0};e.numberFlags|=InvalidMetric;}
            else{e.numberFlags|=n.type==NumberType::Float&&MetricType(n.key)==NumberType::Float&&!(n.bits>>32)&&((uint32_t(n.bits)&0x7f800000u)==0x7f800000u)?NonFiniteNumber:InvalidNumberType;n.type=NumberType::Unavailable;n.bits=0;}
        }else if(n.type==NumberType::Unavailable)e.numberFlags|=NumbersInvalid;
        for(size_t j=0;j<i;++j)if(n.key!=Metric::None&&e.numbers[j].key==n.key){n={Metric::None,NumberType::Unavailable,0};e.numberFlags|=NumbersInvalid|DuplicateNumber;break;}
    }
}
}
bool AddUnsigned(Event&e,Metric key,uint64_t value)noexcept{return AppendNumber(e,{key,NumberType::Unsigned,value});}
bool AddSigned(Event&e,Metric key,int64_t value)noexcept{return AppendNumber(e,{key,NumberType::Signed,(uint64_t)value});}
bool AddFloat(Event&e,Metric key,float value)noexcept{
    uint32_t bits=0;std::memcpy(&bits,&value,sizeof bits);
    const auto error=(bits&0x7f800000u)==0x7f800000u?uint8_t(NumbersInvalid|NonFiniteNumber):uint8_t(0);
    return AppendNumber(e,{key,NumberType::Float,bits},error);
}

namespace {
constexpr size_t kCapacity=4096,kBatch=128;
constexpr uint64_t kFileLimit=4u*1024u*1024u;
constexpr unsigned kFiles=4;
constexpr uint64_t kControlReserve=2048;
static_assert(kFileLimit>kControlReserve+512,"segment must leave room for counters");
static_assert(std::atomic<uint32_t>::is_always_lock_free,"recorder counters must not hide locks");
static_assert(std::atomic<bool>::is_always_lock_free,"recorder switch must not hide locks");
static_assert(std::atomic<SnapshotExportResult>::is_always_lock_free,"snapshot status must not hide locks");
struct Recorder {
    struct Segment {unsigned number=0;DWORD volume=0,indexHigh=0,indexLow=0;uint32_t events=0;uint64_t first=0,last=0;HANDLE handle=INVALID_HANDLE_VALUE;};
    std::atomic_flag guard=ATOMIC_FLAG_INIT;
    std::atomic_flag criticalGuard=ATOMIC_FLAG_INIT;
    struct Deferred {Event event{};uint32_t epoch=0;};
    std::array<Deferred,64> critical{};size_t criticalHead=0,criticalCount=0;
    std::atomic<uint32_t> criticalBuffered{0},criticalPending{0},criticalReserveFull{0},incompleteMetadata{0};
    uint32_t recordingEpoch=0;
    std::array<Event,kCapacity> history{};
    uint64_t total=0,frequency=0,recordingStarted=0;
    std::atomic<uint32_t> accepted{0},busyDrops{0},lifecycleDrops{0},historyOverwrites{0},drainDrops{0},ioErrors{0},epoch{0};
    std::atomic<uint32_t> segmentsEvicted{0},eventsEvicted{0},rotationFailures{0},exportFailed{0};
    std::atomic<bool> enabled{false},running{false},stopIncomplete{false};
    HANDLE writer=nullptr;
    wchar_t path[2048]{};
    _locale_t locale=nullptr;
    HANDLE file=INVALID_HANDLE_VALUE;
    uint64_t written=0;
    unsigned segment=0;
    std::array<Segment,kFiles> segments{};
    unsigned segmentCount=0;
    std::atomic<uint32_t> snapshotRequested{0},snapshotCompleted{0};
    std::atomic<SnapshotExportResult> snapshotResult{SnapshotExportResult::None};
    // Writer-owned automatic metadata export; independent of the four user snapshots.
    uint64_t sessionContextDue=0;unsigned sessionContextAttempts=0;bool sessionContextSettled=false;
    CapturePolicy policy{};
    std::atomic<uint32_t> attempted{0},filtered{0},sampledOut{0},criticalDrops{0};
    std::atomic<uint32_t> scmPreAttempted{0},scmPreSampled{0},scmPreFiltered{0},scmPreRateLimited{0},scmPreBusy{0};
    uint64_t scmWindow=0;uint32_t scmAdmitted=0; // protected by guard
    std::atomic<uint32_t> nativePreAttempted{0},nativePreFiltered{0},nativePreSampled{0},nativePreBusy{0},nativePreBudget{0};
    uint64_t nativeWindow=0;uint32_t nativeAdmitted=0;
};
static_assert(std::is_trivially_destructible_v<Recorder>,"recorder must outlive a timed-out writer during CRT exit");
Recorder g_recorder;
std::atomic<SnapshotSupplement> g_supplement{nullptr};
Recorder& Get()noexcept{return g_recorder;}
bool TryLock(Recorder&r)noexcept{return !r.guard.test_and_set(std::memory_order_acquire);}
void Unlock(Recorder&r)noexcept{r.guard.clear(std::memory_order_release);}
void Add(std::atomic<uint32_t>& counter,uint64_t n)noexcept{
    counter.fetch_add((uint32_t)std::min<uint64_t>(n,0xffffffffu),std::memory_order_relaxed);
}
void Commit(Recorder&r,Event event)noexcept{
    if(r.total>=kCapacity)r.historyOverwrites.fetch_add(1,std::memory_order_relaxed);
    event.localSeq=r.total+1;r.history[(size_t)(r.total++%kCapacity)]=event;
    r.accepted.fetch_add(1,std::memory_order_relaxed);
}
// Lock order is history -> reserve. The fallback owns reserve only and never
// tries history. No producer waits or retries either lock.
bool FlushCriticalLocked(Recorder&r)noexcept{
    if(r.criticalGuard.test_and_set(std::memory_order_acquire))return false;
    while(r.criticalCount){
        auto&entry=r.critical[r.criticalHead];
        if(entry.epoch==r.recordingEpoch)Commit(r,entry.event);
        else{r.lifecycleDrops.fetch_add(1,std::memory_order_relaxed);r.criticalDrops.fetch_add(1,std::memory_order_relaxed);}
        r.criticalHead=(r.criticalHead+1)%r.critical.size();--r.criticalCount;
    }
    r.criticalPending.store(0,std::memory_order_release);
    r.criticalGuard.clear(std::memory_order_release);return true;
}
bool DeferCritical(Recorder&r,Event event,uint32_t epoch)noexcept{
    if(r.criticalGuard.test_and_set(std::memory_order_acquire)){
        r.busyDrops.fetch_add(1,std::memory_order_relaxed);r.criticalDrops.fetch_add(1,std::memory_order_relaxed);return false;
    }
    bool queued=false;
    if(!r.enabled.load(std::memory_order_acquire)||r.epoch.load(std::memory_order_acquire)!=epoch){
        r.lifecycleDrops.fetch_add(1,std::memory_order_relaxed);r.criticalDrops.fetch_add(1,std::memory_order_relaxed);
    }else if(r.criticalCount==r.critical.size()){
        r.criticalReserveFull.fetch_add(1,std::memory_order_relaxed);r.criticalDrops.fetch_add(1,std::memory_order_relaxed);
    }else{
        r.critical[(r.criticalHead+r.criticalCount)%r.critical.size()]={event,epoch};++r.criticalCount;
        r.criticalBuffered.fetch_add(1,std::memory_order_relaxed);r.criticalPending.store((uint32_t)r.criticalCount,std::memory_order_release);queued=true;
    }
    r.criticalGuard.clear(std::memory_order_release);return queued;
}
bool WriteBytes(Recorder&r,const char*data,DWORD size){
    DWORD done=0;
    if(r.file==INVALID_HANDLE_VALUE||!WriteFile(r.file,data,size,&done,nullptr)||done!=size){
        if(r.file!=INVALID_HANDLE_VALUE){CloseHandle(r.file);if(r.segmentCount)r.segments[r.segmentCount-1].handle=INVALID_HANDLE_VALUE;r.file=INVALID_HANDLE_VALUE;}
        r.ioErrors.fetch_add(1,std::memory_order_relaxed);r.exportFailed.store(1,std::memory_order_relaxed);return false;
    }
    r.written+=size;return true;
}
bool SegmentName(const Recorder&r,unsigned index,wchar_t(&name)[2064]){
    int chars=index?swprintf(name,2064,L"%ls.%u",r.path,index):swprintf(name,2064,L"%ls",r.path);
    return chars>=0&&(size_t)chars<2064;
}
uint64_t RetainedFrom(const Recorder&r){
    for(unsigned i=0;i<r.segmentCount;++i)if(r.segments[i].events)return r.segments[i].first;
    return 0;
}
uint64_t RetainedThrough(const Recorder&r){
    for(unsigned i=r.segmentCount;i>0;--i)if(r.segments[i-1].events)return r.segments[i-1].last;
    return 0;
}
bool EvictOldest(Recorder&r){
    if(!r.segmentCount)return false;
    const auto old=r.segments[0];wchar_t name[2064];if(!SegmentName(r,old.number,name))return false;
    HANDLE h=CreateFileW(name,DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool same=GetFileInformationByHandle(h,&info)&&info.dwVolumeSerialNumber==old.volume&&
              info.nFileIndexHigh==old.indexHigh&&info.nFileIndexLow==old.indexLow;
    FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
    bool removed=same&&SetFileInformationByHandle(h,FileDispositionInfo,&disposition,sizeof disposition);
    CloseHandle(h);
    if(!removed)return false;
    if(old.handle!=INVALID_HANDLE_VALUE)CloseHandle(old.handle);
    for(unsigned i=1;i<r.segmentCount;++i)r.segments[i-1]=r.segments[i];
    --r.segmentCount;r.segmentsEvicted.fetch_add(1,std::memory_order_relaxed);Add(r.eventsEvicted,old.events);
    return true;
}
bool OpenSegment(Recorder&r,unsigned index,bool initial){
    HANDLE next=INVALID_HANDLE_VALUE;BY_HANDLE_FILE_INFORMATION info{};unsigned chosen=index;
    for(unsigned tries=0;tries<64;++tries){
        wchar_t name[2064];if(!SegmentName(r,chosen,name))break;
        next=CreateFileW(name,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_DELETE,
                         nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(next!=INVALID_HANDLE_VALUE)break;
        DWORD error=GetLastError();if(initial||(error!=ERROR_FILE_EXISTS&&error!=ERROR_ALREADY_EXISTS)||chosen==UINT32_MAX)break;
        ++chosen;
    }
    if(next==INVALID_HANDLE_VALUE){r.ioErrors.fetch_add(1,std::memory_order_relaxed);return false;}
    if(!GetFileInformationByHandle(next,&info)){CloseHandle(next);r.ioErrors.fetch_add(1,std::memory_order_relaxed);return false;}
    r.file=next;r.written=0;r.segment=chosen;
    auto&segment=r.segments[r.segmentCount++];segment={chosen,info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow,0,0,0,next};
    const auto capture=CaptureStats();
    char b[768];int n=snprintf(b,sizeof b,"{\"event\":\"flight_meta\",\"version\":1,\"qpc_frequency\":%llu,\"pid\":%u,\"recording_start_ticks\":%llu,\"capture_policy_available\":%s,\"state_sample_every\":%u,\"scm_sample_every\":%u,\"category_mask\":%u,\"segment\":%u,\"segments_evicted\":%u,\"events_evicted\":%u,\"retained_from_seq\":%llu,\"retained_through_seq\":%llu}\n",
        (unsigned long long)r.frequency,(unsigned)GetCurrentProcessId(),(unsigned long long)r.recordingStarted,capture.busy?"false":"true",capture.busy?0:capture.policy.stateEvery,capture.busy?0:capture.policy.scmEvery,capture.busy?0:capture.policy.categories,chosen,r.segmentsEvicted.load(),r.eventsEvicted.load(),(unsigned long long)RetainedFrom(r),(unsigned long long)RetainedThrough(r));
    return n>0&&WriteBytes(r,b,(DWORD)n);
}
void RotationFailed(Recorder&r){
    r.rotationFailures.fetch_add(1,std::memory_order_relaxed);r.exportFailed.store(1,std::memory_order_relaxed);
    // Reserve space before rotation so readers may see why export stopped.
    // If the device itself refuses this write, missing status means UNKNOWN.
    if(r.file!=INVALID_HANDLE_VALUE){char b[256];int n=snprintf(b,sizeof b,"{\"event\":\"flight_export_failed\",\"rotation_failures\":%u,\"segments_evicted\":%u,\"events_evicted\":%u,\"retained_from_seq\":%llu,\"retained_through_seq\":%llu}\n",
        r.rotationFailures.load(),r.segmentsEvicted.load(),r.eventsEvicted.load(),(unsigned long long)RetainedFrom(r),(unsigned long long)RetainedThrough(r));
        if(n>0&&(size_t)n<sizeof b&&r.written+(unsigned)n<=kFileLimit)WriteBytes(r,b,(DWORD)n);
        if(r.file!=INVALID_HANDLE_VALUE){CloseHandle(r.file);if(r.segmentCount)r.segments[r.segmentCount-1].handle=INVALID_HANDLE_VALUE;r.file=INVALID_HANDLE_VALUE;}}
}
bool Rotate(Recorder&r){
    if(r.segment==UINT32_MAX||(r.segmentCount==kFiles&&!EvictOldest(r))){RotationFailed(r);return false;}
    if(!OpenSegment(r,r.segment+1,false)){RotationFailed(r);return false;}
    return true;
}

int FormatNumbers(Recorder&r,const Event&input,char*out,size_t capacity){
    Event e=input;NormalizeNumbers(e);size_t used=0;bool ok=true;
    auto append=[&](const char*format,auto...args){
        if(!ok)return;int n=snprintf(out+used,capacity-used,format,args...);
        if(n<0||(size_t)n>=capacity-used){ok=false;return;}used+=(size_t)n;
    };
    append(",\"metadata_flags\":%u,\"native_values\":[",(unsigned)e.numberFlags);
    for(size_t i=0;i<e.numberCount&&ok;++i){const auto&n=e.numbers[i];
        if(i)append(",");
        append("{\"key\":%u,\"name\":\"%s\",\"type\":%u,\"value\":",(unsigned)n.key,MetricName(n.key),(unsigned)n.type);
        if(n.type==NumberType::Unsigned)append("%llu",(unsigned long long)n.bits);
        else if(n.type==NumberType::Signed){int64_t value=0;std::memcpy(&value,&n.bits,sizeof value);append("%lld",(long long)value);}
        else if(n.type==NumberType::Float){uint32_t bits=(uint32_t)n.bits;float value=0;std::memcpy(&value,&bits,sizeof value);char text[32]{};int n=_snprintf_l(text,sizeof text,"%.9g",r.locale,value);if(n<=0||(size_t)n>=sizeof text){ok=false;break;}append("%s",text);}
        else append("null");
        append("}");
    }
    append("]}\n");return ok?(int)used:-1;
}

int FormatEvent(Recorder&r,const Event&e,char(&b)[4096]){
    // Fixed stack storage on the drain thread, large enough even for decimal
    // UINT64_MAX in every field and scientific notation for every finite float.
    char f[5][32];
    const float values[]={e.health,e.maxHealth,e.armour,e.beforeHealth,e.afterHealth};
    for(size_t i=0;i<5;++i)if(std::isfinite(values[i]))_snprintf_l(f[i],sizeof f[i],"%.9g",r.locale,values[i]);else snprintf(f[i],sizeof f[i],"null");
    const auto&c=e.context;
    int n=snprintf(b,sizeof b,"{\"event\":\"flight\",\"category\":\"%s\",\"ticks\":%llu,\"local_seq\":%llu,\"pid\":%u,\"tid\":%u,\"stage\":%u,\"reason\":%u,\"msg\":%u,\"channel\":%u,\"source\":%u,\"recipient\":%u,\"local_peer\":%u,\"wire_time\":%u,\"incarnation\":%u,\"world\":%u,\"id\":%u,\"generation\":%llu,\"fingerprint\":%llu,\"motion_epoch\":%u,\"native_handle\":%u,\"health\":%s,\"max_health\":%s,\"armour\":%s,\"before_health\":%s,\"after_health\":%s,\"detail_known\":%u,\"span\":%llu,\"mission_run\":%llu,\"mission_phase\":%u,\"script_birth\":%llu,\"script_offset\":%u,\"opcode\":%u,\"owner\":%u,\"owner_epoch\":%u}\n",
        CategoryName(EventCategory(e)),(unsigned long long)e.ticks,(unsigned long long)e.localSeq,e.pid,e.tid,e.stage,e.reason,c.msg,c.channel,c.source,c.recipient,e.localPeer,c.wireTime,c.incarnation,c.world,c.entity,(unsigned long long)c.generation,(unsigned long long)c.fingerprint,e.motionEpoch,e.nativeHandle,f[0],f[1],f[2],f[3],f[4],e.detailKnown,(unsigned long long)e.span,(unsigned long long)e.missionRun,e.missionPhase,(unsigned long long)e.scriptBirth,e.scriptOffset,e.opcode,e.owner,e.ownerEpoch);
    if(n>1&&(size_t)n<sizeof b&&(e.numberCount||e.numberFlags)){
        const int extra=FormatNumbers(r,e,b+n-2,sizeof b-(size_t)(n-2));
        return extra<0?-1:n-2+extra;
    }
    return n;
}
bool Output(Recorder&r,const Event&e){
    if(r.file==INVALID_HANDLE_VALUE)return false;
    char b[4096];int n=FormatEvent(r,e,b);
    if(n<=0||(size_t)n>=sizeof b){r.ioErrors.fetch_add(1,std::memory_order_relaxed);return false;}
    if(r.written+(unsigned)n>kFileLimit-kControlReserve&&!Rotate(r))return false;
    if(!WriteBytes(r,b,(DWORD)n))return false;
    auto&segment=r.segments[r.segmentCount-1];if(!segment.events)segment.first=e.localSeq;
    segment.last=e.localSeq;++segment.events;return true;
}
void Drain(Recorder&r,uint64_t&cursor){
    std::array<Event,kBatch> batch{};size_t count=0;
    if(!TryLock(r))return;
    (void)FlushCriticalLocked(r);
    uint64_t oldest=r.total>kCapacity?r.total-kCapacity:0;
    if(cursor<oldest){Add(r.drainDrops,oldest-cursor);cursor=oldest;}
    while(cursor<r.total&&count<kBatch)batch[count++]=r.history[(size_t)(cursor++%kCapacity)];
    Unlock(r);
    for(size_t i=0;i<count;++i)if(!Output(r,batch[i]))Add(r.drainDrops,1);
}
void FinishDrain(Recorder&r,uint64_t&cursor){
    // Lifecycle worker only. Producers have crossed Stop's disabled/epoch
    // barrier. Drain the finite retained tail; never wait indefinitely for a
    // reader holding the guard. Stop still bounds/cancels blocked OS I/O.
    const auto deadline=GetTickCount64()+250;
    for(;;){
        if(TryLock(r)){const bool flushed=FlushCriticalLocked(r);const bool done=flushed&&cursor>=r.total;Unlock(r);if(done)return;}
        if(GetTickCount64()>=deadline)break;
        Drain(r,cursor);Sleep(1);
    }
    if(TryLock(r)){(void)FlushCriticalLocked(r);if(cursor<r.total){Add(r.drainDrops,r.total-cursor);cursor=r.total;}Unlock(r);}
    // The cutoff was not completely drained within its budget. In particular,
    // a reader may still own a guard, so a zero drop count is not completeness.
    r.stopIncomplete.store(true,std::memory_order_release);r.exportFailed.store(1,std::memory_order_relaxed);
}
void OutputCapturePolicy(Recorder&r){
    auto sample=CaptureStats();if(sample.busy)return;
    char b[2048];int n=snprintf(b,sizeof b,"{\"event\":\"flight_capture_policy\",\"attempted\":%u,\"filtered\":%u,\"sampled_out\":%u,\"critical_drops\":%u,\"category_mask\":%u,\"state_every\":%u,\"scm_every\":%u,\"scm_spans_per_second\":%u,\"scm_pre_attempted\":%u,\"scm_pre_sampled\":%u,\"scm_pre_filtered\":%u,\"scm_pre_rate_limited\":%u,\"scm_pre_busy\":%u,\"critical_buffered\":%u,\"critical_pending\":%u,\"critical_reserve_full\":%u,\"incomplete_metadata\":%u,\"native_observations_per_second\":%u,\"native_pre_attempted\":%u,\"native_pre_filtered\":%u,\"native_pre_sampled\":%u,\"native_pre_busy\":%u,\"native_pre_budget\":%u,\"peer_filter\":%u,\"entity_filter\":%u,\"packet_filter\":%u,\"counters_concurrent\":true}\n",sample.attempted,sample.filtered,sample.sampledOut,sample.criticalDrops,sample.policy.categories,sample.policy.stateEvery,sample.policy.scmEvery,sample.policy.scmSpansPerSecond,sample.scmPreAttempted,sample.scmPreSampled,sample.scmPreFiltered,sample.scmPreRateLimited,sample.scmPreBusy,sample.criticalBuffered,sample.criticalPending,sample.criticalReserveFull,sample.incompleteMetadata,sample.policy.nativeObservationsPerSecond,sample.nativePreAttempted,sample.nativePreFiltered,sample.nativePreSampled,sample.nativePreBusy,sample.nativePreBudget,sample.policy.peer,sample.policy.entity,sample.policy.packet);
    if(n<=0||(size_t)n>=sizeof b)return;
    if(r.written+(unsigned)n>kFileLimit-kControlReserve&&!Rotate(r))return;
    WriteBytes(r,b,(DWORD)n);
}
void OutputCounters(Recorder&r){
    if(r.file==INVALID_HANDLE_VALUE)return;
    // A rotation may evict the oldest segment and change both retained bounds
    // and eviction counters. Build the footer only after that transition.
    if(r.written+512>kFileLimit-kControlReserve&&!Rotate(r))return;
    char b[512];int n=snprintf(b,sizeof b,"{\"event\":\"flight_counters\",\"accepted\":%u,\"busy_drops\":%u,\"lifecycle_drops\":%u,\"history_overwrites\":%u,\"drain_drops\":%u,\"io_errors\":%u,\"segments_evicted\":%u,\"events_evicted\":%u,\"rotation_failures\":%u,\"export_failed\":%u,\"retained_from_seq\":%llu,\"retained_through_seq\":%llu}\n",
        r.accepted.load(),r.busyDrops.load(),r.lifecycleDrops.load(),r.historyOverwrites.load(),r.drainDrops.load(),r.ioErrors.load(),r.segmentsEvicted.load(),r.eventsEvicted.load(),r.rotationFailures.load(),r.exportFailed.load(),(unsigned long long)RetainedFrom(r),(unsigned long long)RetainedThrough(r));
    if(n<=0||(size_t)n>=sizeof b){r.ioErrors.fetch_add(1);return;}
    WriteBytes(r,b,(DWORD)n);
}
void ExportSnapshot(Recorder&r){
    const auto request=r.snapshotRequested.load(std::memory_order_acquire);
    if(request==r.snapshotCompleted.load(std::memory_order_acquire))return;
    auto finish=[&](SnapshotExportResult result){
        r.snapshotResult.store(result,std::memory_order_relaxed);
        r.snapshotCompleted.store(request,std::memory_order_release);
    };
    if(!r.running.load(std::memory_order_acquire)){finish(SnapshotExportResult::Stopped);return;}
    r.snapshotResult.store(SnapshotExportResult::Writing,std::memory_order_release);
    std::unique_ptr<Event[]> events(new(std::nothrow) Event[kCapacity]);
    if(!events){finish(SnapshotExportResult::IoError);return;}
    const auto snapshot=Snapshot(events.get(),kCapacity);
    if(snapshot.busy){finish(SnapshotExportResult::Busy);return;}
    wchar_t path[2112]{};
    int chars=swprintf(path,std::size(path),L"%ls.snapshot-%u.jsonl",r.path,request);
    if(chars<0||(size_t)chars>=std::size(path)){finish(SnapshotExportResult::IoError);return;}
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){finish(SnapshotExportResult::IoError);return;}
    uint64_t written=0;
    auto write=[&](const char*data,int size){
        if(!r.running.load(std::memory_order_acquire)||size<=0||written+(unsigned)size>8u*1024u*1024u)return false;
        DWORD done=0;
        if(!WriteFile(file,data,(DWORD)size,&done,nullptr)||done!=(DWORD)size)return false;
        written+=(unsigned)size;return true;
    };
    char b[4096];
    int n=snprintf(b,sizeof b,"{\"event\":\"flight_snapshot\",\"version\":1,\"request_seq\":%u,\"snapshot_only\":true,\"capacity\":4096,\"captured\":%u,\"qpc_frequency\":%llu,\"pid\":%u}\n",
        request,(unsigned)snapshot.count,(unsigned long long)r.frequency,(unsigned)GetCurrentProcessId());
    bool ok=n>0&&(size_t)n<sizeof b&&write(b,n);
    for(size_t i=0;ok&&i<snapshot.count;++i){n=FormatEvent(r,events[i],b);ok=n>0&&(size_t)n<sizeof b&&write(b,n);}
    if(ok){
        const auto&c=snapshot.counters;
        n=snprintf(b,sizeof b,"{\"event\":\"flight_snapshot_end\",\"request_seq\":%u,\"complete\":true,\"trace_complete\":false,\"counters_concurrent\":true,\"captured\":%u,\"retained_from_seq\":%llu,\"retained_through_seq\":%llu,\"accepted_sample\":%u,\"busy_drops\":%u,\"lifecycle_drops\":%u,\"history_overwrites\":%u,\"drain_drops\":%u,\"io_errors\":%u,\"segments_evicted\":%u,\"events_evicted\":%u,\"rotation_failures\":%u,\"export_failed\":%u}\n",
            request,(unsigned)snapshot.count,(unsigned long long)(snapshot.count?events[0].localSeq:0),(unsigned long long)(snapshot.count?events[snapshot.count-1].localSeq:0),
            c.accepted,c.busyDrops,c.lifecycleDrops,c.historyOverwrites,c.drainDrops,c.ioErrors,c.segmentsEvicted,c.eventsEvicted,c.rotationFailures,c.exportFailed);
        ok=n>0&&(size_t)n<sizeof b&&write(b,n);
    }
    if(!CloseHandle(file))ok=false;
    if(ok)if(auto supplement=g_supplement.load(std::memory_order_acquire)){
        ok=r.running.load(std::memory_order_acquire)&&supplement(path);
        if(!ok&&r.running.load(std::memory_order_acquire)){r.ioErrors.fetch_add(1,std::memory_order_relaxed);r.exportFailed.store(1,std::memory_order_relaxed);}
    }
    finish(ok?SnapshotExportResult::Saved:r.running.load(std::memory_order_acquire)?SnapshotExportResult::IoError:SnapshotExportResult::Stopped);
}
// Existing optional supplement runs on this writer only, outside Record and
// exception handlers. Each attempt gets a new exclusive filename, so a partial
// previous file is preserved rather than overwritten or mistaken for success.
void SessionContext(Recorder&r,bool stopping=false){
    if(r.sessionContextSettled)return;
    auto exporter=g_supplement.load(std::memory_order_acquire);
    const char*status=nullptr;
    if(stopping){status=exporter?"stopped_before_saved":"not_applicable";r.sessionContextSettled=true;}
    else{
        if(!r.enabled.load(std::memory_order_acquire)||!r.running.load(std::memory_order_acquire))return;
        auto now=GetTickCount64();if(now<r.sessionContextDue)return;
        if(!exporter){status="not_applicable";r.sessionContextSettled=true;}
        else{
            r.sessionContextDue=now+1000;++r.sessionContextAttempts;
            wchar_t path[2112]{};int chars=swprintf(path,std::size(path),L"%ls.session-context-%u",r.path,r.sessionContextAttempts);
            bool ok=chars>0&&(size_t)chars<std::size(path)&&r.running.load(std::memory_order_acquire)&&exporter(path);
            if(ok){status="saved";r.sessionContextSettled=true;}
            else{
                // The bool callback cannot distinguish busy metadata from IO;
                // preserve a failed attempt as incomplete export, never an IO claim.
                r.exportFailed.store(1,std::memory_order_relaxed);
                status=r.sessionContextAttempts>=3?"failed":"retry";
                r.sessionContextSettled=r.sessionContextAttempts>=3;
            }
        }
    }
    char b[512];int n=snprintf(b,sizeof b,"{\"event\":\"flight_session_context\",\"status\":\"%s\",\"attempt\":%u,\"attempt_limit\":3,\"recording_epoch\":%u,\"recording_start_ticks\":%llu,\"pid\":%u,\"automatic\":true,\"registry_historical\":true,\"callback_failure_reason_known\":false}\n",status,r.sessionContextAttempts,r.recordingEpoch,(unsigned long long)r.recordingStarted,(unsigned)GetCurrentProcessId());
    if(n<=0||(size_t)n>=sizeof b){r.ioErrors.fetch_add(1,std::memory_order_relaxed);return;}
    if(r.written+(unsigned)n>kFileLimit-kControlReserve&&!Rotate(r))return;
    WriteBytes(r,b,(DWORD)n);
}
DWORD WINAPI Worker(void*param){
    auto*r=static_cast<Recorder*>(param);
    uint64_t cursor=0,lastCounters=0;
    while(r->running.load(std::memory_order_acquire)){
        Drain(*r,cursor);auto now=GetTickCount64();
        if(now-lastCounters>=1000){OutputCounters(*r);OutputCapturePolicy(*r);lastCounters=now;}
        ExportSnapshot(*r);SessionContext(*r);
        Sleep(20);
    }
    FinishDrain(*r,cursor);
    SessionContext(*r,true);
    ExportSnapshot(*r); // settle any request not processed before Stop
    OutputCounters(*r);OutputCapturePolicy(*r); // best effort: Stop has a bounded wait and reports timeout
    for(unsigned i=0;i<r->segmentCount;++i)if(r->segments[i].handle!=INVALID_HANDLE_VALUE){CloseHandle(r->segments[i].handle);r->segments[i].handle=INVALID_HANDLE_VALUE;}
    r->file=INVALID_HANDLE_VALUE;
    if(r->locale){_free_locale(r->locale);r->locale=nullptr;}
    return 0;
}
}
bool Start(const std::wstring&path){
    auto&r=Get();if(path.empty()||path.size()>=std::size(r.path)||r.enabled.load(std::memory_order_acquire)||r.writer)return false;
    LARGE_INTEGER f{},began{};if(!QueryPerformanceFrequency(&f)||f.QuadPart<=0||!QueryPerformanceCounter(&began)||began.QuadPart<=0)return false;
    _locale_t locale=_create_locale(LC_NUMERIC,"C");if(!locale){r.ioErrors.fetch_add(1);return false;}
    if(!TryLock(r)){_free_locale(locale);return false;}
    if(r.criticalGuard.test_and_set(std::memory_order_acquire)){Unlock(r);_free_locale(locale);return false;}
    r.total=0;r.frequency=(uint64_t)f.QuadPart;r.recordingStarted=(uint64_t)began.QuadPart;std::wmemcpy(r.path,path.c_str(),path.size()+1);r.locale=locale;r.segment=0;r.segmentCount=0;
    r.accepted=0;r.busyDrops=0;r.lifecycleDrops=0;r.historyOverwrites=0;r.drainDrops=0;r.ioErrors=0;r.stopIncomplete=false;
    r.segmentsEvicted=0;r.eventsEvicted=0;r.rotationFailures=0;r.exportFailed=0;
    r.snapshotRequested=0;r.snapshotCompleted=0;r.snapshotResult=SnapshotExportResult::None;
    r.sessionContextDue=GetTickCount64()+1000;r.sessionContextAttempts=0;r.sessionContextSettled=false;
    r.attempted=0;r.filtered=0;r.sampledOut=0;r.criticalDrops=0;
    r.scmPreAttempted=0;r.scmPreSampled=0;r.scmPreFiltered=0;r.scmPreRateLimited=0;r.scmPreBusy=0;
    r.scmWindow=0;r.scmAdmitted=0;r.nativeWindow=0;r.nativeAdmitted=0;
    r.nativePreAttempted=0;r.nativePreFiltered=0;r.nativePreSampled=0;r.nativePreBusy=0;r.nativePreBudget=0;
    r.criticalHead=0;r.criticalCount=0;r.criticalBuffered=0;r.criticalPending=0;r.criticalReserveFull=0;r.incompleteMetadata=0;
    r.recordingEpoch=r.epoch.fetch_add(1,std::memory_order_release)+1;
    r.criticalGuard.clear(std::memory_order_release);
    Unlock(r);
    if(!OpenSegment(r,0,true)){_free_locale(r.locale);r.locale=nullptr;return false;}
    HMODULE pinned=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)&Worker,&pinned)){
        CloseHandle(r.file);r.file=INVALID_HANDLE_VALUE;_free_locale(r.locale);r.locale=nullptr;r.ioErrors.fetch_add(1);return false;
    }
    r.running.store(true,std::memory_order_release);
    r.writer=CreateThread(nullptr,0,Worker,&r,0,nullptr);
    if(!r.writer){r.running=false;CloseHandle(r.file);r.file=INVALID_HANDLE_VALUE;_free_locale(r.locale);r.locale=nullptr;r.ioErrors.fetch_add(1);return false;}
    r.enabled.store(true,std::memory_order_release);return true;
}
bool Stop(uint32_t timeoutMs){
    auto&r=Get();r.enabled.store(false,std::memory_order_release);r.epoch.fetch_add(1,std::memory_order_acq_rel);
    if(!r.writer)return !r.stopIncomplete.load(std::memory_order_acquire);
    const auto began=GetTickCount64();
    // A producer that already passed its second epoch check owns this guard.
    // Do not let the writer close its footer until that producer has committed.
    for(;;){
        if(TryLock(r)){
            if(!r.criticalGuard.test_and_set(std::memory_order_acquire)){r.criticalGuard.clear(std::memory_order_release);Unlock(r);break;}
            Unlock(r);
        }
        if(GetTickCount64()-began>=timeoutMs)return false;
        Sleep(1); // lifecycle only, never a game/network callback
    }
    r.running.store(false,std::memory_order_release);
    auto elapsed=GetTickCount64()-began;DWORD remaining=elapsed>=timeoutMs?0:timeoutMs-(DWORD)elapsed;
    // Give a healthy writer a short opportunity to finish its finite tail.
    // Cancel immediately after this grace if an OS write is stalled.
    auto waited=WaitForSingleObject(r.writer,std::min<DWORD>(remaining,50));
    if(waited!=WAIT_OBJECT_0){
        CancelSynchronousIo(r.writer);
        elapsed=GetTickCount64()-began;remaining=elapsed>=timeoutMs?0:timeoutMs-(DWORD)elapsed;
        if(WaitForSingleObject(r.writer,remaining)!=WAIT_OBJECT_0)return false;
    }
    CloseHandle(r.writer);r.writer=nullptr;return !r.stopIncomplete.load(std::memory_order_acquire);
}
bool Enabled()noexcept{return Get().enabled.load(std::memory_order_acquire);}
uint32_t ActiveRecordingToken()noexcept{auto&r=Get();if(!r.enabled.load(std::memory_order_acquire))return 0;const auto token=r.epoch.load(std::memory_order_acquire);return r.enabled.load(std::memory_order_acquire)&&r.epoch.load(std::memory_order_acquire)==token?token:0;}
bool SetSnapshotSupplement(SnapshotSupplement exporter)noexcept{
    auto&r=Get();if(r.enabled.load(std::memory_order_acquire)||r.writer)return false;
    g_supplement.store(exporter,std::memory_order_release);return true;
}
bool Record(Event event)noexcept{
    auto&r=Get();auto epoch=r.epoch.load(std::memory_order_acquire);if(!r.enabled.load(std::memory_order_acquire))return false;
    event.detailKnown|=(event.context.diagnosticFlags&(CriticalState|StateCacheBusy|StateCacheEvicted))<<8;
    NormalizeNumbers(event);if(event.numberFlags)r.incompleteMetadata.fetch_add(1,std::memory_order_relaxed);
    const bool critical=CriticalEvent(event);r.attempted.fetch_add(1,std::memory_order_relaxed);
    LARGE_INTEGER t{};if(!QueryPerformanceCounter(&t))return false;
    event.ticks=(uint64_t)t.QuadPart;event.pid=GetCurrentProcessId();event.tid=GetCurrentThreadId();
    if(!TryLock(r)){if(critical)return DeferCritical(r,event,epoch);r.busyDrops.fetch_add(1,std::memory_order_relaxed);return false;}
    if(!r.enabled.load(std::memory_order_acquire)||r.epoch.load(std::memory_order_acquire)!=epoch){r.lifecycleDrops.fetch_add(1,std::memory_order_relaxed);if(critical)r.criticalDrops.fetch_add(1,std::memory_order_relaxed);Unlock(r);return false;}
    if(!critical){
        const auto&p=r.policy;const auto&c=event.context;
        if(!(p.categories&(1u<<unsigned(EventCategory(event))))||
           (p.peer!=255&&p.peer!=event.localPeer&&p.peer!=c.source&&p.peer!=c.recipient)||
           (p.packet&&p.packet!=c.msg)||
           (p.entity&&p.entity!=c.entity)){r.filtered.fetch_add(1,std::memory_order_relaxed);Unlock(r);return false;}
        uint32_t every=1;uint64_t key=0;
        if(event.stage>=ScmProcessEnter&&event.stage<=ScmCommandLeave&&(event.detailKnown&SpanKnown)){every=p.scmEvery;key=event.span;}
        else if(c.msg!=proto::G_PLAYER_PASSENGER&&(c.msg==proto::G_PLAYER_ONFOOT||c.msg==proto::G_PLAYER_DRIVER||c.msg==proto::G_VEH_SYNC||c.msg==proto::G_PED_SYNC||c.msg==proto::G_OBJ_SYNC||c.msg==proto::G_PROJECTILE_FLIGHT)){every=p.stateEvery;key=c.fingerprint;}
        // One deterministic key at every hop and both ends of an SCM span.
        // Missing keys are retained, not sampled using unrelated local order.
        if(key&&every>1&&key%every){r.sampledOut.fetch_add(1,std::memory_order_relaxed);Unlock(r);return false;}
    }
    Commit(r,event);Unlock(r);return true;
}
Counters GetCounters()noexcept{auto&r=Get();return {r.accepted.load(),r.busyDrops.load(),r.lifecycleDrops.load(),r.historyOverwrites.load(),r.drainDrops.load(),r.ioErrors.load(),r.segmentsEvicted.load(),r.eventsEvicted.load(),r.rotationFailures.load(),r.exportFailed.load()};}
SnapshotResult Snapshot(Event*out,size_t cap)noexcept{
    auto&r=Get();SnapshotResult result;result.counters=GetCounters();if(!out||!cap)return result;
    if(!TryLock(r)){result.busy=true;return result;}
    if(!FlushCriticalLocked(r)){result.busy=true;Unlock(r);return result;}
    result.counters=GetCounters();result.recordingStartTicks=r.recordingStarted;result.recordingEpoch=r.recordingEpoch;result.recordingEnabled=r.enabled.load(std::memory_order_acquire);
    result.count=(size_t)std::min<uint64_t>(std::min<size_t>(cap,kCapacity),r.total);
    uint64_t start=r.total-result.count;
    for(size_t i=0;i<result.count;++i)out[i]=r.history[(size_t)((start+i)%kCapacity)];
    Unlock(r);return result;
}
uint64_t TickFrequency()noexcept{LARGE_INTEGER f{};return QueryPerformanceFrequency(&f)&&f.QuadPart>0?(uint64_t)f.QuadPart:0;}
SnapshotExportResult RequestSnapshotExport()noexcept{
    auto&r=Get();auto epoch=r.epoch.load(std::memory_order_acquire);
    if(!r.enabled.load(std::memory_order_acquire))return SnapshotExportResult::Disabled;
    if(!TryLock(r))return SnapshotExportResult::Busy;
    SnapshotExportResult result=SnapshotExportResult::Queued;
    auto request=r.snapshotRequested.load(std::memory_order_acquire);
    if(!r.enabled.load(std::memory_order_acquire)||r.epoch.load(std::memory_order_acquire)!=epoch)result=SnapshotExportResult::Disabled;
    else if(request!=r.snapshotCompleted.load(std::memory_order_acquire))result=SnapshotExportResult::Busy;
    else if(request>=4)result=SnapshotExportResult::Limit;
    else {r.snapshotResult.store(SnapshotExportResult::Queued,std::memory_order_relaxed);r.snapshotRequested.store(request+1,std::memory_order_release);}
    Unlock(r);return result;
}
SnapshotExportStatus GetSnapshotExportStatus()noexcept{
    auto&r=Get();SnapshotExportStatus status;
    if(!TryLock(r)){status.busy=true;status.result=SnapshotExportResult::Busy;return status;}
    status.requested=r.snapshotRequested.load(std::memory_order_acquire);
    status.completed=r.snapshotCompleted.load(std::memory_order_acquire);
    status.result=r.snapshotResult.load(std::memory_order_acquire);
    if(status.requested>status.completed&&status.result!=SnapshotExportResult::Writing)status.result=SnapshotExportResult::Queued;
    Unlock(r);
    return status;
}
uint64_t NextDiagnosticSpan() noexcept {static thread_local uint64_t next=0;if(!++next)++next;return next;}

Category EventCategory(const Event&e)noexcept{
    if(e.stage>=840&&e.stage<=856)return Category::Ped;
    if(e.stage>=800&&e.stage<820)return Category::Sync;
    if(e.stage>=780&&e.stage<800)return Category::Entity;
    if(e.stage>=820&&e.stage<840)return Category::Mission;
    if(e.stage>=ScmProcessEnter&&e.stage<=ScmCommandLeave)return Category::Scm;
    if(e.stage>=700&&e.stage<720)return Category::Hook;
    if(e.stage>=720&&e.stage<740)return Category::Memory;
    if(e.stage>=600&&e.stage<620)return Category::Assert;
    if(e.stage>=620&&e.stage<640)return Category::Vehicle;
    if(e.stage>=640&&e.stage<660)return Category::Ped;
    if(e.stage>=660&&e.stage<680)return Category::Object;
    if(e.stage>=680&&e.stage<700)return Category::Streaming;
    if(e.stage>=740&&e.stage<760)return Category::Crash;
    if(e.stage>=760&&e.stage<780)return Category::Perf;
    using namespace proto;switch(e.context.msg){
        case C_HELLO:case C_CHAT:case C_PROFILE_SAVE:case C_WORLD_BEGIN:case C_WORLD_CHUNK:case C_WORLD_END:case C_WORLD_REQUEST:case C_STATUS:case C_WORLD_KEEP:case C_WORLD_NEWGAME:case C_WORLD_FOLLOW:case S_WELCOME:case S_REJECT:case S_PLAYER_JOIN:case S_PLAYER_LEAVE:case S_CHAT:case S_PROFILE:case S_WORLD_BEGIN:case S_WORLD_CHUNK:case S_WORLD_END:case S_WORLD_NONE:case S_HOST_CHANGED:case S_PINGS:case S_KICKED:case S_WORLD_WANTED:case S_INVITE:case S_WORLD_NEWGAME:case S_PROFILE_SAVED:case S_WORLD_WAIT:case G_VOICE:return Category::Net;
        case G_PLAYER_ONFOOT:case G_PLAYER_AIM:case G_PLAYER_LOOK:case G_PLAYER_STATS:case G_PLAYER_WEAPONS:case G_PLAYER_ANIM:case G_PED_SYNC:case G_PED_STATUS:case G_PLAYER_EXTRAS:case G_PED_SPEECH:case G_PED_ACTION:return Category::Ped;
        case G_PLAYER_DRIVER:case G_PLAYER_PASSENGER:case G_PLAYER_ENTER_CAR:case G_PLAYER_EXIT_CAR:case G_VEH_SYNC:case G_VEH_STATUS:case G_VEH_SCRIPT_FREE:case G_GARAGE:case G_VEH_CLEAR:case G_WATER_CANNON:case G_TRAIN:case G_PLAYER_CARJACK:case G_VEH_RECENT:case G_SCRIPT_LOCK:case G_GARAGE_DOOR:case G_SCRIPT_VEH_REMOVE:case G_VEH_CONTACT:case G_TYRE_HIT:case G_VEH_HORN:case G_HELI_SHOT:case G_VEH_ATTACHMENT:return Category::Vehicle;
        case G_PLAYER_SPAWN:case G_PLAYER_DEATH:case G_VEH_CREATE:case G_VEH_REMOVE:case G_PED_CREATE:case G_PED_REMOVE:case G_OBJ_CREATE:case G_OBJ_REMOVE:case G_ENTITY_REQUEST:return Category::Entity;
        case G_BULLET:case G_DAMAGE:case G_EXPLOSION:case G_PROJECTILE:case G_SYNC_AUDIT:case G_PROJECTILE_FLIGHT:return Category::Sync;
        case G_VEH_OWNER:case G_VEH_HANDOVER:case G_PED_OWNER:return Category::Ownership;
        case G_RESYNC:return Category::Reconnect;
        case G_OBJ_SYNC:case G_PICKUP_EVENT:case G_PICKUP_CREATE:case G_OBJ_DAMAGE:case G_FIRE:case G_PROPERTY:case G_PROP:case G_OBJ_CARRY:return Category::Object;
        case G_WORLD_STATE:case G_WORLD_OBJ:case G_STORY_WORLD:return Category::Streaming;
        case G_MISSION_START:case G_MISSION_END:case G_TEXT:case G_BLIP:case G_MARKER:case G_ONSCREEN:case G_FADE:case G_CUTSCENE:case G_SCRIPT_VARS:case G_MISSION_REWARD:case G_MISSION_WAIT:case G_MISSION_WEAPON:case G_CHECKPOINTS:case G_SCRIPT_SOUND:case G_STORY_WARP:case G_STORY_PICKUPS:case G_STORY_CONTACTS:case G_ACTIVITY_STATE:case G_AUDIO_RECEIPT:case G_MISSION_STATE:case G_STORY_ACTION:case G_STORY_STAGE:case G_STORY_COOP:case G_STORY_EQUIPMENT:return Category::Mission;
        default:return Category::Packet;
    }
}
const char*CategoryName(Category c)noexcept{
    static const char*names[]={"NET","PACKET","SYNC","ENTITY","OWNERSHIP","VEHICLE","PED","OBJECT","MISSION","SCM","HOOK","MEMORY","STREAMING","RECONNECT","ASSERT","CRASH","PERF"};
    auto n=unsigned(c);return n<unsigned(Category::Count)?names[n]:"UNKNOWN";
}
bool CriticalEvent(const Event&e)noexcept{
    if(e.stage==856)return true; // Selected admitted flush native-state boundaries.
    if(e.stage>=843&&e.stage<850)return true; // selected discrete task lifecycle/gap transitions

    if((e.detailKnown&(CriticalStateHint|StateCacheBusyHint|StateCacheEvictedHint))||(e.context.diagnosticFlags&(CriticalState|StateCacheBusy|StateCacheEvicted)))return true;
    if(e.numberFlags)return true;
    if(e.stage>=680&&e.stage<=688)return true; // discrete deduplicated streaming lifecycle/ready/error transitions
    // Projectile pose0/interpolated18 and intentional policy suppression21..23
    // stay sampled. Missing lifetime/body and admission failures are critical.
    if(e.stage==812)return e.reason!=0&&e.reason!=18&&e.reason!=21&&e.reason!=22&&e.reason!=23;
    if(e.stage==650||e.stage==651||e.stage==661||e.stage==662||e.stage==665||e.stage==666)return true;
    if(e.stage>=780&&e.stage<800)return e.stage!=783||e.reason!=0;
    if(e.stage>=800&&e.stage<820&&e.stage!=812)return true;
    if(e.stage>=820&&e.stage<840)return true;
    // Existing passenger causal chain remains complete by default.
    if(e.context.msg==proto::G_PLAYER_PASSENGER)return true;
    if(e.stage>=600&&e.stage<620)return true;
    if(e.stage>=700&&e.stage<760)return true;
    if(e.stage>=620&&e.stage<700&&e.reason)return true;
    if(e.stage>=1&&e.stage<=12&&e.reason)return true;
    if(e.stage==passenger_trace::Metadata&&e.reason==passenger_trace::Malformed)return true;
    if((e.stage==passenger_trace::DispatchEnd||e.stage==passenger_trace::ServiceDispatchEnd)&&e.reason!=passenger_trace::Ok&&e.reason!=passenger_trace::HandlerReturned)return true;
    if(e.stage>=ScmProcessEnter&&e.stage<=ScmCommandLeave)return e.reason==1||e.reason==2||e.reason==3||e.reason>=0x300||(e.reason>=0x100&&(e.reason&0xff)>=2);
    using namespace proto;switch(e.context.msg){
        case G_PLAYER_SPAWN:case G_PLAYER_DEATH:case G_VEH_CREATE:case G_VEH_REMOVE:case G_VEH_OWNER:case G_VEH_HANDOVER:case G_DAMAGE:
        case G_PED_CREATE:case G_PED_REMOVE:case G_PED_OWNER:case G_PED_STATUS:case G_OBJ_CREATE:case G_OBJ_REMOVE:
        case G_STORY_WORLD:case G_PICKUP_CREATE:case G_VEH_CLEAR:case G_SCRIPT_VEH_REMOVE:case G_SEAT_ASSIGN:case G_PROP:case G_FIRE:
        case G_VEH_ATTACHMENT:case G_RESYNC:return true;
        default:return false;
    }
}
bool SetCapturePolicy(CapturePolicy policy)noexcept{
    if(policy.categories&~kAllCategories||!policy.stateEvery||policy.stateEvery>65536||!policy.scmEvery||policy.scmEvery>65536||policy.scmSpansPerSecond>65536||policy.nativeObservationsPerSecond>65536)return false;
    auto&r=Get();if(!TryLock(r))return false;r.policy=policy;Unlock(r);return true;
}
bool AdmitScmSpan(uint64_t span,bool critical)noexcept{
    auto&r=Get();const auto epoch=r.epoch.load(std::memory_order_acquire);
    if(!r.enabled.load(std::memory_order_acquire))return false;
    r.scmPreAttempted.fetch_add(1,std::memory_order_relaxed);
    if(critical)return true;
    if(!TryLock(r)){r.scmPreBusy.fetch_add(1,std::memory_order_relaxed);return false;}
    bool admitted=false;
    if(!r.enabled.load(std::memory_order_acquire)||r.epoch.load(std::memory_order_acquire)!=epoch){
        r.scmPreBusy.fetch_add(1,std::memory_order_relaxed);
    }else if(!(r.policy.categories&(1u<<unsigned(Category::Scm)))){
        r.scmPreFiltered.fetch_add(1,std::memory_order_relaxed);
    }else if(span&&r.policy.scmEvery>1&&span%r.policy.scmEvery){
        r.scmPreSampled.fetch_add(1,std::memory_order_relaxed);
    }else{
        const auto window=GetTickCount64()/1000;
        if(window!=r.scmWindow){r.scmWindow=window;r.scmAdmitted=0;}
        if(r.policy.scmSpansPerSecond&&r.scmAdmitted>=r.policy.scmSpansPerSecond){
            r.scmPreRateLimited.fetch_add(1,std::memory_order_relaxed);
        }else{++r.scmAdmitted;admitted=true;}
    }
    Unlock(r);return admitted;
}
CaptureStatistics CaptureStats()noexcept{
    auto&r=Get();CaptureStatistics s;auto readCounters=[&]{s.attempted=r.attempted.load();s.filtered=r.filtered.load();s.sampledOut=r.sampledOut.load();s.criticalDrops=r.criticalDrops.load();
    s.scmPreAttempted=r.scmPreAttempted.load();s.scmPreSampled=r.scmPreSampled.load();s.scmPreFiltered=r.scmPreFiltered.load();s.scmPreRateLimited=r.scmPreRateLimited.load();s.scmPreBusy=r.scmPreBusy.load();
    s.incompleteMetadata=r.incompleteMetadata.load();s.criticalBuffered=r.criticalBuffered.load();s.criticalPending=r.criticalPending.load();s.criticalReserveFull=r.criticalReserveFull.load();
    s.nativePreAttempted=r.nativePreAttempted.load();s.nativePreFiltered=r.nativePreFiltered.load();s.nativePreSampled=r.nativePreSampled.load();s.nativePreBusy=r.nativePreBusy.load();s.nativePreBudget=r.nativePreBudget.load();
    };readCounters();
    if(!TryLock(r)){s.busy=true;return s;}readCounters();s.policy=r.policy;s.recordingStartTicks=r.recordingStarted;s.recordingEpoch=r.recordingEpoch;s.recordingEnabled=r.enabled.load(std::memory_order_acquire);Unlock(r);return s;
}

NativeAdmission AdmitNativeObservation(const Context&context,uint16_t stage,uint8_t localPeer,bool critical)noexcept{
    auto&r=Get();const auto epoch=r.epoch.load(std::memory_order_acquire);
    if(!r.enabled.load(std::memory_order_acquire))return NativeAdmission::Disabled;
    r.nativePreAttempted.fetch_add(1,std::memory_order_relaxed);
    if(!TryLock(r)){r.nativePreBusy.fetch_add(1,std::memory_order_relaxed);return NativeAdmission::Busy;}
    NativeAdmission result=NativeAdmission::Admitted;
    const auto&p=r.policy;Event event{};event.context=context;event.stage=stage;event.localPeer=localPeer;
    if(!r.enabled.load(std::memory_order_acquire)||r.epoch.load(std::memory_order_acquire)!=epoch){result=NativeAdmission::Disabled;}
    else if(!critical&&(!(p.categories&(1u<<unsigned(EventCategory(event))))||
        (p.peer!=255&&p.peer!=localPeer&&p.peer!=context.source&&p.peer!=context.recipient)||
        (p.packet&&p.packet!=context.msg)||(p.entity&&p.entity!=context.entity))){
        r.nativePreFiltered.fetch_add(1,std::memory_order_relaxed);result=NativeAdmission::Filtered;
    }else if(!critical&&context.fingerprint&&p.stateEvery>1&&context.fingerprint%p.stateEvery){
        r.nativePreSampled.fetch_add(1,std::memory_order_relaxed);result=NativeAdmission::Sampled;
    }else{
        const auto window=GetTickCount64()/1000;
        if(window!=r.nativeWindow){r.nativeWindow=window;r.nativeAdmitted=0;}
        if(p.nativeObservationsPerSecond&&r.nativeAdmitted>=p.nativeObservationsPerSecond){
            r.nativePreBudget.fetch_add(1,std::memory_order_relaxed);result=NativeAdmission::Budget;
        }else ++r.nativeAdmitted;
    }
    Unlock(r);return result;
}

}
